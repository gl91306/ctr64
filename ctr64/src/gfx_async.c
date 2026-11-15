/* The RDP on a second CPU core (New 3DS core 2).
 *
 * On the N64 the RDP renders frame N while the CPU runs the game logic of frame N+1. Here the
 * CPU emulation runs on the app core and GLideN64 on core 2: the core's calls into the graphics
 * plugin (display lists, VI updates) are queued to a worker thread instead of running inline.
 *
 *  - GLideN64 sees the N64 registers and RSP memory through a shadow copy (its GFX_INFO
 *    pointers are redirected here), refreshed from a snapshot taken when each call was queued,
 *    so the CPU may change the real ones meanwhile. RDRAM is shared, as on the N64 (games double
 *    buffer what the RSP and RDP read).
 *  - As on the N64, the RSP part of a display list task is done right away (audio tasks are not
 *    held up), while the RDP's "done" interrupt comes later in emulated time: the core checks
 *    from an event (rdp_core.c, ctr64_gfx_dlist_ready) and, when it is time, waits for the
 *    worker and applies what GLideN64 did to the registers (the RDP interrupt, DPC and SP
 *    status bits). Until then the CPU emulation runs on in parallel.
 *  - Every other plugin call waits for the queue to drain and runs inline, as do all calls
 *    when the option is off or core 2 is not available (Old 3DS).
 *
 * Save states, the HOME menu and shutdown drain the queue first (ctr64_gfx_drain). */
int ctr64_core1_ready(void); /* platform/ctr_jit.c */
#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "api/m64p_plugin.h"
#include "device/device.h"
#include "device/r4300/interrupt.h"
#include "main/main.h"
#include "plugin/plugin.h"
#include "settings.h"

#define MI_INTR_DP 0x20

enum { R_MI, R_SP, R_DPC0, R_VI0 = R_DPC0 + 8, R_COUNT = R_VI0 + 14 };

typedef struct {
    unsigned int r[R_COUNT];
} Regs;

typedef enum { C_DLIST, C_SCREEN, C_VISTATUS, C_VIWIDTH, C_SHOWCFB, C_QUIT } Kind;

typedef struct {
    Kind kind;
    Regs in;
    u8 dmem[0x1000], imem[0x1000]; /* display lists only */
} Cmd;

typedef struct {
    Regs in, out;
    u64 start; /* system tick when queued */
} DlResult;

#define QUEUE 6
#define RESULTS 8

static unsigned int *g_real[R_COUNT]; /* the core's registers */
static Regs g_shadow;                 /* what GLideN64's pointers point at */
static u8 g_shadow_dmem[0x1000] __attribute__((aligned(8)));
static u8 g_shadow_imem[0x1000] __attribute__((aligned(8)));
static unsigned char *g_real_dmem, *g_real_imem;
static gfx_plugin_functions g_orig;

static Cmd g_queue[QUEUE];
static DlResult g_results[RESULTS];
static unsigned g_put, g_got;               /* commands queued / completed */
static unsigned g_dlists_put, g_dlists_got; /* display lists queued / completed */
static unsigned g_dlists_taken;             /* display list results consumed by the core */
static LightLock g_lock;
/* Waits re-check their condition this often: a wakeup between cores was seen to get lost
 * (Azahar, SM64 with the audio thread: the CPU slept on with the task long done) */
#define WAKE_RECHECK_NS 1000000LL
static CondVar g_changed;
static Thread g_thread;
static bool g_running;

bool ctr64_gfx_dlist_async;    /* the last display list went to the worker (read by rsp_core.c) */
unsigned ctr64_gfx_wait_ticks; /* CPU-side time spent waiting for the worker (stats) */
unsigned ctr64_gfx_wait_why[3];  /* of which: [0] a display list's RDP done, [1] queue full, [2] drains */
static int g_wait_why = 2;
unsigned ctr64_vi_count;       /* VIs queued, counted on the CPU thread (input scripts) */

/* ---- register snapshots ----------------------------------------------------------------------- */

static void snapshot(Regs *s)
{
    for (int i = 0; i < R_COUNT; i++)
        s->r[i] = g_real[i] ? *g_real[i] : 0;
}

/* GLideN64 only sets MI bits; other registers take the bits it changed */
static void apply_changes(const Regs *in, const Regs *out)
{
    if (g_real[R_MI])
        *g_real[R_MI] |= out->r[R_MI] & ~in->r[R_MI];
    for (int i = R_SP; i < R_COUNT; i++)
        if (g_real[i]) {
            const unsigned int changed = in->r[i] ^ out->r[i];
            *g_real[i] = (*g_real[i] & ~changed) | (out->r[i] & changed);
        }
}

/* ---- the worker --------------------------------------------------------------------------------- */

static void run(Kind kind)
{
    switch (kind) {
    case C_DLIST: g_orig.processDList(); break;
    case C_SCREEN: g_orig.updateScreen(); break;
    case C_VISTATUS: g_orig.viStatusChanged(); break;
    case C_VIWIDTH: g_orig.viWidthChanged(); break;
    case C_SHOWCFB: g_orig.showCFB(); break;
    case C_QUIT: break;
    }
}

unsigned ctr64_gfx_worker_ticks[3]; /* worker time: [0] idle, [1] display lists, [2] other calls */

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        LightLock_Lock(&g_lock);
        const u64 t_idle = svcGetSystemTick();
        while (g_got == g_put)
            CondVar_WaitTimeout(&g_changed, &g_lock, WAKE_RECHECK_NS);
        const u64 t_run = svcGetSystemTick();
        ctr64_gfx_worker_ticks[0] += (unsigned)(t_run - t_idle);
        Cmd *c = &g_queue[g_got % QUEUE];
        LightLock_Unlock(&g_lock);
        if (c->kind == C_QUIT)
            break;

        g_shadow = c->in;
        DlResult *res = NULL;
        if (c->kind == C_DLIST) {
            memcpy(g_shadow_dmem, c->dmem, sizeof(g_shadow_dmem));
            memcpy(g_shadow_imem, c->imem, sizeof(g_shadow_imem));
            g_shadow.r[R_MI] &= ~MI_INTR_DP;
            res = &g_results[g_dlists_got % RESULTS];
            res->in = g_shadow;
        }
        run(c->kind);
        ctr64_gfx_worker_ticks[c->kind == C_DLIST ? 1 : 2] += (unsigned)(svcGetSystemTick() - t_run);
        if (res)
            res->out = g_shadow;

        LightLock_Lock(&g_lock);
        if (res)
            g_dlists_got++;
        g_got++;
        CondVar_Broadcast(&g_changed);
        LightLock_Unlock(&g_lock);
    }
}

static void probe_main(void *arg)
{
    (void)arg;
    svcExitThread();
}

static bool start_worker(void)
{
    bool new3ds = false;
    APT_CheckNew3DS(&new3ds);
    /* New 3DS: core 2, the app's own. Old 3DS: core 1, shared with the system, which the app
     * may use for up to its CPU time limit (raised here to 80%; the system keeps the rest). */
    int core = 2;
    if (!new3ds) {
        if (!ctr64_core1_ready())
            return false;
        core = 1;
    }
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    LightLock_Init(&g_lock);
    CondVar_Init(&g_changed);
    g_put = g_got = g_dlists_put = g_dlists_got = g_dlists_taken = 0;
    g_thread = threadCreate(worker, NULL, 256 * 1024, prio - 1, core, false);
    g_running = g_thread != NULL;
    {   /* core.log: whether the video thread runs (Smash Remix on a New 3DS once ran without it,
         * with nothing to say why) */
        extern void ctr64_log_line(const char *line);
        struct mallinfo mi = mallinfo();
        char line[160];
        snprintf(line, sizeof(line), "video thread: %s on core %d at VI %u (priority %ld, heap free %u KB)",
                 g_running ? "started" : "COULD NOT START", core, ctr64_vi_count, (long)(prio - 1),
                 (unsigned)(mi.fordblks / 1024));
        ctr64_log_line(line);
        if (!g_running) {
            /* why: the kernel's answer for a small thread on that core, and the system's CPU time
             * limit for core 1 (a New 3DS app gets its extra cores from the system) */
            static u8 probe_stack[4096] __attribute__((aligned(8)));
            Handle h = 0;
            const Result rc = svcCreateThread(&h, probe_main, 0, (u32 *)(probe_stack + sizeof(probe_stack)),
                                              prio - 1, core);
            if (R_SUCCEEDED(rc)) {
                svcWaitSynchronization(h, U64_MAX);
                svcCloseHandle(h);
            }
            u32 limit = 0;
            const Result rl = APT_GetAppCpuTimeLimit(&limit);
            s64 cores = 0;
            svcGetSystemInfo(&cores, 26, 0); /* number of cores the kernel runs */
            snprintf(line, sizeof(line), "video thread: probe thread on core %d -> %08lX; core 1 limit %lu%% (%08lX); "
                     "cores %lld", core, (unsigned long)rc, (unsigned long)limit, (unsigned long)rl, (long long)cores);
            ctr64_log_line(line);
        }
    }
    return g_running;
}

static bool threaded(void)
{
    /* a start that fails is tried again a second later, a few times (until then the graphics
     * run on the CPU thread, as with the setting off) */
    static int tries;
    static unsigned next_vi;
    if (!g_settings.threaded_video)
        return false;
    if (!g_running && tries < 5 && (int)(ctr64_vi_count - next_vi) >= 0) {
        tries++;
        next_vi = ctr64_vi_count + 60;
        start_worker();
    }
    return g_running;
}

volatile int ctr64_cpu_waiting; /* stall report: 10+why in wait_for, 1 in the audio wait */
void ctr64_gfx_debug(FILE *f)
{
    fprintf(f, "  gfx put %u got %u dlists put %u got %u taken %u | cpu waiting %d\n", g_put, g_got,
            g_dlists_put, g_dlists_got, g_dlists_taken, ctr64_cpu_waiting);
}

/* waits until `done` (a completion counter) reaches `target` */
static void wait_for(volatile unsigned *done, unsigned target)
{
    if ((int)(*done - target) >= 0)
        return;
    const u64 t0 = svcGetSystemTick();
    ctr64_cpu_waiting = 10 + g_wait_why;
    LightLock_Lock(&g_lock);
    while ((int)(*done - target) < 0)
        CondVar_WaitTimeout(&g_changed, &g_lock, WAKE_RECHECK_NS);
    LightLock_Unlock(&g_lock);
    ctr64_cpu_waiting = 0;
    const unsigned dt = (unsigned)(svcGetSystemTick() - t0);
    ctr64_gfx_wait_ticks += dt;
    ctr64_gfx_wait_why[g_wait_why] += dt;
    g_wait_why = 2;
}

void ctr64_gfx_drain(void)
{
    if (g_running)
        wait_for(&g_got, g_put);
}

static void enqueue(Kind kind)
{
    /* wait for a free slot (and for a free display list result) */
    g_wait_why = 1;
    wait_for(&g_got, g_put - (QUEUE - 1));
    if (kind == C_DLIST) {
        g_wait_why = 1;
        wait_for(&g_dlists_got, g_dlists_put - (RESULTS - 1));
    }
    Cmd *c = &g_queue[g_put % QUEUE];
    c->kind = kind;
    snapshot(&c->in);
    if (kind == C_DLIST) {
        memcpy(c->dmem, g_real_dmem, sizeof(c->dmem));
        memcpy(c->imem, g_real_imem, sizeof(c->imem));
        g_results[g_dlists_put % RESULTS].start = svcGetSystemTick();
        g_dlists_put++;
    }
    LightLock_Lock(&g_lock);
    g_put++;
    CondVar_Broadcast(&g_changed);
    LightLock_Unlock(&g_lock);
}

/* inline call with the shadow registers (option off, or a call that must not be deferred) */
static void run_inline(Kind kind, void (*fn)(void))
{
    ctr64_gfx_drain();
    Regs in;
    snapshot(&in);
    g_shadow = in;
    memcpy(g_shadow_dmem, g_real_dmem, sizeof(g_shadow_dmem));
    memcpy(g_shadow_imem, g_real_imem, sizeof(g_shadow_imem));
    if (fn)
        fn();
    else
        run(kind);
    apply_changes(&in, &g_shadow);
    /* RSP memory written by the plugin (some microcodes) */
    memcpy(g_real_dmem, g_shadow_dmem, sizeof(g_shadow_dmem));
}

/* ---- the core's side ---------------------------------------------------------------------------- */

/* the oldest queued display list: waits for it and applies its register changes; returns whether
 * it raised the RDP interrupt */
bool ctr64_gfx_dlist_finish(void)
{
    if (g_dlists_taken == g_dlists_put)
        return false;
    g_wait_why = 0;
    wait_for(&g_dlists_got, g_dlists_taken + 1);
    const DlResult *res = &g_results[g_dlists_taken % RESULTS];
    g_dlists_taken++;
    apply_changes(&res->in, &res->out);
    const bool dp = (res->out.r[R_MI] & MI_INTR_DP) != 0;
    if (dp && g_real[R_MI])
        *g_real[R_MI] &= ~MI_INTR_DP; /* the core raises it as an event (rdp_core.c) */
    return dp;
}

/* When the RDP of a threaded display list is done in emulated time. The core checks every 1/16
 * VI (ctr64_gfx_dlist_step) from the start of the task:
 *  - the worker is done: complete;
 *  - below full speed (the speed limiter's verdict): emulate on, the more overlap the better;
 *  - else, while emulation is behind real time since the start (the CPU is busy, really
 *    overlapping the worker): emulate on; once it is ahead (the game idles, waiting for the
 *    frame, so emulated time races): wait for the worker, so emulated time does not outrun the
 *    real rendering time and the game paces its frames as with a real RDP;
 *  - after 2 VIs: wait in any case.
 *  - but a game at ctr64_gfx_defer_fps or less (OoT, MM: 3 VIs per frame; MK64: 2) emulates on to the VI's end even
 *    then: the speed limiter's wait at the VI end overlaps the worker. Waiting mid-VI made OoT's
 *    VIs 33 + 2 + 2 + 13 ms instead of 16.7 each, frames shown 33-58 ms apart instead of 50
 *    (hardware, 2026-11-10); a frame needs the RDP done only ~3 VIs later. Games at 30-60 fps
 *    keep the wait (a later "done" could cost them a frame). Dev switch nodefer.txt: off.
 * <data dir>/gfxdelay.txt (quarter VIs, ctr64_gfx_delay_q4) fixes the delay instead. */
unsigned ctr64_gfx_delay_q4; /* 0 = adaptive */
bool ctr64_gfx_nodefer;      /* dev switch nodefer.txt */
static bool g_slow_game;     /* <= ctr64_gfx_defer_fps game frames/s over the last 30 VIs (w_update_screen) */
/* 30: also 30 fps games (MK64's races, HW test 2: the CPU waited 13.4 ms per frame mid-VI for the RDP
 * while the worker idled 12.6 ms; 23% of frames shown 10-20 or 45-55 ms apart instead of 33).
 * Dev switch deferfps.txt (a number) overrides, e.g. 24 = the rule of HW test 2. */
unsigned ctr64_gfx_defer_fps = 30;

static unsigned vi_count_cycles(void)
{
    return g_dev.vi.delay ? g_dev.vi.delay : 780000;
}

unsigned ctr64_gfx_dlist_step(void)
{
    return ctr64_gfx_delay_q4 ? vi_count_cycles() * ctr64_gfx_delay_q4 / 4 : vi_count_cycles() / 16;
}

/* `elapsed`: emulated count cycles since the newest pending display list started */
bool ctr64_gfx_dlist_ready(unsigned elapsed)
{
    if (ctr64_gfx_delay_q4 || g_dlists_taken == g_dlists_put || g_dlists_got != g_dlists_taken)
        return true;
    const unsigned vi = vi_count_cycles();
    if (elapsed >= 2 * vi)
        return true;
    extern volatile bool ctr64_speed_behind;
    if (ctr64_speed_behind)
        return false;
    /* real time since the start, in count cycles at the N64's rate (VIs at 60 Hz) */
    const u64 real = (svcGetSystemTick() - g_results[g_dlists_taken % RESULTS].start) * (u64)vi * 60 / SYSCLOCK_ARM11;
    if (elapsed < real)
        return false;
    if (g_slow_game && !ctr64_gfx_nodefer) {
        extern uint32_t ctr64_gfx_start_count; /* rsp_core.c */
        const unsigned int *vi_at = get_event(&g_dev.r4300.cp0.q, VI_INT);
        if (vi_at && (int32_t)(*vi_at - (ctr64_gfx_start_count + elapsed)) > 0)
            return false; /* on to the VI's end, where the speed limiter waits anyway */
    }
    return true;
}

static void w_process_dlist(void)
{
    ctr64_gfx_dlist_async = threaded();
    if (ctr64_gfx_dlist_async)
        enqueue(C_DLIST);
    else
        run_inline(C_DLIST, NULL);
}

unsigned ctr64_vi_frames; /* VIs whose origin changed: the game's frames (stats.log) */
static void w_update_screen(void)
{
    static uint32_t last_origin;
    ctr64_vi_count++;
    if (g_dev.vi.regs[VI_ORIGIN_REG] != last_origin) {
        last_origin = g_dev.vi.regs[VI_ORIGIN_REG];
        ctr64_vi_frames++;
    }
    {
        static unsigned win_vi, win_frames;
        if (ctr64_vi_count - win_vi >= 30) {
            g_slow_game = (ctr64_vi_frames - win_frames) * 60 <= (ctr64_vi_count - win_vi) * ctr64_gfx_defer_fps;
            win_vi = ctr64_vi_count;
            win_frames = ctr64_vi_frames;
        }
    }
    if (threaded())
        enqueue(C_SCREEN);
    else
        run_inline(C_SCREEN, NULL);
}

static void w_vi_status(void)
{
    if (threaded())
        enqueue(C_VISTATUS);
    else
        run_inline(C_VISTATUS, NULL);
}

static void w_vi_width(void)
{
    if (threaded())
        enqueue(C_VIWIDTH);
    else
        run_inline(C_VIWIDTH, NULL);
}

static void w_show_cfb(void)
{
    if (threaded())
        enqueue(C_SHOWCFB);
    else
        run_inline(C_SHOWCFB, NULL);
}

/* calls that always run inline, after the queue */
static void w_process_rdp_list(void) { run_inline(C_QUIT, g_orig.processRDPList); }
static void w_change_window(void) { ctr64_gfx_drain(); g_orig.changeWindow(); }
static void w_rom_closed(void) { ctr64_gfx_drain(); g_orig.romClosed(); }
static int w_rom_open(void) { ctr64_gfx_drain(); return g_orig.romOpen(); }
static void w_move_screen(int x, int y) { ctr64_gfx_drain(); g_orig.moveScreen(x, y); }
static void w_read_screen(void *d, int *w, int *h, int f) { ctr64_gfx_drain(); g_orig.readScreen(d, w, h, f); }
static void w_resize(int w, int h) { ctr64_gfx_drain(); g_orig.resizeVideoOutput(w, h); }
static void w_fb_read(unsigned int a) { ctr64_gfx_drain(); g_orig.fBRead(a); }
static void w_fb_write(unsigned int a, unsigned int s) { ctr64_gfx_drain(); g_orig.fBWrite(a, s); }
static void w_fb_info(void *p) { ctr64_gfx_drain(); g_orig.fBGetFrameBufferInfo(p); }

/* plugin.c (plugin_start_gfx): redirects GLideN64's register pointers to the shadow and wraps
 * the plugin calls */
void ctr64_gfx_async_install(gfx_plugin_functions *gfx, GFX_INFO *info)
{
    unsigned int **const ptrs[R_COUNT] = {
        &info->MI_INTR_REG,      &info->SP_STATUS_REG,   &info->DPC_START_REG,   &info->DPC_END_REG,
        &info->DPC_CURRENT_REG,  &info->DPC_STATUS_REG,  &info->DPC_CLOCK_REG,   &info->DPC_BUFBUSY_REG,
        &info->DPC_PIPEBUSY_REG, &info->DPC_TMEM_REG,    &info->VI_STATUS_REG,   &info->VI_ORIGIN_REG,
        &info->VI_WIDTH_REG,     &info->VI_INTR_REG,     &info->VI_V_CURRENT_LINE_REG, &info->VI_TIMING_REG,
        &info->VI_V_SYNC_REG,    &info->VI_H_SYNC_REG,   &info->VI_LEAP_REG,     &info->VI_H_START_REG,
        &info->VI_V_START_REG,   &info->VI_V_BURST_REG,  &info->VI_X_SCALE_REG,  &info->VI_Y_SCALE_REG,
    };
    _Static_assert(sizeof(ptrs) / sizeof(ptrs[0]) == R_COUNT, "register list");
    ctr64_gfx_drain();
    for (int i = 0; i < R_COUNT; i++) {
        g_real[i] = *ptrs[i];
        *ptrs[i] = &g_shadow.r[i];
    }
    g_real_dmem = info->DMEM;
    g_real_imem = info->IMEM;
    info->DMEM = g_shadow_dmem;
    info->IMEM = g_shadow_imem;

    if (gfx->processDList != w_process_dlist)
        g_orig = *gfx;
    gfx->processDList = w_process_dlist;
    gfx->updateScreen = w_update_screen;
    gfx->viStatusChanged = w_vi_status;
    gfx->viWidthChanged = w_vi_width;
    gfx->showCFB = w_show_cfb;
    if (g_orig.processRDPList) gfx->processRDPList = w_process_rdp_list;
    if (g_orig.changeWindow) gfx->changeWindow = w_change_window;
    if (g_orig.romClosed) gfx->romClosed = w_rom_closed;
    if (g_orig.romOpen) gfx->romOpen = w_rom_open;
    if (g_orig.moveScreen) gfx->moveScreen = w_move_screen;
    if (g_orig.readScreen) gfx->readScreen = w_read_screen;
    if (g_orig.resizeVideoOutput) gfx->resizeVideoOutput = w_resize;
    if (g_orig.fBRead) gfx->fBRead = w_fb_read;
    if (g_orig.fBWrite) gfx->fBWrite = w_fb_write;
    if (g_orig.fBGetFrameBufferInfo) gfx->fBGetFrameBufferInfo = w_fb_info;
}

/* frontend shutdown: stops the worker */
void ctr64_gfx_async_stop(void)
{
    if (!g_running)
        return;
    ctr64_gfx_drain();
    enqueue(C_QUIT);
    threadJoin(g_thread, U64_MAX);
    threadFree(g_thread);
    g_running = false;
}
