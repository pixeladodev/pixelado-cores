/* The buttons. On the handhelds (evdev): every pad and keyboard under /dev/input, read but never grabbed (bananad
 * and Party read the same devices), mapped as BananaOS maps them: A east, B south (Nintendo layout), L/R, Start,
 * Select, the d-pad, the hat and the left stick as the d-pad. F (BTN_MODE) is the hotkey: F + Start quits, F alone
 * (a tap) opens the menu, as do HOME (BTN_Z), F2 and L3; a keyboard's Esc quits. A menu opens when its button is let go, and only if it was
 * pressed while the game had the screen (ROOM-PROTOCOL §12.7: the F that closes Party's menu must not open ours).
 * For tests (script:<file>): lines "<frame> <keys>", keys a,b,x,y,l,r,start,select,up,down,left,right, "-" (none),
 * "menu" or "quit". */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "px.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PX_HAVE_EVDEV
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#define J(id) (1u << RETRO_DEVICE_ID_JOYPAD_##id)

static uint32_t g_pad, g_pressed_edges;
static int g_menu_ev, g_quit_ev;

/* ---- script ---- */
typedef struct sev {
    long long frame;
    uint32_t pad;
    int menu, quit;
} sev;
static sev *g_script;
static int g_nscript, g_iscript;

static uint32_t key_bit(const char *k)
{
    static const struct { const char *n; uint32_t b; } T[] = {
        {"a", J(A)}, {"b", J(B)}, {"x", J(X)}, {"y", J(Y)}, {"l", J(L)}, {"r", J(R)}, {"start", J(START)},
        {"select", J(SELECT)}, {"up", J(UP)}, {"down", J(DOWN)}, {"left", J(LEFT)}, {"right", J(RIGHT)}};
    size_t i;
    for (i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(T[i].n, k)) return T[i].b;
    return 0;
}

static int script_open(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    if (!f) { px_log("[input] no script %s", path); return 0; }
    while (fgets(line, sizeof line, f)) {
        sev e;
        char keys[200], *tok, *save = NULL;
        if (line[0] == '#' || sscanf(line, "%lld %199s", &e.frame, keys) != 2) continue;
        e.pad = 0;
        e.menu = e.quit = 0;
        for (tok = strtok_r(keys, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            if (!strcmp(tok, "menu")) e.menu = 1;
            else if (!strcmp(tok, "quit")) e.quit = 1;
            else e.pad |= key_bit(tok);
        }
        g_script = realloc(g_script, (size_t)(g_nscript + 1) * sizeof *g_script);
        if (!g_script) { fclose(f); return 0; }
        g_script[g_nscript++] = e;
    }
    fclose(f);
    px_log("[input] script %s, %d steps", path, g_nscript);
    return 1;
}

static void script_poll(long long frame)
{
    while (g_iscript < g_nscript && g_script[g_iscript].frame <= frame) {
        const sev *e = &g_script[g_iscript++];
        g_pressed_edges |= e->pad & ~g_pad;
        g_pad = e->pad;
        if (e->menu) g_menu_ev = 1;
        if (e->quit) g_quit_ev = 1;
    }
}

/* ---- evdev ---- */
#ifdef PX_HAVE_EVDEV
#define MAXDEV 16
#define NBITS(x) ((((x) - 1) / (8 * sizeof(long))) + 1)
typedef struct dev {
    int fd;
    int absmin[2], absmax[2];   /* the left stick's range (ABS_X, ABS_Y) */
    int hat[2], stick[2];
} dev;
static dev g_dev[MAXDEV];
static int g_ndev;
static unsigned char g_down[KEY_MAX + 1], g_ign[KEY_MAX + 1], g_seen[KEY_MAX + 1];
static int g_f_used;
static long long g_quiet_until, g_menu_since;

static int test_bit(const unsigned long *b, int n) { return (int)((b[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1); }

static int evdev_open(void)
{
    int i;
    for (i = 0; i < 32 && g_ndev < MAXDEV; i++) {
        char p[64], name[128] = "";
        unsigned long keys[NBITS(KEY_MAX + 1)], abs[NBITS(ABS_MAX + 1)];
        int fd;
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        memset(keys, 0, sizeof keys);
        memset(abs, 0, sizeof abs);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
        /* the pad, HOME's adc keys and keyboards; not the power key, the volume keys, the lid, the jack, the touch */
        if (strstr(name, "pwrkey") || strstr(name, "volume") || strstr(name, "hall") || strstr(name, "Headphones") ||
            strstr(name, "Touch") || strstr(name, "touch") ||
            !(test_bit(keys, BTN_SOUTH) || test_bit(keys, BTN_DPAD_UP) || test_bit(keys, BTN_Z) ||
              test_bit(keys, BTN_MODE) || test_bit(keys, KEY_F2) || test_bit(keys, KEY_ENTER) || test_bit(keys, BTN_START) ||
              test_bit(abs, ABS_HAT0X))) {
            close(fd);
            continue;
        }
        memset(&g_dev[g_ndev], 0, sizeof g_dev[0]);
        g_dev[g_ndev].fd = fd;
        {
            int a;
            for (a = 0; a < 2; a++) {
                struct input_absinfo ai;
                if (test_bit(abs, a == 0 ? ABS_X : ABS_Y) && ioctl(fd, EVIOCGABS(a == 0 ? ABS_X : ABS_Y), &ai) == 0) {
                    g_dev[g_ndev].absmin[a] = ai.minimum;
                    g_dev[g_ndev].absmax[a] = ai.maximum;
                }
            }
        }
        px_log("[input] %s (%s)", p, name);
        g_ndev++;
    }
    return 1;
}

static uint32_t code_bit(int c)
{
    switch (c) {
    case BTN_EAST: case KEY_ENTER: return J(A);   /* the RG DS's A is east (Nintendo layout) */
    case BTN_SOUTH: case KEY_BACKSPACE: return J(B);
    case BTN_NORTH: case KEY_X: return J(X);
    case BTN_WEST: case KEY_Y: return J(Y);
    case BTN_TL: case KEY_PAGEUP: return J(L);
    case BTN_TR: case KEY_PAGEDOWN: return J(R);
    case BTN_TL2: return J(L2);
    case BTN_TR2: return J(R2);
    case BTN_START: case KEY_S: return J(START);
    case BTN_SELECT: case KEY_TAB: return J(SELECT);
    case BTN_DPAD_UP: case KEY_UP: return J(UP);
    case BTN_DPAD_DOWN: case KEY_DOWN: return J(DOWN);
    case BTN_DPAD_LEFT: case KEY_LEFT: return J(LEFT);
    case BTN_DPAD_RIGHT: case KEY_RIGHT: return J(RIGHT);
    }
    return 0;
}
static int is_f(int c) { return c == BTN_MODE || c == KEY_F1; }
static int is_menu(int c) { return c == BTN_Z || c == KEY_F2 || c == BTN_THUMBL; }

static void key(int c, int v)
{
    const long long now = px_now_ms();
    if (c < 0 || c > KEY_MAX || v == 2) return;
    if (!v && g_ign[c]) { g_ign[c] = 0; g_down[c] = 0; return; }
    if (g_ign[c]) return;
    g_down[c] = (unsigned char)(v != 0);
    if (c == KEY_ESC && v) g_quit_ev = 1;   /* (a keyboard's Esc quits, as in BananaDS) */
    if (is_f(c)) {
        if (v) { g_f_used = 0; g_seen[c] = now >= g_quiet_until; }
        else if (g_seen[c] && !g_f_used) g_menu_ev = 1;
        if (!v) g_seen[c] = 0;
    } else if (is_menu(c)) {
        if (v) g_seen[c] = now >= g_quiet_until;
        else if (g_seen[c]) g_menu_ev = 1;
        if (!v) g_seen[c] = 0;
    } else if (v && (g_down[BTN_MODE] || g_down[KEY_F1])) {
        g_f_used = 1;
        if (code_bit(c) == J(START)) g_quit_ev = 1;
    }
}

static void evdev_poll(void)
{
    int i, c;
    uint32_t pad = 0;
    for (i = 0; i < g_ndev; i++) {
        struct input_event ev[32];
        ssize_t n;
        dev *d = &g_dev[i];
        while ((n = read(d->fd, ev, sizeof ev)) > 0) {
            int k;
            for (k = 0; k < (int)(n / (ssize_t)sizeof ev[0]); k++) {
                const struct input_event *e = &ev[k];
                if (e->type == EV_KEY) key(e->code, e->value);
                else if (e->type == EV_ABS && (e->code == ABS_HAT0X || e->code == ABS_HAT0Y))
                    d->hat[e->code == ABS_HAT0Y] = e->value;
                else if (e->type == EV_ABS && (e->code == ABS_X || e->code == ABS_Y)) {
                    const int a = e->code == ABS_Y, mn = d->absmin[a], mx = d->absmax[a];
                    const int mid = (mn + mx) / 2, th = (mx - mn) / 4;
                    d->stick[a] = mx > mn ? (e->value < mid - th ? -1 : e->value > mid + th ? 1 : 0) : 0;
                }
            }
        }
        if (n < 0 && errno == ENODEV) { close(d->fd); d->fd = -1; }
        if (d->hat[0] < 0 || d->stick[0] < 0) pad |= J(LEFT);
        if (d->hat[0] > 0 || d->stick[0] > 0) pad |= J(RIGHT);
        if (d->hat[1] < 0 || d->stick[1] < 0) pad |= J(UP);
        if (d->hat[1] > 0 || d->stick[1] > 0) pad |= J(DOWN);
    }
    for (c = 0; c <= KEY_MAX; c++)
        if (g_down[c]) pad |= code_bit(c);
    if (g_down[BTN_MODE] || g_down[KEY_F1]) pad = 0;   /* (F + a button is a hotkey, not the game's) */
    g_pressed_edges |= pad & ~g_pad;
    g_pad = pad;
    if (g_down[BTN_MODE] || g_down[KEY_F1] || g_down[BTN_Z] || g_down[KEY_F2] || g_down[BTN_THUMBL]) {
        if (!g_menu_since) g_menu_since = px_now_ms();
    } else
        g_menu_since = 0;
}
#endif /* PX_HAVE_EVDEV */

static int g_kind;   /* 0 none, 1 evdev, 2 script */

int px_input_open(const char *kind)
{
    if (kind && !strncmp(kind, "script:", 7)) { g_kind = 2; return script_open(kind + 7); }
    if (kind && !strcmp(kind, "none")) { g_kind = 0; return 1; }
#ifdef PX_HAVE_EVDEV
    g_kind = 1;
    return evdev_open();
#else
    g_kind = 0;
    return 1;
#endif
}

void px_input_close(void)
{
#ifdef PX_HAVE_EVDEV
    int i;
    for (i = 0; i < g_ndev; i++)
        if (g_dev[i].fd >= 0) close(g_dev[i].fd);
    g_ndev = 0;
#endif
    free(g_script);
    g_script = NULL;
}

void px_input_poll(long long frame)
{
    if (g_kind == 2) { if (frame >= 0) script_poll(frame); return; }
#ifdef PX_HAVE_EVDEV
    if (g_kind == 1) evdev_poll();
#endif
    (void)frame;
}

uint32_t px_input_pad(void) { return g_pad; }

int px_input_menu(void)
{
    const int e = g_menu_ev;
    g_menu_ev = 0;
    return e;
}

int px_input_quit(void)
{
    const int e = g_quit_ev;
    g_quit_ev = 0;
    return e;
}

int px_input_menu_held_ms(void)
{
#ifdef PX_HAVE_EVDEV
    if (g_kind == 1) {
        evdev_poll();
        return g_menu_since ? (int)(px_now_ms() - g_menu_since) : 0;
    }
#endif
    return 0;
}

void px_input_regain(void)
{
#ifdef PX_HAVE_EVDEV
    if (g_kind == 1) {
        int c;
        evdev_poll();   /* (what came while Party had the screen: read and dropped) */
        for (c = 0; c <= KEY_MAX; c++) {
            if (g_down[c]) { g_ign[c] = 1; g_down[c] = 0; }
            g_seen[c] = 0;
        }
        g_quiet_until = px_now_ms() + 250;
        g_pad = 0;
    }
#endif
    g_menu_ev = 0;
    g_quit_ev = 0;
    g_pressed_edges = 0;
}

int px_input_any_pressed(uint32_t mask)
{
    const uint32_t e = g_pressed_edges & mask;
    g_pressed_edges &= ~mask;
    return e != 0;
}
