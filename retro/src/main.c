/* pixelado-retro's program: one libretro core, the handheld's panels, sound and buttons, the link to the other
 * handhelds, and Pixelado Party's channel (its menu, the group's pause). See retro/README.md for the options and
 * common/README.md for how Party runs it. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "px.h"
#include "pixelado/partyctl.h"
#include "pixelado/roomctl.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PX_SYS
#define PX_SYS "retro"
#endif
#ifndef PX_VERSION
#define PX_VERSION "0.0.0"
#endif
#ifndef PX_DEFAULT_OPTIONS
#define PX_DEFAULT_OPTIONS ""
#endif
#ifndef PX_SAVE_EXT
#define PX_SAVE_EXT ".srm"
#endif

enum { RC_QUIT = 0, RC_ERROR = 2, RC_END = 10, RC_NO_SESSION = 11, RC_SESSION_LOST = 12 };

static volatile sig_atomic_t g_stop;
static int g_fps_on, g_fps;
static long long g_frame;
static char g_title[128], g_nick[64];
static int g_role, g_player;
static px_card g_card_shown;
static int g_card_valid;
static char g_pause_line[64];
static int g_paused_ui;

static void on_signal(int s) { (void)s; g_stop = 1; }

static void usage(void)
{
    fprintf(stderr,
            "pixelado-core-" PX_SYS " " PX_VERSION "\n"
            "usage: pixelado-core-" PX_SYS " [options] ROM\n"
            "  --room host|join        a room's game: host it, or join --room-addr\n"
            "  --room-addr IP          the host (join)\n"
            "  --room-port N           the link's TCP port (7067)\n"
            "  --room-players N        the most players (host, 2-4)\n"
            "  --player N              this player's seat (0 the host)\n"
            "  --nickname NAME         shown to the others\n"
            "  --party-fd FD           Pixelado Party's channel (PIXELADO_PARTY_FD)\n"
            "  --room-ctl SOCK         the room's socket (PIXELADO_ROOM_CTL)\n"
            "  --room-game GID         the room's game id (PIXELADO_ROOM_GAME)\n"
            "  --set key=value         party.fd, room.ctl, room.gid (as the DS core takes them)\n"
            "  --save-dir DIR          where the save goes (<save-dir>/<save-name>" PX_SAVE_EXT ")\n"
            "  --save-name NAME        the save's name (default: the ROM's, without its extension)\n"
            "  --system-dir DIR        the BIOS folder (optional)\n"
            "  --option key=value      a core option\n"
            "  --title TEXT            the game's name as shown\n"
            "  --screen top|bottom     the game's panel (top); the card on the other\n"
            "  --video kms|null  --audio alsa|null  --input evdev|none|script:FILE\n"
            "  --frames N  --dump-frame N:FILE.ppm  --unpaced  --test-pause-ms N  --log FILE  --fps\n");
}

/* ---- the card ---- */
static void card_update(void)
{
    px_card k;
    px_canvas *c = px_video_card();
    const int st = px_link_state(), n = px_link_peers();
    memset(&k, 0, sizeof k);
    snprintf(k.title, sizeof k.title, "%s", g_title);
    if (g_role == 0) snprintf(k.line[0], sizeof k.line[0], "PLAYING ALONE");
    else snprintf(k.line[0], sizeof k.line[0], "PLAYER %d%s%s%s", g_player + 1, g_role == 1 ? " - HOST" : "",
                  g_nick[0] ? " - " : "", g_nick);
    k.line_col[0] = PX_INK;
    if (g_role) {
        switch (st) {
        case PX_LINK_CONNECTING: snprintf(k.line[1], sizeof k.line[1], "LINK: CONNECTING..."); break;
        case PX_LINK_LISTENING: snprintf(k.line[1], sizeof k.line[1], "LINK: WAITING FOR PLAYERS"); break;
        case PX_LINK_CONNECTED:
            snprintf(k.line[1], sizeof k.line[1], "LINK: %d PLAYERS", n);
            k.line_col[1] = PX_GOOD;
            break;
        case PX_LINK_LOST:
            snprintf(k.line[1], sizeof k.line[1], "LINK LOST");
            k.line_col[1] = PX_WARN;
            break;
        case PX_LINK_FAILED:
            snprintf(k.line[1], sizeof k.line[1], "LINK FAILED: %s", px_link_reason());
            k.line_col[1] = PX_WARN;
            break;
        }
    }
    if (g_pause_line[0]) {
        snprintf(k.line[2], sizeof k.line[2], "%s", g_pause_line);
        k.line_col[2] = PX_ACCENT;
    }
    snprintf(k.foot, sizeof k.foot, "%s", g_paused_ui ? "A: RESUME   F+START: QUIT" : "F: MENU   F+START: QUIT");
    k.fps = g_fps_on ? g_fps : -1;
    if (g_card_valid && !memcmp(&k, &g_card_shown, sizeof k)) return;
    g_card_shown = k;
    g_card_valid = 1;
    px_card_draw(c, &k);
    px_video_card_commit();
}

/* ---- the room's link state, said to the room once per change ---- */
static int g_said_state = -1;
static void room_track(void)
{
    const int st = px_link_state();
    if (st == g_said_state) return;
    if (st == PX_LINK_LISTENING && g_said_state < 0) px_room_session("listening", NULL);
    else if (st == PX_LINK_CONNECTED && g_role == 2) px_room_session("connected", NULL);
    else if (st == PX_LINK_LOST) px_room_session("lost", px_link_reason());
    else if (st == PX_LINK_FAILED) px_room_session("failed", px_link_reason());
    g_said_state = st;
}

/* ---- Party's host ---- */
static int h_drop(void *c) { (void)c; return px_video_drop_master(); }
static int h_take(void *c, int ms) { (void)c; return px_video_take_master(ms); }
static void h_size(void *c, int *w, int *h) { (void)c; px_video_panel_size(w, h); }
static int h_grab(void *c, int p, uint32_t *d, int w, int h) { (void)c; return px_video_grab(p, d, w, h); }
static void h_repaint(void *c) { (void)c; g_card_valid = 0; card_update(); px_video_repaint(); }
static void h_hold(void *c, int on) { (void)c; px_link_hold(on); }
static int h_others(void *c, char *n, size_t cap) { (void)c; return px_link_holders(n, cap); }
static void h_opts_open(void *c, int *v, int *b, int *f)
{
    (void)c;
    *v = px_sys_volume_get();
    *b = px_sys_brightness_get();
    *f = g_fps_on;
}
static void h_opt_set(void *c, const char *k, int v)
{
    (void)c;
    if (!strcmp(k, "volume")) px_sys_volume_set(v);
    else if (!strcmp(k, "brightness")) px_sys_brightness_set(v);
    else if (!strcmp(k, "fps")) g_fps_on = v != 0;
}
static int h_tick(void *c, int *v, int *b) { (void)c; return px_sys_opts_tick(v, b); }
static void h_opts_close(void *c) { (void)c; px_sys_brightness_save(); }
static void h_sound(void *c, int id) { (void)c; px_sys_sound(id); }
static void h_quit(void *c) { (void)c; }
static int h_held(void *c) { (void)c; return px_input_menu_held_ms(); }
static int h_stopping(void *c) { (void)c; return g_stop || px_room_poll(); }
static void h_idle(void *c) { (void)c; px_link_service(0); room_track(); }
static int h_lan(void *c, char *o, size_t cap) { (void)c; return px_link_lan_lines(o, cap); }
static void h_log(void *c, const char *l) { (void)c; px_log("%s", l); }

static px_party_host g_host = {NULL, h_drop, h_take, h_size, h_grab, h_repaint, h_hold, h_others, h_opts_open,
                               h_opt_set, h_tick, h_opts_close, h_sound, h_quit, h_held, h_stopping, h_idle, h_lan,
                               NULL, h_log};

static void sleep_ms(int ms)
{
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

/* the pause without Party (no channel, or Party failed): the card says it, the game panel keeps the last frame.
 * held: our own (A, B, Start or the menu button resumes); else others hold the group (wait until they let go).
 * Returns 0 to go on, 1 to quit */
static int g_test_pause_ms;
static int own_pause(int held)
{
    const long long t0 = px_now_ms();
    char names[256];
    int was_held = -1;
    for (;;) {
        int n;
        if (g_stop || px_room_poll()) return 1;
        px_link_service(0);
        room_track();
        px_input_poll(-1);
        if (px_input_quit()) return 1;
        n = px_link_holders(names, sizeof names);
        if (held) {
            if (px_input_any_pressed((1u << RETRO_DEVICE_ID_JOYPAD_A) | (1u << RETRO_DEVICE_ID_JOYPAD_B) |
                                     (1u << RETRO_DEVICE_ID_JOYPAD_START)) || px_input_menu() ||
                (g_test_pause_ms > 0 && px_now_ms() - t0 >= g_test_pause_ms)) {
                held = 0;
                px_link_hold(0);
                px_log("[pause] our pause is over%s", n ? ": waiting for the others" : "");
            }
        } else {
            if (n == 0) return 0;
            if (px_input_menu()) {
                held = 1;
                px_link_hold(1);
                px_log("[pause] our menu too");
            }
        }
        if (held != was_held || held == 0) {
            char *nl = strchr(names, '\n');
            if (nl) *nl = 0;   /* (the first one's name on the card) */
            if (held) snprintf(g_pause_line, sizeof g_pause_line, "PAUSED");
            else snprintf(g_pause_line, sizeof g_pause_line, "%s IS IN THE MENU", names[0] ? names : "A PLAYER");
            g_paused_ui = held;
            card_update();
            was_held = held;
        }
        px_video_service();
        sleep_ms(16);
    }
}

/* a menu (ours) or the others' pause: Party's screens when Party is there, else our own. 0 go on, 1 quit */
static int pause_group(int ours)
{
    int quit = 0;
    char names[256];
    if (ours) px_link_hold(1);
    px_audio_pause(1);
    px_log("[pause] %s (frame %lld)", ours ? "our menu" : "the others' menu", g_frame);
    if (px_link_holders(names, sizeof names) || ours) {
        if (!ours) {
            char *nl = strchr(names, '\n');
            if (nl) *nl = 0;
            snprintf(g_pause_line, sizeof g_pause_line, "%s IS IN THE MENU", names);
        } else
            snprintf(g_pause_line, sizeof g_pause_line, "PAUSED");
        card_update();
    }
    if (px_party_up()) {
        const px_party_menu_result r = px_party_menu(&g_host, ours);
        if (r == PX_PARTY_QUIT) quit = 1;
        else if (r != PX_PARTY_DONE) quit = own_pause(ours);   /* FALLBACK (our hold kept), UNAVAILABLE */
    } else
        quit = own_pause(ours);
    if (!quit) px_link_hold(0);
    g_pause_line[0] = 0;
    g_paused_ui = 0;
    card_update();
    px_input_regain();
    px_audio_pause(0);
    px_log("[pause] over (frame %lld)", g_frame);
    return quit;
}

static void sleep_until_us(long long t)
{
#if defined(__linux__)
    struct timespec ts;
    ts.tv_sec = (time_t)(t / 1000000);
    ts.tv_nsec = (long)(t % 1000000) * 1000;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR && !g_stop) {}
#else
    long long d = t - px_now_us();
    if (d > 0) {
        struct timespec ts = {(time_t)(d / 1000000), (long)(d % 1000000) * 1000};
        nanosleep(&ts, NULL);
    }
#endif
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int main(int argc, char **argv)
{
    const char *rom = NULL, *save_dir = NULL, *save_name = NULL, *system_dir = NULL, *title = NULL;
    const char *video = NULL, *audio = NULL, *input = NULL, *logf = NULL, *addr = NULL, *dump = NULL;
    const char *party_fd = getenv("PIXELADO_PARTY_FD"), *room_ctl = getenv("PIXELADO_ROOM_CTL");
    const char *gid = getenv("PIXELADO_ROOM_GAME"), *top = getenv("PIXELADO_CORE_TOP");
    const char *opts[32];
    char defopts[256], save_path[1024];
    int nopts = 0, port = 7067, players = 2, screen = 0, unpaced = 0, rc = RC_QUIT, i;
    long long frames = -1, dump_at = -1;
    px_core_cfg cc;
    px_video_cfg vc;
    px_link_cfg lc;
    double fps;
    long long next, fps_t0;
    int fps_n = 0;

    if (!top || !*top) top = getenv("BANANADS_TOP");
    if (getenv("PIXELADO_ROOM_PLAYER")) g_player = atoi(getenv("PIXELADO_ROOM_PLAYER"));
    /* the build's own option defaults first, the command line's over them */
    snprintf(defopts, sizeof defopts, "%s", PX_DEFAULT_OPTIONS);
    {
        char *save = NULL, *t;
        for (t = strtok_r(defopts, " ", &save); t && nopts < 16; t = strtok_r(NULL, " ", &save)) opts[nopts++] = t;
    }
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define TAKE(name) (!strcmp(a, name) && v && (i++, 1))
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else if (!strcmp(a, "--version")) { printf("pixelado-core-%s %s\n", PX_SYS, PX_VERSION); return 0; }
        else if (TAKE("--room")) g_role = !strcmp(v, "host") ? 1 : !strcmp(v, "join") ? 2 : 0;
        else if (TAKE("--room-addr")) addr = v;
        else if (TAKE("--room-port")) port = atoi(v);
        else if (TAKE("--room-players")) players = atoi(v);
        else if (TAKE("--player")) g_player = atoi(v);
        else if (TAKE("--nickname")) snprintf(g_nick, sizeof g_nick, "%s", v);
        else if (TAKE("--party-fd")) party_fd = v;
        else if (TAKE("--room-ctl")) room_ctl = v;
        else if (TAKE("--room-game")) gid = v;
        else if (TAKE("--save-dir")) save_dir = v;
        else if (TAKE("--save-name")) save_name = v;
        else if (TAKE("--system-dir")) system_dir = v;
        else if (TAKE("--option")) { if (nopts < 32) opts[nopts++] = v; }
        else if (TAKE("--title")) title = v;
        else if (TAKE("--screen")) screen = !strcmp(v, "bottom");
        else if (TAKE("--top")) top = v;
        else if (TAKE("--video")) video = v;
        else if (TAKE("--audio")) audio = v;
        else if (TAKE("--input")) input = v;
        else if (TAKE("--frames")) frames = atoll(v);
        else if (TAKE("--dump-frame")) { dump_at = atoll(v); dump = strchr(v, ':') ? strchr(v, ':') + 1 : NULL; }
        else if (TAKE("--log")) logf = v;
        else if (TAKE("--test-pause-ms")) g_test_pause_ms = atoi(v);
        else if (!strcmp(a, "--unpaced")) unpaced = 1;
        else if (!strcmp(a, "--fps")) g_fps_on = 1;
        else if (TAKE("--set")) {
            if (!strncmp(v, "party.fd=", 9)) party_fd = v + 9;
            else if (!strncmp(v, "room.ctl=", 9)) room_ctl = v + 9;
            else if (!strncmp(v, "room.gid=", 9)) gid = v + 9;
            else if (!strncmp(v, "link.port=", 10)) port = atoi(v + 10);
            else fprintf(stderr, "pixelado-core-%s: --set %s ignored\n", PX_SYS, v);
        } else if (a[0] == '-' && a[1] == '-') {
            fprintf(stderr, "pixelado-core-%s: unknown option %s\n", PX_SYS, a);
            usage();
            return RC_ERROR;
        } else
            rom = a;
#undef TAKE
    }
    if (!rom) { usage(); return RC_ERROR; }
    if (g_role == 2 && (!addr || !*addr)) { fprintf(stderr, "--room join needs --room-addr\n"); return RC_ERROR; }
    if (g_role == 1) g_player = 0;
    if (players < 2) players = 2;

    px_log_open(logf);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);
    px_log("pixelado-core-%s %s: %s (%s%s%s)", PX_SYS, PX_VERSION, rom, g_role == 1 ? "host" : g_role == 2 ? "join " : "alone",
           g_role == 2 ? addr : "", party_fd ? ", with Party" : "");

    /* the title: as given, else the ROM's name without its folder and extension */
    {
        const char *b = base_name(rom);
        const char *dot = strrchr(b, '.');
        size_t n = dot ? (size_t)(dot - b) : strlen(b);
        if (title) snprintf(g_title, sizeof g_title, "%s", title);
        else {
            if (n >= sizeof g_title) n = sizeof g_title - 1;
            memcpy(g_title, b, n);
            g_title[n] = 0;
        }
        /* the save: <save-dir>/<save-name>.srm, the save-name the ROM's (BananaOS's RetroArch and BananaFront name it so) */
        if (save_dir) {
            char sn[512];
            if (save_name) snprintf(sn, sizeof sn, "%s", save_name);
            else {
                if (n >= sizeof sn) n = sizeof sn - 1;
                memcpy(sn, b, n);
                sn[n] = 0;
            }
            mkdir(save_dir, 0755);
            snprintf(save_path, sizeof save_path, "%s/%s%s", save_dir, sn, PX_SAVE_EXT);
        } else {
            /* (no folder given: beside the ROM, as a standalone emulator does) */
            snprintf(save_path, sizeof save_path, "%.*s%s", (int)(strrchr(rom, '.') ? strrchr(rom, '.') - rom : (long)strlen(rom)),
                     rom, PX_SAVE_EXT);
        }
    }

    if (room_ctl && *room_ctl) {
        if (px_room_open(room_ctl, PX_SYS, gid, "pause-own")) px_log("[room] on the room's socket");
        else px_log("[room] no room socket at %s: playing without it", room_ctl);
    }

    memset(&vc, 0, sizeof vc);
    vc.kind = video;
    vc.top = top;
    vc.game_panel = screen;
    if (!px_video_open(&vc)) {
        px_log("no display");
        px_room_close("error");
        return RC_ERROR;
    }
    px_input_open(input);

    memset(&cc, 0, sizeof cc);
    cc.rom = rom;
    cc.system_dir = system_dir;
    cc.save_path = save_path;
    for (i = 0; i < nopts; i++) cc.options[cc.noptions++] = opts[i];
    if (!px_core_load(&cc)) {
        px_room_close("error");
        px_video_close();
        return RC_ERROR;
    }
    px_audio_open(audio, px_core_sample_rate());
    fps = px_core_fps();

    if (party_fd && *party_fd) {
        if (px_party_open(atoi(party_fd), PX_SYS, "pause-own,screen", g_title, 0, gid))
            px_log("[party] on Party's channel (fd %s)", party_fd);
        else
            px_log("[party] fd %s is no channel: our own pause", party_fd);
    }

    memset(&lc, 0, sizeof lc);
    lc.role = g_role;
    lc.addr = addr;
    lc.port = port;
    lc.players = players;
    lc.player = g_player;
    lc.name = g_nick;
    lc.retry_s = 15;
    {
        const struct retro_netpacket_callback *np = px_core_netpacket();
        lc.proto = np && np->protocol_version ? np->protocol_version : px_core_version();
        if (g_role) px_link_start(&lc, np);
    }
    card_update();

    /* a guest: the game starts once it is in the host's session (tried for 15 s) */
    while (g_role == 2 && px_link_state() == PX_LINK_CONNECTING && !g_stop) {
        px_link_service(0);
        if (px_room_poll()) break;
        px_input_poll(-1);
        if (px_input_quit()) { g_stop = 1; break; }
        card_update();
        sleep_ms(20);
    }
    room_track();
    if (px_room_ended()) rc = RC_END;
    else if (g_role && px_link_state() == PX_LINK_FAILED) {
        px_log("no session: %s", px_link_reason());
        rc = RC_NO_SESSION;
        g_stop = 1;
    }
    card_update();

    next = px_now_us();
    fps_t0 = px_now_ms();
    while (!g_stop) {
        int menu, others;
        char names[256];
        px_link_service(1);
        room_track();
        if (px_room_poll()) { rc = RC_END; break; }
        px_input_poll(g_frame);
        if (px_input_quit()) { rc = RC_QUIT; break; }
        if (frames >= 0 && g_frame >= frames) break;
        menu = px_input_menu();
        others = px_link_holders(names, sizeof names);
        if (menu || others) {
            if (pause_group(menu)) {
                rc = px_room_ended() ? RC_END : RC_QUIT;
                break;
            }
            next = px_now_us();
            continue;
        }
        px_core_run();
        g_frame++;
        {
            int w, h, pitch;
            size_t n;
            const uint16_t *f = px_core_frame(&w, &h, &pitch);
            const int16_t *s = px_core_samples(&n);
            if (f) px_video_frame(f, w, h, pitch);
            if (s && n) px_audio_write(s, n);
        }
        if (dump && g_frame == dump_at) {
            px_video_dump_ppm(dump);
            px_log("[test] frame %lld dumped to %s", g_frame, dump);
        }
        px_core_save(0);
        fps_n++;
        if (px_now_ms() - fps_t0 >= 1000) {
            g_fps = (int)(fps_n * 1000 / (px_now_ms() - fps_t0));
            fps_n = 0;
            fps_t0 = px_now_ms();
            if (g_fps_on) card_update();
            else if (g_role) card_update();   /* (the link's state) */
        }
        /* the pace: the core's own rate (the GBA's 59.73 Hz), nudged by at most 0.5% to keep the sound buffer
         * near 40 ms (the panels' 60 Hz shows a frame twice every ~4 s: no tear, no drop) */
        if (!unpaced) {
            double period = 1e6 / fps;
            const int fill = px_audio_fill_ms();
            const long long now = px_now_us();
            if (fill >= 0) {
                double adj = (fill - 40) / 40.0 * 0.005;
                if (adj > 0.005) adj = 0.005;
                if (adj < -0.005) adj = -0.005;
                period *= 1.0 + adj;
            }
            next += (long long)period;
            if (next < now - 100000) next = now;   /* (late by much: a stop, a sleep; started again from now) */
            sleep_until_us(next);
            px_video_service();
        }
    }

    px_log("quitting (frame %lld, code %d)", g_frame, rc);
    px_link_stop();
    px_core_unload();
    px_room_close(rc == RC_END ? "end" : rc == RC_QUIT ? "quit" : "error");
    px_party_close();
    px_audio_close();
    px_input_close();
    px_video_close();
    return rc;
}
