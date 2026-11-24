/* The libretro host: the one core this program is built with (linked in, never loaded), its environment, its options
 * (the core's defaults, then --option key=value), and its save RAM in a file of its own (.srm, RetroArch's format:
 * the raw save, so the same file goes on in BananaOS's RetroArch or BananaFront). */
#include "px.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static px_core_cfg g_cfg;
static struct retro_system_info g_info;
static struct retro_system_av_info g_av;
static const struct retro_netpacket_callback *g_np;
static enum retro_pixel_format g_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static uint16_t *g_frame;    /* RGB565, as the video takes it */
static int g_fw, g_fh;
static int16_t *g_audio;
static size_t g_nframes, g_acap;
static uint8_t *g_save_copy;
static size_t g_save_size;
static long long g_save_checked;
static char *g_romdata;

/* ---- options ---- */
typedef struct opt {
    char key[64];
    char value[64];
} opt;
static opt g_opts[96];
static int g_nopts;

static opt *opt_find(const char *key)
{
    int i;
    for (i = 0; i < g_nopts; i++)
        if (!strcmp(g_opts[i].key, key)) return &g_opts[i];
    return NULL;
}

static void opt_put(const char *key, const char *value, int over)
{
    opt *o = opt_find(key);
    if (!o) {
        if (g_nopts >= (int)(sizeof g_opts / sizeof g_opts[0])) return;
        o = &g_opts[g_nopts++];
        snprintf(o->key, sizeof o->key, "%s", key);
    } else if (!over)
        return;
    snprintf(o->value, sizeof o->value, "%s", value);
}

static void apply_overrides(void)
{
    int i;
    for (i = 0; i < g_cfg.noptions; i++) {
        char k[64];
        const char *eq = strchr(g_cfg.options[i], '=');
        size_t n;
        if (!eq) continue;
        n = (size_t)(eq - g_cfg.options[i]);
        if (n >= sizeof k) continue;
        memcpy(k, g_cfg.options[i], n);
        k[n] = 0;
        opt_put(k, eq + 1, 1);
    }
}

/* ---- callbacks ---- */
static void RETRO_CALLCONV core_log(enum retro_log_level level, const char *fmt, ...)
{
    char b[512];
    va_list ap;
    if (level < RETRO_LOG_INFO) return;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    px_log("[core] %s", b);
}

static bool RETRO_CALLCONV env(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = g_cfg.system_dir;
        return g_cfg.system_dir != NULL;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = NULL;   /* (the save RAM is the frontend's: no files from the core) */
        return false;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        const enum retro_pixel_format f = *(const enum retro_pixel_format *)data;
        if (f != RETRO_PIXEL_FORMAT_RGB565 && f != RETRO_PIXEL_FORMAT_XRGB8888 && f != RETRO_PIXEL_FORMAT_0RGB1555)
            return false;
        g_fmt = f;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 0;   /* (the core falls back to SET_VARIABLES) */
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
        const struct retro_variable *v = data;
        for (; v && v->key; v++) {
            /* "Description; first|second|...": the first is the default */
            const char *semi = strchr(v->value, ';'), *s, *bar;
            char d[64];
            size_t n;
            if (!semi) continue;
            s = semi + 1;
            while (*s == ' ') s++;
            bar = strchr(s, '|');
            n = bar ? (size_t)(bar - s) : strlen(s);
            if (n >= sizeof d) n = sizeof d - 1;
            memcpy(d, s, n);
            d[n] = 0;
            opt_put(v->key, d, 0);
        }
        apply_overrides();
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        const opt *o = v && v->key ? opt_find(v->key) : NULL;
        if (!o) return false;
        v->value = o->value;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE:
        g_np = data;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        if (data && ((const struct retro_message *)data)->msg) px_log("[core] %s", ((const struct retro_message *)data)->msg);
        return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        if (data) g_av = *(const struct retro_system_av_info *)data;
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        if (data) g_av.geometry = *(const struct retro_game_geometry *)data;
        return true;
    }
    return false;
}

static void RETRO_CALLCONV video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    unsigned y, x;
    if (!data) return;   /* (a duped frame: the last one stays) */
    if ((int)w != g_fw || (int)h != g_fh || !g_frame) {
        free(g_frame);
        g_frame = malloc((size_t)w * h * 2);
        if (!g_frame) return;
        g_fw = (int)w;
        g_fh = (int)h;
    }
    for (y = 0; y < h; y++) {
        const uint8_t *row = (const uint8_t *)data + (size_t)y * pitch;
        uint16_t *o = g_frame + (size_t)y * w;
        if (g_fmt == RETRO_PIXEL_FORMAT_RGB565)
            memcpy(o, row, (size_t)w * 2);
        else if (g_fmt == RETRO_PIXEL_FORMAT_XRGB8888)
            for (x = 0; x < w; x++) {
                const uint32_t p = ((const uint32_t *)(const void *)row)[x];
                o[x] = (uint16_t)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
            }
        else
            for (x = 0; x < w; x++) {
                const uint16_t p = ((const uint16_t *)(const void *)row)[x];
                o[x] = (uint16_t)(((p << 1) & 0xFFC0) | (p & 0x1F));
            }
    }
}

static void put_audio(const int16_t *s, size_t frames)
{
    if (g_nframes + frames > g_acap) {
        size_t cap = g_acap ? g_acap : 4096;
        int16_t *b;
        while (cap < g_nframes + frames) cap *= 2;
        b = realloc(g_audio, cap * 4);
        if (!b) return;
        g_audio = b;
        g_acap = cap;
    }
    memcpy(g_audio + g_nframes * 2, s, frames * 4);
    g_nframes += frames;
}

static void RETRO_CALLCONV audio_cb(int16_t l, int16_t r)
{
    int16_t s[2] = {l, r};
    put_audio(s, 1);
}

static size_t RETRO_CALLCONV audio_batch_cb(const int16_t *data, size_t frames)
{
    put_audio(data, frames);
    return frames;
}

static void RETRO_CALLCONV input_poll_cb(void) {}

static int16_t RETRO_CALLCONV input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    const uint32_t pad = px_input_pad();
    (void)index;
    if (port != 0 || (device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)(pad & 0xFFFF);
    return id < 16 ? (int16_t)((pad >> id) & 1) : 0;
}

/* ---- the save RAM ---- */
static void save_load(void)
{
    uint8_t *m = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    const size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    FILE *f;
    if (!m || !n || !g_cfg.save_path) return;
    f = fopen(g_cfg.save_path, "rb");
    if (f) {
        const size_t got = fread(m, 1, n, f);
        fclose(f);
        px_log("[save] %s (%zu bytes)", g_cfg.save_path, got);
    } else
        px_log("[save] none yet at %s", g_cfg.save_path);
    g_save_copy = malloc(n);
    if (g_save_copy) memcpy(g_save_copy, m, n);
    g_save_size = n;
}

void px_core_save(int force)
{
    uint8_t *m = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    const size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    char tmp[1100];
    FILE *f;
    const long long now = px_now_ms();
    if (!m || !n || !g_cfg.save_path || !g_save_copy || n != g_save_size) return;
    if (!force && now - g_save_checked < 2000) return;   /* (looked at every 2 s) */
    g_save_checked = now;
    if (!memcmp(m, g_save_copy, n)) return;
    /* written whole beside it and renamed over it: a power cut leaves the old save or the new one */
    snprintf(tmp, sizeof tmp, "%s.tmp", g_cfg.save_path);
    f = fopen(tmp, "wb");
    if (!f) { px_log("[save] can't write %s (%s)", tmp, strerror(errno)); return; }
    if (fwrite(m, 1, n, f) != n || fflush(f) || fsync(fileno(f))) {
        px_log("[save] writing %s failed (%s)", tmp, strerror(errno));
        fclose(f);
        unlink(tmp);
        return;
    }
    fclose(f);
    if (rename(tmp, g_cfg.save_path)) { px_log("[save] %s: %s", g_cfg.save_path, strerror(errno)); return; }
    memcpy(g_save_copy, m, n);
    px_log("[save] written %s", g_cfg.save_path);
}

/* ---- the core ---- */
int px_core_load(const px_core_cfg *cfg)
{
    struct retro_game_info gi;
    g_cfg = *cfg;
    retro_set_environment(env);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();
    retro_get_system_info(&g_info);
    apply_overrides();
    memset(&gi, 0, sizeof gi);
    gi.path = cfg->rom;
    if (!g_info.need_fullpath) {
        FILE *f = fopen(cfg->rom, "rb");
        long n;
        if (!f) { px_log("[core] can't read %s", cfg->rom); return 0; }
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        g_romdata = malloc(n > 0 ? (size_t)n : 1);
        if (!g_romdata || fread(g_romdata, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 0; }
        fclose(f);
        gi.data = g_romdata;
        gi.size = (size_t)n;
    }
    if (!retro_load_game(&gi)) {
        px_log("[core] %s didn't load %s", g_info.library_name, cfg->rom);
        return 0;
    }
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    retro_get_system_av_info(&g_av);
    px_log("[core] %s %s: %ux%u at %.4f fps, sound %.0f Hz%s", g_info.library_name, g_info.library_version,
           g_av.geometry.base_width, g_av.geometry.base_height, g_av.timing.fps, g_av.timing.sample_rate,
           g_np ? ", link (netpacket)" : "");
    save_load();
    return 1;
}

void px_core_run(void)
{
    g_nframes = 0;
    retro_run();
}

void px_core_unload(void)
{
    px_core_save(1);
    retro_unload_game();
    retro_deinit();
    free(g_frame);
    free(g_audio);
    free(g_save_copy);
    free(g_romdata);
    g_frame = NULL;
    g_audio = NULL;
    g_save_copy = NULL;
    g_romdata = NULL;
}

double px_core_fps(void) { return g_av.timing.fps > 1 ? g_av.timing.fps : 60.0; }
int px_core_sample_rate(void) { return g_av.timing.sample_rate > 1000 ? (int)(g_av.timing.sample_rate + 0.5) : 48000; }
const struct retro_netpacket_callback *px_core_netpacket(void) { return g_np; }
const char *px_core_name(void) { return g_info.library_name ? g_info.library_name : "?"; }
const char *px_core_version(void) { return g_info.library_version ? g_info.library_version : "?"; }

const uint16_t *px_core_frame(int *w, int *h, int *pitch)
{
    *w = g_fw;
    *h = g_fh;
    *pitch = g_fw;
    return g_frame;
}

const int16_t *px_core_samples(size_t *frames)
{
    *frames = g_nframes;
    return g_audio;
}
