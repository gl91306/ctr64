/* ctr64 - Mupen64Plus-Next for Nintendo 3DS.
 *
 * The core (ARM dynarec, RSP HLE, audio) runs with GLideN64 on the PICA200 backend (src/gfx),
 * which presents to the top screen from inside each frame. Hold SELECT while starting (or press
 * SELECT in the ROM browser) to run the M0 GPU test pattern. */

#include <3ds.h>
#include <malloc.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#include "audio_ndsp.h"
#include "core_host.h"
#include "crash.h"
#include "debug_crumb.h"
#include "device/device.h"
#include "device/r4300/r4300_core.h"
#include "main/main.h"
#include "menu.h"
#include "ui/ui.h"
#include "settings.h"
#include "gpu_test.h"
#include "pica/pica.h"
#include "prof.h"
#include "rom_browser.h"
#include "video.h"

/* JIT profiling counters (platform/ctr_jit.c) */
extern u32 ctr_jit_flush_calls, ctr_jit_flush_bytes;
extern u64 ctr_jit_flush_ticks;
extern unsigned int ctr_jit_compiles;
extern u64 ctr_jit_compile_ticks;
extern unsigned int ctr64_gfx_stats[8];
extern unsigned long long ctr64_gfx_ticks[3];
extern unsigned int ctr64_gfx_mem[4];
extern const char *ctr64_allocfail_where;
extern unsigned ctr64_allocfail_bytes;
extern unsigned long long ctr64_sp_ticks[3];
extern unsigned long long ctr64_prof_ticks[40];
extern unsigned int ctr64_prof_calls[40];
extern const char *const ctr64_prof_names[40];

/* audio-list command time by opcode (mupen64plus-rsp-hle alist.c) */
unsigned long long ctr64_alist_ticks[32];

/* Fine-grained timers (GLideN64 scopes, backend draws, audio commands): on with Debug info or
 * <data dir>/prof.txt. Each tick read is a kernel call, thousands per frame, so off otherwise
 * (their stats.log figures then read 0). */
int ctr64_prof_on;
static bool g_prof_file;

/* clock for the core's profiling counters (rsp_core.c) */
unsigned long long ctr64_ticks(void)
{
    return svcGetSystemTick();
}

/* GPU-visible linear heap: command/vertex rings (5 MB), GLideN64 textures, audio. libctru's
 * 32 MB default would starve the main heap, which holds the ROM (up to 64 MB) and core state. */
u32 __ctru_linear_heap_size = 16 * 1024 * 1024;

/* New 3DS (124 MB application memory): 8 MB of it is left unallocated at boot and handed out when a
 * game starts (ctr64_mem_prepare): as extra GPU-visible memory when the game starts with
 * anti-aliasing (frame buffers rendered at twice the resolution, which do not fit in the 6 MB of
 * VRAM plus the 16 MB linear heap), or as more main heap when that runs out (Paper Mario ~60 MB,
 * Majora's Mask ~58 MB of heap). OoT with its 32 MB ROM needs ~52 MB of heap, so the reserve
 * cannot simply be a bigger linear heap. Runs before the heaps exist (linker --wrap). */
#define XRES_BYTES (8u * 1024 * 1024)
#define XRES_MIN_HEAP (54u * 1024 * 1024)
#define XRES_ROM_MAX (32 * 1024 * 1024) /* bigger ROMs (Paper Mario) need the reserve as heap */
extern u32 __ctru_heap, __ctru_heap_size;
extern char *fake_heap_end;
static bool g_xres;          /* reserve exists (New 3DS) */
static int g_xres_state;     /* 0 unused, 1 mapped as linear pool, 2 given to the main heap */
/* the GPU pool: up to two linear blocks (the reserve, and main heap lent to it; one block of both
 * sizes is refused, linear memory must be physically contiguous) */
#define XSEG_BLOCKS 1024
typedef struct {
    u32 base, bytes;
    struct { u32 off, size; } blk[XSEG_BLOCKS]; /* live allocations, sorted by offset */
    int n;
} XSeg;
#define XSEG_MAX 12 /* the reserve + heap lent in pieces (a big linear block may not be free on HW) */
static XSeg g_xseg[XSEG_MAX];
static char g_xdiag[96];     /* how the lending went, for core.log */
static u32 g_heap_lent;      /* main heap given to the pool (ctr64_mem_prepare) */

void __real___system_allocateHeaps(void);
void __wrap___system_allocateHeaps(void)
{
    /* free commit room, measured the way libctru's own __system_allocateHeaps does */
    Handle lim = 0;
    s64 max = 0, cur = 0;
    ResourceLimitType t = RESLIMIT_COMMIT;
    if (R_SUCCEEDED(svcGetResourceLimit(&lim, CUR_PROCESS_HANDLE))) {
        svcGetResourceLimitLimitValues(&max, lim, &t, 1);
        svcGetResourceLimitCurrentValues(&cur, lim, &t, 1);
        svcCloseHandle(lim);
    }
    const u32 remaining = (u32)(max - cur) & ~0xFFFu;
    /* New 3DS: ~83 MB here, Old 3DS: ~20 MB. The heap without the reserve must still fit OoT. */
    if (remaining >= __ctru_linear_heap_size + XRES_BYTES + XRES_MIN_HEAP) {
        __ctru_heap_size = remaining - __ctru_linear_heap_size - XRES_BYTES;
        g_xres = true;
    }
    __real___system_allocateHeaps();
}

/* menu: the console can do anti-aliasing */
int ctr64_aa_available(void)
{
    return g_xres;
}

/* GLideN64 config: the running game has the memory for it */
int ctr64_aa_ready(void)
{
    return g_xres_state == 1;
}

/* the running game can never have anti-aliasing: its heap needs the GPU reserve (a ROM over
 * XRES_ROM_MAX, or a game in heap_hungry) - the menu says so instead of "next game started" */
static int g_aa_excluded;
int ctr64_aa_excluded(void)
{
    return g_aa_excluded;
}

#define XPOOL_HEAP_MARGIN (32u * 1024 * 1024) /* heap kept above the ROM size (MK64 uses ~20 MB) */
#define XPOOL_LEND_MAX (8u * 1024 * 1024)

/* the heap lent to the pool goes back on top of the main heap */
static void heap_take_back(void)
{
    u32 addr;
    if (g_heap_lent && R_SUCCEEDED(svcControlMemory(&addr, __ctru_heap + __ctru_heap_size, 0, g_heap_lent,
                                                    MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE))) {
        __ctru_heap_size += g_heap_lent;
        fake_heap_end += g_heap_lent;
    }
    g_heap_lent = 0;
}

static bool pool_in_use(void)
{
    for (int s = 0; s < XSEG_MAX; s++)
        if (g_xseg[s].n)
            return true;
    return false;
}

/* frees an empty pool: the reserve is unused again, the lent heap is back in the heap */
static void pool_release(void)
{
    u32 addr;
    for (int s = 0; s < XSEG_MAX; s++) {
        if (g_xseg[s].bytes)
            svcControlMemory(&addr, g_xseg[s].base, 0, g_xseg[s].bytes, MEMOP_FREE, 0);
        g_xseg[s].bytes = 0;
        g_xseg[s].n = 0;
    }
    g_xres_state = 0;
    heap_take_back();
}

/* Called before a ROM of rom_bytes is loaded: a game started with anti-aliasing gets the reserve
 * as GPU memory; otherwise an unused linear pool is returned so the main heap can take it. */
void ctr64_mem_prepare(long rom_bytes, const char *game_id)
{
    extern int ctr64_setting_aa, ctr64_setting_fb_emulation;
    /* games whose heap needs the reserve even with a ROM that fits under XRES_ROM_MAX: Majora's
     * Mask (32 MB ROM, up to ~58 MB of heap; with the reserve as GPU memory it ran out of heap
     * after ~3 minutes on hardware: anti-aliasing stays off for it) */
    static const char *const heap_hungry[] = {"NZS"};
    bool hungry = false;
    for (unsigned i = 0; i < sizeof(heap_hungry) / sizeof(heap_hungry[0]); i++)
        if (game_id && !strncmp(game_id, heap_hungry[i], 3))
            hungry = true;
    const bool want = ctr64_setting_aa && ctr64_setting_fb_emulation && rom_bytes <= XRES_ROM_MAX && !hungry;
    g_aa_excluded = rom_bytes > XRES_ROM_MAX || hungry;
    u32 addr;

    if (!g_xres)
        return;
    if (want && g_xres_state == 0) {
        if (R_FAILED(svcControlMemory(&addr, 0, 0, XRES_BYTES, MEMOP_ALLOC_LINEAR, MEMPERM_READ | MEMPERM_WRITE)))
            return;
        g_xseg[0].base = addr;
        g_xseg[0].bytes = XRES_BYTES;
        g_xres_state = 1;
        /* A small ROM leaves much of the main heap idle (Mario Kart 64: 12 MB ROM, ~32 MB of
         * heap used of ~58), while anti-aliasing can need more GPU memory than the reserve (the
         * MK64 menus). Lend the pool the heap top above ROM + XPOOL_HEAP_MARGIN, if unused. */
        u32 lend = 0;
        const u32 keep = ((u32)rom_bytes + XPOOL_HEAP_MARGIN + 0xFFFFF) & ~0xFFFFFu;
        const u32 brk = (u32)sbrk(0);
        if (__ctru_heap_size > keep) {
            lend = (__ctru_heap_size - keep) & ~0xFFFFFu;
            if (lend > XPOOL_LEND_MAX)
                lend = XPOOL_LEND_MAX;
            if (brk + 0x100000 > __ctru_heap + __ctru_heap_size - lend)
                lend = 0;
        }
        Result rf = 0, ra = 0;
        u32 got = 0;
        int segs = 1;
        if (lend && R_SUCCEEDED(rf = svcControlMemory(&addr, __ctru_heap + __ctru_heap_size - lend, 0, lend,
                                                      MEMOP_FREE, 0))) {
            __ctru_heap_size -= lend;
            fake_heap_end -= lend;
            /* linear memory is physically contiguous: on hardware the freed heap may not be one
             * free block (Azahar takes 8 MB in one piece, a New 3DS refused it), so take it in
             * the biggest pieces available, down to 256 KB */
            u32 chunk = lend;
            while (got < lend && segs < XSEG_MAX && chunk >= 0x40000) {
                if (chunk > lend - got)
                    chunk = lend - got;
                if (R_SUCCEEDED(ra = svcControlMemory(&addr, 0, 0, chunk, MEMOP_ALLOC_LINEAR,
                                                      MEMPERM_READ | MEMPERM_WRITE))) {
                    g_xseg[segs].base = addr;
                    g_xseg[segs].bytes = chunk;
                    segs++;
                    got += chunk;
                } else {
                    chunk = (chunk / 2) & ~0xFFFu;
                }
            }
            /* what was not taken goes back to the heap */
            g_heap_lent = lend - got;
            heap_take_back();
            g_heap_lent = got;
        }
        snprintf(g_xdiag, sizeof(g_xdiag), "lend %lu KB brk +%lu KB free %lx: %lu KB in %d pieces, last %lx",
                 (unsigned long)lend / 1024, (unsigned long)(brk - __ctru_heap) / 1024, (unsigned long)rf,
                 (unsigned long)got / 1024, segs - 1, (unsigned long)ra);
    } else if (!want && g_xres_state == 1 && !pool_in_use()) {
        pool_release();
    }
}

/* The main heap ran out: extend it in place (it is one block from 0x08000000) with the reserve
 * when no anti-aliasing game holds it. Big games (Paper Mario, Majora's Mask) get here. Once
 * taken it stays heap until ctr64 is restarted. */
static bool heap_take_reserve(void)
{
    u32 addr;

    if (!g_xres || g_xres_state == 2)
        return false;
    if (g_xres_state == 1) {
        if (pool_in_use())
            return false;
        pool_release();
    }
    if (R_FAILED(svcControlMemory(&addr, __ctru_heap + __ctru_heap_size, 0, XRES_BYTES, MEMOP_ALLOC,
                                  MEMPERM_READ | MEMPERM_WRITE)))
        return false;
    __ctru_heap_size += XRES_BYTES;
    fake_heap_end += XRES_BYTES;
    g_xres_state = 2;
    return true;
}

struct _reent;
void *__real__sbrk_r(struct _reent *r, ptrdiff_t incr);
void *__wrap__sbrk_r(struct _reent *r, ptrdiff_t incr)
{
    void *p = __real__sbrk_r(r, incr);
    if (p == (void *)-1 && incr > 0 && heap_take_reserve())
        p = __real__sbrk_r(r, incr);
    return p;
}

const char *ctr64_mem_reserve_state(void)
{
    static const char *const names[] = {"free", "GPU", "heap"};
    static char b[160];
    if (g_xres && g_xres_state == 1) {
        u32 total = 0;
        for (int s = 0; s < XSEG_MAX; s++)
            total += g_xseg[s].bytes;
        snprintf(b, sizeof(b), "GPU %lu KB (%lu KB lent by the heap; %s)", (unsigned long)total / 1024,
                 (unsigned long)g_heap_lent / 1024, g_xdiag);
        return b;
    }
    return g_xres ? names[g_xres_state] : "none";
}

/* First-fit allocator over the GPU pool, used when the linear heap is full. */
static void *xseg_alloc(XSeg *g, u32 size, u32 align)
{
    u32 at = 0;
    if (!g->bytes || g->n == XSEG_BLOCKS)
        return NULL;
    for (int i = 0; i <= g->n; i++) {
        u32 a = (g->base + at + align - 1) & ~(align - 1);
        u32 limit = i < g->n ? g->blk[i].off : g->bytes;
        if (a - g->base + size <= limit) {
            memmove(&g->blk[i + 1], &g->blk[i], (g->n - i) * sizeof(g->blk[0]));
            g->blk[i].off = a - g->base;
            g->blk[i].size = size;
            g->n++;
            return (void *)a;
        }
        if (i < g->n)
            at = g->blk[i].off + g->blk[i].size;
    }
    return NULL;
}

void *ctr64_xlinear_alloc(u32 size, u32 align)
{
    if (g_xres_state != 1 || !size)
        return NULL;
    if (align < 0x80)
        align = 0x80;
    size = (size + 0x7F) & ~0x7Fu;
    for (int s = 0; s < XSEG_MAX; s++) {
        void *p = xseg_alloc(&g_xseg[s], size, align);
        if (p)
            return p;
    }
    return NULL;
}

/* true when p came from ctr64_xlinear_alloc (and is now freed) */
bool ctr64_xlinear_free(void *p)
{
    if (g_xres_state != 1)
        return false;
    for (int s = 0; s < XSEG_MAX; s++) {
        XSeg *g = &g_xseg[s];
        const u32 off = (u32)p - g->base;
        if (!g->bytes || (u32)p < g->base || off >= g->bytes)
            continue;
        for (int i = 0; i < g->n; i++)
            if (g->blk[i].off == off) {
                memmove(&g->blk[i], &g->blk[i + 1], (g->n - i - 1) * sizeof(g->blk[0]));
                g->n--;
                break;
            }
        return true;
    }
    return false;
}

u32 ctr64_xlinear_free_bytes(void)
{
    u32 total = 0;
    if (g_xres_state != 1)
        return 0;
    for (int s = 0; s < XSEG_MAX; s++) {
        total += g_xseg[s].bytes;
        for (int i = 0; i < g_xseg[s].n; i++)
            total -= g_xseg[s].blk[i].size;
    }
    return total;
}

#define DATA_DIR "sdmc:/3ds/ctr64"
#define ROM_DIR DATA_DIR "/roms"

static void wait_for_start(void)
{
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gspWaitForVBlank();
    }
}

const char *volatile ctr64_crumb = "start";
volatile u32 ctr64_heartbeat; /* also advanced by the menu (menu.c) */
static volatile bool g_watchdog_quit;
static FILE *g_stats; /* <data dir>/stats.log: one line per second, for unattended test runs */

/* Higher-priority thread on the app core: if the main loop stops advancing, report the last
 * breadcrumb (debug_crumb.h) on the bottom screen. */
static void watchdog(void *arg)
{
    (void)arg;
    u32 last = 0, stuck = 0;
    while (!g_watchdog_quit) {
        svcSleepThread(1000000000LL);
        if (ctr64_heartbeat == last) {
            if (++stuck >= 2) {
                if (g_settings.debug_info)
                    printf("\x1b[26;0Hstall %lus at: %s          \n", stuck, ctr64_crumb ? ctr64_crumb : "?");
                if (g_stats) {
                    fprintf(g_stats, "stall %lus at: %s\n", stuck, ctr64_crumb ? ctr64_crumb : "?");
                    extern void ctr64_audio_debug(FILE *f); /* audio_async.c */
                    ctr64_audio_debug(g_stats);
                    fflush(g_stats);
                }
            }
        } else {
            stuck = 0;
        }
        last = ctr64_heartbeat;
    }
}

/* Keeps emulation at the N64's VI rate (~59.83 Hz NTSC, 50 Hz PAL): one core_host_run_frame()
 * is one VI. Presentation is triple-buffered and audio drops what the DSP cannot take, so nothing
 * else limits the speed. The core reports a nominal 60/50 Hz, so the period is also trimmed by up
 * to 1% to hold the DSP queue near its target: the game then produces audio exactly as fast as
 * the 3DS plays it, with no drops or underruns from rate drift. A run that falls more than a few
 * VIs behind restarts the schedule instead of catching up. <data dir>/nolimit.txt turns it off
 * (profiling headroom). */
static bool g_nolimit, g_halfspeed;
#define AUDIO_TARGET_MS 60

static void limit_speed(void)
{
    static u64 next;
    /* development aid: halfspeed.txt halves the rate (Azahar runs the CPU thread ~2x slower than a
     * New 3DS: this gives it about the idle time hardware has, e.g. for remembered blocks) */
    const double fps = (core_host_fps() > 55 ? 59.826 : 50.0) / (g_halfspeed ? 2 : 1);
    double trim = 1.0;
    if (audio_ndsp_active() && audio_ndsp_rate()) {
        trim += ((double)audio_ndsp_queued_ms() - AUDIO_TARGET_MS) * (0.01 / 64);
        trim = trim < 0.99 ? 0.99 : trim > 1.01 ? 1.01 : trim;
    }
    const u64 period = (u64)(SYSCLOCK_ARM11 / fps * trim);
    const u64 now = svcGetSystemTick();
    extern volatile bool ctr64_speed_behind; /* frameskip.c */
    /* behind: the schedule has slipped by more than a whole VI (not one slow VI: OoT draws on
     * every third VI, so single VIs run late at full speed). It holds until emulation is ahead
     * again (this function sleeps), across the schedule restarts below, for up to half a second. */
    static unsigned hold;
    if (next && now > next + period) {
        ctr64_speed_behind = true;
        hold = 30;
    } else if (!next || now < next) {
        ctr64_speed_behind = false;
        hold = 0;
    } else if (hold) {
        hold--;
    } else {
        ctr64_speed_behind = false;
    }
    if (!next || now > next + 4 * period) {
        next = now + period;
        return;
    }
    if (now < next) {
        /* the time left: remembered blocks are compiled (new_dynarec.c ctr64_rb_idle) */
        extern int ctr64_rb_idle(unsigned long long deadline);
        extern void ctr64_bg_feed(int may_finish);
        if (g_settings.remember_blocks) { /* menu: Precompile known code */
            ctr64_bg_feed(0); /* core 1 (new_dynarec.c), then this core */
            ctr64_rb_idle(next);
        }
        const u64 t = svcGetSystemTick();
        ctr64_prof_rbidle_ticks += t - now;
        if (t < next)
            svcSleepThread((s64)((next - t) * 1000000000ull / SYSCLOCK_ARM11));
        ctr64_prof_sleep_ticks += svcGetSystemTick() - t;
    }
    next += period;
}

/* Heap use for stats.log: used, and not in use (never obtained from the system yet, plus free
 * blocks inside the arena). mallinfo() walks the whole heap, so it is refreshed every 10 s. */
static unsigned g_heap_used_kb, g_heap_free_kb;

static void update_heap_info(unsigned second)
{
    extern u32 __ctru_heap_size;
    if (second % 10 != 1)
        return;
    const struct mallinfo mi = mallinfo();
    g_heap_used_kb = (unsigned)(mi.uordblks / 1024);
    g_heap_free_kb = (unsigned)((__ctru_heap_size - mi.arena + mi.fordblks) / 1024);
}

static MenuResult g_quit = MENU_EXIT; /* how the game was left */

#ifdef CTR64_PGO_GEN
/* instrumented build: the profile counters (section gcov_info, -fprofile-info-section) as .gcda
 * data in sdmc:/3ds/ctr64/pgo/<n>.gcda, one file per object (merged on the PC with gcov-tool) */
#include <gcov.h>
extern const struct gcov_info *const __start_gcov_info[], *const __stop_gcov_info[];
static void pgo_fname(const char *f, void *arg) { (void)f; (void)arg; }
static void pgo_write(const void *d, unsigned n, void *arg) { fwrite(d, 1, n, (FILE *)arg); }
static void *pgo_alloc(unsigned n, void *arg) { (void)arg; return malloc(n); }
static void pgo_dump(void)
{
    mkdir(DATA_DIR "/pgo", 0777);
    int k = 0;
    for (const struct gcov_info *const *i = __start_gcov_info; i < __stop_gcov_info; i++, k++) {
        char path[96];
        snprintf(path, sizeof(path), DATA_DIR "/pgo/%d.gcda", k);
        FILE *f = fopen(path, "wb");
        if (!f)
            continue;
        __gcov_info_to_gcda(*i, pgo_fname, pgo_write, pgo_alloc, f);
        fclose(f);
    }
}
#endif

static void run_game(void)
{
    u64 t_last = svcGetSystemTick();
    float emu_ms = 0;
    u32 frames = 0;
    FILE *nl = fopen(DATA_DIR "/nolimit.txt", "r");
    g_nolimit = nl != NULL;
    if (nl)
        fclose(nl);
    FILE *hs = fopen(DATA_DIR "/halfspeed.txt", "r");
    g_halfspeed = hs != NULL;
    if (hs)
        fclose(hs);
    FILE *pf = fopen(DATA_DIR "/prof.txt", "r");
    g_prof_file = pf != NULL;
    if (pf)
        fclose(pf);
    ctr64_prof_on = g_settings.debug_info || g_prof_file || ctr64_profiler == 2;
    /* development aid: crashtest.txt crashes at VI 300 to check the crash reporter ("abort" in it
     * calls abort(), otherwise an undefined instruction) */
    FILE *ct = fopen(DATA_DIR "/crashtest.txt", "r");
    FILE *stt = fopen(DATA_DIR "/statetest.txt", "r");
    const bool g_statetest = stt != NULL;
    if (stt)
        fclose(stt);
    int g_crashtest = 0;
    /* development aid (framelog.txt): one line per VI, "vi frame_us compile_us flush_us" and the
     * dispatch events (new_dynarec.c ctr64_disp: get_addr_ht, hash misses, dynamic_linker,
     * unlinked, verify_dirty, interrupts), to measure hitches and dispatch costs */
    FILE *g_framelog = fopen(DATA_DIR "/framelog.txt", "r");
    if (g_framelog) {
        fclose(g_framelog);
        g_framelog = fopen(DATA_DIR "/framelog.log", "w");
    }
    if (ct) {
        g_crashtest = fgetc(ct) == 'a' ? 2 : 1;
        fclose(ct);
    }

    extern unsigned ctr64_frames_skipped; /* frameskip.c */
    while (aptMainLoop()) {
        u32 kdown, kheld, kup;
        ui_scan(&kdown, &kheld, &kup); /* hidScanInput (+ the development UI script) */
        if ((kdown & KEY_SELECT) || menu_running_touched(kdown)) {
            core_host_rb_save();
            core_host_flush_log();
            prof_mark("menu");
            const MenuResult r = menu_run();
            if (r != MENU_CONTINUE) {
                g_quit = r;
                break;
            }
            /* the pause is not emulation time */
            prof_vi_resync();
            t_last = svcGetSystemTick();
            emu_ms = 0;
            frames -= frames % 60;
            ctr64_frames_skipped = 0;
            memset(ctr64_gfx_stats, 0, sizeof(ctr64_gfx_stats));
        }

        ctr64_heartbeat++;
        prof_vi_begin();
        CRUMB("main: run_frame");
        u64 t0 = svcGetSystemTick();
        const u64 fl_c0 = ctr_jit_compile_ticks, fl_f0 = ctr_jit_flush_ticks;
        extern unsigned int ctr64_disp[8]; /* new_dynarec.c: dispatch events, framelog columns 5.. */
        unsigned fl_d0[6];
        memcpy(fl_d0, ctr64_disp, sizeof(fl_d0));
        {   /* menu: Fast compile (compiles between frames always optimise fully: ctr64_rb_idle) */
            extern int ctr64_fast_compile, ctr64_fast_compile_dev, ctr64_bg_want;
            ctr64_bg_want = g_settings.remember_blocks;
            ctr64_fast_compile = ctr64_fast_compile_dev >= 0 ? ctr64_fast_compile_dev : g_settings.fast_compile ? 15 : 0;
        }
        core_host_run_frame();
        if (g_framelog) {
            const u64 t1 = svcGetSystemTick();
            /* kept in memory and written once a second with stats.log: writing every frame hung
             * MK64 next to the frame dumps (dumpframes.txt) */
            static u32 fl[60][10];
            static unsigned fl_n;
            fl[fl_n][0] = frames;
            fl[fl_n][1] = (u32)((t1 - t0) * 1000000ULL / SYSCLOCK_ARM11);
            fl[fl_n][2] = (u32)((ctr_jit_compile_ticks - fl_c0) * 1000000ULL / SYSCLOCK_ARM11);
            fl[fl_n][3] = (u32)((ctr_jit_flush_ticks - fl_f0) * 1000000ULL / SYSCLOCK_ARM11);
            for (int k = 0; k < 6; k++)
                fl[fl_n][4 + k] = ctr64_disp[k] - fl_d0[k];
            if (++fl_n == 60) {
                for (unsigned i = 0; i < 60; i++)
                    fprintf(g_framelog, "%lu %lu %lu %lu %lu %lu %lu %lu %lu %lu\n", fl[i][0], fl[i][1], fl[i][2],
                            fl[i][3], fl[i][4], fl[i][5], fl[i][6], fl[i][7], fl[i][8], fl[i][9]);
                fflush(g_framelog);
                fl_n = 0;
            }
        }
        CRUMB("main: after run_frame");
        core_host_rb_tick();
        prof_vi_run(svcGetSystemTick() - t0);
        emu_ms += (float)(svcGetSystemTick() - t0) * 1000.0f / SYSCLOCK_ARM11;
        if (!g_nolimit && g_settings.speed_limit) {
            limit_speed();
        } else {
            extern volatile bool ctr64_speed_behind; /* no limit: always as fast as possible */
            ctr64_speed_behind = true;
        }
        if (g_statetest && (frames == 300 || frames == 900)) {
            /* development aid (statetest.txt): save slot 9, load it 600 VIs later, save slot 8 */
            if (frames == 300)
                core_host_save_state(9);
            else if (core_host_load_state(9))
                core_host_save_state(8);
        }
#ifdef CTR64_PGO_GEN
        {   /* instrumented build (CTR64_PGO=gen): pgodump.txt holds the VI at which the profile of
             * new_dynarec.c is written (merged into sdmc:/3ds/ctr64/pgo) */
            static int pgo_at = -2;
            if (pgo_at == -2) {
                FILE *pg = fopen(DATA_DIR "/pgodump.txt", "r");
                pgo_at = -1;
                if (pg) {
                    if (fscanf(pg, "%d", &pgo_at) != 1)
                        pgo_at = 1800;
                    fclose(pg);
                }
            }
            if (pgo_at >= 0 && frames == (u32)pgo_at) {
                pgo_dump();
                pgo_at = -1;
            }
        }
#endif
        if (g_crashtest && frames == 300) {
            CRUMB("main: crashtest.txt");
            if (g_crashtest == 2)
                abort();
            __builtin_trap();
        }

        /* GLideN64 presents from inside the frame (VI update -> swapBuffers). */

        /* development aid (exitafter.txt: a VI count): leave the game there as from the menu, so
         * everything is saved (recording remembered blocks); done.txt is written at the end */
        static int exit_after = -2;
        if (exit_after == -2) {
            FILE *ea = fopen(DATA_DIR "/exitafter.txt", "r");
            exit_after = -1;
            if (ea) {
                if (fscanf(ea, "%d", &exit_after) != 1)
                    exit_after = -1;
                fclose(ea);
            }
        }
        /* development aid (savestateat.txt: a VI count): save slot 7 there, e.g. a gameplay
         * starting point for recording runs (tools/lists: copied to loadstate.st) */
        static int save_at = -2;
        if (save_at == -2) {
            FILE *sa = fopen(DATA_DIR "/savestateat.txt", "r");
            save_at = -1;
            if (sa) {
                if (fscanf(sa, "%d", &save_at) != 1)
                    save_at = -1;
                fclose(sa);
            }
        }
        if (save_at >= 0 && frames + 1 == (u32)save_at)
            core_host_save_state(7);
        if (exit_after >= 0 && frames + 1 >= (u32)exit_after) {
            g_quit = MENU_EXIT;
            break;
        }

        {
            extern volatile bool ctr64_speed_behind;
            prof_vi_end(frames, ctr64_speed_behind);
        }
        if (++frames % 60 == 0) {
            u64 now = svcGetSystemTick();
            float wall = (float)(now - t_last) * 1000.0f / SYSCLOCK_ARM11;
            const PicaStats *s = pica_stats();
            const float vi_target = core_host_fps() > 55 ? 59.83f : 50.0f;
            MenuStats ms = {
                .vi_per_s = 60.0f * 1000.0f / wall,
                .vi_target = vi_target,
                .game_fps = ctr64_gfx_stats[3] * 1000.0f / wall,
                .game_fps_full = (ctr64_gfx_stats[3] + ctr64_frames_skipped) * vi_target / 60.0f,
                .skipped = ctr64_frames_skipped,
                .cpu_ms = emu_ms / 60,
            };
            menu_set_stats(&ms);
            menu_draw_running();
            extern unsigned ctr64_frames_drawskip; /* frameskip.c */
            if (g_stats)
                fprintf(g_stats, "  frameskip: %u frames drawn skipped, %u presents dropped\n",
                        ctr64_frames_drawskip, ctr64_frames_skipped);
            ctr64_frames_drawskip = 0;
            ctr64_frames_skipped = 0;
            ctr64_prof_on = g_settings.debug_info || g_prof_file || ctr64_profiler == 2;
            if (g_settings.debug_info) {
            printf("\x1b[%d;0Hemu    %6.2f ms/frame   \n", menu_debug_row(), emu_ms / 60);
            printf("gpu    %6.2f ms cpu, %5.2f ms wait   \n", s->cpu_ms, s->fence_wait_ms);
            printf("vi     st %08lx org %06lx w %lu   \n", g_dev.vi.regs[VI_STATUS_REG],
                   g_dev.vi.regs[VI_ORIGIN_REG], g_dev.vi.regs[VI_WIDTH_REG]);
            printf("jit    %lu blocks, %lu flushes, %lu KB, %.2f ms   \n", (u32)ctr_jit_compiles,
                   ctr_jit_flush_calls, ctr_jit_flush_bytes / 1024,
                   (float)ctr_jit_flush_ticks * 1000.0f / SYSCLOCK_ARM11 / 60);
            printf("gfx/s  tri %u rect %u clr %u swap %u drop %u fbo %u  \n", ctr64_gfx_stats[0],
                   ctr64_gfx_stats[1], ctr64_gfx_stats[2], ctr64_gfx_stats[3], ctr64_gfx_stats[4],
                   ctr64_gfx_stats[5]);
            {
                const uint32_t *cp0 = r4300_cp0_regs(&g_dev.r4300.cp0);
                printf("pc %08lx sr %08lx cause %08lx\n", (u32)*r4300_pc(&g_dev.r4300),
                       (u32)cp0[CP0_STATUS_REG], (u32)cp0[CP0_CAUSE_REG]);
                printf("count %08lx cmp %08lx mi %02lx/%02lx sp %04lx\n", (u32)cp0[CP0_COUNT_REG],
                       (u32)cp0[CP0_COMPARE_REG], (u32)g_dev.mi.regs[MI_INTR_REG],
                       (u32)g_dev.mi.regs[MI_INTR_MASK_REG], (u32)g_dev.sp.regs[SP_STATUS_REG]);
                extern unsigned int ctr64_rcp_irq[6];
                printf("irq/s sp %u si %u ai %u vi %u pi %u dp %u  \n", ctr64_rcp_irq[0], ctr64_rcp_irq[1],
                       ctr64_rcp_irq[2], ctr64_rcp_irq[3], ctr64_rcp_irq[4], ctr64_rcp_irq[5]);
                for (int i = 0; i < 6; i++)
                    ctr64_rcp_irq[i] = 0;
                extern unsigned int ctr64_sp_tasks[3];
                extern int current_rsp_type;
                printf("rsp(type %d) tasks/s gfx %u audio %u other %u  \n", current_rsp_type,
                       ctr64_sp_tasks[0], ctr64_sp_tasks[1], ctr64_sp_tasks[2]);
                ctr64_sp_tasks[0] = ctr64_sp_tasks[1] = ctr64_sp_tasks[2] = 0;
            }
            printf("audio  %s %u Hz, %u ms queued   \n", audio_ndsp_active() ? "on" : "OFF (no dspfirm?)",
                   audio_ndsp_rate(), audio_ndsp_queued_ms());
            }
            if (g_stats) {
                update_heap_info(frames / 60);
                fprintf(g_stats, "t %lu fps %.1f emu %.2f gpu %.2f/%.2f dlist %.2f draw %.2f present %.2f "
                        "tri %u rect %u clr %u swap %u drop %u fbo %u allocfail %u subdiv %u | tex linear %u KB vram %u KB "
                        "pending %u KB max %u KB | linear free %lu KB vram free %lu KB heap used %u KB free %u KB | vbl %u %.2f ms fence %u %.2f ms"
                        " | rsp gfx %.2f audio %.2f other %.2f ms | aq %u ms @ %u Hz\n",
                        frames / 60, 60.0f * 1000.0f / wall, emu_ms / 60, s->cpu_ms, s->fence_wait_ms,
                        (float)ctr64_gfx_ticks[1] * 1000.0f / SYSCLOCK_ARM11 / 60,
                        (float)ctr64_gfx_ticks[0] * 1000.0f / SYSCLOCK_ARM11 / 60,
                        (float)ctr64_gfx_ticks[2] * 1000.0f / SYSCLOCK_ARM11 / 60,
                        ctr64_gfx_stats[0], ctr64_gfx_stats[1], ctr64_gfx_stats[2], ctr64_gfx_stats[3],
                        ctr64_gfx_stats[4], ctr64_gfx_stats[5], ctr64_gfx_stats[6], ctr64_gfx_stats[7], ctr64_gfx_mem[0] / 1024,
                        ctr64_gfx_mem[1] / 1024, ctr64_gfx_mem[3] / 1024, ctr64_gfx_mem[2] / 1024,
                        (linearSpaceFree() + ctr64_xlinear_free_bytes()) / 1024, vramSpaceFree() / 1024, g_heap_used_kb, g_heap_free_kb, s->vblank_waits, s->vblank_ms / 60, s->fence_waits, s->fence_ms / 60,
                        (float)ctr64_sp_ticks[0] * 1000.0f / SYSCLOCK_ARM11 / 60,
                        (float)ctr64_sp_ticks[1] * 1000.0f / SYSCLOCK_ARM11 / 60,
                        (float)ctr64_sp_ticks[2] * 1000.0f / SYSCLOCK_ARM11 / 60,
                        audio_ndsp_queued_ms(), audio_ndsp_rate());
                {
                    /* the game's frames per second: VIs with a new origin (as DaedalusX64 counts) */
                    extern unsigned ctr64_vi_frames;
                    static unsigned frames_seen;
                    extern unsigned int ctr64_gfx_fcram[2];
                    fprintf(g_stats, "  game frames %.1f/s, draws into FCRAM targets: color %u depth %u\n",
                            (ctr64_vi_frames - frames_seen) * 1000.0f / wall, ctr64_gfx_fcram[0], ctr64_gfx_fcram[1]);
                    ctr64_gfx_fcram[0] = ctr64_gfx_fcram[1] = 0;
                    extern unsigned int ctr64_gfx_vram_moves[2];
                    if (ctr64_gfx_vram_moves[0] || ctr64_gfx_vram_moves[1])
                        fprintf(g_stats, "  render targets moved: %u KB into VRAM, %u KB out\n", ctr64_gfx_vram_moves[0],
                                ctr64_gfx_vram_moves[1]);
                    ctr64_gfx_vram_moves[0] = ctr64_gfx_vram_moves[1] = 0;
                    extern char ctr64_rt_mode[64];
                    if (ctr64_rt_mode[0])
                        fprintf(g_stats, "  frame buffer storage: %s\n", ctr64_rt_mode);
                    extern unsigned int ctr64_gfx_shadow[3];
                    if (ctr64_gfx_shadow[0] || ctr64_gfx_shadow[2])
                        fprintf(g_stats, "  shadow copies for sampling: %u (%u KB), copies by transfer: %u\n",
                                ctr64_gfx_shadow[0], ctr64_gfx_shadow[1], ctr64_gfx_shadow[2]);
                    ctr64_gfx_shadow[0] = ctr64_gfx_shadow[1] = ctr64_gfx_shadow[2] = 0;
                    extern unsigned int ctr64_gfx_reused;
                    if (ctr64_gfx_reused)
                        fprintf(g_stats, "  render target storage reused: %u\n", ctr64_gfx_reused);
                    ctr64_gfx_reused = 0;
                    frames_seen = ctr64_vi_frames;
                }
                {
                    /* GLideN64 scopes (Ctr64Prof.h ids), ms per frame and calls per frame */
                    const char *const *names = ctr64_prof_names;
                    fprintf(g_stats, "  prof");
                    for (int i = 0; i < 40; i++)
                        if (ctr64_prof_calls[i])
                            fprintf(g_stats, " %s %.2f/%u", names[i],
                                    (float)ctr64_prof_ticks[i] * 1000.0f / SYSCLOCK_ARM11 / 60,
                                    ctr64_prof_calls[i] / 60);
                    fprintf(g_stats, "\n  jit %u blocks, %lu flushes, %lu KB, %.2f ms/frame, compiling %.2f ms/frame",
                            ctr_jit_compiles, ctr_jit_flush_calls, ctr_jit_flush_bytes / 1024,
                            (float)ctr_jit_flush_ticks * 1000.0f / SYSCLOCK_ARM11 / 60,
                            (float)ctr_jit_compile_ticks * 1000.0f / SYSCLOCK_ARM11 / 60);
                    {
                        extern unsigned long long ctr_jit_pass_ticks[11]; /* new_dynarec.c */
                        extern unsigned int ctr_jit_insns; /* platform/ctr_jit.c */
                        if (ctr_jit_compile_ticks) {
                            extern unsigned int ctr64_debug_messages;
                            fprintf(g_stats, " %u insns, %u log calls (passes", ctr_jit_insns, ctr64_debug_messages);
                            ctr64_debug_messages = 0;
                            ctr_jit_insns = 0;
                            for (int i = 0; i < 11; i++)
                                fprintf(g_stats, " %d:%.2f", i, (float)ctr_jit_pass_ticks[i] * 1000.0f / SYSCLOCK_ARM11 / 60);
                            fprintf(g_stats, ")");
                        }
                        memset(ctr_jit_pass_ticks, 0, sizeof(ctr_jit_pass_ticks));
                    }
                    {
                        extern unsigned long long ctr_jit_len_ticks[6];
                        extern unsigned int ctr_jit_len_blocks[6], ctr_jit_len_insns[6];
                        if (ctr_jit_len_blocks[0] + ctr_jit_len_blocks[1] + ctr_jit_len_blocks[2] + ctr_jit_len_blocks[3] +
                            ctr_jit_len_blocks[4] + ctr_jit_len_blocks[5]) {
                            fprintf(g_stats, "\n  compile by length (blocks/insns/ms):");
                            for (int i = 0; i < 6; i++)
                                fprintf(g_stats, " %u/%u/%.1f", ctr_jit_len_blocks[i], ctr_jit_len_insns[i],
                                        (float)ctr_jit_len_ticks[i] * 1000.0f / SYSCLOCK_ARM11);
                        }
                        {
                            extern unsigned long long ctr64_rb_ticks; /* new_dynarec.c remembered blocks */
                            extern unsigned int ctr64_rb_blocks, ctr64_rb_insns, ctr64_rb_stop_time, ctr64_rb_stop_room;
                            if (ctr64_rb_blocks || ctr64_rb_stop_time || ctr64_rb_stop_room)
                                fprintf(g_stats, "\n  remembered blocks compiled between frames: %u blocks, %u insns, %.1f ms (stopped: %u time, %u room)",
                                        ctr64_rb_blocks, ctr64_rb_insns, (float)ctr64_rb_ticks * 1000.0f / SYSCLOCK_ARM11,
                                        ctr64_rb_stop_time, ctr64_rb_stop_room);
                            extern unsigned long long ctr64_bg_ticks, ctr_jit_ct_flush_ticks; /* background precompile */
                            extern unsigned int ctr64_bg_blocks, ctr64_bg_posted, ctr64_bg_stale;
                            if (ctr64_bg_posted || ctr64_bg_blocks)
                                fprintf(g_stats, "\n  precompiled on core 1: %u blocks, %.1f ms compiling + %.1f ms cache (%u posted, %u stale)",
                                        ctr64_bg_blocks, (float)ctr64_bg_ticks * 1000.0f / SYSCLOCK_ARM11,
                                        (float)ctr_jit_ct_flush_ticks * 1000.0f / SYSCLOCK_ARM11, ctr64_bg_posted, ctr64_bg_stale);
                            ctr64_bg_ticks = ctr_jit_ct_flush_ticks = 0;
                            ctr64_bg_blocks = ctr64_bg_posted = ctr64_bg_stale = 0;
                            extern unsigned int ctr64_rb_why[8]; /* why compiled blocks were (not) recorded */
                            fprintf(g_stats, "\n  block recording: %u new, %u listed, %u long, %u kseg, %u unmapped, %u split, %u outside, %u full",
                                    ctr64_rb_why[0], ctr64_rb_why[1], ctr64_rb_why[2], ctr64_rb_why[3],
                                    ctr64_rb_why[4], ctr64_rb_why[5], ctr64_rb_why[6], ctr64_rb_why[7]);
                            memset(ctr64_rb_why, 0, sizeof(ctr64_rb_why));
                            ctr64_rb_ticks = 0;
                            ctr64_rb_blocks = ctr64_rb_insns = ctr64_rb_stop_time = ctr64_rb_stop_room = 0;
                        }
                        memset(ctr_jit_len_ticks, 0, sizeof(ctr_jit_len_ticks));
                        memset(ctr_jit_len_blocks, 0, sizeof(ctr_jit_len_blocks));
                        memset(ctr_jit_len_insns, 0, sizeof(ctr_jit_len_insns));
                        extern unsigned int ctr64_interp_blocks, ctr64_interp_insns; /* new_dynarec.c */
                        extern unsigned int ctr64_ct_posts, ctr64_ct_waits;
                        fprintf(g_stats, "\n  interpreted %u blocks, %u insns; compile thread: %u compiles, %u waits",
                                ctr64_interp_blocks, ctr64_interp_insns, ctr64_ct_posts, ctr64_ct_waits);
                        ctr64_interp_blocks = ctr64_interp_insns = ctr64_ct_posts = ctr64_ct_waits = 0;
                        extern unsigned ctr64_dirty_stat[4]; /* new_dynarec.c get_dirty */
                        fprintf(g_stats, "\n  overwritten code: %u blocks reused, %u changed, %u expiring, %u never compiled",
                                ctr64_dirty_stat[0], ctr64_dirty_stat[1], ctr64_dirty_stat[2], ctr64_dirty_stat[3]);
                        memset(ctr64_dirty_stat, 0, sizeof(ctr64_dirty_stat));
                        extern unsigned int ctr64_dyn_cnt[8], ctr64_icache_absorbs; /* new_dynarec.c, ctr_jit.c */
                        fprintf(g_stats, "\n  dynarec events: get_addr %u, feeds %u, invalidations %u (%u pages, %u whole), blocks invalidated %u, I-cache absorbs %u, flushes %u",
                                ctr64_dyn_cnt[0], ctr64_dyn_cnt[1], ctr64_dyn_cnt[2], ctr64_dyn_cnt[3], ctr64_dyn_cnt[4],
                                ctr64_dyn_cnt[5], ctr64_icache_absorbs, (unsigned)ctr_jit_flush_calls);
                        memset(ctr64_dyn_cnt, 0, sizeof(ctr64_dyn_cnt));
                        extern unsigned int ctr64_junk_stops, ctr64_junk_first, ctr64_junk_first_vi, stop_after_jal;
                        extern unsigned int ctr64_waitloops;
                        fprintf(g_stats, ", wait loops %u", ctr64_waitloops);
                        if (ctr64_junk_stops)
                            fprintf(g_stats, ", junk compiled %u (first %08x at VI %u), stop after calls %u",
                                    ctr64_junk_stops, ctr64_junk_first, ctr64_junk_first_vi, stop_after_jal);
                        extern unsigned int ctr64_slow_rd[512], ctr64_slow_wr[512]; /* new_dynarec.c */
                        fprintf(g_stats, "\n  slow memory accesses (MB region: reads/writes):");
                        for (int k = 0; k < 4; k++) { /* the 4 busiest regions */
                            unsigned best = 0, bi = 0;
                            for (unsigned i = 0; i < 512; i++)
                                if (ctr64_slow_rd[i] + ctr64_slow_wr[i] > best) {
                                    best = ctr64_slow_rd[i] + ctr64_slow_wr[i];
                                    bi = i;
                                }
                            if (!best)
                                break;
                            fprintf(g_stats, " %03x: %u/%u", bi, ctr64_slow_rd[bi], ctr64_slow_wr[bi]);
                            ctr64_slow_rd[bi] = ctr64_slow_wr[bi] = 0;
                        }
                        memset(ctr64_slow_rd, 0, sizeof(ctr64_slow_rd));
                        memset(ctr64_slow_wr, 0, sizeof(ctr64_slow_wr));
                        ctr64_icache_absorbs = 0;
                    }
                    fprintf(g_stats, "\n  alist");
                    for (int i = 0; i < 32; i++)
                        if ((float)ctr64_alist_ticks[i] * 1000.0f / SYSCLOCK_ARM11 / 60 >= 0.01f)
                            fprintf(g_stats, " %02x %.2f", i, (float)ctr64_alist_ticks[i] * 1000.0f / SYSCLOCK_ARM11 / 60);
                    memset(ctr64_alist_ticks, 0, sizeof(ctr64_alist_ticks));
                    fprintf(g_stats, "\n  waits");
                    pica_wait_report(g_stats);
                    {
                        extern unsigned pica_queue_drains; /* pica.c */
                        if (pica_queue_drains)
                            fprintf(g_stats, " (full GX queue drains %u)", pica_queue_drains);
                        pica_queue_drains = 0;
                    }
                    {
                        extern bool ctr64_gfx_dlist_async;
                        extern unsigned ctr64_gfx_wait_ticks;
                        fprintf(g_stats, "\n  gfx thread %s, cpu waited %.2f ms/VI", ctr64_gfx_dlist_async ? "on" : "off",
                                (float)ctr64_gfx_wait_ticks * 1000.0f / SYSCLOCK_ARM11 / 60);
                        extern unsigned ctr64_gfx_wait_why[3];
                        fprintf(g_stats, " (rdp done %.2f, queue full %.2f, drains %.2f)",
                                (float)ctr64_gfx_wait_why[0] * 1000.0f / SYSCLOCK_ARM11 / 60,
                                (float)ctr64_gfx_wait_why[1] * 1000.0f / SYSCLOCK_ARM11 / 60,
                                (float)ctr64_gfx_wait_why[2] * 1000.0f / SYSCLOCK_ARM11 / 60);
                        memset(ctr64_gfx_wait_why, 0, sizeof(ctr64_gfx_wait_why));
                        extern unsigned ctr64_gfx_worker_ticks[3];
                        fprintf(g_stats, "; worker idle %.2f, dlists %.2f, other %.2f ms/VI",
                                (float)ctr64_gfx_worker_ticks[0] * 1000.0f / SYSCLOCK_ARM11 / 60,
                                (float)ctr64_gfx_worker_ticks[1] * 1000.0f / SYSCLOCK_ARM11 / 60,
                                (float)ctr64_gfx_worker_ticks[2] * 1000.0f / SYSCLOCK_ARM11 / 60);
                        memset(ctr64_gfx_worker_ticks, 0, sizeof(ctr64_gfx_worker_ticks));
                        ctr64_gfx_wait_ticks = 0;
                    }
                    if (ctr64_allocfail_bytes) {
                        fprintf(g_stats, "\n  allocfail last: %s %u bytes",
                                ctr64_allocfail_where ? ctr64_allocfail_where : "?", ctr64_allocfail_bytes);
                        ctr64_allocfail_bytes = 0;
                    }
                    {
                        extern unsigned ctr64_gfx_evicted; /* pica_ContextImpl.cpp */
                        if (ctr64_gfx_evicted)
                            fprintf(g_stats, "\n  textures evicted for memory: %u times", ctr64_gfx_evicted);
                        ctr64_gfx_evicted = 0;
                    }
                    fprintf(g_stats, "\n");
                    memset(ctr64_prof_ticks, 0, sizeof(ctr64_prof_ticks));
                    memset(ctr64_prof_calls, 0, sizeof(ctr64_prof_calls));
                }
                fflush(g_stats);
                pica_stats_reset_vblank();
                ctr64_sp_ticks[0] = ctr64_sp_ticks[1] = ctr64_sp_ticks[2] = 0;
            }
            ctr64_gfx_ticks[0] = ctr64_gfx_ticks[1] = ctr64_gfx_ticks[2] = 0;
            memset(ctr64_gfx_stats, 0, sizeof(ctr64_gfx_stats));
            ctr_jit_compiles = 0;
            ctr_jit_compile_ticks = 0;
            ctr_jit_flush_calls = ctr_jit_flush_bytes = 0;
            ctr_jit_flush_ticks = 0;
            if (g_settings.debug_info)
                ui_present(); /* the debug text printed above */
            t_last = now;
            emu_ms = 0;
        }
    }
}

/* VFP RunFast mode: flush denormals to zero and use default NaNs. Without it the ARM11 VFP
 * traps to slow support code on denormal operands, which float-heavy emulation hits constantly.
 * FPSCR is per thread; the core's coroutine runs on this thread. */
static void vfp_enable_runfast(void)
{
    u32 fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24) | (1u << 25); /* FZ | DN */
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
}

/* HOME menu, sleep: the system takes the GPU from the app, so the video worker must be idle */
static void apt_hook(APT_HookType hook, void *param)
{
    (void)param;
    extern void ctr64_gfx_drain(void);
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP || hook == APTHOOK_ONEXIT)
        ctr64_gfx_drain();
    /* new precompile-list blocks (no periodic saves during play). Not on sleep: that hook runs
     * on libctru's APT thread while the emulation and compile threads keep using the list. */
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONEXIT) {
        core_host_rb_save();
        core_host_flush_log();
    }
}

/* emulator state for crash.txt */
static void crash_extra(FILE *f)
{
    extern unsigned ctr64_video_frames;
    fprintf(f, "n64 pc %08lx, VI %u, game loaded %d\n", (u32)*r4300_pc(&g_dev.r4300), ctr64_video_frames,
            ctr64_heartbeat != 0);
}

int main(void)
{
    char rom[300];
    bool new3ds = false;

    vfp_enable_runfast();
    gfxInitDefault();
    ui_set_console(consoleInit(GFX_BOTTOM, NULL));
    crash_install(crash_extra);
    APT_CheckNew3DS(&new3ds);
    osSetSpeedupEnable(true);
    hidScanInput();
    bool gpu_test = (hidKeysHeld() & KEY_SELECT) != 0;

    if (!video_init()) {
        printf("GPU init failed. START: exit\n");
        wait_for_start();
        gfxExit();
        return 1;
    }
    RomBrowserResult choice = ROM_BROWSER_QUIT;
    /* Development aid: <data dir>/autoboot.txt holding a ROM path skips the browser. */
    FILE *ab = gpu_test ? NULL : fopen(DATA_DIR "/autoboot.txt", "r");
    if (ab) {
        if (fgets(rom, sizeof(rom), ab)) {
            rom[strcspn(rom, "\r\n")] = 0;
            if (rom[0])
                choice = ROM_BROWSER_PICKED;
        }
        fclose(ab);
    }
    /* menu "Restart" (settings that apply when a game starts): the game to start again */
    FILE *rl = gpu_test || choice == ROM_BROWSER_PICKED ? NULL : fopen(DATA_DIR "/relaunch.txt", "r");
    if (rl) {
        /* "<time> <ROM path>", honoured for 2 minutes only: a relaunch that did not happen must
         * not start the game the next time ctr64 is opened */
        long long when = 0;
        const long long now = (long long)time(NULL);
        if (fscanf(rl, "%lld ", &when) == 1 && now >= when && now - when < 120 && fgets(rom, sizeof(rom), rl)) {
            rom[strcspn(rom, "\r\n")] = 0;
            if (rom[0])
                choice = ROM_BROWSER_PICKED;
        }
        fclose(rl);
        remove(DATA_DIR "/relaunch.txt");
    }
    settings_load(SETTINGS_PATH); /* the theme for the game list (loaded again for the game) */
    ui_set_dark(g_settings.dark_mode);
    if (choice != ROM_BROWSER_PICKED)
        choice = gpu_test ? ROM_BROWSER_GPU_TEST : rom_browser_pick(ROM_DIR, rom, sizeof(rom));

    if (choice == ROM_BROWSER_GPU_TEST) {
        gpu_test_run();
    } else if (choice == ROM_BROWSER_PICKED) {
        rom_browser_loading(rom);
        core_host_init(DATA_DIR);
        /* the memory reserve (ctr64_mem_prepare) is decided by the anti-aliasing setting before the
         * ROM is loaded; menu_init loads the settings again later */
        settings_load(DATA_DIR "/settings.txt");
        settings_publish();
        if (core_host_load(rom)) {
            s32 prio = 0x30;
            svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
            /* menu: Profiler - the statistics go into the session's profile instead of stats.log */
            ctr64_profiler = g_settings.profiler;
            if (ctr64_profiler && prof_begin(rom) && prof_stats_stream())
                g_stats = prof_stats_stream();
            else
                g_stats = prof_stats_begin(DATA_DIR "/stats.log"); /* written by a background thread */
            menu_init(DATA_DIR "/settings.txt");
            static aptHookCookie apt_cookie;
            aptHook(&apt_cookie, apt_hook, NULL);
            Thread wd = threadCreate(watchdog, NULL, 4096, prio - 4, -2, false);
            run_game();
            g_watchdog_quit = true;
            if (wd) {
                threadJoin(wd, U64_MAX);
                threadFree(wd);
            }
            prof_end(); /* closes the statistics stream too */
            g_stats = NULL;
            core_host_unload();
            {   /* dev switch blockmap.txt: the rest of the block map (ctr_jit.c) */
                extern void ctr_jit_blockmap_flush(void);
                ctr_jit_blockmap_flush();
            }
            FILE *dn = fopen(DATA_DIR "/exitafter.txt", "r"); /* tell the run script (see above) */
            if (dn) {
                fclose(dn);
                dn = fopen(DATA_DIR "/done.txt", "w");
                if (dn)
                    fclose(dn);
            }
        } else {
            if (rom_browser_error(rom))
                g_quit = MENU_GAME_LIST;
        }
        core_host_shutdown();
    }

    /* back to the game list: the app restarts itself, everything starts clean (GLideN64 and the
     * core keep static state that a second game in the same process would inherit). A .3dsx
     * returns to the Homebrew Launcher instead. */
    if (g_quit == MENU_RESTART) {
        FILE *rf = fopen(DATA_DIR "/relaunch.txt", "w");
        if (rf) {
            fprintf(rf, "%lld %s\n", (long long)time(NULL), rom);
            fclose(rf);
        }
    }
    if ((g_quit == MENU_GAME_LIST || g_quit == MENU_RESTART) && !envIsHomebrew())
        aptSetChainloaderToSelf();
    pica_wait_idle();
    video_exit();
    gfxExit();
    return 0;
}
