/* See pixelado/line.h. */
#include "pixelado/line.h"

#include <stdlib.h>
#include <string.h>

static int hexv(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void unesc(char *out, size_t cap, const char *s, size_t n)
{
    size_t o = 0, i;
    for (i = 0; i < n && o + 1 < cap; i++) {
        if (s[i] == '%' && i + 2 < n && hexv((unsigned char)s[i + 1]) >= 0 && hexv((unsigned char)s[i + 2]) >= 0) {
            out[o++] = (char)(hexv((unsigned char)s[i + 1]) * 16 + hexv((unsigned char)s[i + 2]));
            i += 2;
        } else
            out[o++] = s[i];
    }
    out[o] = 0;
}

void px_line_parse(px_line *l, const char *s)
{
    const char *p = s, *e;
    size_t n;
    memset(l, 0, sizeof *l);
    while (*p == ' ') p++;
    e = strchr(p, ' ');
    n = e ? (size_t)(e - p) : strlen(p);
    if (n >= sizeof l->verb) n = sizeof l->verb - 1;
    memcpy(l->verb, p, n);
    l->verb[n] = 0;
    while (e) {
        const char *tok = e + 1, *eq;
        size_t tl;
        e = strchr(tok, ' ');
        tl = e ? (size_t)(e - tok) : strlen(tok);
        eq = memchr(tok, '=', tl);
        if (!eq || l->nkv >= PX_LINE_KEYS) continue;
        if ((size_t)(eq - tok) >= sizeof l->key[0]) continue;
        memcpy(l->key[l->nkv], tok, (size_t)(eq - tok));
        l->key[l->nkv][eq - tok] = 0;
        unesc(l->val[l->nkv], sizeof l->val[0], eq + 1, tl - (size_t)(eq - tok) - 1);
        l->nkv++;
    }
}

const char *px_line_str(const px_line *l, const char *key, const char *def)
{
    int i;
    for (i = 0; i < l->nkv; i++)
        if (!strcmp(l->key[i], key)) return l->val[i];
    return def;
}

long px_line_num(const px_line *l, const char *key, long def)
{
    const char *v = px_line_str(l, key, NULL);
    char *e;
    long n;
    if (!v || !*v) return def;
    n = strtol(v, &e, 10);
    return *e ? def : n;
}

size_t px_line_esc(char *out, size_t cap, const char *s)
{
    static const char hx[] = "0123456789ABCDEF";
    size_t o = 0;
    if (!cap) return 0;
    for (; s && *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c < 0x21 || c == '%' || c == '=' || c == 0x7F) {
            if (o + 3 >= cap) break;
            out[o++] = '%';
            out[o++] = hx[c >> 4];
            out[o++] = hx[c & 15];
        } else {
            if (o + 1 >= cap) break;
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    return o;
}

void px_rx_feed(px_rx *r, const char *b, size_t n)
{
    if (n > sizeof r->buf - r->len) {
        /* (a line longer than the buffer: what is there can't be a whole line, dropped) */
        if (!memchr(r->buf, '\n', r->len)) r->len = 0;
        if (n > sizeof r->buf - r->len) n = sizeof r->buf - r->len;
    }
    memcpy(r->buf + r->len, b, n);
    r->len += n;
}

int px_rx_next(px_rx *r, char *line, size_t cap)
{
    for (;;) {
        char *nl = memchr(r->buf, '\n', r->len);
        size_t n;
        if (!nl) {
            if (r->len >= PX_LINE_MAX) r->len = 0;   /* too long: dropped */
            return 0;
        }
        n = (size_t)(nl - r->buf);
        if (n > 0 && r->buf[n - 1] == '\r') n--;
        if (n < cap && n < PX_LINE_MAX) {
            memcpy(line, r->buf, n);
            line[n] = 0;
        } else
            line[0] = 0;
        memmove(r->buf, nl + 1, r->len - (size_t)(nl + 1 - r->buf));
        r->len -= (size_t)(nl + 1 - r->buf);
        if (line[0]) return 1;
    }
}
