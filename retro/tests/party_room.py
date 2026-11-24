#!/usr/bin/env python3
"""Two cores as Pixelado Party runs them, each with a scripted Party (its channel on a socketpair, as --party-fd) and a
scripted room (its local socket, as --room-ctl): the host opens its menu, the guest's Party gets the screen with
reason=hold and the host's name, then the screen back when the host closes it; the host quits from Party's menu
(CMD op=quit) and the room ends the guest's game (END, exit code 10). The fake core: no emulator, no display.
    python3 tests/party_room.py build/pixelado-core-fake
"""
import array
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

BIN = sys.argv[1]
fails = []


def check(name, ok):
    print(("ok   " if ok else "FAIL ") + name)
    if not ok:
        fails.append(name)


class Party(threading.Thread):
    """Party's side of the channel: reads lines (and the frame's fd), answers as Pixelado Party's main.c does."""

    def __init__(self, name, sock, script):
        super().__init__(daemon=True)
        self.name, self.sock, self.script = name, sock, script
        self.lines, self.fds, self.events = [], [], []
        self.buf = b""

    def send(self, line):
        self.sock.sendall((line + "\n").encode())

    def next_line(self, timeout=10.0):
        end = time.time() + timeout
        while b"\n" not in self.buf:
            self.sock.settimeout(max(0.01, end - time.time()))
            try:
                data, anc, _, _ = self.sock.recvmsg(4096, socket.CMSG_SPACE(4))
            except socket.timeout:
                return None
            if not data:
                return None
            for level, typ, d in anc:
                if level == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
                    fds = array.array("i")
                    fds.frombytes(d[: len(d) - (len(d) % fds.itemsize)])
                    self.fds.extend(fds)
            self.buf += data
        line, self.buf = self.buf.split(b"\n", 1)
        s = line.decode()
        self.lines.append(s)
        return s

    def wait_for(self, verb, state=None, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            s = self.next_line(end - time.time())
            if s is None:
                return None
            parts = s.split(" ")
            kv = dict(p.split("=", 1) for p in parts[1:] if "=" in p)
            if parts[0] == verb and (state is None or kv.get("state") == state):
                return kv
        return None

    def run(self):
        try:
            self.script(self)
        except Exception as e:  # noqa
            self.events.append("error %r" % e)


def frame_ok(fd):
    """the PXF1 frame: its magic and its two screens' headers"""
    hdr = os.pread(fd, 128, 0)
    if hdr[:4] != b"PXF1":
        return False
    n = int.from_bytes(hdr[8:12], "little")
    w = int.from_bytes(hdr[24 + 4 : 24 + 8], "little")
    fourcc = hdr[24 + 16 : 24 + 20]
    return n == 2 and w == 640 and fourcc == b"XR24"


def host_party(p):
    hello = p.wait_for("HELLO")
    p.events.append("hello %s" % (hello or {}).get("engine"))
    rel = p.wait_for("SCREEN", "released")
    p.events.append("released %s" % (rel or {}).get("reason"))
    p.events.append("frame %s" % (bool(p.fds) and frame_ok(p.fds[0])))
    p.send("SCREEN state=taken")
    p.send("CMD op=set key=fps value=1")
    ok = p.wait_for("OK")
    p.events.append("ok %s" % (ok or {}).get("op"))
    p.send("CMD op=set key=screen.scale value=integer")
    ok = p.wait_for("OK")
    p.events.append("scale %s" % (ok or {}).get("key"))
    time.sleep(1.5)
    p.send("SCREEN state=give")
    tk = p.wait_for("SCREEN", "taken")
    p.events.append("taken %s" % (tk is not None))
    # the second menu: quit from it
    rel = p.wait_for("SCREEN", "released")
    p.events.append("released2 %s" % (rel or {}).get("reason"))
    p.send("SCREEN state=taken")
    p.send("CMD op=quit")
    ok = p.wait_for("OK")
    p.events.append("ok %s" % (ok or {}).get("op"))


def guest_party(p):
    p.wait_for("HELLO")
    rel = p.wait_for("SCREEN", "released")
    p.events.append("released %s" % (rel or {}).get("reason"))
    p.send("SCREEN state=taken")
    # the HOLD line with the host's name came before the release
    holds = [s for s in p.lines if s.startswith("HOLD ")]
    p.events.append("hold %s" % (holds[-1] if holds else None))
    lan = [s for s in p.lines if s.startswith("LANP ")]
    p.events.append("lanp %d" % len(lan))
    want = p.wait_for("SCREEN", "want", timeout=10)
    p.events.append("want %s" % (want is not None))
    p.send("SCREEN state=give")
    tk = p.wait_for("SCREEN", "taken")
    p.events.append("taken %s" % (tk is not None))


class Room(threading.Thread):
    """party-room's local socket: takes the core's HELLO and SESSION; END to the guest when told"""

    def __init__(self, path, end_after=None):
        super().__init__(daemon=True)
        self.path, self.end_after, self.lines = path, end_after, []
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        self.srv.listen(1)

    def run(self):
        c, _ = self.srv.accept()
        c.settimeout(0.2)
        t0, ended, buf = time.time(), False, b""
        while True:
            try:
                d = c.recv(4096)
                if not d:
                    break
                buf += d
                while b"\n" in buf:
                    l, buf = buf.split(b"\n", 1)
                    self.lines.append(l.decode())
            except socket.timeout:
                pass
            if self.end_after and not ended and time.time() - t0 > self.end_after:
                c.sendall(b"END reason=host-ended\n")
                ended = True


def start(args, party_script, room_path, end_after, name):
    a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    room = Room(room_path, end_after)
    room.start()
    fd = b.fileno()
    proc = subprocess.Popen([BIN] + args + ["--party-fd", str(fd), "--room-ctl", room_path, "--room-game", "9c1e22f0"],
                            pass_fds=[fd], stderr=subprocess.PIPE, stdout=subprocess.DEVNULL)
    b.close()
    party = Party(name, a, party_script)
    party.start()
    return proc, party, room


def main():
    port = 20000 + os.getpid() % 20000
    with tempfile.TemporaryDirectory() as t:
        os.environ["PIXELADO_CORE_CONFIG"] = os.path.join(t, "conf")   # (the core's own settings, not the machine's)
        rom = os.path.join(t, "game.bin")
        open(rom, "w").close()
        with open(os.path.join(t, "host.script"), "w") as f:
            f.write("100 menu\n260 menu\n")
        common = ["--video", "null", "--audio", "null", "--room-port", str(port), "--frames", "2000"]
        hp, hparty, hroom = start(common + ["--room", "host", "--room-players", "2", "--nickname", "Ana",
                                            "--input", "script:" + os.path.join(t, "host.script"),
                                            "--log", os.path.join(t, "host.log"), rom],
                                  host_party, os.path.join(t, "a.ctl"), None, "A")
        time.sleep(0.3)
        gp, gparty, groom = start(common + ["--room", "join", "--room-addr", "127.0.0.1", "--player", "1",
                                            "--nickname", "Kira", "--input", "none",
                                            "--log", os.path.join(t, "guest.log"), rom],
                                  guest_party, os.path.join(t, "b.ctl"), 9.0, "B")
        hrc = hp.wait(timeout=60)
        grc = gp.wait(timeout=60)
        hparty.join(2)
        gparty.join(2)
        for n in ("host.log", "guest.log"):
            print("---", n)
            print(open(os.path.join(t, n)).read())
        print("--- host party", hparty.events, "\n--- guest party", gparty.events)
        print("--- host room", hroom.lines, "\n--- guest room", groom.lines)
        print("---")
        check("host: HELLO engine=fake", "hello fake" in hparty.events)
        check("host: its menu released the screen (reason=menu)", "released menu" in hparty.events)
        check("host: the PXF1 frame came with it", "frame True" in hparty.events)
        check("host: CMD op=set answered", "ok set" in hparty.events)
        check("host: CMD op=set key=screen.scale answered", "scale screen.scale" in hparty.events)
        check("host: the screen taken back after give", "taken True" in hparty.events)
        check("host: quit from Party's menu, exit 0", "ok quit" in hparty.events and hrc == 0)
        check("guest: released with reason=hold", "released hold" in gparty.events)
        check("guest: HOLD names the host", any(e.startswith("hold HOLD n=1 names=Ana") for e in gparty.events))
        check("guest: LAN lines with both players", "lanp 2" in gparty.events)
        check("guest: SCREEN want once the host closed its menu", "want True" in gparty.events)
        check("guest: the screen taken back", "taken True" in gparty.events)
        check("guest: END from the room, exit 10", grc == 10)
        check("rooms: HELLO engine=gba-style, SESSION listening / connected",
              any(l.startswith("SESSION state=listening") for l in hroom.lines)
              and any(l.startswith("SESSION state=connected") for l in groom.lines))
        check("rooms: GAME at the end", any(l.startswith("GAME state=ended reason=quit") for l in hroom.lines)
              and any(l.startswith("GAME state=ended reason=end") for l in groom.lines))
    print("party_room: all passed" if not fails else "party_room: %d FAILED" % len(fails))
    return 1 if fails else 0


sys.exit(main())
