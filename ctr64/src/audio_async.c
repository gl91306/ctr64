/* RSP audio tasks on another CPU core (New 3DS: core 2, see ctr64_core1_ready).
 *
 * The audio HLE (mupen64plus-rsp-hle's alist code) runs on its own thread (higher priority than
 * the video worker, gfx_async.c, should they share core 2). The task runs on a second HLE instance whose RSP
 * memory and registers are this thread's copies (plugin.c, ctr64_hle_audio_*); its output goes
 * to RDRAM. The core (rsp_core.c) reports the task done from the SP interrupt event once the
 * thread has finished (ctr64_audio_ready, ctr64_audio_finish); the game's audio timing follows
 * when its tasks finish, so that is at most 1/4 VI after the start (then the CPU waits).
 * A separate setting, on by default (New 3DS): the thread runs on core 2 beside the video worker
 * (core 1 only with the dev switch core1.txt). */
int ctr64_core1_ready(void); /* platform/ctr_jit.c */
#include <3ds.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "device/device.h"
#include "main/main.h"
#include "settings.h"
#include "debug_crumb.h"

void ctr64_hle_audio_setup(unsigned char *dmem, unsigned char *imem);
void ctr64_hle_audio_run(void);

static u8 g_dmem[0x1000] __attribute__((aligned(8))); /* the HLE instance's RSP memory */
static u8 g_imem[0x1000] __attribute__((aligned(8)));
static u8 g_next_dmem[0x1000] __attribute__((aligned(8))); /* the queued task's snapshot */
static u8 g_next_imem[0x1000] __attribute__((aligned(8)));
static LightLock g_lock;
/* Waits re-check their condition this often: a wakeup between cores was seen to get lost
 * (Azahar, SM64 with the audio thread: the CPU slept on with the task long done) */
#define WAKE_RECHECK_NS 1000000LL
static CondVar g_changed;
static Thread g_thread;
static bool g_running, g_quit, g_set_up;
static volatile unsigned g_put, g_got; /* tasks queued / done */


extern unsigned ctr64_gfx_wait_ticks; /* gfx_async.c: CPU-side waiting, for the stats */

static void worker(void *arg)
{
    (void)arg;
    extern void ctr64_vfp_runfast(int who); /* main.c: VFP RunFast, per thread */
    ctr64_vfp_runfast(2);
    for (;;) {
        LightLock_Lock(&g_lock);
        while (g_got == g_put && !g_quit)
            CondVar_WaitTimeout(&g_changed, &g_lock, WAKE_RECHECK_NS);
        LightLock_Unlock(&g_lock);
        if (g_quit)
            break;
        memcpy(g_dmem, g_next_dmem, sizeof(g_dmem));
        memcpy(g_imem, g_next_imem, sizeof(g_imem));
        ctr64_hle_audio_run();
        LightLock_Lock(&g_lock);
        g_got++;
        CondVar_Broadcast(&g_changed);
        LightLock_Unlock(&g_lock);
    }
}

static bool start(void)
{
    static bool tried;
    if (g_running || tried)
        return g_running;
    tried = true;
    bool new3ds = false;
    APT_CheckNew3DS(&new3ds);
    if (!new3ds)
        return false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    LightLock_Init(&g_lock);
    CondVar_Init(&g_changed);
    /* Core 1 (shared with the system, up to the app's CPU time limit) keeps audio off both the
     * CPU emulation (core 0) and the video worker (core 2); core 2 if core 1 is not available. */
    g_thread = NULL;
    FILE *c2 = fopen("sdmc:/3ds/ctr64/audiocore2.txt", "r"); /* development: force core 2 */
    if (c2)
        fclose(c2);
    if (!c2 && ctr64_core1_ready())
        g_thread = threadCreate(worker, NULL, 64 * 1024, prio - 2, 1, false);
    if (!g_thread)
        g_thread = threadCreate(worker, NULL, 64 * 1024, prio - 2, 2, false);
    g_running = g_thread != NULL;
    return g_running;
}

static void wait_done(void)
{
    if (g_got == g_put)
        return;
    const u64 t0 = svcGetSystemTick();
    const char *crumb = ctr64_crumb;
    CRUMB("audio: wait for the thread");
    extern volatile int ctr64_cpu_waiting;
    ctr64_cpu_waiting = 1;
    LightLock_Lock(&g_lock);
    while (g_got != g_put)
        CondVar_WaitTimeout(&g_changed, &g_lock, WAKE_RECHECK_NS);
    LightLock_Unlock(&g_lock);
    CRUMB(crumb);
    ctr64_cpu_waiting = 0;
    ctr64_gfx_wait_ticks += (unsigned)(svcGetSystemTick() - t0);
    extern unsigned long long ctr64_prof_audio_wait_ticks;
    ctr64_prof_audio_wait_ticks += svcGetSystemTick() - t0;
}

/* the stall watchdog's report (main.c) */
void ctr64_audio_debug(FILE *f)
{
    extern struct device g_dev;
    extern unsigned ctr64_audio_debug_pending(void);
    extern void ctr64_gfx_debug(FILE *f);
    ctr64_gfx_debug(f);
    fprintf(f, "  audio put %u got %u pending %u | SP status %08x MI intr %08x mask %08x | pc %08x cycle %d next %08x count %08x\n",
            g_put, g_got, ctr64_audio_debug_pending(), (unsigned)g_dev.sp.regs[SP_STATUS_REG],
            (unsigned)g_dev.mi.regs[MI_INTR_REG], (unsigned)g_dev.mi.regs[MI_INTR_MASK_REG],
            (unsigned)g_dev.r4300.new_dynarec_hot_state.pcaddr, g_dev.r4300.new_dynarec_hot_state.cycle_count,
            (unsigned)*r4300_cp0_next_interrupt(&g_dev.r4300.cp0),
            (unsigned)g_dev.r4300.new_dynarec_hot_state.cp0_regs[CP0_COUNT_REG]);
}

/* rsp_core.c, for an audio task: queues it (true), or false to run it inline */
bool ctr64_audio_thread_off; /* development: <data dir>/noaudiothread.txt (core_host.c) */
bool ctr64_audio_task_start(const unsigned char *dmem, const unsigned char *imem)
{
    if (!g_settings.threaded_video || !g_settings.threaded_audio || ctr64_audio_thread_off || !start())
        return false;
    wait_done(); /* the RSP runs one task at a time; this one is a precaution */
    if (!g_set_up) {
        ctr64_hle_audio_setup(g_dmem, g_imem);
        g_set_up = true;
    }
    memcpy(g_next_dmem, dmem, sizeof(g_next_dmem));
    memcpy(g_next_imem, imem, sizeof(g_next_imem));
    LightLock_Lock(&g_lock);
    g_put++;
    CondVar_Broadcast(&g_changed);
    LightLock_Unlock(&g_lock);
    return true;
}

/* `elapsed`: emulated count cycles since the task started */
/* After 1/16 VI (about 1 ms, an N64 audio task's own length) the CPU waits for the thread. It was
 * 1/4 VI: on HW (core 1 is shared with the system) a task then often ended late in emulated
 * time, and Rayman 2 built display lists that call themselves without end at its save slot
 * screen (graphics stopped, music played on; HW playtest 2026-11-20). Azahar: the same with
 * audio inline but reported done ~1/4 VI late; clean at 1/8 VI and with this cap. */
bool ctr64_audio_ready(unsigned elapsed)
{
    const unsigned vi = g_dev.vi.delay ? g_dev.vi.delay : 780000;
    extern bool ctr64_determ(void); /* gfx_async.c */
    return (g_got == g_put && !ctr64_determ()) || elapsed >= vi / 16;
}

void ctr64_audio_finish(void)
{
    wait_done();
}

unsigned ctr64_audio_step(void)
{
    const unsigned vi = g_dev.vi.delay ? g_dev.vi.delay : 780000;
    return vi / 64;
}

/* the program ends on an error (crash.c): waits at most 2 s for the thread */
void ctr64_audio_async_abandon(void)
{
    if (!g_running || threadGetCurrent() == g_thread)
        return;
    g_quit = true; /* looked at every WAKE_RECHECK_NS */
    threadJoin(g_thread, 2000000000LL);
}

void ctr64_audio_async_stop(void)
{
    if (!g_running)
        return;
    wait_done();
    LightLock_Lock(&g_lock);
    g_quit = true;
    CondVar_Broadcast(&g_changed);
    LightLock_Unlock(&g_lock);
    threadJoin(g_thread, U64_MAX);
    threadFree(g_thread);
    g_running = false;
}
