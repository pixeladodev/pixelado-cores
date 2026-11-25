/* pixelado-retro: the minimal libretro frontend the retro cores (pixelado-core-gpsp, later -snes) are built on. One
 * libretro core linked in statically, run as a program of its own that Pixelado Party launches (common/README.md).
 * This header is the frontend's own modules; nothing here is a public API. */
#ifndef PX_RETRO_H
#define PX_RETRO_H

#include <stddef.h>
#include <stdint.h>

#include "libretro.h"

/* ---- log.c ---- */
void px_log_open(const char *path);
void px_log(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;
long long px_now_ms(void);
long long px_now_us(void);

/* ---- card.c: the second panel's card, and text in a 5x7 pixel font ---- */
typedef struct px_canvas {
    uint32_t *px;
    int w, h, pitch;   /* pitch in pixels */
} px_canvas;
#define PX_BG 0xFF0B1530u
#define PX_BG2 0xFF13224Au
#define PX_INK 0xFFE6EEFFu
#define PX_MUTED 0xFF8FA3CCu
#define PX_ACCENT 0xFF3D7BFFu
#define PX_GOOD 0xFF3DDC84u
#define PX_WARN 0xFFFFB43Du
void px_fill(px_canvas *c, int x, int y, int w, int h, uint32_t col);
/* text at (x, y), each font pixel s x s; returns the width drawn */
int px_text(px_canvas *c, int x, int y, int s, uint32_t col, const char *t);
int px_text_w(int s, const char *t);
typedef struct px_card {
    char title[64];
    char line[4][64];
    uint32_t line_col[4];
    char foot[64];
    int fps;          /* -1: not shown */
} px_card;
void px_card_draw(px_canvas *c, const px_card *k);

/* ---- video: the game on one panel, scaled (fit or whole numbers); the card on the other ---- */
typedef struct px_video_cfg {
    const char *kind;     /* "kms" or "null" */
    const char *top;      /* the top panel's connector name (BANANADS_TOP: DSI-2 on the RG DS) */
    int game_panel;       /* 0 top, 1 bottom */
    int null_w, null_h;   /* "null": the panels' size */
} px_video_cfg;
int px_video_open(const px_video_cfg *cfg);
void px_video_close(void);
/* a game frame (RGB565, pitch in pixels): kept, scaled and shown */
void px_video_frame(const uint16_t *src, int w, int h, int pitch);
/* the card panel: draw into the canvas, then commit */
px_canvas *px_video_card(void);
void px_video_card_commit(void);
void px_video_scale_set(int fit);     /* 1: the largest size that keeps the game's aspect ratio; 0: whole numbers only */
int px_video_scale_get(void);
void px_video_service(void);          /* page flips that finished */
void px_video_panel_size(int *w, int *h);
int px_video_grab(int panel, uint32_t *dst, int w, int h);
int px_video_drop_master(void);
int px_video_take_master(int ms);
void px_video_repaint(void);
int px_video_dump_ppm(const char *path);   /* the game panel as shown, for tests */

/* ---- audio ---- */
int px_audio_open(const char *kind, int rate);
void px_audio_close(void);
void px_audio_write(const int16_t *stereo, size_t frames);
int px_audio_fill_ms(void);       /* how much is queued, -1: unknown */
void px_audio_pause(int on);      /* paused: nothing written, the device kept */
void px_audio_sfx(const int16_t *stereo, size_t frames, int rate);   /* a menu sound, while paused */
int px_audio_rate(void);

/* ---- input ---- */
int px_input_open(const char *kind);   /* "evdev", "none", or "script:<file>" (tests) */
void px_input_close(void);
void px_input_poll(long long frame);
uint32_t px_input_pad(void);           /* RETRO_DEVICE_ID_JOYPAD_* bits */
int px_input_menu(void);               /* 1 once: a menu button let go (pressed while the game had the screen) */
int px_input_quit(void);               /* 1 once: F + Start */
int px_input_menu_held_ms(void);       /* a menu button held now, ms */
void px_input_regain(void);            /* the screen came back: keys down ignored until let go, menu keys quiet 250 ms */
int px_input_any_pressed(uint32_t mask);   /* 1 once per press of one of these pad bits (the own pause card) */

/* ---- link.c: the libretro netpacket interface over TCP between the handhelds ---- */
enum { PX_LINK_OFF, PX_LINK_CONNECTING, PX_LINK_LISTENING, PX_LINK_CONNECTED, PX_LINK_FAILED, PX_LINK_LOST };
typedef struct px_link_cfg {
    int role;             /* 0 none, 1 host, 2 join */
    const char *addr;     /* join: the host's address */
    int port;             /* 7067 */
    int players;          /* the most players (host) */
    int player;           /* this one's index (0 host) */
    const char *name;     /* the nickname, shown to the others */
    int retry_s;          /* join: tried for this long */
    const char *proto;    /* the core's netpacket protocol (must match) */
} px_link_cfg;
int px_link_start(const px_link_cfg *cfg, const struct retro_netpacket_callback *cb);
void px_link_service(int deliver);    /* the sockets; deliver: packets to the core (between frames only) */
int px_link_state(void);
const char *px_link_reason(void);
int px_link_peers(void);              /* players in the session, us included (0: no session) */
int px_link_id(void);
void px_link_hold(int on);            /* our menu holds the group */
int px_link_holders(char *names, size_t cap);   /* the others holding it (names '\n' between) */
int px_link_lan_lines(char *out, size_t cap);   /* the session as Party's LAN lines */
void px_link_stop(void);

/* ---- core.c: the libretro host ---- */
typedef struct px_core_cfg {
    const char *rom;
    const char *system_dir;
    const char *save_path;            /* the save RAM's file (.srm) */
    const char *options[32];          /* "key=value" over the core's defaults */
    int noptions;
} px_core_cfg;
int px_core_load(const px_core_cfg *cfg);
void px_core_run(void);
void px_core_unload(void);
double px_core_fps(void);
int px_core_sample_rate(void);
const struct retro_netpacket_callback *px_core_netpacket(void);
const char *px_core_name(void);
const char *px_core_version(void);
void px_core_save(int force);          /* the save RAM written when it changed (force: whatever) */
/* the frame and the samples of the last px_core_run */
const uint16_t *px_core_frame(int *w, int *h, int *pitch);
const int16_t *px_core_samples(size_t *frames);

/* ---- system.c: the handheld (BananaOS): volume, brightness, the menu's sounds ---- */
int px_sys_volume_get(void);
void px_sys_volume_set(int v);
int px_sys_brightness_get(void);
void px_sys_brightness_set(int v);
void px_sys_brightness_save(void);
/* the core's own settings, `key=value` lines in /data/pixelado-cores/<sys>.conf (PIXELADO_CORE_CONFIG: the folder) */
int px_sys_conf_get(const char *key, char *out, size_t cap);   /* 1: found */
void px_sys_conf_set(const char *key, const char *value);
int px_sys_opts_tick(int *vol, int *bri);
void px_sys_sound(int id);

#endif
