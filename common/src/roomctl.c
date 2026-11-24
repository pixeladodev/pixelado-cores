/* See pixelado/roomctl.h. A C version of the DS core's roomctl.cpp (ds/patches/bananads 0006), without its thread. */
#include "pixelado/roomctl.h"
#include "pixelado/line.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int g_fd = -1, g_ended;
static px_rx g_rx;

static int send_line(const char *l)
{
    char b[PX_LINE_MAX + 1];
    size_t n = strlen(l);
    if (g_fd < 0) return 0;
    if (n > PX_LINE_MAX - 1) n = PX_LINE_MAX - 1;
    memcpy(b, l, n);
    b[n++] = '\n';
    if (send(g_fd, b, n, MSG_NOSIGNAL) != (ssize_t)n) {
        close(g_fd);
        g_fd = -1;
        return 0;
    }
    return 1;
}

int px_room_open(const char *path, const char *engine, const char *gid, const char *caps)
{
    int i;
    char b[PX_LINE_MAX], g[80];
    if (!path || !*path || g_fd >= 0) return g_fd >= 0;
    for (i = 0; i < 20 && g_fd < 0; i++) {
        struct sockaddr_un su;
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        memset(&su, 0, sizeof su);
        su.sun_family = AF_UNIX;
        snprintf(su.sun_path, sizeof su.sun_path, "%s", path);
        if (fd >= 0 && connect(fd, (struct sockaddr *)&su, sizeof su) == 0) g_fd = fd;
        else {
            struct timespec ts = {0, 100000000};
            if (fd >= 0) close(fd);
            nanosleep(&ts, NULL);
        }
    }
    if (g_fd < 0) return 0;
    fcntl(g_fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    { int one = 1; setsockopt(g_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
    memset(&g_rx, 0, sizeof g_rx);
    px_line_esc(g, sizeof g, gid ? gid : "");
    snprintf(b, sizeof b, "HELLO role=engine engine=%s proto=1 gid=%s caps=%s pid=%d", engine, g, caps ? caps : "",
             (int)getpid());
    return send_line(b);
}

int px_room_up(void) { return g_fd >= 0; }

void px_room_session(const char *state, const char *reason)
{
    char b[200], r[80];
    if (g_fd < 0) return;
    if (reason && *reason) {
        px_line_esc(r, sizeof r, reason);
        snprintf(b, sizeof b, "SESSION state=%s reason=%s", state, r);
    } else
        snprintf(b, sizeof b, "SESSION state=%s", state);
    send_line(b);
}

int px_room_poll(void)
{
    char b[1024], line[PX_LINE_MAX];
    if (g_fd < 0) return g_ended;
    for (;;) {
        ssize_t n = recv(g_fd, b, sizeof b, MSG_DONTWAIT);
        if (n > 0) { px_rx_feed(&g_rx, b, (size_t)n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {   /* the room's socket closed */
            close(g_fd);
            g_fd = -1;
        }
        break;
    }
    while (px_rx_next(&g_rx, line, sizeof line)) {
        px_line l;
        px_line_parse(&l, line);
        if (!strcmp(l.verb, "END")) g_ended = 1;
    }
    return g_ended;
}

int px_room_ended(void) { return g_ended; }

void px_room_close(const char *reason)
{
    char b[96];
    if (g_fd < 0) return;
    snprintf(b, sizeof b, "GAME state=ended reason=%s", reason && *reason ? reason : "quit");
    send_line(b);
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
}
