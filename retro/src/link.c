/* The link between the handhelds: libretro's netpacket interface (RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE) over
 * TCP, a star around the room's host as in RetroArch's netplay: the host listens (port 7067, ROOM-PROTOCOL §4), each
 * guest connects to it, and the host relays what a guest sends to another or to everyone. Client id 0 is the host,
 * the guests 1..n (a guest asks for its room seat as its id). The core sees exactly libretro's interface: start,
 * receive, stop, connected, disconnected; packets arrive in order (TCP), between frames or when the core polls.
 *
 * Beside the core's packets the link carries the group's pause: HOLD (a player's menu is open: everyone stops at the
 * end of the frame, so a core's link timeout, gpSP's 4 s, never runs while one side is paused), the players' names,
 * and a ping every second (10 s of silence and the peer is gone; the pause never stops the pings).
 *
 * The wire: frames of [u32 length, big endian][u8 type][payload]; length counts the type and the payload.
 *   1 HELLO   guest -> host  text: proto=<the core's netpacket protocol> player=<seat> name=<nickname>
 *   2 WELCOME host -> guest  text: id=<client id> max=<players>
 *   3 DENY    host -> guest  text: reason=version|full
 *   4 PKT     both           u16 from, u16 to, the core's bytes (to 0xFFFF: everyone)
 *   5 HOLD    both           u16 who, u8 on
 *   6 PING / 7 PONG          u64 the sender's clock (ms), echoed
 *   8 PEER    host -> guest  u16 id, u8 present, name
 * Text payloads are PXROOM lines' key=value pairs (common/include/pixelado/line.h). */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "px.h"
#include "pixelado/line.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define MAXP 8
#define MAXFRAME (70 * 1024)
#define PING_MS 1000
#define SILENT_MS 10000
enum { T_HELLO = 1, T_WELCOME, T_DENY, T_PKT, T_HOLD, T_PING, T_PONG, T_PEER };

typedef struct conn {
    int fd;
    int id;          /* -1: not a member yet */
    uint8_t *in, *out;
    size_t inlen, incap, outlen, outcap;
    long long heard, pinged, opened;
} conn;

typedef struct peer {
    int present, hold, rtt;
    char name[48];
} peer;

typedef struct qpkt {
    struct qpkt *next;
    uint16_t from;
    size_t len;
    uint8_t data[];
} qpkt;

static px_link_cfg g_cfg;
static char g_addr[64], g_name[48], g_proto[96], g_reason[64];
static const struct retro_netpacket_callback *g_cb;
static int g_state = PX_LINK_OFF, g_me = -1, g_started;
static int g_lfd = -1;
static conn g_c[MAXP];     /* host: the guests' connections; guest: g_c[0] is the host */
static peer g_peer[MAXP];  /* by client id */
static qpkt *g_qh, *g_qt;
static size_t g_qbytes;
static long long g_try_until, g_next_try;
static int g_in_deliver;

static void set_reason(const char *r) { snprintf(g_reason, sizeof g_reason, "%s", r); }

/* ---- connections ---- */
static void conn_free(conn *c)
{
    if (c->fd >= 0) close(c->fd);
    free(c->in);
    free(c->out);
    memset(c, 0, sizeof *c);
    c->fd = -1;
    c->id = -1;
}

static int conn_flush(conn *c)
{
    while (c->fd >= 0 && c->outlen) {
        ssize_t w = send(c->fd, c->out, c->outlen, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
            return 0;
        }
        memmove(c->out, c->out + w, c->outlen - (size_t)w);
        c->outlen -= (size_t)w;
    }
    return 1;
}

static void conn_put(conn *c, int type, const void *a, size_t alen, const void *b, size_t blen)
{
    const size_t n = 4 + 1 + alen + blen;
    uint8_t *o;
    uint32_t len = (uint32_t)(1 + alen + blen);
    if (c->fd < 0) return;
    if (c->outlen + n > c->outcap) {
        size_t cap = c->outcap ? c->outcap : 4096;
        while (cap < c->outlen + n) cap *= 2;
        if (cap > 8u << 20) {   /* (a peer that reads nothing for this long is gone) */
            px_log("[link] client %d isn't reading: dropped", c->id);
            close(c->fd);
            c->fd = -1;
            return;
        }
        o = realloc(c->out, cap);
        if (!o) return;
        c->out = o;
        c->outcap = cap;
    }
    o = c->out + c->outlen;
    o[0] = (uint8_t)(len >> 24);
    o[1] = (uint8_t)(len >> 16);
    o[2] = (uint8_t)(len >> 8);
    o[3] = (uint8_t)len;
    o[4] = (uint8_t)type;
    if (alen) memcpy(o + 5, a, alen);
    if (blen) memcpy(o + 5 + alen, b, blen);
    c->outlen += n;
}

static void put_text(conn *c, int type, const char *text) { conn_put(c, type, text, strlen(text), NULL, 0); }

static void put_pkt(conn *c, uint16_t from, uint16_t to, const void *buf, size_t len)
{
    uint8_t h[4] = {(uint8_t)(from >> 8), (uint8_t)from, (uint8_t)(to >> 8), (uint8_t)to};
    conn_put(c, T_PKT, h, 4, buf, len);
}

static void put_hold(conn *c, int who, int on)
{
    uint8_t h[3] = {(uint8_t)(who >> 8), (uint8_t)who, (uint8_t)(on ? 1 : 0)};
    conn_put(c, T_HOLD, h, 3, NULL, 0);
}

static void put_peer(conn *c, int id)
{
    uint8_t h[3] = {(uint8_t)(id >> 8), (uint8_t)id, (uint8_t)(g_peer[id].present ? 1 : 0)};
    conn_put(c, T_PEER, h, 3, g_peer[id].name, strlen(g_peer[id].name));
}

static void put_time(conn *c, int type, uint64_t t)
{
    uint8_t h[8];
    int i;
    for (i = 0; i < 8; i++) h[i] = (uint8_t)(t >> (56 - 8 * i));
    conn_put(c, type, h, 8, NULL, 0);
}

static void nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#if defined(IPTOS_LOWDELAY) && defined(IP_TOS)
    {
        int tos = 0xB8;   /* (DSCP EF, as the DS core's LAN) */
        setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    }
#endif
}

/* ---- the core's packets waiting for a frame boundary ---- */
static void enqueue(uint16_t from, const uint8_t *b, size_t n)
{
    qpkt *p;
    if (g_qbytes + n > (16u << 20)) { px_log("[link] the core isn't taking packets: one dropped"); return; }
    p = malloc(sizeof *p + n);
    if (!p) return;
    p->next = NULL;
    p->from = from;
    p->len = n;
    memcpy(p->data, b, n);
    if (g_qt) g_qt->next = p;
    else g_qh = p;
    g_qt = p;
    g_qbytes += n;
}

static void deliver(void)
{
    if (g_in_deliver) return;
    g_in_deliver = 1;
    while (g_qh) {
        qpkt *p = g_qh;
        g_qh = p->next;
        if (!g_qh) g_qt = NULL;
        g_qbytes -= p->len;
        if (g_started && g_cb && g_cb->receive) g_cb->receive(p->data, p->len, p->from);
        free(p);
    }
    g_in_deliver = 0;
}

static void drop_queue(void)
{
    while (g_qh) {
        qpkt *p = g_qh;
        g_qh = p->next;
        free(p);
    }
    g_qt = NULL;
    g_qbytes = 0;
}

/* ---- the core's side of the interface ---- */
static void RETRO_CALLCONV np_send(int flags, const void *buf, size_t len, uint16_t to)
{
    int i;
    (void)flags;   /* (every packet is reliable and flushed at once: TCP with no delay) */
    if (buf && len) {
        if (len > 65535) len = 65535;
        if (g_cfg.role == 1) {
            for (i = 0; i < MAXP; i++)
                if (g_c[i].fd >= 0 && g_c[i].id > 0 && (to == 0xFFFF || to == g_c[i].id))
                    put_pkt(&g_c[i], 0, to, buf, len);
        } else if (g_state == PX_LINK_CONNECTED && to != g_me)
            put_pkt(&g_c[0], (uint16_t)g_me, to, buf, len);
    }
    for (i = 0; i < MAXP; i++)
        if (g_c[i].fd >= 0) conn_flush(&g_c[i]);
}

static void RETRO_CALLCONV np_poll_receive(void)
{
    px_link_service(1);
}

static void core_start(int id)
{
    if (g_started || !g_cb || !g_cb->start) return;
    g_started = 1;
    g_cb->start((uint16_t)id, np_send, np_poll_receive);
    px_log("[link] the core's session started as client %d", id);
}

static void core_stop(void)
{
    if (!g_started) return;
    g_started = 0;
    drop_queue();
    if (g_cb && g_cb->stop) g_cb->stop();
    px_log("[link] the core's session stopped");
}

/* ---- the host ---- */
static void host_broadcast_except(int except, void (*put)(conn *, int, int), int a, int b)
{
    int i;
    for (i = 0; i < MAXP; i++)
        if (g_c[i].fd >= 0 && g_c[i].id > 0 && g_c[i].id != except) put(&g_c[i], a, b);
}
static void put_peer2(conn *c, int id, int unused) { (void)unused; put_peer(c, id); }

static void host_leave(conn *c, const char *why)
{
    const int id = c->id;
    conn_free(c);
    if (id <= 0) return;
    px_log("[link] %s (client %d) left: %s", g_peer[id].name, id, why);
    g_peer[id].present = 0;
    g_peer[id].hold = 0;
    host_broadcast_except(-1, put_peer2, id, 0);
    if (g_cb && g_cb->disconnected && g_started) g_cb->disconnected((uint16_t)id);
}

static void host_hello(conn *c, const char *text)
{
    px_line l;
    char line[PX_LINE_MAX], w[96];
    int want, id = -1, i;
    snprintf(line, sizeof line, "HELLO %s", text);
    px_line_parse(&l, line);
    if (strcmp(px_line_str(&l, "proto", ""), g_proto)) {
        px_log("[link] a guest with another core (%s, ours %s): refused", px_line_str(&l, "proto", "?"), g_proto);
        put_text(c, T_DENY, "reason=version");
        conn_flush(c);
        conn_free(c);
        return;
    }
    want = (int)px_line_num(&l, "player", -1);
    if (want > 0 && want < MAXP && want < g_cfg.players && !g_peer[want].present) id = want;
    for (i = 1; id < 0 && i < MAXP && i < g_cfg.players; i++)
        if (!g_peer[i].present) id = i;
    if (id < 0 || (g_cb && g_cb->connected && !g_cb->connected((uint16_t)id))) {
        px_log("[link] a guest refused: the session is full");
        put_text(c, T_DENY, "reason=full");
        conn_flush(c);
        conn_free(c);
        return;
    }
    c->id = id;
    g_peer[id].present = 1;
    g_peer[id].hold = 0;
    g_peer[id].rtt = -1;
    snprintf(g_peer[id].name, sizeof g_peer[id].name, "%s", px_line_str(&l, "name", ""));
    if (!g_peer[id].name[0]) snprintf(g_peer[id].name, sizeof g_peer[id].name, "P%d", id + 1);
    snprintf(w, sizeof w, "id=%d max=%d", id, g_cfg.players);
    put_text(c, T_WELCOME, w);
    for (i = 0; i < MAXP; i++)
        if (g_peer[i].present) {
            put_peer(c, i);
            if (g_peer[i].hold) put_hold(c, i, 1);
        }
    host_broadcast_except(id, put_peer2, id, 0);
    conn_flush(c);
    px_log("[link] %s joined as client %d", g_peer[id].name, id);
}

/* ---- frames in ---- */
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static int handle(conn *c, int type, const uint8_t *p, size_t n)
{
    char text[PX_LINE_MAX];
    int i;
    c->heard = px_now_ms();
    if (type == T_HELLO || type == T_WELCOME || type == T_DENY) {
        if (n >= sizeof text) n = sizeof text - 1;
        memcpy(text, p, n);
        text[n] = 0;
    }
    switch (type) {
    case T_HELLO:
        if (g_cfg.role == 1 && c->id < 0) host_hello(c, text);
        return c->fd >= 0;
    case T_WELCOME:
        if (g_cfg.role == 2 && g_state == PX_LINK_CONNECTING) {
            char line[PX_LINE_MAX];
            px_line l;
            snprintf(line, sizeof line, "WELCOME %s", text);
            px_line_parse(&l, line);
            g_me = (int)px_line_num(&l, "id", -1);
            if (g_me <= 0 || g_me >= MAXP) { set_reason("proto"); g_state = PX_LINK_FAILED; return 0; }
            c->id = 0;
            g_state = PX_LINK_CONNECTED;
            snprintf(g_peer[g_me].name, sizeof g_peer[g_me].name, "%s", g_name);
            g_peer[g_me].present = 1;
            px_log("[link] in the host's session as client %d", g_me);
            core_start(g_me);
        }
        return 1;
    case T_DENY:
        px_log("[link] the host refused us (%s)", text);
        set_reason(strstr(text, "version") ? "version" : strstr(text, "full") ? "full" : "denied");
        g_state = PX_LINK_FAILED;
        return 0;
    case T_PKT: {
        uint16_t from, to;
        if (n < 4 || c->id < 0) return 1;
        from = be16(p);
        to = be16(p + 2);
        if (g_cfg.role == 1) {
            from = (uint16_t)c->id;   /* (a guest speaks for itself only) */
            if (to == 0 || to == 0xFFFF) enqueue(from, p + 4, n - 4);
            for (i = 0; i < MAXP; i++)
                if (g_c[i].fd >= 0 && g_c[i].id > 0 && g_c[i].id != from && (to == 0xFFFF || to == g_c[i].id))
                    put_pkt(&g_c[i], from, to, p + 4, n - 4);
        } else
            enqueue(from, p + 4, n - 4);
        return 1;
    }
    case T_HOLD: {
        int who, on;
        if (n < 3 || c->id < 0) return 1;
        who = be16(p);
        on = p[2] != 0;
        if (g_cfg.role == 1) who = c->id;
        if (who < 0 || who >= MAXP) return 1;
        if (g_peer[who].hold != on)
            px_log("[link] %s %s the menu", g_peer[who].name[0] ? g_peer[who].name : "a player", on ? "opened" : "closed");
        g_peer[who].hold = on;
        if (g_cfg.role == 1)
            for (i = 0; i < MAXP; i++)
                if (g_c[i].fd >= 0 && g_c[i].id > 0 && g_c[i].id != who) put_hold(&g_c[i], who, on);
        return 1;
    }
    case T_PING: {
        uint64_t t = 0;
        if (n >= 8)
            for (i = 0; i < 8; i++) t = t << 8 | p[i];
        put_time(c, T_PONG, t);
        return 1;
    }
    case T_PONG: {
        uint64_t t = 0;
        int id = g_cfg.role == 1 ? c->id : 0;
        if (n >= 8)
            for (i = 0; i < 8; i++) t = t << 8 | p[i];
        if (id >= 0 && id < MAXP) g_peer[id].rtt = (int)(px_now_ms() - (long long)t);
        return 1;
    }
    case T_PEER: {
        int id;
        if (g_cfg.role != 2 || n < 3) return 1;
        id = be16(p);
        if (id < 0 || id >= MAXP || id == g_me) return 1;
        g_peer[id].present = p[2] != 0;
        if (!g_peer[id].present) g_peer[id].hold = 0;
        n -= 3;
        if (n >= sizeof g_peer[id].name) n = sizeof g_peer[id].name - 1;
        memcpy(g_peer[id].name, p + 3, n);
        g_peer[id].name[n] = 0;
        return 1;
    }
    }
    return 1;   /* (an unknown frame: skipped) */
}

/* bytes in; 0: the connection is over */
static int conn_read(conn *c)
{
    for (;;) {
        ssize_t r;
        if (c->incap - c->inlen < 16384) {
            size_t cap = c->incap ? c->incap * 2 : 32768;
            uint8_t *b;
            if (cap > 4 * MAXFRAME) cap = 4 * MAXFRAME;
            if (cap <= c->incap) return 0;
            b = realloc(c->in, cap);
            if (!b) return 0;
            c->in = b;
            c->incap = cap;
        }
        r = recv(c->fd, c->in + c->inlen, c->incap - c->inlen, 0);
        if (r > 0) { c->inlen += (size_t)r; continue; }
        if (r == 0) return 0;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return 0;
    }
    for (;;) {
        uint32_t len;
        size_t off = 0;
        if (c->inlen < 4) break;
        len = (uint32_t)c->in[0] << 24 | (uint32_t)c->in[1] << 16 | (uint32_t)c->in[2] << 8 | c->in[3];
        if (len < 1 || len > MAXFRAME) { px_log("[link] a frame of %u bytes: not our protocol", len); return 0; }
        if (c->inlen < 4 + (size_t)len) break;
        if (!handle(c, c->in[4], c->in + 5, len - 1)) return 0;
        if (c->fd < 0) return 0;
        off = 4 + len;
        memmove(c->in, c->in + off, c->inlen - off);
        c->inlen -= off;
    }
    return 1;
}

/* ---- the guest's connection ---- */
static void guest_try(void)
{
    struct sockaddr_in a;
    conn *c = &g_c[0];
    int fd;
    if (c->fd >= 0) return;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    nodelay(fd);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)g_cfg.port);
    if (inet_pton(AF_INET, g_addr, &a.sin_addr) != 1) {
        close(fd);
        set_reason("address");
        g_state = PX_LINK_FAILED;
        return;
    }
    if (connect(fd, (struct sockaddr *)&a, sizeof a) && errno != EINPROGRESS) { close(fd); return; }
    conn_free(c);
    c->fd = fd;
    c->id = -1;
    c->opened = px_now_ms();
}

static void guest_connected(conn *c)
{
    char h[300], n[150], pr[150];
    px_line_esc(n, sizeof n, g_name);
    px_line_esc(pr, sizeof pr, g_proto);
    snprintf(h, sizeof h, "proto=%s player=%d name=%s", pr, g_cfg.player, n);
    put_text(c, T_HELLO, h);
    conn_flush(c);
    c->id = 0;   /* (the host, from here on; a member once it says WELCOME) */
}

/* ---- the frontend's side ---- */
int px_link_start(const px_link_cfg *cfg, const struct retro_netpacket_callback *cb)
{
    int i;
    for (i = 0; i < MAXP; i++) { g_c[i].fd = -1; g_c[i].id = -1; }
    memset(g_peer, 0, sizeof g_peer);
    for (i = 0; i < MAXP; i++) g_peer[i].rtt = -1;
    g_cfg = *cfg;
    g_cb = cb;
    snprintf(g_addr, sizeof g_addr, "%s", cfg->addr ? cfg->addr : "");
    snprintf(g_name, sizeof g_name, "%s", cfg->name && *cfg->name ? cfg->name : cfg->role == 1 ? "P1" : "P2");
    snprintf(g_proto, sizeof g_proto, "%s", cfg->proto ? cfg->proto : "");
    g_cfg.addr = g_addr;
    g_cfg.name = g_name;
    g_cfg.proto = g_proto;
    if (g_cfg.port <= 0) g_cfg.port = 7067;
    if (g_cfg.players < 2) g_cfg.players = 2;
    if (g_cfg.players > MAXP) g_cfg.players = MAXP;
    if (cfg->role == 0) { g_state = PX_LINK_OFF; return 1; }
    if (!cb) {
        px_log("[link] this core has no netpacket interface: playing alone");
        set_reason("core");
        g_state = PX_LINK_FAILED;
        return 0;
    }
    if (cfg->role == 1) {
        struct sockaddr_in a;
        int one = 1;
        g_lfd = socket(AF_INET, SOCK_STREAM, 0);
        if (g_lfd < 0) return 0;
        setsockopt(g_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        fcntl(g_lfd, F_SETFL, fcntl(g_lfd, F_GETFL) | O_NONBLOCK);
        fcntl(g_lfd, F_SETFD, FD_CLOEXEC);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)g_cfg.port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(g_lfd, (struct sockaddr *)&a, sizeof a) || listen(g_lfd, 4)) {
            px_log("[link] can't listen on port %d (%s)", g_cfg.port, strerror(errno));
            close(g_lfd);
            g_lfd = -1;
            set_reason("listen");
            g_state = PX_LINK_FAILED;
            return 0;
        }
        g_me = 0;
        g_peer[0].present = 1;
        g_peer[0].rtt = 0;
        snprintf(g_peer[0].name, sizeof g_peer[0].name, "%s", g_name);
        g_state = PX_LINK_LISTENING;
        px_log("[link] hosting on port %d, up to %d players (core protocol %s)", g_cfg.port, g_cfg.players, g_proto);
        core_start(0);
        return 1;
    }
    g_state = PX_LINK_CONNECTING;
    g_try_until = px_now_ms() + (cfg->retry_s > 0 ? cfg->retry_s : 15) * 1000LL;
    g_next_try = 0;
    px_log("[link] joining %s:%d as player %d", g_addr, g_cfg.port, g_cfg.player + 1);
    return 1;
}

void px_link_service(int deliver_now)
{
    struct pollfd pf[MAXP + 1];
    int map[MAXP + 1], n = 0, i;
    const long long now = px_now_ms();
    if (g_state == PX_LINK_OFF || g_state == PX_LINK_FAILED) return;
    if (g_state == PX_LINK_CONNECTING) {
        if (now > g_try_until) {
            px_log("[link] no host at %s:%d", g_addr, g_cfg.port);
            conn_free(&g_c[0]);
            set_reason("timeout");
            g_state = PX_LINK_FAILED;
            return;
        }
        if (g_c[0].fd < 0 && now >= g_next_try) {
            g_next_try = now + 500;
            guest_try();
        }
    }
    if (g_lfd >= 0) {
        pf[n].fd = g_lfd;
        pf[n].events = POLLIN;
        pf[n].revents = 0;
        map[n++] = -1;
    }
    for (i = 0; i < MAXP; i++)
        if (g_c[i].fd >= 0) {
            pf[n].fd = g_c[i].fd;
            pf[n].events = POLLIN | (g_c[i].outlen || (g_cfg.role == 2 && g_c[i].id < 0) ? POLLOUT : 0);
            pf[n].revents = 0;
            map[n++] = i;
        }
    if (n && poll(pf, (nfds_t)n, 0) > 0) {
        for (i = 0; i < n; i++) {
            conn *c;
            if (!pf[i].revents) continue;
            if (map[i] < 0) {
                for (;;) {
                    int fd = accept(g_lfd, NULL, NULL), k;
                    if (fd < 0) break;
                    for (k = 0; k < MAXP && g_c[k].fd >= 0; k++) {}
                    if (k == MAXP) { close(fd); continue; }
                    nodelay(fd);
                    conn_free(&g_c[k]);
                    g_c[k].fd = fd;
                    g_c[k].opened = g_c[k].heard = now;
                }
                continue;
            }
            c = &g_c[map[i]];
            if (g_cfg.role == 2 && c->id < 0 && (pf[i].revents & (POLLOUT | POLLERR | POLLHUP))) {
                int err = 0;
                socklen_t el = sizeof err;
                getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el);
                if (err || (pf[i].revents & (POLLERR | POLLHUP))) { conn_free(c); continue; }   /* (not up yet: again) */
                c->heard = now;
                guest_connected(c);
            }
            if (pf[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (!conn_read(c)) {
                    if (g_cfg.role == 1) host_leave(c, "closed");
                    else if (g_state == PX_LINK_CONNECTED) {
                        px_log("[link] the host is gone");
                        conn_free(c);
                        for (i = 0; i < MAXP; i++) g_peer[i].hold = 0;
                        set_reason("host-gone");
                        g_state = PX_LINK_LOST;
                        core_stop();
                        return;
                    } else
                        conn_free(c);   /* (refused, or closed before WELCOME: tried again, or failed by DENY) */
                    continue;
                }
            }
            if (c->fd >= 0 && c->outlen && !conn_flush(c)) {
                if (g_cfg.role == 1) host_leave(c, "write");
                else conn_free(c);
            }
        }
    }
    /* pings, and the silent ones */
    for (i = 0; i < MAXP; i++) {
        conn *c = &g_c[i];
        if (c->fd < 0) continue;
        if (g_cfg.role == 1 && c->id < 0) {
            if (now - c->opened > 5000) conn_free(c);   /* (no HELLO in 5 s) */
            continue;
        }
        if (g_cfg.role == 2 && g_state != PX_LINK_CONNECTED) continue;
        if (now - c->pinged >= PING_MS) {
            c->pinged = now;
            put_time(c, T_PING, (uint64_t)now);
            conn_flush(c);
        }
        if (now - c->heard > SILENT_MS) {
            if (g_cfg.role == 1) host_leave(c, "silent");
            else {
                px_log("[link] nothing from the host in %d s", SILENT_MS / 1000);
                conn_free(c);
                for (i = 0; i < MAXP; i++) g_peer[i].hold = 0;
                set_reason("host-silent");
                g_state = PX_LINK_LOST;
                core_stop();
                return;
            }
        }
    }
    if (g_cfg.role == 1) {
        int k = 0;
        for (i = 1; i < MAXP; i++) k += g_peer[i].present;
        g_state = k ? PX_LINK_CONNECTED : PX_LINK_LISTENING;
    }
    if (deliver_now) deliver();
}

int px_link_state(void) { return g_state; }
const char *px_link_reason(void) { return g_reason; }
int px_link_id(void) { return g_me; }

int px_link_peers(void)
{
    int i, k = 0;
    if (g_state != PX_LINK_CONNECTED && g_state != PX_LINK_LISTENING) return 0;
    for (i = 0; i < MAXP; i++) k += g_peer[i].present;
    return k;
}

void px_link_hold(int on)
{
    int i;
    if (g_me < 0 || g_me >= MAXP) return;
    if (g_peer[g_me].hold == on) return;
    g_peer[g_me].hold = on;
    if (g_cfg.role == 1) {
        for (i = 0; i < MAXP; i++)
            if (g_c[i].fd >= 0 && g_c[i].id > 0) { put_hold(&g_c[i], 0, on); conn_flush(&g_c[i]); }
    } else if (g_state == PX_LINK_CONNECTED) {
        put_hold(&g_c[0], g_me, on);
        conn_flush(&g_c[0]);
    }
}

int px_link_holders(char *names, size_t cap)
{
    int i, k = 0;
    size_t o = 0;
    if (cap) names[0] = 0;
    if (g_state != PX_LINK_CONNECTED && g_state != PX_LINK_LISTENING) return 0;
    for (i = 0; i < MAXP; i++)
        if (i != g_me && g_peer[i].present && g_peer[i].hold) {
            const int w = snprintf(names + o, o < cap ? cap - o : 0, "%s%s", k ? "\n" : "", g_peer[i].name);
            if (w > 0) o += (size_t)w;
            if (o >= cap) o = cap ? cap - 1 : 0;
            k++;
        }
    return k;
}

int px_link_lan_lines(char *out, size_t cap)
{
    /* Party's game menu draws the DS's local wireless session from these (core/game_menu.c): mode 1 host / 2 join,
     * state 1 busy / 2 ready / 3 failed; lock=1, so its page offers nothing to change */
    int i, np = 0, st, o;
    char e[150];
    if (g_cfg.role == 0) return 0;
    for (i = 0; i < MAXP; i++) np += g_peer[i].present;
    st = g_state == PX_LINK_CONNECTED || g_state == PX_LINK_LISTENING ? 2 : g_state == PX_LINK_CONNECTING ? 1 : 3;
    px_line_esc(e, sizeof e, g_cfg.role == 2 ? g_addr : "");
    o = snprintf(out, cap, "LAN have=1 mode=%d state=%d phase=0 np=%d max=%d hp=%d nonet=0 direct=0 link=0 lstate=0 "
                 "lock=1 tdls=0 nhosts=0 addr=%s msg=", g_cfg.role == 1 ? 1 : 2, st, st == 2 ? np : 0, g_cfg.players,
                 np, e);
    for (i = 0; i < MAXP && st == 2 && o > 0 && (size_t)o < cap; i++) {
        if (!g_peer[i].present) continue;
        px_line_esc(e, sizeof e, g_peer[i].name);
        o += snprintf(out + o, cap - (size_t)o, "\nLANP name=%s host=%d me=%d ping=%d hold=%d", e, i == 0, i == g_me,
                      i == g_me ? 0 : g_peer[i].rtt, g_peer[i].hold);
    }
    if (o > 0 && (size_t)o < cap) snprintf(out + o, cap - (size_t)o, "\nLAN_END");
    return 1;
}

void px_link_stop(void)
{
    int i;
    core_stop();
    for (i = 0; i < MAXP; i++) {
        if (g_c[i].fd >= 0) conn_flush(&g_c[i]);
        conn_free(&g_c[i]);
    }
    if (g_lfd >= 0) close(g_lfd);
    g_lfd = -1;
    if (g_state != PX_LINK_OFF) g_state = PX_LINK_OFF;
}
