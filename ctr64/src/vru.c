/* The N64 Voice Recognition Unit on the 3DS microphone: see vru.h.
 *
 * The microphone records into a ring in shared memory (MIC service, 16-bit at 8182 Hz) from the
 * moment the game switches the VRU's microphone on; nothing runs per frame. Holding the talk
 * button marks where the speech starts, letting go where it ends; that stretch is copied out
 * and matched against the game's words on a low-priority thread (core 2 on a New 3DS, which has
 * spare time in every frame), and the VRU reports "busy" (Z held on port 4) until it is done,
 * so the game waits as it does for the real unit. */
#include "vru.h"
#include "vru_rec.h"

#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void ctr64_log_line(const char *line);

#define MIC_BUF 0x30000           /* ~12 s at 8182 Hz, 16-bit */
#define MIC_RATE 8182
#define MAX_SAMPLES (MIC_RATE * 6) /* longest stretch matched */
#define MAX_WORDS 256

int ctr64_vru_port = -1; /* read by the core's input layer (emulate_game_controller_via_libretro.c) */

static struct {
    bool japanese;
    VruWord words[MAX_WORDS];
    int nwords, expected;
    bool mic_wanted, mic_on, mic_failed;
    u8 *ring;
    bool talking;
    u32 start; /* ring offset where the talk button went down */
    int16_t *pcm;
    int samples;
    volatile bool busy;  /* matching in progress */
    bool have_result;
    VruResult result;
    Thread thread;
    LightEvent go;
    volatile bool quit;
    FILE *log; /* sdmc:/3ds/ctr64/vru.log: words and results (vru.log.txt switch) */
} V;

static void vlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void vlog(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ctr64_log_line(line);
    if (V.log) {
        fprintf(V.log, "%s\n", line);
        fflush(V.log);
    }
}

extern int vru_rec_debug; /* vru_rec.c */
void vru_rec_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vru_rec_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (V.log) {
        fprintf(V.log, "%s\n", line);
        fflush(V.log);
    }
}

/* dev switch vru_test.raw (16-bit mono at 8182 Hz): matched instead of the microphone, for tests
 * without one (Azahar) */
static int test_audio(void)
{
    FILE *f = fopen("sdmc:/3ds/ctr64/vru_test.raw", "rb");
    if (!f)
        return -1;
    const int n = (int)fread(V.pcm, 2, MAX_SAMPLES, f);
    fclose(f);
    return n;
}

void vru_detect(const unsigned char *h)
{
    ctr64_vru_port = -1;
    const bool pikachu = h[0x3B] == 'N' && h[0x3C] == 'P' && h[0x3D] == 'G';
    const bool densha = h[0x3B] == 'N' && h[0x3C] == 'D' && h[0x3D] == '6';
    if (!pikachu && !densha)
        return;
    ctr64_vru_port = 3;
    V.japanese = h[0x3E] == 'J';
    FILE *f = fopen("sdmc:/3ds/ctr64/vrulog.txt", "r");
    if (f) {
        fclose(f);
        V.log = fopen("sdmc:/3ds/ctr64/vru.log", "w");
        vru_rec_debug = V.log != NULL;
    }
    vlog("vru: Voice Recognition Unit on port 4 (%s, %s words)", pikachu ? "Hey You, Pikachu!" : "Densha de GO! 64",
         V.japanese ? "kana" : "sound unit");
}

bool vru_active(void)
{
    return ctr64_vru_port >= 0;
}

/* ---- microphone ---- */

static void mic_start(void)
{
    if (V.mic_on || V.mic_failed)
        return;
    if (!V.ring)
        V.ring = memalign(0x1000, MIC_BUF);
    Result r = V.ring ? micInit(V.ring, MIC_BUF) : -1;
    if (R_SUCCEEDED(r)) {
        MICU_SetAllowShellClosed(false);
        r = MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_8180, 0, micGetSampleDataSize(), true);
        if (R_FAILED(r))
            micExit();
    }
    if (R_FAILED(r)) {
        V.mic_failed = true;
        vlog("vru: microphone not available (%08lX)", (unsigned long)r);
        return;
    }
    V.mic_on = true;
    vlog("vru: microphone on");
}

static void mic_stop(void)
{
    if (!V.mic_on)
        return;
    MICU_StopSampling();
    micExit();
    V.mic_on = false;
    vlog("vru: microphone off");
}

/* the samples between two ring offsets (bytes), the newest MAX_SAMPLES of them */
static int mic_copy(u32 from, u32 to)
{
    const u32 size = micGetSampleDataSize();
    u32 bytes = (to + size - from) % size;
    if (bytes > MAX_SAMPLES * 2) {
        from = (to + size - MAX_SAMPLES * 2) % size;
        bytes = MAX_SAMPLES * 2;
    }
    bytes &= ~1u;
    const u32 first = from + bytes <= size ? bytes : size - from;
    memcpy(V.pcm, V.ring + from, first);
    memcpy((u8 *)V.pcm + first, V.ring, bytes - first);
    return (int)(bytes / 2);
}

/* ---- matching thread ---- */

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        LightEvent_Wait(&V.go);
        if (V.quit)
            break;
        const u64 t0 = svcGetSystemTick();
        VruResult r;
        memset(&r, 0, sizeof(r));
        vru_recognize(V.pcm, V.samples, MIC_RATE, V.words, V.nwords, &r);
        V.result = r;
        V.have_result = true;
        __sync_synchronize();
        V.busy = false;
        vlog("vru: %d samples (%.2f s) matched in %.1f ms: flags %04X, %u results, first %u (deviance %04X)", V.samples,
             V.samples / (float)MIC_RATE, (svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000.0), r.error_flags,
             r.num_results, r.match[0], r.match[1]);
    }
}

static bool worker_start(void)
{
    if (V.thread)
        return true;
    if (!V.pcm)
        V.pcm = malloc(MAX_SAMPLES * 2);
    if (!V.pcm)
        return false;
    LightEvent_Init(&V.go, RESET_ONESHOT);
    bool new3ds = false;
    APT_CheckNew3DS(&new3ds);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* below everything of the game's (the video and audio threads share core 2) */
    V.thread = threadCreate(worker, NULL, 64 * 1024, prio + 3 > 0x3F ? 0x3F : prio + 3, new3ds ? 2 : 0, false);
    if (!V.thread)
        vlog("vru: matching thread not created");
    return V.thread != NULL;
}

/* ---- talk button (port 4 Z) ---- */

bool vru_talk(bool held)
{
    if (!vru_active())
        return false;
    if (held && !V.talking && (V.mic_on || V.mic_wanted) && !V.busy) {
        V.talking = true;
        V.start = V.mic_on ? micGetLastSampleOffset() : 0;
    } else if (!held && V.talking) {
        V.talking = false;
        if (worker_start()) {
            const int t = test_audio();
            if (t < 0 && !V.mic_on)
                return false;
            V.samples = t >= 0 ? t : mic_copy(V.start, micGetLastSampleOffset());
            V.busy = true;
            __sync_synchronize();
            LightEvent_Signal(&V.go);
        }
    }
    return V.talking || V.busy;
}

/* ---- the core's VRU calls (mupen64plus-core plugin/dummy_input.c) ---- */

void ctr64_vru_clear_words(uint8_t length)
{
    V.nwords = 0;
    V.expected = length;
    vlog("vru: word list cleared, %u words to come", length);
}

void ctr64_vru_send_word(uint16_t length, uint16_t *word, uint8_t lang)
{
    if (V.nwords >= MAX_WORDS)
        return;
    VruWord *w = &V.words[V.nwords++];
    w->japanese = lang != 0;
    w->n = length > VRU_MAX_UNITS ? VRU_MAX_UNITS : (uint8_t)length;
    memcpy(w->unit, word, w->n * sizeof(uint16_t));
    if (V.log) {
        char hex[VRU_MAX_UNITS * 5 + 1];
        int k = 0;
        for (int i = 0; i < w->n; i++)
            k += snprintf(hex + k, sizeof(hex) - k, "%04X ", w->unit[i]);
        hex[k] = 0;
        fprintf(V.log, "word %d (%u units): %s\n", V.nwords - 1, w->n, hex);
        fflush(V.log);
    }
    if (V.nwords == V.expected)
        vlog("vru: %d words loaded", V.nwords);
}

void ctr64_vru_set_mic(int state)
{
    V.mic_wanted = state != 0;
    if (V.mic_wanted)
        mic_start();
    else
        mic_stop();
}

void ctr64_vru_read_results(uint16_t *error_flags, uint16_t *num_results, uint16_t *mic_level, uint16_t *voice_level,
                            uint16_t *voice_length, uint16_t *matches)
{
    VruResult r;
    if (V.have_result && !V.busy) {
        r = V.result;
        V.have_result = false;
    } else { /* nothing said: "voice too low" */
        memset(&r, 0, sizeof(r));
        r.error_flags = 0x0400;
        for (int i = 0; i < 10; i += 2)
            r.match[i] = 0x7FFF;
    }
    *error_flags = r.error_flags;
    *num_results = r.num_results;
    *mic_level = r.mic_level;
    *voice_level = r.voice_level;
    *voice_length = r.voice_length;
    memcpy(matches, r.match, sizeof(r.match));
}

void vru_shutdown(void)
{
    if (V.thread) {
        V.quit = true;
        LightEvent_Signal(&V.go);
        threadJoin(V.thread, U64_MAX);
        threadFree(V.thread);
    }
    mic_stop();
    free(V.ring);
    free(V.pcm);
    if (V.log)
        fclose(V.log);
    memset(&V, 0, sizeof(V));
    ctr64_vru_port = -1;
}
