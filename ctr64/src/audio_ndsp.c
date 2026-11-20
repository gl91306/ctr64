#include "audio_ndsp.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define CHANNEL 0
#define BUF_COUNT 8
#define BUF_FRAMES 1024 /* ~23 ms at 44.1 kHz; 8 buffers give up to ~185 ms of queue */

static bool g_ok;
static unsigned g_rate;
static ndspWaveBuf g_wave[BUF_COUNT];
static int16_t *g_pcm;
static int g_fill_buf;
static size_t g_fill_frames;

bool audio_ndsp_init(unsigned sample_rate)
{
    float mix[12] = {1.0f, 1.0f};

    g_ok = false;
    if (R_FAILED(ndspInit()))
        return false;
    g_pcm = linearAlloc(BUF_COUNT * BUF_FRAMES * 4);
    if (!g_pcm) {
        ndspExit();
        return false;
    }
    memset(g_pcm, 0, BUF_COUNT * BUF_FRAMES * 4);
    g_rate = sample_rate;

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(CHANNEL);
    ndspChnSetInterp(CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(CHANNEL, (float)sample_rate);
    ndspChnSetFormat(CHANNEL, NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetMix(CHANNEL, mix);

    memset(g_wave, 0, sizeof(g_wave));
    for (int i = 0; i < BUF_COUNT; i++) {
        g_wave[i].data_vaddr = g_pcm + i * BUF_FRAMES * 2;
        g_wave[i].status = NDSP_WBUF_DONE;
    }
    g_fill_buf = 0;
    g_fill_frames = 0;
    g_ok = true;
    return true;
}

void audio_ndsp_exit(void)
{
    if (!g_ok)
        return;
    ndspChnWaveBufClear(CHANNEL);
    ndspExit();
    linearFree(g_pcm);
    g_ok = false;
}

size_t audio_ndsp_write(const int16_t *frames, size_t count)
{
    size_t total = count;

    if (!g_ok)
        return total;
    while (count) {
        ndspWaveBuf *wb = &g_wave[g_fill_buf];
        if (wb->status != NDSP_WBUF_DONE && wb->status != NDSP_WBUF_FREE)
            break; /* DSP is behind: drop the rest rather than block emulation */

        size_t n = BUF_FRAMES - g_fill_frames;
        if (n > count)
            n = count;
        memcpy((int16_t *)wb->data_vaddr + g_fill_frames * 2, frames, n * 4);
        g_fill_frames += n;
        frames += n * 2;
        count -= n;

        if (g_fill_frames == BUF_FRAMES) {
            wb->nsamples = BUF_FRAMES;
            DSP_FlushDataCache(wb->data_vaddr, BUF_FRAMES * 4);
            ndspChnWaveBufAdd(CHANNEL, wb);
            g_fill_buf = (g_fill_buf + 1) % BUF_COUNT;
            g_fill_frames = 0;
        }
    }
    return total;
}

/* Called by the core's audio backend with samples at the game's own rate (see the __3DS__
 * branch in audio_backend_libretro.c). The DSP resamples in hardware. */
/* development aid: sdmc:/3ds/ctr64/audiodump.txt records the first 10 s to audio.raw (s16 stereo) */
static void dump_audio(const int16_t *frames, size_t count)
{
    static int state; /* 0 unchecked, 1 dumping, 2 off */
    static FILE *f;
    static size_t written;
    if (state == 0) {
        FILE *t = fopen("sdmc:/3ds/ctr64/audiodump.txt", "r");
        state = t ? 1 : 2;
        if (t) {
            fclose(t);
            f = fopen("sdmc:/3ds/ctr64/audio.raw", "wb");
        }
    }
    if (state != 1 || !f)
        return;
    fwrite(frames, 4, count, f);
    written += count;
    if (written >= 10 * 32000) {
        fclose(f);
        f = NULL;
        state = 2;
    }
}

unsigned ctr64_audio_in_frames; /* stats.log "audio in" */
void ctr64_audio_push(const int16_t *frames, size_t count, unsigned rate)
{
    ctr64_audio_in_frames += count;
    dump_audio(frames, count);
    if (!g_ok)
        return;
    if (rate && rate != g_rate) {
        g_rate = rate;
        ndspChnSetRate(CHANNEL, (float)rate);
    }
    audio_ndsp_write(frames, count);
}

bool audio_ndsp_active(void) { return g_ok; }
unsigned audio_ndsp_rate(void) { return g_rate; }

unsigned audio_ndsp_queued_ms(void)
{
    unsigned queued = 0;

    if (!g_ok || !g_rate)
        return 0;
    for (int i = 0; i < BUF_COUNT; i++)
        if (g_wave[i].status == NDSP_WBUF_QUEUED || g_wave[i].status == NDSP_WBUF_PLAYING)
            queued += BUF_FRAMES;
    return queued * 1000 / g_rate;
}

void audio_ndsp_set_volume(float v)
{
    float mix[12] = {v, v};
    if (g_ok)
        ndspChnSetMix(CHANNEL, mix);
}

void audio_ndsp_pause(bool paused)
{
    if (g_ok)
        ndspChnSetPaused(CHANNEL, paused);
}
