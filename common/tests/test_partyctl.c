/* The channel with Party and the screen handover, without a display: the core (a child process with a fake host)
 * against a scripted Party (this process) over a socketpair, as Pixelado Party runs a core.
 *   make -C common test */
#include "pixelado/line.h"
#include "pixelado/partyctl.h"
#include "pixelado/roomctl.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the core's side: a fake host that records what Party asked for ---- */
typedef struct {
    int vol, bri, scale, sound, holds_on, holds_off, quits, others_ms, masters_dropped, masters_taken, repaints, idles;
    long long t0;
} fake;

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}
static int f_drop(void *c) { ((fake *)c)->masters_dropped++; return 1; }
static int f_take(void *c, int ms) { (void)ms; ((fake *)c)->masters_taken++; return 1; }
static void f_size(void *c, int *w, int *h) { (void)c; *w = 64; *h = 48; }
static int f_grab(void *c, int panel, uint32_t *dst, int w, int h)
{
    int i;
    (void)c;
    for (i = 0; i < w * h; i++) dst[i] = panel ? 0x00445566u : 0x00112233u;
    return 1;
}
static void f_repaint(void *c) { ((fake *)c)->repaints++; }
static void f_hold(void *c, int on) { if (on) ((fake *)c)->holds_on++; else ((fake *)c)->holds_off++; }
static int f_others(void *c, char *names, size_t cap)
{
    fake *f = c;
    if (f->others_ms > 0 && now_ms() - f->t0 < f->others_ms) { snprintf(names, cap, "Kira"); return 1; }
    names[0] = 0;
    return 0;
}
static void f_open(void *c, int *v, int *b, int *fps, int *scale) { (void)c; *v = 50; *b = 80; *fps = 0; *scale = 1; }
static void f_set(void *c, const char *k, int v) { fake *f = c; if (!strcmp(k, "volume")) f->vol = v; if (!strcmp(k, "brightness")) f->bri = v; if (!strcmp(k, "screen.scale")) f->scale = v; }
static int f_tick(void *c, int *v, int *b) { (void)c; (void)v; (void)b; return 0; }
static void f_close(void *c) { (void)c; }
static void f_sound(void *c, int id) { ((fake *)c)->sound = id; }
static void f_quit(void *c) { ((fake *)c)->quits++; }
static int f_held(void *c) { (void)c; return 0; }
static int f_stop(void *c) { (void)c; return 0; }
static void f_idle(void *c) { ((fake *)c)->idles++; }
static int f_lan(void *c, char *out, size_t cap)
{
    (void)c;
    snprintf(out, cap, "LAN have=1 mode=1 state=2 np=2 max=2 lock=1\nLANP name=Ana host=1 me=1 ping=0 hold=0\n"
                       "LANP name=Kira host=0 me=0 ping=4 hold=0\nLAN_END");
    return 1;
}
static void f_log(void *c, const char *l) { (void)c; fprintf(stderr, "  core: %s", l); }

static int core_child(int fd, int scenario)
{
    fake f;
    px_party_host h;
    px_party_menu_result r;
    memset(&f, 0, sizeof f);
    memset(&h, 0, sizeof h);
    h.ctx = &f;
    h.drop_master = f_drop; h.take_master = f_take; h.panel_size = f_size; h.grab = f_grab; h.repaint = f_repaint;
    h.hold_set = f_hold; h.hold_others = f_others; h.opts_open = f_open; h.opt_set = f_set; h.opts_tick = f_tick;
    h.opts_close = f_close; h.sound = f_sound; h.quit = f_quit; h.menu_held_ms = f_held; h.stopping = f_stop;
    h.idle = f_idle; h.lan_lines = f_lan; h.log = f_log;
    if (!px_party_open(fd, "gba", "pause-own,screen", "Link Test", 0, "9c1e22f0")) return 90;
    f.t0 = now_ms();
    if (scenario == 4) f.others_ms = 300;
    r = px_party_menu(&h, scenario != 4);
    switch (scenario) {
    case 1:   /* menu, settings, Party gives the screen back */
        if (r != PX_PARTY_DONE) return 10;
        if (f.vol != 30) return 11;
        if (f.scale != 0) return 16;   /* CMD op=set key=screen.scale value=integer */
        if (f.sound != 1) return 12;
        if (f.holds_off != 1) return 13;
        if (f.masters_dropped != 1 || f.masters_taken != 1 || f.repaints != 1) return 14;
        if (f.idles < 1) return 15;
        return 0;
    case 2:   /* Party silent: our own menu */
        return r == PX_PARTY_FALLBACK && f.masters_taken == 1 ? 0 : 20;
    case 3:   /* quit */
        return r == PX_PARTY_QUIT && f.quits == 1 ? 0 : 30;
    case 4:   /* another player's menu: the wait screen until it is over */
        return r == PX_PARTY_DONE && f.holds_on == 0 ? 0 : 40;
    }
    return 99;
}

/* ---- Party's side ---- */
static px_rx rx;
static int pending_fd = -1;

static int read_line(int fd, char *line, size_t cap, int ms)
{
    long long end = now_ms() + ms;
    for (;;) {
        char b[4096];
        char cb[CMSG_SPACE(sizeof(int))];
        struct iovec io;
        struct msghdr mh;
        struct pollfd p;
        ssize_t n;
        if (px_rx_next(&rx, line, cap)) return 1;
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, (int)(end - now_ms() > 0 ? end - now_ms() : 0)) <= 0) return 0;
        io.iov_base = b;
        io.iov_len = sizeof b;
        memset(&mh, 0, sizeof mh);
        mh.msg_iov = &io;
        mh.msg_iovlen = 1;
        mh.msg_control = cb;
        mh.msg_controllen = sizeof cb;
        n = recvmsg(fd, &mh, 0);
        if (n <= 0) return 0;
        {
            struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
            if (c && c->cmsg_type == SCM_RIGHTS) memcpy(&pending_fd, CMSG_DATA(c), sizeof(int));
        }
        px_rx_feed(&rx, b, (size_t)n);
    }
}

static void say(int fd, const char *s)
{
    char b[512];
    snprintf(b, sizeof b, "%s\n", s);
    if (write(fd, b, strlen(b)) < 0) perror("write");
}

/* reads lines until one with this verb (and state, if given); 1: found */
static int expect(int fd, const char *verb, const char *state, px_line *out)
{
    char line[PX_LINE_MAX];
    while (read_line(fd, line, sizeof line, 3000)) {
        px_line l;
        px_line_parse(&l, line);
        printf("  party <- %s\n", line);
        if (!strcmp(l.verb, verb) && (!state || !strcmp(px_line_str(&l, "state", ""), state))) {
            if (out) *out = l;
            return 1;
        }
    }
    return 0;
}

static int run(int scenario)
{
    int sv[2], st = 0;
    pid_t pid;
    px_line l;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { perror("socketpair"); return 1; }
    memset(&rx, 0, sizeof rx);
    pending_fd = -1;
    pid = fork();
    if (pid == 0) {
        close(sv[0]);
        _exit(core_child(sv[1], scenario));
    }
    close(sv[1]);
    printf("scenario %d\n", scenario);
    CHECK(expect(sv[0], "HELLO", NULL, &l), "no HELLO");
    CHECK(!strcmp(px_line_str(&l, "engine", ""), "gba") && !strcmp(px_line_str(&l, "title", ""), "Link Test"),
          "HELLO engine/title");
    {
        px_line o;
        CHECK(expect(sv[0], "OPT", NULL, &o), "no OPT");
        if (scenario == 1) {
            int found = strcmp(px_line_str(&o, "key", ""), "screen.scale") == 0;
            while (!found && expect(sv[0], "OPT", NULL, &o)) found = strcmp(px_line_str(&o, "key", ""), "screen.scale") == 0;
            CHECK(found && !strcmp(px_line_str(&o, "type", ""), "enum") && !strcmp(px_line_str(&o, "value", ""), "1") &&
                      !strcmp(px_line_str(&o, "choices", ""), "integer,fit"), "OPT screen.scale enum");
        }
    }
    CHECK(expect(sv[0], "SCREEN", "released", &l), "no SCREEN released");
    if (scenario == 1) {
        uint8_t *m;
        CHECK(pending_fd >= 0, "no frame fd");
        if (pending_fd >= 0) {
            m = mmap(NULL, 128 + 2 * 64 * 4 * 48, PROT_READ, MAP_SHARED, pending_fd, 0);
            CHECK(m != MAP_FAILED, "mmap the frame");
            if (m != MAP_FAILED) {
                uint32_t top, bot, w, fourcc;
                memcpy(&w, m + 24 + 4, 4);
                memcpy(&fourcc, m + 24 + 16, 4);
                memcpy(&top, m + 128, 4);
                memcpy(&bot, m + 128 + 64 * 4 * 48, 4);
                CHECK(!memcmp(m, "PXF1", 4), "PXF1 magic");
                CHECK(w == 64 && fourcc == PX_FOURCC_XR24, "PXF1 screen header");
                CHECK(top == 0x00112233u && bot == 0x00445566u, "PXF1 pixels %08x %08x", top, bot);
                munmap(m, 128 + 2 * 64 * 4 * 48);
            }
        }
    }
    if (scenario == 2) {   /* Party never answers */
        waitpid(pid, &st, 0);
        close(sv[0]);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "scenario 2 child %d", WEXITSTATUS(st));
        return 0;
    }
    say(sv[0], "SCREEN state=taken");
    if (scenario == 1) {
        say(sv[0], "CMD op=set key=volume value=30");
        CHECK(expect(sv[0], "OK", NULL, &l) && !strcmp(px_line_str(&l, "key", ""), "volume"), "OK op=set");
        say(sv[0], "CMD op=set key=screen.scale value=integer");
        CHECK(expect(sv[0], "OK", NULL, &l) && !strcmp(px_line_str(&l, "key", ""), "screen.scale"), "OK op=set screen.scale");
        say(sv[0], "SOUND id=select");
        say(sv[0], "PING t=5");
        CHECK(expect(sv[0], "PONG", NULL, &l) && !strcmp(px_line_str(&l, "t", ""), "5"), "PONG");
        say(sv[0], "CMD op=lan mode=2 players=2 addr=1.2.3.4");
        CHECK(expect(sv[0], "ERR", NULL, &l) && !strcmp(px_line_str(&l, "op", ""), "lan"), "ERR op=lan");
        say(sv[0], "HOLD state=off");
        CHECK(expect(sv[0], "SCREEN", "want", NULL), "no SCREEN want after HOLD off");
        say(sv[0], "SCREEN state=give");
        CHECK(expect(sv[0], "SCREEN", "taken", NULL), "no SCREEN taken after give");
    } else if (scenario == 3) {
        say(sv[0], "CMD op=quit");
        CHECK(expect(sv[0], "OK", NULL, &l) && !strcmp(px_line_str(&l, "op", ""), "quit"), "OK op=quit");
    } else if (scenario == 4) {
        CHECK(expect(sv[0], "HOLD", NULL, &l), "HOLD");
        CHECK(expect(sv[0], "SCREEN", "want", NULL), "no SCREEN want once the others let go");
        say(sv[0], "SCREEN state=give");
        CHECK(expect(sv[0], "SCREEN", "taken", NULL), "no SCREEN taken after give");
    }
    waitpid(pid, &st, 0);
    close(sv[0]);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "scenario %d: the core says %d", scenario, WEXITSTATUS(st));
    if (pending_fd >= 0) close(pending_fd);
    return 0;
}

/* the room's socket: HELLO, SESSION, END, GAME */
static int run_room(void)
{
    char path[] = "/tmp/pxroom-test-XXXXXX", sock[64], line[PX_LINE_MAX];
    struct sockaddr_un su;
    int ls, c, st = 0, got = 0;
    pid_t pid;
    if (!mkdtemp(path)) return 1;
    snprintf(sock, sizeof sock, "%s/room.ctl", path);
    ls = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&su, 0, sizeof su);
    su.sun_family = AF_UNIX;
    snprintf(su.sun_path, sizeof su.sun_path, "%s", sock);
    if (bind(ls, (struct sockaddr *)&su, sizeof su) || listen(ls, 1)) { perror("bind"); return 1; }
    printf("room socket\n");
    pid = fork();
    if (pid == 0) {
        int i;
        if (!px_room_open(sock, "gba", "9c1e22f0", "pause-own")) _exit(50);
        px_room_session("listening", NULL);
        for (i = 0; i < 300 && !px_room_poll(); i++) usleep(10000);
        if (!px_room_ended()) _exit(51);
        px_room_close("end");
        _exit(0);
    }
    c = accept(ls, NULL, NULL);
    memset(&rx, 0, sizeof rx);
    while (read_line(c, line, sizeof line, 3000)) {
        px_line l;
        px_line_parse(&l, line);
        printf("  room <- %s\n", line);
        if (!strcmp(l.verb, "HELLO")) got |= !strcmp(px_line_str(&l, "engine", ""), "gba") ? 1 : 0;
        if (!strcmp(l.verb, "SESSION")) { got |= 2; say(c, "END reason=host-ended"); }
        if (!strcmp(l.verb, "GAME")) { got |= !strcmp(px_line_str(&l, "reason", ""), "end") ? 4 : 0; break; }
    }
    waitpid(pid, &st, 0);
    close(c);
    close(ls);
    unlink(sock);
    rmdir(path);
    CHECK(got == 7, "room lines (%d)", got);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "room core says %d", WEXITSTATUS(st));
    return 0;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    run(1);
    run(2);
    run(3);
    run(4);
    run_room();
    printf(fails ? "common: %d FAILED\n" : "common: all passed\n", fails);
    return fails ? 1 : 0;
}
