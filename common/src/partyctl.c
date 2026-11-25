/* See pixelado/partyctl.h. The channel is only read while Party has the screen (and for a moment as it is let go of):
 * outside that, Party has nothing to say to the core. A C port of the DS core's partyctl.cpp (cores/nds/bananads/patches/bananads
 * 0008), with the same lines and the same timings. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "pixelado/partyctl.h"
#include "pixelado/line.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* how long Party has to say it has the screen once it was let go of (it tries for DRM master for 500 ms) */
#define TAKE_MS 1000
/* with the screen Party says PING every 2 s: this long with nothing from it and it is taken for gone */
#define SILENT_MS 6000
/* a menu button held this long while Party has the screen quits the game (ROOM-PROTOCOL.md §12.5) */
#define HOLD_QUIT_MS 3000
/* the screen taken back: DRM master tried for this long */
#define RETAKE_MS 2000

static int g_fd = -1, g_dead;
static px_rx g_rx;
static int g_mfd = -1;          /* the frame's memfd, sent with the first release and again when its size changes */
static size_t g_msize;
static uint8_t *g_map;
static int g_msent;
static uint64_t g_seq;

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static void hlog(const px_party_host *h, const char *fmt, ...)
{
    char b[300];
    va_list ap;
    if (!h->log) return;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    h->log(h->ctx, b);
}

/* a line out (the newline added), with an fd riding on it (SCM_RIGHTS) when fd >= 0 */
static int send_raw(const char *line, int fd)
{
    char s[PX_LINE_MAX + 1];
    size_t n = strlen(line), off = 0;
    if (g_fd < 0 || g_dead) return 0;
    if (n > PX_LINE_MAX - 1) n = PX_LINE_MAX - 1;   /* (§4.2's limit; nothing of ours is near it) */
    memcpy(s, line, n);
    s[n++] = '\n';
    while (off < n) {
        ssize_t w;
        if (fd >= 0 && off == 0) {
            struct iovec io;
            char cb[CMSG_SPACE(sizeof(int))];
            struct msghdr mh;
            struct cmsghdr *c;
            memset(cb, 0, sizeof cb);
            memset(&mh, 0, sizeof mh);
            io.iov_base = s;
            io.iov_len = n;
            mh.msg_iov = &io;
            mh.msg_iovlen = 1;
            mh.msg_control = cb;
            mh.msg_controllen = sizeof cb;
            c = CMSG_FIRSTHDR(&mh);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(c), &fd, sizeof(int));
            w = sendmsg(g_fd, &mh, MSG_NOSIGNAL);
        } else
            w = send(g_fd, s + off, n - off, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd p = {g_fd, POLLOUT, 0};
                if (poll(&p, 1, 200) > 0) continue;
            }
            g_dead = 1;
            return 0;
        }
        off += (size_t)w;
    }
    return 1;
}

static int sendf(const char *fmt, ...)
{
    char b[PX_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return send_raw(b, -1);
}

/* lines out of a block ('\n' between them) */
static void send_block(const char *s)
{
    char b[PX_LINE_MAX];
    while (s && *s) {
        const char *e = strchr(s, '\n');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n >= sizeof b) n = sizeof b - 1;
        memcpy(b, s, n);
        b[n] = 0;
        if (n) send_raw(b, -1);
        s = e ? e + 1 : NULL;
    }
}

/* what Party has sent: waits at most ms for something, then takes every whole line (into out, up to max).
 * 0: the channel is gone */
static int read_lines(int ms, px_line *out, int max, int *n)
{
    struct pollfd p;
    char b[2048], line[PX_LINE_MAX];
    int r;
    *n = 0;
    if (g_fd < 0 || g_dead) return 0;
    p.fd = g_fd;
    p.events = POLLIN;
    p.revents = 0;
    r = poll(&p, 1, ms);
    if (r < 0 && errno != EINTR) { g_dead = 1; return 0; }
    if (r > 0) {
        for (;;) {
            ssize_t k = recv(g_fd, b, sizeof b, MSG_DONTWAIT);
            if (k > 0) { px_rx_feed(&g_rx, b, (size_t)k); continue; }
            if (k < 0 && errno == EINTR) continue;
            if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            g_dead = 1;   /* (0: Party closed its end) */
            break;
        }
    }
    while (*n < max && px_rx_next(&g_rx, line, sizeof line)) px_line_parse(&out[(*n)++], line);
    return !g_dead;
}

/* the frame of both panels into the memfd (PXF1, ROOM-PROTOCOL.md §12.5): a new one when the size changed */
static int write_frame(const px_party_host *h, int *fresh)
{
    int w = 0, hh = 0, s, i;
    size_t stride, need;
    *fresh = 0;
    if (h->panel_size) h->panel_size(h->ctx, &w, &hh);
    if (w <= 0 || hh <= 0) return 0;
    stride = (size_t)w * 4;
    need = PX_PXF1_HEADER + 2 * stride * (size_t)hh;
    if (g_mfd < 0 || g_msize != need) {
        void *m;
        if (g_map) munmap(g_map, g_msize);
        if (g_mfd >= 0) close(g_mfd);
        g_map = NULL;
        g_mfd = -1;
#ifdef __linux__
        g_mfd = memfd_create("pixelado-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
#else
        {   /* (a computer without memfd: a file that is gone as soon as it is open, the same to Party) */
            char tmpl[] = "/tmp/pixelado-frame-XXXXXX";
            g_mfd = mkstemp(tmpl);
            if (g_mfd >= 0) unlink(tmpl);
        }
#endif
        if (g_mfd < 0) return 0;
        if (ftruncate(g_mfd, (off_t)need) != 0) { close(g_mfd); g_mfd = -1; return 0; }
#ifdef F_SEAL_SHRINK
        fcntl(g_mfd, F_ADD_SEALS, F_SEAL_SHRINK);
#endif
        m = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, g_mfd, 0);
        if (m == MAP_FAILED) { close(g_mfd); g_mfd = -1; return 0; }
        g_map = (uint8_t *)m;
        g_msize = need;
        g_msent = 0;
    }
    *fresh = !g_msent;
#define PUT32(at, v) do { uint32_t v_ = (uint32_t)(v); for (i = 0; i < 4; i++) g_map[(at) + i] = (uint8_t)(v_ >> (8 * i)); } while (0)
    memset(g_map, 0, PX_PXF1_HEADER);
    memcpy(g_map, "PXF1", 4);
    PUT32(4, PX_PXF1_HEADER);
    PUT32(8, 2);
    g_seq++;
    for (i = 0; i < 8; i++) g_map[16 + i] = (uint8_t)(g_seq >> (8 * i));
    for (s = 0; s < 2; s++) {
        const size_t at = 24 + 24 * (size_t)s, off = PX_PXF1_HEADER + (size_t)s * stride * (size_t)hh;
        PUT32(at, s);
        PUT32(at + 4, w);
        PUT32(at + 8, hh);
        PUT32(at + 12, stride);
        PUT32(at + 16, PX_FOURCC_XR24);
        PUT32(at + 20, off);
        if (!h->grab || !h->grab(h->ctx, s, (uint32_t *)(void *)(g_map + off), w, hh))
            memset(g_map + off, 0, stride * (size_t)hh);
    }
#undef PUT32
    return 1;
}

static void send_opts(int vol, int bri, int fps, int scale)
{
    sendf("OPT key=volume type=int value=%d min=0 max=100", vol);
    sendf("OPT key=brightness type=int value=%d min=5 max=100", bri);
    if (fps >= 0) sendf("OPT key=fps type=bool value=%d", fps ? 1 : 0);
    if (scale >= 0) sendf("OPT key=screen.scale type=enum value=%d choices=integer,fit", scale ? 1 : 0);
    send_raw("OPTS_END", -1);
}

static void send_hold(int n, const char *names)
{
    char e[600];
    px_line_esc(e, sizeof e, names);
    sendf("HOLD n=%d names=%s", n, e);
}

int px_party_open(int fd, const char *engine, const char *caps, const char *title, int resume, const char *gid)
{
    int type = 0;
    socklen_t tl = sizeof type;
    char t[400], g[80], b[PX_LINE_MAX];
    px_party_close();
    if (fd < 0) return 0;
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) != 0 || type != SOCK_STREAM) return 0;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
    g_fd = fd;
    g_dead = 0;
    memset(&g_rx, 0, sizeof g_rx);
    px_line_esc(t, sizeof t, title ? title : "");
    px_line_esc(g, sizeof g, gid ? gid : "");
    snprintf(b, sizeof b, "HELLO role=engine engine=%s proto=1 pid=%d caps=%s resume=%d%s%s title=%s", engine,
             (int)getpid(), caps, resume, g[0] ? " gid=" : "", g, t);
    return send_raw(b, -1);
}

int px_party_fd(void) { return g_dead ? -1 : g_fd; }

int px_party_up(void)
{
    struct pollfd p;
    if (g_fd < 0 || g_dead) return 0;
    /* (a closed end shows as readable with nothing to read) */
    p.fd = g_fd;
    p.events = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, 0) > 0) {
        if (p.revents & (POLLHUP | POLLERR)) g_dead = 1;
        else if (p.revents & POLLIN) {
            char c;
            if (recv(g_fd, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0) g_dead = 1;
        }
    }
    return !g_dead;
}

void px_party_close(void)
{
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
    g_dead = 0;
    if (g_map) munmap(g_map, g_msize);
    if (g_mfd >= 0) close(g_mfd);
    g_map = NULL;
    g_mfd = -1;
    g_msize = 0;
    g_msent = 0;
}

px_party_menu_result px_party_menu(const px_party_host *h, int ours)
{
    static px_line in[32];
    char names[512] = "", n2[512], lan[2048] = "", l2[2048];
    int fresh = 0, vol = 50, bri = 80, fps = -1, scale = -1, hold_n = 0, nin, i;
    int taken = 0, held = ours, wanted = 0, quitting = 0, have_lan = 0;
    long long t0, heard, ticked, lan_at, hold_at;
    px_party_menu_result out = PX_PARTY_DONE;

    if (!px_party_up()) return PX_PARTY_UNAVAILABLE;
    read_lines(0, in, 32, &nin);   /* what Party said while it didn't have the screen (nothing expected): dropped */
    if (!write_frame(h, &fresh)) {
        hlog(h, "[party] no frame for Party: our own menu\n");
        return PX_PARTY_UNAVAILABLE;
    }
    if (h->opts_open) h->opts_open(h->ctx, &vol, &bri, &fps, &scale);
    if (h->hold_others) hold_n = h->hold_others(h->ctx, names, sizeof names);
    if (h->lan_lines) have_lan = h->lan_lines(h->ctx, lan, sizeof lan);
    if (h->drop_master && !h->drop_master(h->ctx))
        hlog(h, "[party] dropping DRM master failed (%s): Party may not get it\n", strerror(errno));
    /* what Party needs to draw first, then the screen */
    send_opts(vol, bri, fps, scale);
    if (have_lan) send_block(lan);
    send_hold(hold_n, names);
    {
        char b[96];
        snprintf(b, sizeof b, "SCREEN state=released reason=%s seq=%llu", ours ? "menu" : "hold", (unsigned long long)g_seq);
        if (!send_raw(b, fresh ? g_mfd : -1)) {
            if (h->take_master) h->take_master(h->ctx, RETAKE_MS);
            hlog(h, "[party] Party is gone: our own menu\n");
            return PX_PARTY_UNAVAILABLE;
        }
    }
    if (fresh) g_msent = 1;
    hlog(h, "[party] the screen to Party (%s)\n", ours ? "our menu" : "another player's menu");

    t0 = now_ms();
    heard = ticked = lan_at = hold_at = t0;
    for (;;) {
        int give = 0, alive;
        long long t;
        alive = read_lines(10, in, 32, &nin);
        t = now_ms();
        if (nin) heard = t;
        for (i = 0; i < nin; i++) {
            const px_line *l = &in[i];
            if (!strcmp(l->verb, "SCREEN")) {
                const char *st = px_line_str(l, "state", "");
                if (!strcmp(st, "taken")) taken = 1;
                else if (!strcmp(st, "failed")) {
                    hlog(h, "[party] Party couldn't take the screen (%s): our own menu\n", px_line_str(l, "reason", "?"));
                    out = PX_PARTY_FALLBACK;
                    give = 1;
                } else if (!strcmp(st, "give"))
                    give = 1;
            } else if (!strcmp(l->verb, "CMD")) {
                const char *op = px_line_str(l, "op", "");
                char e[64];
                if (!strcmp(op, "set")) {
                    const char *val = px_line_str(l, "value", "0");
                    /* (an enum's value is its index, or its name) */
                    const int num = !strcmp(val, "fit") ? 1 : !strcmp(val, "integer") ? 0 : (int)px_line_num(l, "value", 0);
                    if (h->opt_set) h->opt_set(h->ctx, px_line_str(l, "key", ""), num);
                    px_line_esc(e, sizeof e, px_line_str(l, "key", ""));
                    sendf("OK op=set key=%s", e);
                } else if (!strcmp(op, "lan")) {
                    if (h->lan_set && h->lan_set(h->ctx, (int)px_line_num(l, "mode", 0), (int)px_line_num(l, "players", 0),
                                                 px_line_str(l, "addr", ""))) {
                        send_raw("OK op=lan", -1);
                        lan_at = 0;   /* (the new state soon) */
                    } else
                        send_raw("ERR op=lan msg=unsupported", -1);
                } else if (!strcmp(op, "quit")) {
                    hlog(h, "[party] quit from Party's menu\n");
                    send_raw("OK op=quit", -1);
                    quitting = 1;
                } else if (!strcmp(op, "resume")) {
                    send_raw("OK op=resume", -1);
                } else {
                    px_line_esc(e, sizeof e, op);
                    sendf("ERR op=%s msg=unknown", e);
                }
            } else if (!strcmp(l->verb, "HOLD")) {
                const int on = !strcmp(px_line_str(l, "state", ""), "on");
                if (on != held) {
                    if (h->hold_set) h->hold_set(h->ctx, on);
                    held = on;
                    wanted = 0;
                    hlog(h, "[party] %s\n", on ? "our menu opened in Party (the group held)" : "our menu closed in Party");
                }
            } else if (!strcmp(l->verb, "SOUND")) {
                const char *id = px_line_str(l, "id", "");
                if (h->sound) h->sound(h->ctx, !strcmp(id, "select") ? 1 : !strcmp(id, "back") ? 2 : 0);
            } else if (!strcmp(l->verb, "PING")) {
                char e[64];
                px_line_esc(e, sizeof e, px_line_str(l, "t", ""));
                sendf("PONG t=%s", e);
            }
        }
        if (h->idle) h->idle(h->ctx);
        if (quitting) {
            if (h->opts_close) h->opts_close(h->ctx);
            if (held && h->hold_set) h->hold_set(h->ctx, 0);
            if (h->quit) h->quit(h->ctx);
            return PX_PARTY_QUIT;
        }
        if (give) break;
        if (!alive) {
            hlog(h, "[party] Party went away with the screen: taken back\n");
            out = PX_PARTY_FALLBACK;
            break;
        }
        if (h->stopping && h->stopping(h->ctx)) {   /* (SIGTERM, END: the core quits; the kernel hands the screen on) */
            if (held && h->hold_set) h->hold_set(h->ctx, 0);
            if (h->opts_close) h->opts_close(h->ctx);
            return PX_PARTY_QUIT;
        }
        if (!taken && t - t0 > TAKE_MS) {
            hlog(h, "[party] Party didn't take the screen in %d ms: our own menu\n", TAKE_MS);
            out = PX_PARTY_FALLBACK;
            break;
        }
        if (taken && t - heard > SILENT_MS) {
            hlog(h, "[party] nothing from Party in %d s: the screen taken back\n", SILENT_MS / 1000);
            out = PX_PARTY_FALLBACK;
            break;
        }
        if (h->menu_held_ms && h->menu_held_ms(h->ctx) >= HOLD_QUIT_MS) {
            hlog(h, "[party] the menu button held 3 s while Party had the screen: quit\n");
            if (h->take_master) h->take_master(h->ctx, RETAKE_MS);
            if (held && h->hold_set) h->hold_set(h->ctx, 0);
            if (h->opts_close) h->opts_close(h->ctx);
            if (h->quit) h->quit(h->ctx);
            return PX_PARTY_QUIT;
        }
        /* the group's pause by the others, for the wait screen; over (and not ours): the screen wanted back */
        if (t - hold_at >= 30) {
            int k = 0;
            hold_at = t;
            n2[0] = 0;
            if (h->hold_others) k = h->hold_others(h->ctx, n2, sizeof n2);
            if (k != hold_n || strcmp(n2, names)) {
                hold_n = k;
                memcpy(names, n2, sizeof names);
                send_hold(hold_n, names);
            }
            if (!held && hold_n <= 0 && !wanted) {
                wanted = send_raw("SCREEN state=want", -1);
                hlog(h, "[party] the group's pause is over: the screen asked back\n");
            }
        }
        if (h->lan_lines && t - lan_at >= 500) {
            lan_at = t;
            l2[0] = 0;
            if (h->lan_lines(h->ctx, l2, sizeof l2) && strcmp(l2, lan)) {
                memcpy(lan, l2, sizeof lan);
                send_block(lan);
            }
        }
        if (t - ticked >= 500) {
            int v = vol, br = bri;
            ticked = t;
            if (h->opts_tick && h->opts_tick(h->ctx, &v, &br) && (v != vol || br != bri)) {
                vol = v;
                bri = br;
                send_opts(vol, bri, fps, scale);
            }
        }
    }
    /* the screen back */
    if (h->opts_close) h->opts_close(h->ctx);
    /* (our own menu next keeps our hold of the group: it is ours again at once; the wait screen next lets it go) */
    if (held && (out != PX_PARTY_FALLBACK || !ours)) {
        if (h->hold_set) h->hold_set(h->ctx, 0);
        held = 0;
    }
    if (h->take_master && !h->take_master(h->ctx, RETAKE_MS))
        hlog(h, "[party] DRM master not back in %d ms (%s); trying anyway\n", RETAKE_MS, strerror(errno));
    if (h->repaint) h->repaint(h->ctx);
    if (out == PX_PARTY_DONE) send_raw("SCREEN state=taken", -1);
    hlog(h, "[party] the screen is back (%s)\n", out == PX_PARTY_DONE ? "Party's menu closed" : "our own menu next");
    return out;
}
