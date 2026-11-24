/* The log: one line per event, with the time since the start, to stderr or a file (--log FILE). */
#include "px.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static FILE *g_log;
static long long g_t0;

long long px_now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

long long px_now_ms(void) { return px_now_us() / 1000; }

void px_log_open(const char *path)
{
    g_t0 = px_now_ms();
    if (path && *path) {
        FILE *f = fopen(path, "a");
        if (f) {
            setvbuf(f, NULL, _IOLBF, 0);
            g_log = f;
        }
    }
}

void px_log(const char *fmt, ...)
{
    FILE *f = g_log ? g_log : stderr;
    char b[1024];
    size_t n;
    va_list ap;
    if (!g_t0) g_t0 = px_now_ms();
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    n = strlen(b);
    fprintf(f, "[%7.3f] %s%s", (px_now_ms() - g_t0) / 1000.0, b, n && b[n - 1] == '\n' ? "" : "\n");
    fflush(f);
}
