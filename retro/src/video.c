/* The picture. On the handhelds (kms): DRM master on the card with the two panels (the RG DS's DSI-1 and DSI-2, the
 * top one named by BANANADS_TOP, as BananaDS's frontend_kms finds them), the game on one panel, scaled to fit
 * keeping its aspect ratio (default) or by a whole number (2x on a 640x480 panel, 4x on 1024x768), centred; the card on
 * the other. Frames go to the screen
 * by page flips through three dumb buffers (the newest frame waits for the next flip: never a tear, never a wait).
 * The modes stay as they are; closing the device gives everything back.
 * Elsewhere (null): the same scaling into memory, for tests (--dump-frame). */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "px.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef PX_HAVE_KMS
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif

static int g_kms;                 /* 1: kms, 0: null */
static int g_W = 640, g_H = 480;  /* the panels' size */
static int g_game = 0;            /* the game's panel (0 top, 1 bottom) */
static int g_npanels = 2;
static uint16_t *g_frame;         /* the last frame, as the core gave it */
static int g_fw, g_fh;
static int g_k = 1, g_ox, g_oy;   /* the whole-number scale, and where the picture starts */
static int g_ow, g_oh;            /* the picture's size on the panel */
static int g_scale_fit = 1;       /* 1 fit (default), 0 integer */
static unsigned g_dirty;          /* buffers whose borders are stale (bit per game buffer) */
static uint16_t g_xmap[4096], g_ymap[4096];
static uint32_t g_lut[65536];
static px_canvas g_card;
static uint32_t *g_null_game;     /* null: the game panel as it would be shown */

static void lut_init(void)
{
    uint32_t i;
    for (i = 0; i < 65536; i++) {
        const uint32_t r = (i >> 11) & 31, g = (i >> 5) & 63, b = i & 31;
        g_lut[i] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
    }
}

static void clear(uint32_t *dst, int pitch, uint32_t col)
{
    int y, x;
    for (y = 0; y < g_H; y++)
        for (x = 0; x < g_W; x++) dst[(size_t)y * (size_t)pitch + x] = col;
}

/* Fit: the largest size that fits the panel keeping the game's own aspect ratio (a GBA's 3:2), centred. Integer: the
 * largest whole number. Both are nearest-neighbour from one table per axis, taken from the middle of each destination
 * pixel, so source pixels are spread as evenly as the ratio allows and pixel art stays crisp. */
static void layout(void)
{
    if (g_fw <= 0 || g_fh <= 0) return;
    if (g_scale_fit) {
        if ((long)g_W * g_fh <= (long)g_H * g_fw) {
            g_ow = g_W;
            g_oh = (int)((long)g_W * g_fh / g_fw);
        } else {
            g_oh = g_H;
            g_ow = (int)((long)g_H * g_fw / g_fh);
        }
    } else {
        int kx = g_W / g_fw, ky = g_H / g_fh;
        g_k = kx < ky ? kx : ky;
        if (g_k < 1) g_k = 1;
        g_ow = g_fw * g_k;
        g_oh = g_fh * g_k;
    }
    if (g_ow < 1) g_ow = 1;
    if (g_oh < 1) g_oh = 1;
    if (g_ow > g_W) g_ow = g_W;
    if (g_oh > g_H) g_oh = g_H;
    g_ox = (g_W - g_ow) / 2;
    g_oy = (g_H - g_oh) / 2;
    {
        int i;
        for (i = 0; i < g_ow; i++) g_xmap[i] = (uint16_t)(((2L * i + 1) * g_fw) / (2L * g_ow));
        for (i = 0; i < g_oh; i++) g_ymap[i] = (uint16_t)(((2L * i + 1) * g_fh) / (2L * g_oh));
    }
}

void px_video_scale_set(int fit)
{
    fit = fit ? 1 : 0;
    if (fit == g_scale_fit) return;
    g_scale_fit = fit;
    layout();
    g_dirty = ~0u;   /* the borders change: every buffer cleared before it is drawn on again */
}
int px_video_scale_get(void) { return g_scale_fit; }

/* the last frame into a panel-sized buffer (pitch in pixels); the border is left as it is. buf: which buffer (the
 * dirty bits say whether it still holds the old layout's borders) */
static void render_into(uint32_t *dst, int pitch, int buf)
{
    static uint32_t row[4096];
    int y, x;
    if (!g_frame || g_ow > 4096 || g_oh <= 0) return;
    if (g_dirty & (1u << buf)) {
        clear(dst, pitch, 0xFF000000u);
        g_dirty &= ~(1u << buf);
    }
    for (y = 0; y < g_oh; y++) {
        uint32_t *d = dst + (size_t)(g_oy + y) * (size_t)pitch + g_ox;
        if (y > 0 && g_ymap[y] == g_ymap[y - 1]) {
            memcpy(d, d - pitch, (size_t)g_ow * 4);
            continue;
        }
        {
            const uint16_t *s = g_frame + (size_t)g_ymap[y] * (size_t)g_fw;
            for (x = 0; x < g_ow; x++) row[x] = g_lut[s[g_xmap[x]]];
        }
        memcpy(d, row, (size_t)g_ow * 4);
    }
}

/* ======================================== kms ======================================== */
#ifdef PX_HAVE_KMS
typedef struct kpanel {
    uint32_t conn, crtc;
    drmModeModeInfo mode;
    char name[32];
} kpanel;
typedef struct kbuf {
    uint32_t handle, pitch, fb;
    uint64_t size;
    uint8_t *map;
} kbuf;
#define NGB 3
static int kfd = -1;
static kpanel kp[2];
static kbuf gbuf[NGB], cbuf;
static int k_front = -1, k_pend = -1, k_queued = -1;

static int mkbuf(kbuf *b)
{
    struct drm_mode_create_dumb c;
    struct drm_mode_map_dumb m;
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    memset(&c, 0, sizeof c);
    c.width = (uint32_t)g_W;
    c.height = (uint32_t)g_H;
    c.bpp = 32;
    if (drmIoctl(kfd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) return 0;
    b->handle = c.handle;
    b->pitch = c.pitch;
    b->size = c.size;
    handles[0] = c.handle;
    pitches[0] = c.pitch;
    if (drmModeAddFB2(kfd, (uint32_t)g_W, (uint32_t)g_H, 0x34325258u /* XR24 */, handles, pitches, offsets, &b->fb, 0))
        return 0;
    memset(&m, 0, sizeof m);
    m.handle = c.handle;
    if (drmIoctl(kfd, DRM_IOCTL_MODE_MAP_DUMB, &m)) return 0;
    b->map = mmap(NULL, (size_t)c.size, PROT_READ | PROT_WRITE, MAP_SHARED, kfd, (off_t)m.offset);
    if (b->map == MAP_FAILED) { b->map = NULL; return 0; }
    clear((uint32_t *)(void *)b->map, (int)(b->pitch / 4), 0xFF000000u);
    return 1;
}

static void freebuf(kbuf *b)
{
    struct drm_mode_destroy_dumb d;
    if (b->map) munmap(b->map, (size_t)b->size);
    if (b->fb) drmModeRmFB(kfd, b->fb);
    if (b->handle) {
        memset(&d, 0, sizeof d);
        d.handle = b->handle;
        drmIoctl(kfd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    }
    memset(b, 0, sizeof *b);
}

static int find_panels(const char *top)
{
    drmModeRes *res = drmModeGetResources(kfd);
    struct { kpanel q; int top; } f[2];
    uint32_t used = 0;
    int i, n = 0;
    char type[32];
    const char *dash;
    if (!res) return 0;
    /* the connector type to look for: the top's name without its number ("DSI-2": DSI) */
    snprintf(type, sizeof type, "%s", top);
    dash = strrchr(type, '-');
    if (dash) type[dash - type] = 0;
    memset(f, 0, sizeof f);
    for (i = 0; i < res->count_connectors && n < 2; i++) {
        drmModeConnector *c = drmModeGetConnector(kfd, res->connectors[i]);
        const char *tn;
        int m, e, j;
        if (!c) continue;
        tn = drmModeGetConnectorTypeName(c->connector_type);
        if (tn && !strcmp(tn, type) && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            kpanel q;
            memset(&q, 0, sizeof q);
            /* the kernel command line's mode (video=<connector>:<mode>) first, then the panel's preferred one */
            q.mode = c->modes[0];
            for (m = 0; m < c->count_modes; m++)
                if (c->modes[m].type & DRM_MODE_TYPE_PREFERRED) { q.mode = c->modes[m]; break; }
            for (m = 0; m < c->count_modes; m++)
                if (c->modes[m].type & DRM_MODE_TYPE_USERDEF) { q.mode = c->modes[m]; break; }
            for (e = 0; e < c->count_encoders && !q.crtc; e++) {
                drmModeEncoder *enc = drmModeGetEncoder(kfd, c->encoders[e]);
                if (!enc) continue;
                for (j = 0; j < res->count_crtcs && !q.crtc; j++)
                    if ((enc->possible_crtcs & (1u << j)) && !(used & (1u << j))) {
                        q.crtc = res->crtcs[j];
                        used |= 1u << j;
                    }
                drmModeFreeEncoder(enc);
            }
            snprintf(q.name, sizeof q.name, "%s-%u", tn, c->connector_type_id);
            q.conn = c->connector_id;
            if (q.crtc) {
                f[n].q = q;
                f[n].top = !strcmp(q.name, top);
                n++;
            }
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    if (n == 0) return 0;
    if (n == 2) {
        /* the top panel: the named connector, else the second one (as banana-ui and BananaDS) */
        if (!f[0].top && !f[1].top) { kpanel t = f[0].q; f[0].q = f[1].q; f[1].q = t; }
        else if (f[1].top) { kpanel t = f[0].q; f[0].q = f[1].q; f[1].q = t; }
        if (f[0].q.mode.hdisplay != f[1].q.mode.hdisplay || f[0].q.mode.vdisplay != f[1].q.mode.vdisplay)
            px_log("[kms] the panels differ (%dx%d, %dx%d): the card follows the game's size", f[0].q.mode.hdisplay,
                   f[0].q.mode.vdisplay, f[1].q.mode.hdisplay, f[1].q.mode.vdisplay);
    }
    kp[0] = f[0].q;
    kp[1] = f[n == 2 ? 1 : 0].q;
    g_npanels = n;
    return 1;
}

static int game_i(void) { return g_npanels == 2 ? g_game : 0; }

/* what another program may have left on our panels (Party's menu, a frontend before us): every plane upright, and
 * the planes other than the primary ones off on our CRTCs (a legacy modeset only sets the primary) */
static void planes_clean(void)
{
    drmModePlaneRes *pr;
    uint32_t j;
    drmSetClientCap(kfd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    pr = drmModeGetPlaneResources(kfd);
    if (!pr) return;
    for (j = 0; j < pr->count_planes; j++) {
        drmModePlane *pl = drmModeGetPlane(kfd, pr->planes[j]);
        drmModeObjectProperties *pp = drmModeObjectGetProperties(kfd, pr->planes[j], DRM_MODE_OBJECT_PLANE);
        uint64_t type = 1;   /* (DRM_PLANE_TYPE_PRIMARY when it doesn't say) */
        uint32_t i;
        for (i = 0; pp && i < pp->count_props; i++) {
            drmModePropertyRes *prop = drmModeGetProperty(kfd, pp->props[i]);
            if (!prop) continue;
            if (!strcmp(prop->name, "rotation") && pp->prop_values[i] != 1 /* DRM_MODE_ROTATE_0 */)
                drmModeObjectSetProperty(kfd, pr->planes[j], DRM_MODE_OBJECT_PLANE, prop->prop_id, 1);
            if (!strcmp(prop->name, "type")) type = pp->prop_values[i];
            drmModeFreeProperty(prop);
        }
        if (pl && type != 1 && pl->crtc_id && (pl->crtc_id == kp[0].crtc || pl->crtc_id == kp[1].crtc))
            drmModeSetPlane(kfd, pl->plane_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        if (pp) drmModeFreeObjectProperties(pp);
        if (pl) drmModeFreePlane(pl);
    }
    drmModeFreePlaneResources(pr);
}

static int setcrtc(int panel, uint32_t fb)
{
    return drmModeSetCrtc(kfd, kp[panel].crtc, fb, 0, 0, &kp[panel].conn, 1, &kp[panel].mode);
}

static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec, void *data)
{
    (void)fd; (void)seq; (void)sec; (void)usec; (void)data;
    k_front = k_pend;
    k_pend = -1;
}

static void k_events(int wait_ms)
{
    struct pollfd p = {kfd, POLLIN, 0};
    drmEventContext ev;
    memset(&ev, 0, sizeof ev);
    ev.version = 2;
    ev.page_flip_handler = on_flip;
    while (poll(&p, 1, wait_ms) > 0 && (p.revents & POLLIN)) {
        drmHandleEvent(kfd, &ev);
        wait_ms = 0;
    }
}

static void k_flip_queued(void)
{
    if (k_pend >= 0 || k_queued < 0) return;
    if (drmModePageFlip(kfd, kp[game_i()].crtc, gbuf[k_queued].fb, DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0) {
        k_pend = k_queued;
        k_queued = -1;
    } else if (errno != EBUSY) {
        /* (a flip refused for another reason: shown by a modeset, once) */
        setcrtc(game_i(), gbuf[k_queued].fb);
        k_front = k_queued;
        k_queued = -1;
    }
}

static int kms_take(int ms)
{
    long long end = px_now_ms() + ms;
    for (;;) {
        if (drmSetMaster(kfd) == 0) return 1;
        if (px_now_ms() >= end) return 0;
        {
            struct timespec t = {0, 10000000};
            nanosleep(&t, NULL);
        }
    }
}

static int kms_open(const px_video_cfg *cfg)
{
    const char *dev = getenv("PIXELADO_CORE_DRI");
    int i;
    kfd = open(dev && *dev ? dev : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (kfd < 0) { px_log("[kms] no DRM device (%s)", strerror(errno)); return 0; }
    /* (Party let go of master before starting us; a moment in case it is still doing so) */
    if (!kms_take(2000)) px_log("[kms] no DRM master yet (%s): trying anyway", strerror(errno));
    if (!find_panels(cfg->top && *cfg->top ? cfg->top : "DSI-2")) { px_log("[kms] no panel"); return 0; }
    g_W = kp[0].mode.hdisplay;
    g_H = kp[0].mode.vdisplay;
    for (i = 0; i < NGB; i++)
        if (!mkbuf(&gbuf[i])) { px_log("[kms] no buffers (%s)", strerror(errno)); return 0; }
    if (g_npanels == 2 && !mkbuf(&cbuf)) { px_log("[kms] no card buffer (%s)", strerror(errno)); return 0; }
    planes_clean();
    if (setcrtc(game_i(), gbuf[0].fb)) { px_log("[kms] the game panel's modeset failed (%s)", strerror(errno)); return 0; }
    k_front = 0;
    if (g_npanels == 2 && setcrtc(1 - game_i(), cbuf.fb)) px_log("[kms] the card panel's modeset failed (%s)", strerror(errno));
    px_log("[kms] %s %dx%d (game on the %s panel, %s), %s %dx%d", kp[0].name, kp[0].mode.hdisplay, kp[0].mode.vdisplay,
           g_game ? "bottom" : "top", kp[game_i()].name, kp[1].name, kp[1].mode.hdisplay, kp[1].mode.vdisplay);
    return 1;
}

static void kms_close(void)
{
    int i;
    if (kfd < 0) return;
    if (k_pend >= 0) k_events(50);
    for (i = 0; i < NGB; i++) freebuf(&gbuf[i]);
    freebuf(&cbuf);
    drmDropMaster(kfd);
    close(kfd);
    kfd = -1;
}
#endif /* PX_HAVE_KMS */

/* ======================================== the frontend's side ======================================== */
int px_video_open(const px_video_cfg *cfg)
{
    lut_init();
    g_game = cfg->game_panel ? 1 : 0;
    g_kms = 0;
#ifdef PX_HAVE_KMS
    if (!cfg->kind || !strcmp(cfg->kind, "kms")) {
        if (!kms_open(cfg)) return 0;
        g_kms = 1;
    }
#else
    if (cfg->kind && !strcmp(cfg->kind, "kms")) { px_log("[video] this build has no kms"); return 0; }
#endif
    if (!g_kms) {
        g_W = cfg->null_w > 0 ? cfg->null_w : 640;
        g_H = cfg->null_h > 0 ? cfg->null_h : 480;
        g_null_game = calloc((size_t)g_W * (size_t)g_H, 4);
        if (!g_null_game) return 0;
        clear(g_null_game, g_W, 0xFF000000u);
        px_log("[video] null, panels %dx%d", g_W, g_H);
    }
    g_card.w = g_W;
    g_card.h = g_H;
    g_card.pitch = g_W;
    g_card.px = calloc((size_t)g_W * (size_t)g_H, 4);
    return g_card.px != NULL;
}

void px_video_close(void)
{
#ifdef PX_HAVE_KMS
    if (g_kms) kms_close();
#endif
    free(g_null_game);
    free(g_card.px);
    free(g_frame);
    g_null_game = NULL;
    g_card.px = NULL;
    g_frame = NULL;
}

void px_video_frame(const uint16_t *src, int w, int h, int pitch)
{
    int y;
    if (!src || w <= 0 || h <= 0) return;
    if (w != g_fw || h != g_fh || !g_frame) {
        free(g_frame);
        g_frame = malloc((size_t)w * (size_t)h * 2);
        if (!g_frame) return;
        g_fw = w;
        g_fh = h;
        layout();
        g_dirty = ~0u;   /* every buffer cleared before its next frame */
        px_log("[video] the game is %dx%d, shown %dx%d at %d,%d (%s)", w, h, g_ow, g_oh, g_ox, g_oy, g_scale_fit ? "fit" : "integer");
    }
    for (y = 0; y < h; y++) memcpy(g_frame + (size_t)y * (size_t)w, src + (size_t)y * (size_t)pitch, (size_t)w * 2);
#ifdef PX_HAVE_KMS
    if (g_kms) {
        int t = k_queued;
        k_events(0);
        if (t < 0) {
            int i;
            for (i = 0; i < NGB; i++)
                if (i != k_front && i != k_pend) { t = i; break; }
        }
        if (t < 0) return;   /* (can't happen with three buffers) */
        render_into((uint32_t *)(void *)gbuf[t].map, (int)(gbuf[t].pitch / 4), t);
        k_queued = t;
        k_flip_queued();
        return;
    }
#endif
    render_into(g_null_game, g_W, 0);
}

px_canvas *px_video_card(void) { return &g_card; }

void px_video_card_commit(void)
{
#ifdef PX_HAVE_KMS
    if (g_kms && g_npanels == 2 && cbuf.map) {
        int y;
        for (y = 0; y < g_H; y++)
            memcpy(cbuf.map + (size_t)y * cbuf.pitch, g_card.px + (size_t)y * (size_t)g_W, (size_t)g_W * 4);
    }
#endif
}

void px_video_service(void)
{
#ifdef PX_HAVE_KMS
    if (g_kms) {
        k_events(0);
        k_flip_queued();
    }
#endif
}

void px_video_panel_size(int *w, int *h)
{
    *w = g_W;
    *h = g_H;
}

int px_video_grab(int panel, uint32_t *dst, int w, int h)
{
    if (w != g_W || h != g_H) return 0;
    if (panel == g_game || (g_kms && g_npanels < 2)) {
        clear(dst, w, 0xFF000000u);
        render_into(dst, w, 31);
    } else
        memcpy(dst, g_card.px, (size_t)w * (size_t)h * 4);
    return 1;
}

int px_video_drop_master(void)
{
#ifdef PX_HAVE_KMS
    if (g_kms) {
        if (k_pend >= 0) k_events(50);   /* (the last flip done first: Party copies what is on screen) */
        return drmDropMaster(kfd) == 0;
    }
#endif
    return 1;
}

int px_video_take_master(int ms)
{
#ifdef PX_HAVE_KMS
    if (g_kms) return kms_take(ms);
#endif
    (void)ms;
    return 1;
}

void px_video_repaint(void)
{
#ifdef PX_HAVE_KMS
    if (g_kms) {
        /* Party had its own buffers on the CRTCs: ours back with a full modeset */
        int t = k_front >= 0 ? k_front : 0;
        k_pend = k_queued = -1;
        planes_clean();
        render_into((uint32_t *)(void *)gbuf[t].map, (int)(gbuf[t].pitch / 4), t);
        if (setcrtc(game_i(), gbuf[t].fb)) px_log("[kms] repaint: the game panel (%s)", strerror(errno));
        k_front = t;
        if (g_npanels == 2) {
            px_video_card_commit();
            if (setcrtc(1 - game_i(), cbuf.fb)) px_log("[kms] repaint: the card panel (%s)", strerror(errno));
        }
    }
#endif
}

int px_video_dump_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    uint32_t *buf;
    int i;
    if (!f) return 0;
    buf = malloc((size_t)g_W * (size_t)g_H * 4);
    if (!buf) { fclose(f); return 0; }
    px_video_grab(g_game, buf, g_W, g_H);
    fprintf(f, "P6\n%d %d\n255\n", g_W, g_H);
    for (i = 0; i < g_W * g_H; i++) {
        unsigned char p[3] = {(unsigned char)(buf[i] >> 16), (unsigned char)(buf[i] >> 8), (unsigned char)buf[i]};
        fwrite(p, 1, 3, f);
    }
    free(buf);
    fclose(f);
    return 1;
}
