/* Pixelado Party's channel and the screen handover (common/README.md; ROOM-PROTOCOL §12.2, §12.5, §12.7).
 *
 * When Party runs a core as its child it hands it one end of a socketpair (--party-fd N, --set party.fd=N or
 * PIXELADO_PARTY_FD=N). The in-game menu and the "X is in the menu" screen are then Party's: at a menu (the player's,
 * or another player's holding the group) the core pauses, writes the last frame of both panels into a memfd (PXF1),
 * lets go of DRM master and says SCREEN state=released; Party draws over that frame and gives the screen back
 * (SCREEN state=give). The menu's settings come back as CMD lines and are done by the core through px_party_host.
 *
 * No channel, a channel that is gone, or a Party that doesn't answer in time: px_party_menu says so (UNAVAILABLE or
 * FALLBACK) and the core opens its own menu instead, so a game is never left without one.
 *
 * Nothing here knows the display or the emulator: the core gives them through px_party_host. C99 and POSIX; on Linux
 * the frame goes in a sealed memfd, elsewhere (tests on a computer) in an unlinked temporary file. */
#ifndef PIXELADO_PARTYCTL_H
#define PIXELADO_PARTYCTL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct px_party_host {
    void *ctx;
    /* DRM master: let go (1 ok); take it back, tried every 10 ms for up to ms (1 ok). NULL: nothing to do */
    int (*drop_master)(void *ctx);
    int (*take_master)(void *ctx, int ms);
    /* the panels as they were last shown (0 top, 1 bottom), at the panel's size: XRGB8888 rows of w pixels */
    void (*panel_size)(void *ctx, int *w, int *h);
    int (*grab)(void *ctx, int panel, uint32_t *dst, int w, int h);
    /* after the screen came back: the game's last frame on both panels again (a full modeset) */
    void (*repaint)(void *ctx);
    /* the group's pause: ours on or off; the others holding it (their names, '\n' between them, into names) */
    void (*hold_set)(void *ctx, int on);
    int (*hold_others)(void *ctx, char *names, size_t cap);
    /* the menu's settings: as the menu opens (fps -1: no FPS counter), a change, a tick every 500 ms while it is open
     * (1: the handheld's own keys moved the volume or the brightness), and its close (the brightness saved) */
    void (*opts_open)(void *ctx, int *vol, int *bri, int *fps);
    void (*opt_set)(void *ctx, const char *key, int value);
    int (*opts_tick)(void *ctx, int *vol, int *bri);
    void (*opts_close)(void *ctx);
    void (*sound)(void *ctx, int id);   /* 0 move, 1 select, 2 back: the menu's sounds on the core's audio device */
    void (*quit)(void *ctx);            /* the player quit from Party's menu (the core quits after it) */
    /* while Party has the screen: the core's own input read and thrown away; how long a menu button has been held,
     * ms (0: none). Held 3 s: the game quits (Party may have crashed with the screen) */
    int (*menu_held_ms)(void *ctx);
    int (*stopping)(void *ctx);         /* the process is shutting down (SIGTERM, the room's END) */
    /* called every ~10 ms while Party has the screen: what must not stop while the game is paused (a network link's
     * keepalive and its pause messages). May be NULL */
    void (*idle)(void *ctx);
    /* the multiplayer state as LAN/LANP/LAN_END lines ('\n' between them, no final one) into out; 0: none to send.
     * May be NULL */
    int (*lan_lines)(void *ctx, char *out, size_t cap);
    /* a CMD op=lan from Party's multiplayer page (mode, players, addr); NULL: answered ERR */
    int (*lan_set)(void *ctx, int mode, int players, const char *addr);
    void (*log)(void *ctx, const char *line);
} px_party_host;

/* fd: the channel; engine: "ds", "gba"...; caps: e.g. "pause-own,screen"; title: the game's name as the menu shows it;
 * resume: the quit writes a resume state (Party says so); gid: the room's game id or NULL. Says HELLO.
 * 0: no channel (the fd isn't a stream socket) */
int px_party_open(int fd, const char *engine, const char *caps, const char *title, int resume, const char *gid);
int px_party_up(void);    /* the channel is there and Party hasn't gone */
void px_party_close(void);
int px_party_fd(void);    /* -1: none */

typedef enum px_party_menu_result {
    PX_PARTY_UNAVAILABLE,  /* no channel, or Party gone: the screen was never let go of; open the core's own menu */
    PX_PARTY_DONE,         /* Party gave the screen back (the menu closed, or the group's pause is over); repainted */
    PX_PARTY_QUIT,         /* the player quit in Party's menu: Party keeps the screen, the core quits */
    PX_PARTY_FALLBACK      /* Party didn't answer in time or went away while it had the screen: taken back */
} px_party_menu_result;

/* ours: the player's own menu (the group already held by us); else another player's menu holds the group (the wait
 * screen). Blocks the caller while Party has the screen. */
px_party_menu_result px_party_menu(const px_party_host *h, int ours);

/* the PXF1 frame (exposed for tests): header size and the fourcc of its pixels */
#define PX_PXF1_HEADER 128
#define PX_FOURCC_XR24 0x34325258u

#ifdef __cplusplus
}
#endif
#endif
