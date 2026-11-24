# retro: pixelado-retro, the minimal libretro frontend

The frontend the retro cores are built on (`pixelado-core-gba` today, `-snes` later). Not RetroArch: one libretro core
linked in statically, the handheld's panels, sound and buttons, the link between the handhelds, and Pixelado Party's
channel (`common/`). About 3,000 lines of C99.

| File | What |
|---|---|
| `src/main.c` | the options, the loop at the core's own rate, the pause (Party's menu or our own card), the exit codes |
| `src/core.c` | the libretro host: environment, options (`SET_VARIABLES`), the save RAM in a `.srm` |
| `src/video.c` | KMS: the game on one panel, scaled by a whole number and centred; the card on the other; or `null` |
| `src/audio.c` | ALSA `default` at the core's rate (BananaOS's plug converts), non-blocking; or `null` |
| `src/input.c` | evdev, mapped as BananaOS maps it; or a script (tests) |
| `src/link.c` | libretro's netpacket interface over TCP between the handhelds, with the group's pause |
| `src/card.c` | the card on the second panel, in a 5x7 pixel font |
| `src/system.c` | BananaOS's volume, brightness and menu sounds, for Party's menu |
| `include/libretro.h` | libretro's API header (MIT, the RetroArch team) |
| `tests/` | a fake core, the link's loopback test, the Party and room test |

## On the handhelds

- **Picture:** DRM master on the card with the two DSI panels (the top one is `BANANADS_TOP`'s connector, `DSI-2` on
  the RG DS, as BananaDS finds it). The game goes on the top panel (`--screen bottom` for the other), scaled by the
  largest whole number that fits and centred: a GBA at 2x on the RG DS's 640x480, 4x on the RG DS Plus's 1024x768.
  Three dumb buffers and page flips: the newest frame waits for the next flip. The other panel shows a card: the game,
  the player, the link, the pause.
- **Pace:** the core's own rate (the GBA's 59.7275 Hz), each frame on an absolute deadline, nudged by at most 0.5%
  to keep the sound buffer near 40 ms; the 60 Hz panels show a frame twice every ~4 s.
- **Sound:** ALSA `default` at the core's rate (the GBA core asks gpSP for 32768 Hz), stereo, 64 ms of buffer.
- **Buttons:** every pad and keyboard under `/dev/input`, read, never grabbed. A east and B south (Nintendo layout),
  X, Y, L, R, Start, Select, the d-pad, the hat and the left stick as the d-pad. F + Start quits; F alone, HOME, F2 or
  L3 open the menu when let go (and only if pressed while the game had the screen).
- **Saves:** the core's save RAM in `<save-dir>/<save-name>.srm`, RetroArch's format (the raw save), checked every
  2 s and written whole beside it then renamed over it; and at the end.

## The link

libretro's netpacket interface (`RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE`), as RetroArch's netplay gives it to a
core, over TCP: the host listens (`--room-port`, 7067), each guest connects (tried for 15 s), and the host relays.
Client id 0 is the host, the guests ask for their seat (`--player`) as their id. The core's packets are reliable and in
order, delivered between frames or when the core polls; both sides must run the same core protocol
(`protocol_version`), else the host refuses the guest.

Frames: `[u32 length, big endian][u8 type][payload]`.

| Type | Who | Payload |
|---|---|---|
| 1 `HELLO` | guest → host | `proto=<the core's protocol> player=<seat> name=<nickname>` |
| 2 `WELCOME` | host → guest | `id=<client id> max=<players>` |
| 3 `DENY` | host → guest | `reason=version\|full` |
| 4 `PKT` | both | u16 from, u16 to (`0xFFFF` everyone), the core's bytes |
| 5 `HOLD` | both | u16 who, u8 on: a player's menu holds the group |
| 6 `PING`, 7 `PONG` | both | u64 the sender's clock, echoed (every second; 10 s of silence and the peer is gone) |
| 8 `PEER` | host → guest | u16 id, u8 present, the name |

**The group's pause:** a player's menu sends `HOLD on`; every core stops at the end of its frame until nobody holds
the group, so no frame runs anywhere and a core's link timeout (gpSP's: 240 frames) never counts while one side is
paused. The pings go on.

## Options

```
pixelado-core-<sys> [options] ROM
  --room host|join  --room-addr IP  --room-port N (7067)  --room-players N  --player N  --nickname NAME
  --party-fd FD (PIXELADO_PARTY_FD)  --room-ctl SOCK (PIXELADO_ROOM_CTL)  --room-game GID (PIXELADO_ROOM_GAME)
  --set party.fd=|room.ctl=|room.gid=|link.port=      (the DS core's spelling)
  --save-dir DIR  --save-name NAME  --system-dir DIR (the BIOS)  --option key=value (a core option)  --title TEXT
  --screen top|bottom  --top CONNECTOR (BANANADS_TOP)  --video kms|null  --audio alsa|null  --input evdev|none|script:F
  --frames N  --dump-frame N:FILE.ppm  --unpaced  --test-pause-ms N  --log FILE  --fps
```

## Build and test

```sh
make -C retro SYS=gba CORE_LIB=/path/libgpsp.a VERSION=0.1.0     # gba/build/ does this for the handhelds
make -C retro fake test                                           # the loopback test with the fake core (a Mac too)
python3 retro/tests/party_room.py retro/build/pixelado-core-fake  # two cores with a scripted Party and room
```
