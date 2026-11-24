/* The sound. On the handhelds (alsa): ALSA's "default" (BananaOS's plug to the codec at 48 kHz: it converts the
 * core's rate), non-blocking, ~64 ms of buffer; what doesn't fit is dropped and the frame pacing (main.c) keeps the
 * buffer near its middle. The device stays ours while the game is paused (Party's menu sounds play here).
 * Elsewhere (null): nothing, with the same accounting. */
#include "px.h"

#include <stdlib.h>
#include <string.h>

#ifdef PX_HAVE_ALSA
#include <alsa/asoundlib.h>
static snd_pcm_t *g_pcm;
#endif

static int g_rate = 48000;
static int g_paused;

int px_audio_rate(void) { return g_rate; }

int px_audio_open(const char *kind, int rate)
{
    g_rate = rate > 0 ? rate : 48000;
#ifdef PX_HAVE_ALSA
    if (!kind || !strcmp(kind, "alsa")) {
        const char *dev = getenv("PIXELADO_CORE_ALSA");
        int r = snd_pcm_open(&g_pcm, dev && *dev ? dev : "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
        if (r < 0) {
            px_log("[alsa] no sound device (%s): playing without sound", snd_strerror(r));
            g_pcm = NULL;
            return 1;
        }
        r = snd_pcm_set_params(g_pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED, 2, (unsigned)g_rate, 1, 64000);
        if (r < 0) {
            px_log("[alsa] %d Hz refused (%s): playing without sound", g_rate, snd_strerror(r));
            snd_pcm_close(g_pcm);
            g_pcm = NULL;
            return 1;
        }
        px_log("[alsa] %d Hz, stereo, 64 ms", g_rate);
        return 1;
    }
#endif
    (void)kind;
    px_log("[audio] none (%d Hz)", g_rate);
    return 1;
}

void px_audio_close(void)
{
#ifdef PX_HAVE_ALSA
    if (g_pcm) {
        snd_pcm_drop(g_pcm);
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
    }
#endif
}

void px_audio_write(const int16_t *s, size_t frames)
{
#ifdef PX_HAVE_ALSA
    int tries = 0;
    if (!g_pcm || g_paused) return;
    while (frames > 0 && tries++ < 4) {
        snd_pcm_sframes_t w = snd_pcm_writei(g_pcm, s, frames);
        if (w == -EAGAIN) return;          /* full: the rest dropped */
        if (w < 0) {
            if (snd_pcm_recover(g_pcm, (int)w, 1) < 0) return;
            continue;
        }
        s += (size_t)w * 2;
        frames -= (size_t)w;
    }
#else
    (void)s;
    (void)frames;
#endif
}

int px_audio_fill_ms(void)
{
#ifdef PX_HAVE_ALSA
    snd_pcm_sframes_t d = 0;
    if (!g_pcm || g_paused) return -1;
    if (snd_pcm_delay(g_pcm, &d) < 0) return 0;
    return d < 0 ? 0 : (int)(d * 1000 / g_rate);
#else
    return -1;
#endif
}

void px_audio_pause(int on)
{
#ifdef PX_HAVE_ALSA
    if (g_pcm && on && !g_paused) snd_pcm_drop(g_pcm);
    if (g_pcm && !on && g_paused) snd_pcm_prepare(g_pcm);
#endif
    g_paused = on;
}

void px_audio_sfx(const int16_t *s, size_t frames, int rate)
{
#ifdef PX_HAVE_ALSA
    int16_t *o;
    size_t n, i;
    if (!g_pcm || !s || !frames || rate <= 0) return;
    /* (nearest-sample to the device's rate: a menu blip) */
    n = (size_t)((double)frames * g_rate / rate);
    o = malloc(n * 4);
    if (!o) return;
    for (i = 0; i < n; i++) {
        const size_t k = (size_t)((double)i * rate / g_rate);
        o[i * 2] = s[(k < frames ? k : frames - 1) * 2];
        o[i * 2 + 1] = s[(k < frames ? k : frames - 1) * 2 + 1];
    }
    snd_pcm_drop(g_pcm);
    snd_pcm_prepare(g_pcm);
    {
        const int was = g_paused;
        g_paused = 0;
        px_audio_write(o, n);
        g_paused = was;
    }
    free(o);
#else
    (void)s;
    (void)frames;
    (void)rate;
#endif
}
