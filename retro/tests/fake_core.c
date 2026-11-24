/* A libretro core for testing the frontend without an emulator: a 240x160 picture, silence at 32768 Hz, a counter
 * in its save RAM, and the netpacket interface: every frame each side sends its frame number to everyone (the host
 * by broadcast, a guest to the host and by broadcast), and counts what arrives. At the end it logs what it saw:
 *   fake: id=<client id> sent=<n> received=<n> from=<ids seen> stops=<n> max_gap=<frames between two receipts> */
#include "libretro.h"

#include <stdio.h>
#include <string.h>

static retro_environment_t env;
static retro_video_refresh_t video;
static retro_audio_sample_batch_t audio;
static retro_input_poll_t ipoll;
static retro_input_state_t istate;
static retro_log_printf_t logp;
static uint16_t fb[240 * 160];
static int16_t snd[1100 * 2];
static uint8_t sram[8192];
static unsigned frame, sent, received, from_mask, stops, last_rx, max_gap;
static int my_id = -1;
static retro_netpacket_send_t np_send;
static retro_netpacket_poll_receive_t np_poll;

static void RETRO_CALLCONV np_start(uint16_t id, retro_netpacket_send_t s, retro_netpacket_poll_receive_t p)
{
    my_id = id;
    np_send = s;
    np_poll = p;
    fprintf(stderr, "fake: started as %u\n", id);
}
static void RETRO_CALLCONV np_receive(const void *buf, size_t len, uint16_t from)
{
    (void)buf;
    if (len >= 4 && !memcmp(buf, "FAKE", 4)) {
        received++;
        from_mask |= 1u << (from & 15);
        if (last_rx && frame - last_rx > max_gap) max_gap = frame - last_rx;
        last_rx = frame;
    }
}
static void RETRO_CALLCONV np_stop(void) { stops++; np_send = NULL; np_poll = NULL; }
static bool RETRO_CALLCONV np_connected(uint16_t id) { fprintf(stderr, "fake: client %u connected\n", id); return true; }
static void RETRO_CALLCONV np_disconnected(uint16_t id) { fprintf(stderr, "fake: client %u disconnected\n", id); }
static const struct retro_netpacket_callback np = {np_start, np_receive, np_stop, NULL, np_connected, np_disconnected,
                                                   "fake v1"};

void retro_set_environment(retro_environment_t e)
{
    static const struct retro_variable vars[] = {{"fake_color", "Colour; blue|red"}, {NULL, NULL}};
    env = e;
    e(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars);
}
void retro_set_video_refresh(retro_video_refresh_t v) { video = v; }
void retro_set_audio_sample(retro_audio_sample_t a) { (void)a; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t a) { audio = a; }
void retro_set_input_poll(retro_input_poll_t p) { ipoll = p; }
void retro_set_input_state(retro_input_state_t s) { istate = s; }
void retro_init(void)
{
    struct retro_log_callback l;
    enum retro_pixel_format f = RETRO_PIXEL_FORMAT_RGB565;
    if (env(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &l)) logp = l.log;
    env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &f);
    env(RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE, (void *)&np);
}
void retro_deinit(void)
{
    fprintf(stderr, "fake: id=%d sent=%u received=%u from=0x%x stops=%u max_gap=%u frames=%u\n", my_id, sent, received,
            from_mask, stops, max_gap, frame);
}
unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_get_system_info(struct retro_system_info *i)
{
    memset(i, 0, sizeof *i);
    i->library_name = "fake";
    i->library_version = "1";
    i->need_fullpath = true;
    i->valid_extensions = "bin";
}
void retro_get_system_av_info(struct retro_system_av_info *i)
{
    memset(i, 0, sizeof *i);
    i->geometry.base_width = i->geometry.max_width = 240;
    i->geometry.base_height = i->geometry.max_height = 160;
    i->timing.fps = 16777216.0 / 280896.0;
    i->timing.sample_rate = 32768;
}
void retro_set_controller_port_device(unsigned p, unsigned d) { (void)p; (void)d; }
void retro_reset(void) {}
void retro_run(void)
{
    struct retro_variable v = {"fake_color", NULL};
    unsigned i;
    uint16_t col;
    ipoll();
    env(RETRO_ENVIRONMENT_GET_VARIABLE, &v);
    col = v.value && !strcmp(v.value, "red") ? 0xF800 : 0x001F;
    if (istate(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A)) col = 0x07E0;
    for (i = 0; i < 240 * 160; i++) fb[i] = (i % 240) < 120 ? col : (uint16_t)(frame * 64);
    frame++;
    sram[0] = 'F';
    sram[1] = (uint8_t)frame;
    if (np_send) {
        char b[16];
        memcpy(b, "FAKE", 4);
        memcpy(b + 4, &frame, 4);
        np_send(RETRO_NETPACKET_RELIABLE, b, 8, RETRO_NETPACKET_BROADCAST);
        sent++;
        if (np_poll) np_poll();
    }
    video(fb, 240, 160, 480);
    memset(snd, 0, sizeof snd);
    audio(snd, 549);
}
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *d, size_t s) { (void)d; (void)s; return false; }
bool retro_unserialize(const void *d, size_t s) { (void)d; (void)s; return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }
bool retro_load_game(const struct retro_game_info *g) { (void)g; return true; }
bool retro_load_game_special(unsigned t, const struct retro_game_info *g, size_t n) { (void)t; (void)g; (void)n; return false; }
void retro_unload_game(void) {}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? sram : NULL; }
size_t retro_get_memory_size(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? sizeof sram : 0; }
