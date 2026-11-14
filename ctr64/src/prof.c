/* The profiler: see prof.h. Per-VI line ("v"), all times in microseconds:
 *   v vi wall run jit flush rspg rspa rspo wrdp wfull wdrain waud sleep rbidle
 *     wkdl wkoth wkidle fence vblank gpu frames presents behind
 * vi       the VI number (the main loop's frame counter)
 * wall     the VI's real time, from the start of its emulation to the start of the next
 * run      the CPU thread emulating it (core_host_run_frame and the remembered-blocks tick)
 * jit      ... of which compiling new code; flush: of which cache maintenance for it
 * rspg/rspa/rspo  ... of which RSP tasks: graphics (hand-off or, unthreaded, the display list
 *          itself), audio, other (these include the waits below that happen inside them)
 * wrdp/wfull/wdrain  ... of which waiting for the video thread: the RDP must be done, the queue
 *          is full, everything must be done
 * waud     ... of which waiting for the audio thread
 * sleep    the speed limiter sleeping (the VI was early); rbidle: precompiling known code then
 * wkdl/wkoth/wkidle  the video thread (core 2) in this VI: display lists, other calls (screen
 *          updates, presents), idle
 * fence    GPU waits (any thread, mostly the video thread); vblank: waits for the LCD's VBlank
 * gpu      the GPU busy (commands, transfers, fills) in this VI
 * frames   the game's new frames in this VI (VI origin changes); presents: frames shown
 * behind   the speed limiter's verdict: emulation is behind real time
 * ht/htmiss/link/dirty  recompiler dispatches in this VI (new_dynarec.c ctr64_disp): hash table
 *          lookups (get_addr_ht: indirect jumps without a mini_ht hit), their misses,
 *          dynamic_linker calls, verify_dirty calls (counts, not times) */
#include <3ds.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "prof.h"

int ctr64_profiler;
unsigned long long ctr64_prof_sleep_ticks, ctr64_prof_rbidle_ticks, ctr64_prof_audio_wait_ticks;

#define DIR_PATH "sdmc:/3ds/ctr64/profile"
#define BUF_BYTES (32 * 1024) /* ~10 KB/s, written every 0.25 s */

static FILE *g_file;
static FILE *g_stats_stream;
static LightLock g_lock;
static char *g_buf[2];
static unsigned g_len[2], g_cur;
static unsigned g_dropped;
static Thread g_thread;
static volatile bool g_quit;
static LightEvent g_wake;
static bool g_stats_only; /* prof_stats_begin: stats.log, no per-VI lines */

/* ---- the writer ---------------------------------------------------------------------------- */

static void append(const char *s, unsigned n)
{
    LightLock_Lock(&g_lock);
    if (g_len[g_cur] + n <= BUF_BYTES) {
        memcpy(g_buf[g_cur] + g_len[g_cur], s, n);
        g_len[g_cur] += n;
    } else {
        g_dropped += n;
    }
    LightLock_Unlock(&g_lock);
}

static void appendf(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0)
        append(line, n < (int)sizeof(line) ? (unsigned)n : sizeof(line) - 1);
}

static void write_out(void)
{
    LightLock_Lock(&g_lock);
    const unsigned b = g_cur, n = g_len[b];
    const unsigned dropped = g_dropped;
    g_cur ^= 1;
    g_len[g_cur] = 0;
    g_dropped = 0;
    LightLock_Unlock(&g_lock);
    if (n)
        fwrite(g_buf[b], 1, n, g_file);
    if (dropped)
        fprintf(g_file, "# profiler: %u bytes lost (the SD card was too slow)\n", dropped);
    fflush(g_file);
}

static void writer(void *arg)
{
    (void)arg;
    while (!g_quit) {
        LightEvent_WaitTimeout(&g_wake, 500000000LL);
        write_out();
    }
    write_out();
}

static int stats_write(void *cookie, const char *buf, size_t n)
{
    (void)cookie;
    if (n)
        append(buf, (unsigned)n);
    return (int)n;
}

/* ---- session ------------------------------------------------------------------------------- */

static void copy_file(const char *title, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    fprintf(g_file, "# --- %s ---\n", title);
    char line[300];
    while (fgets(line, sizeof(line), f)) {
        fputs("# ", g_file);
        fputs(line, g_file);
        if (!strchr(line, '\n'))
            fputc('\n', g_file);
    }
    fclose(f);
}

static bool alloc_bufs(void)
{
    g_buf[0] = malloc(BUF_BYTES);
    g_buf[1] = malloc(BUF_BYTES);
    if (!g_buf[0] || !g_buf[1]) {
        free(g_buf[0]);
        free(g_buf[1]);
        g_buf[0] = g_buf[1] = NULL;
        fclose(g_file);
        g_file = NULL;
        return false;
    }
    return true;
}

static void start_writer(void)
{
    LightLock_Init(&g_lock);
    LightEvent_Init(&g_wake, RESET_ONESHOT);
    g_len[0] = g_len[1] = 0;
    g_cur = 0;
    g_quit = false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* the lowest priority; core 1 (the compile thread's) */
    g_thread = threadCreate(writer, NULL, 16 * 1024, 0x3F, 1, false);
    if (!g_thread)
        g_thread = threadCreate(writer, NULL, 16 * 1024, 0x3F, -2, false);
}

/* stats.log through the same low-priority writer thread (no SD writes on the emulation
 * thread when the profiler is off); NULL if the file could not be made */
FILE *prof_stats_begin(const char *path)
{
    if (g_file)
        return NULL;
    g_file = fopen(path, "w");
    if (!g_file)
        return NULL;
    if (!alloc_bufs())
        return NULL;
    g_stats_only = true;
    start_writer();
    g_stats_stream = fwopen(NULL, stats_write);
    return g_stats_stream;
}

bool prof_begin(const char *rom_path)
{
    if (!ctr64_profiler || g_file)
        return g_file != NULL;
    mkdir(DIR_PATH, 0777);
    /* "<ROM file name without extension> <date> <time>.prof" */
    const char *base = strrchr(rom_path, '/');
    base = base ? base + 1 : rom_path;
    char name[96];
    snprintf(name, sizeof(name), "%s", base);
    char *dot = strrchr(name, '.');
    if (dot)
        *dot = 0;
    for (char *c = name; *c; c++)
        if (strchr(":*?\"<>|", *c))
            *c = '_';
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now); /* the 3DS clock is local time, kept as UTC by libctru */
    char path[256];
    snprintf(path, sizeof(path), DIR_PATH "/%s %04d-%02d-%02d %02d.%02d.%02d.prof", name, tm->tm_year + 1900,
             tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    g_file = fopen(path, "w");
    if (!g_file)
        return false;
    if (!alloc_bufs())
        return false;
    bool new3ds = false;
    APT_CheckNew3DS(&new3ds);
    fprintf(g_file, "# ctr64 profile 1\n# rom %s\n# console %s, built %s %s, profiler level %d\n", rom_path,
            new3ds ? "New 3DS" : "Old 3DS", __DATE__, __TIME__, ctr64_profiler);
    fprintf(g_file, "# columns v vi wall run jit flush rspg rspa rspo wrdp wfull wdrain waud sleep rbidle "
                    "wkdl wkoth wkidle fence vblank gpu frames presents behind ht htmiss link dirty (times in us)\n");
    copy_file("settings.txt", "sdmc:/3ds/ctr64/settings.txt");
    copy_file("core.log", "sdmc:/3ds/ctr64/core.log");
    fflush(g_file);

    start_writer();
    g_stats_stream = fwopen(NULL, stats_write);
    return true;
}

bool prof_active(void)
{
    return g_file != NULL && !g_stats_only;
}

FILE *prof_stats_stream(void)
{
    return g_stats_stream;
}

void prof_end(void)
{
    if (!g_file)
        return;
    if (g_stats_stream) {
        fflush(g_stats_stream);
        fclose(g_stats_stream);
        g_stats_stream = NULL;
    }
    g_quit = true;
    if (g_thread) {
        LightEvent_Signal(&g_wake);
        threadJoin(g_thread, U64_MAX);
        threadFree(g_thread);
        g_thread = NULL;
    } else {
        write_out();
    }
    if (!g_stats_only)
        fprintf(g_file, "# end\n");
    g_stats_only = false;
    fclose(g_file);
    g_file = NULL;
    free(g_buf[0]);
    free(g_buf[1]);
    g_buf[0] = g_buf[1] = NULL;
}

/* ---- per VI -------------------------------------------------------------------------------- */

/* the counters, all running totals in ticks (some are reset once a second by main.c, after
 * prof_vi_end and before the next prof_vi_begin) */
extern unsigned long long ctr_jit_compile_ticks, ctr_jit_flush_ticks;
extern unsigned long long ctr64_sp_ticks[3];
extern unsigned ctr64_gfx_wait_why[3];
extern unsigned ctr64_gfx_worker_ticks[3];
extern unsigned ctr64_vi_frames;
extern unsigned long long pica_prof_ticks[2]; /* pica.c: fence waits, vblank waits */
unsigned long long pica_prof_gpu_idle(void);
unsigned pica_prof_presents(void);

enum { C_JIT, C_FLUSH, C_RSPG, C_RSPA, C_RSPO, C_WRDP, C_WFULL, C_WDRAIN, C_WAUD, C_SLEEP, C_RBIDLE,
       C_WKDL, C_WKOTH, C_WKIDLE, C_FENCE, C_VBLANK, C_GPUIDLE, C_FRAMES, C_PRESENTS, C_COUNT };

static unsigned long long g_snap[C_COUNT];
static unsigned long long g_t0;

static void snapshot(unsigned long long *s)
{
    s[C_JIT] = ctr_jit_compile_ticks;
    s[C_FLUSH] = ctr_jit_flush_ticks;
    s[C_RSPG] = ctr64_sp_ticks[0];
    s[C_RSPA] = ctr64_sp_ticks[1];
    s[C_RSPO] = ctr64_sp_ticks[2];
    s[C_WRDP] = ctr64_gfx_wait_why[0];
    s[C_WFULL] = ctr64_gfx_wait_why[1];
    s[C_WDRAIN] = ctr64_gfx_wait_why[2];
    s[C_WAUD] = ctr64_prof_audio_wait_ticks;
    s[C_SLEEP] = ctr64_prof_sleep_ticks;
    s[C_RBIDLE] = ctr64_prof_rbidle_ticks;
    s[C_WKDL] = ctr64_gfx_worker_ticks[1];
    s[C_WKOTH] = ctr64_gfx_worker_ticks[2];
    s[C_WKIDLE] = ctr64_gfx_worker_ticks[0];
    s[C_FENCE] = pica_prof_ticks[0];
    s[C_VBLANK] = pica_prof_ticks[1];
    s[C_GPUIDLE] = pica_prof_gpu_idle();
    s[C_FRAMES] = ctr64_vi_frames;
    s[C_PRESENTS] = pica_prof_presents();
}

static unsigned long long g_run_ticks;
static unsigned g_next_vi;

void prof_mark(const char *fmt, ...)
{
    if (!g_file || g_stats_only)
        return;
    char text[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    appendf("m %u %s\n", g_next_vi, text);
}

/* the counters at the start of the VI; its wall time runs from the end of the previous one (so
 * the time between VIs, e.g. writing the per-second statistics, is in it) */
void prof_vi_begin(void)
{
    if (!g_file || g_stats_only)
        return;
    snapshot(g_snap);
    if (!g_t0)
        g_t0 = svcGetSystemTick();
}

/* after a pause (the menu): the next VI starts now */
void prof_vi_resync(void)
{
    g_t0 = svcGetSystemTick();
}

static unsigned us(unsigned long long ticks)
{
    return (unsigned)(ticks * 1000000ULL / SYSCLOCK_ARM11);
}

void prof_vi_end(unsigned vi, bool behind)
{
    if (!g_file || g_stats_only)
        return;
    const unsigned long long now = svcGetSystemTick();
    unsigned long long s[C_COUNT];
    snapshot(s);
    unsigned long long d[C_COUNT];
    for (int i = 0; i < C_COUNT; i++)
        d[i] = s[i] >= g_snap[i] ? s[i] - g_snap[i] : 0; /* a counter reset in between: 0 */
    const unsigned long long wall = now - g_t0;
    g_t0 = now;
    g_next_vi = vi + 1;
    const unsigned long long gpu = wall > d[C_GPUIDLE] ? wall - d[C_GPUIDLE] : 0;
    extern unsigned int ctr64_disp[8]; /* new_dynarec.c */
    static unsigned int disp0[8];
    unsigned int dd[8];
    for (int i = 0; i < 8; i++) {
        dd[i] = ctr64_disp[i] - disp0[i];
        disp0[i] = ctr64_disp[i];
    }
    appendf("v %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %d %u %u %u %u\n", vi, us(wall),
            us(g_run_ticks), us(d[C_JIT]), us(d[C_FLUSH]), us(d[C_RSPG]), us(d[C_RSPA]), us(d[C_RSPO]),
            us(d[C_WRDP]), us(d[C_WFULL]), us(d[C_WDRAIN]), us(d[C_WAUD]), us(d[C_SLEEP]), us(d[C_RBIDLE]),
            us(d[C_WKDL]), us(d[C_WKOTH]), us(d[C_WKIDLE]), us(d[C_FENCE]), us(d[C_VBLANK]), us(gpu),
            (unsigned)d[C_FRAMES], (unsigned)d[C_PRESENTS], behind ? 1 : 0, dd[0], dd[1], dd[2], dd[4]);
    {
        /* where the video thread's time went in a slow VI: "s <vi> <scope> <us> ..." (scopes over
         * 0.5 ms; nested scopes overlap: texrect > texupd > texload > texconv ...) */
        extern unsigned long long ctr64_prof_total[40];
        extern const char *const ctr64_prof_names[40];
        static unsigned long long scope0[40];
        if (wall > SYSCLOCK_ARM11 / 40) { /* > 25 ms */
            char line[512];
            int n = snprintf(line, sizeof(line), "s %u", vi);
            for (int i = 0; i < 40 && n < (int)sizeof(line) - 24; i++) {
                const unsigned long long t = ctr64_prof_total[i] - scope0[i];
                if (t > SYSCLOCK_ARM11 / 2000)
                    n += snprintf(line + n, sizeof(line) - n, " %s %u", ctr64_prof_names[i], us(t));
            }
            if (n > 12)
                appendf("%s\n", line);
        }
        memcpy(scope0, ctr64_prof_total, sizeof(scope0));
    }
    {
        /* a slow VI spent in N64 code (not compiling): "c <vi> <events> <pc> ..." = the PCs of the
         * last 16 events (new_dynarec.c), enough to name a spin loop like SSB's frame wait */
        extern unsigned int ctr64_pcring[16], ctr64_pcring_n;
        static unsigned int ev0;
        const unsigned int ev = ctr64_pcring_n;
        const unsigned long long other = d[C_JIT] + d[C_WRDP] + d[C_WFULL] + d[C_WDRAIN] + d[C_WAUD];
        if (wall > SYSCLOCK_ARM11 / 40 && g_run_ticks > other + SYSCLOCK_ARM11 / 50) {
            char line[256];
            int n = snprintf(line, sizeof(line), "c %u %u", vi, ev - ev0);
            const unsigned int k = ev - ev0 < 16 ? ev - ev0 : 16;
            for (unsigned int i = 0; i < k; i++)
                n += snprintf(line + n, sizeof(line) - n, " %08x", ctr64_pcring[(ev - k + i) & 15]);
            appendf("%s\n", line);
        }
        ev0 = ev;
    }
    if (vi % 15 == 14)
        LightEvent_Signal(&g_wake);
}

/* main.c: the CPU thread's emulation time of this VI */
void prof_vi_run(unsigned long long ticks)
{
    g_run_ticks = ticks;
}
