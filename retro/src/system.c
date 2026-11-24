/* The handheld's own settings, as the DS core's menu does them on BananaOS (bananads menu.c): the volume through
 * /usr/libexec/banana/volume (and bananad told, it keeps the level), the brightness on both panels' backlights
 * (backlight1 the top, backlight0 the bottom), saved in /data/banana/backlight when the menu closes; the menu's sounds
 * are the theme's WAVs. Elsewhere every call does nothing. The slow ones (the volume script) run on a worker thread. */
#include "px.h"

#ifndef PX_SYS
#define PX_SYS "retro"
#endif

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *BLN[2] = {"backlight1", "backlight0"};   /* top, bottom */

static int rd_int(const char *p)
{
    FILE *f = fopen(p, "r");
    int v = -1;
    if (!f) return -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static int bl_pm(int i)   /* a panel's level, thousandths of its maximum (-1: none) */
{
    char p[96];
    int mx, v;
    snprintf(p, sizeof p, "/sys/class/backlight/%s/max_brightness", BLN[i]);
    mx = rd_int(p);
    snprintf(p, sizeof p, "/sys/class/backlight/%s/brightness", BLN[i]);
    v = rd_int(p);
    return mx > 0 && v >= 0 ? v * 1000 / mx : -1;
}

static void bl_set_pm(int i, int pm)
{
    char p[96];
    int mx, v;
    FILE *f;
    snprintf(p, sizeof p, "/sys/class/backlight/%s/max_brightness", BLN[i]);
    mx = rd_int(p);
    if (mx <= 0) return;
    v = mx * pm / 1000;
    if (v < 1) v = 1;
    snprintf(p, sizeof p, "/sys/class/backlight/%s/brightness", BLN[i]);
    f = fopen(p, "w");
    if (f) {
        fprintf(f, "%d\n", v);
        fclose(f);
    }
}

/* ---- the volume, off the caller's thread ---- */
static pthread_mutex_t wm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wc = PTHREAD_COND_INITIALIZER;
static int w_vol = -1, w_read, w_started;
static volatile int g_vol_read = -1;
static int g_vol = 50, g_bri = 80, g_bri_changed, g_bl_base[2] = {-1, -1}, g_bl_avg = 80;
static long long g_vol_set_at, g_bri_set_at;

static int vol_script(void)
{
    FILE *p;
    int v = -1;
    if (access("/usr/libexec/banana/volume", X_OK)) return -1;
    p = popen("/usr/libexec/banana/volume get 2>/dev/null", "r");
    if (!p) return -1;
    if (fscanf(p, "%d", &v) != 1) v = -1;
    pclose(p);
    return v < 0 ? -1 : v > 100 ? 100 : v;
}

static void *worker(void *a)
{
    (void)a;
    for (;;) {
        int v, r;
        pthread_mutex_lock(&wm);
        while (w_vol < 0 && !w_read) pthread_cond_wait(&wc, &wm);
        v = w_vol;
        r = w_read;
        w_vol = -1;
        w_read = 0;
        pthread_mutex_unlock(&wm);
        if (v >= 0 && !access("/usr/libexec/banana/volume", X_OK)) {
            char cmd[96];
            snprintf(cmd, sizeof cmd, "/usr/libexec/banana/volume set %d >/dev/null 2>&1", v);
            if (system(cmd)) {}
            if (system("killall -HUP bananad >/dev/null 2>&1")) {}   /* (it keeps the level across reboots) */
        }
        if (r && v < 0) g_vol_read = vol_script();
    }
    return NULL;
}

static void want(int v, int r)
{
    pthread_mutex_lock(&wm);
    if (!w_started) {
        pthread_t t;
        if (!pthread_create(&t, NULL, worker, NULL)) {
            pthread_detach(t);
            w_started = 1;
        }
    }
    if (v >= 0) w_vol = v;
    if (r) w_read = 1;
    pthread_cond_signal(&wc);
    pthread_mutex_unlock(&wm);
}

int px_sys_volume_get(void)
{
    const int v = vol_script();
    if (v >= 0) g_vol = v;
    return g_vol;
}

void px_sys_volume_set(int v)
{
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if (v == g_vol) return;
    g_vol = v;
    g_vol_set_at = px_now_ms();
    want(v, 0);
}

int px_sys_brightness_get(void)
{
    int i, n = 0, sum = 0;
    for (i = 0; i < 2; i++) {
        g_bl_base[i] = bl_pm(i);
        if (g_bl_base[i] > 0) { sum += g_bl_base[i]; n++; }
    }
    g_bl_avg = n ? (sum / n + 5) / 10 : 80;
    g_bri = g_bl_avg;
    g_bri_changed = 0;
    return g_bri;
}

void px_sys_brightness_set(int v)
{
    int i;
    v = v < 5 ? 5 : v > 100 ? 100 : v;
    if (v == g_bri) return;
    g_bri = v;
    /* both panels moved by the same step from where they were */
    for (i = 0; i < 2; i++) {
        int x;
        if (g_bl_base[i] <= 0) continue;
        x = g_bl_base[i] + (v - g_bl_avg) * 10;
        bl_set_pm(i, x < 50 ? 50 : x > 1000 ? 1000 : x);
    }
    g_bri_changed = 1;
    g_bri_set_at = px_now_ms();
}

void px_sys_brightness_save(void)
{
    int t, u;
    FILE *f;
    if (!g_bri_changed) return;
    g_bri_changed = 0;
    t = bl_pm(0);
    u = bl_pm(1);
    if (t <= 0 && u <= 0) return;
    f = fopen("/data/banana/backlight", "w");
    if (f) {
        fprintf(f, "%d %d\n", t > 0 ? t : u, u > 0 ? u : t);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
}

int px_sys_opts_tick(int *vol, int *bri)
{
    const long long t = px_now_ms();
    int changed = 0, r, i, n = 0, sum = 0;
    want(-1, 1);
    r = g_vol_read;
    if (r >= 0) {
        g_vol_read = -1;
        if (r != g_vol && t - g_vol_set_at > 1500) { g_vol = r; changed = 1; }
    }
    if (t - g_bri_set_at > 1500) {
        for (i = 0; i < 2; i++) {
            const int x = bl_pm(i);
            if (x > 0) { sum += x; n++; }
        }
        if (n) {
            const int nb = (sum / n + 5) / 10;
            if (nb - g_bri >= 3 || g_bri - nb >= 3) { g_bri = px_sys_brightness_get(); changed = 1; }
        }
    }
    *vol = g_vol;
    *bri = g_bri;
    return changed;
}

/* ---- the menu's sounds ---- */
typedef struct wav {
    int16_t *pcm;
    size_t frames;
    int rate;
} wav;
static wav g_snd[3];
static int g_snd_loaded;

static void load_wav(wav *w, const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t h[12], ck[8];
    int ch = 0, bits = 0;
    if (!f) return;
    if (fread(h, 1, 12, f) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
        while (fread(ck, 1, 8, f) == 8) {
            const uint32_t len = ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24;
            if (!memcmp(ck, "fmt ", 4)) {
                uint8_t fm[40];
                const size_t k = len < sizeof fm ? len : sizeof fm;
                if (fread(fm, 1, k, f) != k) break;
                if (len > k) fseek(f, (long)(len - k), SEEK_CUR);
                ch = fm[2] | fm[3] << 8;
                w->rate = fm[4] | fm[5] << 8 | fm[6] << 16 | fm[7] << 24;
                bits = fm[14] | fm[15] << 8;
            } else if (!memcmp(ck, "data", 4) && bits == 16 && (ch == 1 || ch == 2) && len < (4u << 20)) {
                int16_t *raw = malloc(len);
                size_t i, n = len / 2 / (size_t)ch;
                if (raw && fread(raw, 1, len, f) == len) {
                    w->pcm = malloc(n * 4);
                    if (w->pcm) {
                        for (i = 0; i < n; i++) {
                            w->pcm[i * 2] = raw[i * (size_t)ch];
                            w->pcm[i * 2 + 1] = raw[i * (size_t)ch + (size_t)(ch - 1)];
                        }
                        w->frames = n;
                    }
                }
                free(raw);
                break;
            } else
                fseek(f, (long)(len + (len & 1)), SEEK_CUR);
        }
    }
    fclose(f);
}

void px_sys_sound(int id)
{
    static const char *N[3] = {"scroll.wav", "select.wav", "back.wav"};
    if (!g_snd_loaded) {
        int k;
        g_snd_loaded = 1;
        for (k = 0; k < 3; k++) {
            char p[300];
            const char *dir = getenv("PIXELADO_CORE_MENU_SOUNDS");
            snprintf(p, sizeof p, "%s/%s", dir && *dir ? dir : "/usr/share/banana/rnds/sounds", N[k]);
            load_wav(&g_snd[k], p);
        }
    }
    if (id >= 0 && id < 3 && g_snd[id].pcm) px_audio_sfx(g_snd[id].pcm, g_snd[id].frames, g_snd[id].rate);
}

/* ---- the core's own settings: one small file of `key=value` lines per core, kept across games ---- */
static void conf_path(char *p, size_t cap, int mk)
{
    const char *d = getenv("PIXELADO_CORE_CONFIG");
    char dir[200];
    snprintf(dir, sizeof dir, "%s", d && *d ? d : "/data/pixelado-cores");
    if (mk) {
        char *s;
        for (s = dir + 1; *s; s++)
            if (*s == '/') { *s = 0; mkdir(dir, 0755); *s = '/'; }
        mkdir(dir, 0755);
    }
    snprintf(p, cap, "%s/%s.conf", dir, PX_SYS);
}

int px_sys_conf_get(const char *key, char *out, size_t cap)
{
    char p[300], line[160];
    FILE *f;
    const size_t kl = strlen(key);
    int found = 0;
    conf_path(p, sizeof p, 0);
    f = fopen(p, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, key, kl) && line[kl] == '=') {
            size_t n;
            snprintf(out, cap, "%s", line + kl + 1);
            n = strlen(out);
            while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
            found = 1;
        }
    }
    fclose(f);
    return found;
}

void px_sys_conf_set(const char *key, const char *value)
{
    char p[300], tmp[310], line[160], keep[1024] = "";
    FILE *f;
    const size_t kl = strlen(key);
    conf_path(p, sizeof p, 1);
    f = fopen(p, "r");
    if (f) {
        while (fgets(line, sizeof line, f))
            if (!(!strncmp(line, key, kl) && line[kl] == '=') && strlen(keep) + strlen(line) < sizeof keep) strcat(keep, line);
        fclose(f);
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    f = fopen(tmp, "w");
    if (!f) return;
    fputs(keep, f);
    fprintf(f, "%s=%s\n", key, value);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    rename(tmp, p);
}
