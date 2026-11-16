/* JIT support for the 3DS ARM dynarec.
 * Generated code executes on core 0. Keep D-cache maintenance process-aware,
 * then invalidate the local I-cache using Luma's kernel-stack callback (0x80).
 * No application VA is submitted to a remote I-cache maintenance worker.
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api/callbacks.h"

#define JIT_BUILD "JIT-LOCAL-20261021-1"
#define ICACHE_ACK 0x4A495430u

static Handle g_process;
static unsigned g_flush_sequence;
u32 ctr_jit_flush_calls, ctr_jit_flush_bytes;
u64 ctr_jit_flush_ticks;
unsigned int ctr_jit_compiles;
unsigned int ctr_jit_insns; /* MIPS instructions compiled */
unsigned int ctr64_debug_messages; /* DebugMessage calls (api/callbacks.c) */
unsigned long long ctr_jit_len_ticks[6];
unsigned int ctr_jit_len_blocks[6], ctr_jit_len_insns[6];
int ctr64_fast_compile_dev = -1; /* fastcompile.txt bits; -1: the menu setting decides (main.c) */
int ctr_jit_stress; /* dev switch jitstress.txt: extra compiles of every block (profiling) */
u64 ctr_jit_compile_ticks; /* time in new_recompile_block */
u64 ctr_jit_ticks(void) { return svcGetSystemTick(); }

/* dev switch blockmap.txt: every compiled block to blockmap.log (host range, MIPS address and
 * code, the emitted ARM code), to attribute profile samples in JIT code */
void *ctr_jit_blockmap_file;
/* Records go to a RAM buffer (binary: b, e, vaddr, n, n MIPS words, the ARM words) and are
 * written out as text only when it is full and at quit (ctr_jit_blockmap_flush): writing to the
 * SD card while compiling stalled both threads in FSFILE_Write on hardware (2026-11-09 SSB
 * sample, the compile thread waited on by core 0 for whole VIs). */
static u32 *g_bm_buf, g_bm_cap, g_bm_n;

static void bm_hex(FILE *f, char tag, const u32 *p, u32 n)
{
    static const char hx[] = "0123456789abcdef";
    char line[9 * 64 + 4];
    fputc(tag, f);
    while (n) {
        u32 k = n > 64 ? 64 : n, o = 0;
        for (u32 i = 0; i < k; i++) {
            u32 v = p[i];
            line[o++] = ' ';
            for (int s = 28; s >= 0; s -= 4)
                line[o++] = hx[(v >> s) & 15];
        }
        fwrite(line, 1, o, f);
        p += k;
        n -= k;
    }
    fputc('\n', f);
}

void ctr_jit_blockmap_flush(void)
{
    {   /* dev switch pclog.txt (new_dynarec.c dynarec_gen_interrupt) */
        extern unsigned int *ctr64_pclog, ctr64_pclog_n;
        if (ctr64_pclog && ctr64_pclog_n) {
            FILE *f = fopen("sdmc:/3ds/ctr64/pclog.log", "w");
            if (f) {
                setvbuf(f, NULL, _IOFBF, 256 * 1024);
                for (unsigned int i = 0; i + 2 < ctr64_pclog_n; i += 3)
                    fprintf(f, "%u %08x %u\n", ctr64_pclog[i], ctr64_pclog[i + 1], ctr64_pclog[i + 2]);
                fclose(f);
            }
            ctr64_pclog_n = 0;
        }
    }
    FILE *f = (FILE *)ctr_jit_blockmap_file;
    if (!f)
        return;
    for (u32 i = 0; i + 4 <= g_bm_n;) {
        const u32 *r = g_bm_buf + i;
        const u32 n = r[3], na = (r[1] - r[0]) / 4;
        fprintf(f, "B %08lx %08lx %08lx %lu\n", (unsigned long)r[0], (unsigned long)r[1],
                (unsigned long)r[2], (unsigned long)n);
        bm_hex(f, 'M', r + 4, n);
        bm_hex(f, 'A', r + 4 + n, na);
        i += 4 + n + na;
    }
    g_bm_n = 0;
    fflush(f);
}

void ctr_jit_blockmap(const void *b, const void *e, unsigned vaddr, const unsigned *src, int n)
{
    if ((u32)e <= (u32)b)
        e = b; /* the code pointer wrapped: range unknown */
    if (!g_bm_buf) {
        for (g_bm_cap = 12u << 20; g_bm_cap >= (1u << 20); g_bm_cap /= 2)
            if ((g_bm_buf = (u32 *)malloc(g_bm_cap)) != NULL)
                break;
        if (!g_bm_buf)
            return;
        g_bm_cap /= 4;
    }
    const u32 na = ((u32)e - (u32)b) / 4, need = 4 + (u32)n + na;
    if (g_bm_n + need > g_bm_cap)
        ctr_jit_blockmap_flush();
    if (need > g_bm_cap)
        return;
    u32 *r = g_bm_buf + g_bm_n;
    r[0] = (u32)b;
    r[1] = (u32)e;
    r[2] = vaddr;
    r[3] = (u32)n;
    memcpy(r + 4, src, (size_t)n * 4);
    memcpy(r + 4 + n, b, (size_t)na * 4);
    g_bm_n += need;
}

static void jit_fail(const char *stage, u32 result)
{
    DebugMessage(M64MSG_ERROR, "%s FAILED: %s (0x%08lX)",
                 JIT_BUILD, stage, (unsigned long)result);
    printf("%s: %s failed (%08lX). See core.log.\n",
           JIT_BUILD, stage, (unsigned long)result);
    exit(EXIT_FAILURE);
}

/* Runs on the calling core's supervisor stack through Luma SVC 0x80.
 * Do not call services, allocate, log, or schedule from this callback.
 * ARM11: drain writes, invalidate all I-cache and branch prediction entries,
 * drain writes again, then flush the prefetch buffer (instruction sync).
 */
static __attribute__((noinline)) u32 icache_local_callback(void)
{
    u32 zero = 0;
    __asm__ volatile(
        "mcr p15, 0, %0, c7, c10, 4\n"
        "mcr p15, 0, %0, c7, c5, 0\n"
        "mcr p15, 0, %0, c7, c5, 6\n"
        "mcr p15, 0, %0, c7, c10, 4\n"
        "mcr p15, 0, %0, c7, c5, 4\n"
        : : "r"(zero) : "memory");
    return ICACHE_ACK;
}

static u32 invalidate_local_icache(void)
{
    register u32 r0 __asm__("r0") = (u32)icache_local_callback;
    __asm__ volatile("svc 0x80" : "+r"(r0) :
                     : "r1", "r2", "r3", "r12", "cc", "memory");
    return r0;
}

/* The same core-local, but only the lines of [g_line_a, g_line_e): clean them from the D-cache,
 * invalidate them in the I-cache, then the branch target cache. Without the kernel's
 * FlushProcessDataCache (~100 us a call on hardware, a third of a block compile) and without
 * emptying the whole I-cache after every block (the compiler and the game's code then run
 * from a cold cache). The L2 of the New 3DS needs nothing: instruction fetches that miss L1
 * read L2, which the clean writes to. Large ranges: the whole caches. */
static u32 g_line_a, g_line_e;
static u32 g_ct_line_a, g_ct_line_e;    /* the same for the compile thread (core 1) */
static bool g_line_flush;
static __attribute__((always_inline)) inline u32 lines_flush(u32 a, u32 e)
{
    u32 zero = 0;
    if (e - a > 16 * 1024) {
        __asm__ volatile(
            "mcr p15, 0, %0, c7, c10, 0\n" /* clean entire D-cache */
            "mcr p15, 0, %0, c7, c10, 4\n"
            "mcr p15, 0, %0, c7, c5, 0\n"  /* invalidate entire I-cache */
            : : "r"(zero) : "memory");
    } else {
        for (u32 p = a; p < e; p += 32)
            __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(p) : "memory"); /* clean D line */
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(zero) : "memory");
        for (u32 p = a; p < e; p += 32)
            __asm__ volatile("mcr p15, 0, %0, c7, c5, 1" : : "r"(p) : "memory"); /* invalidate I line */
    }
    __asm__ volatile(
        "mcr p15, 0, %0, c7, c5, 6\n"  /* branch target cache */
        "mcr p15, 0, %0, c7, c10, 4\n"
        "mcr p15, 0, %0, c7, c5, 4\n"  /* prefetch buffer */
        : : "r"(zero) : "memory");
    return ICACHE_ACK;
}

static __attribute__((noinline)) u32 icache_lines_callback(void)
{
    return lines_flush(g_line_a, g_line_e);
}

static __attribute__((noinline)) u32 ct_lines_callback(void)
{
    return lines_flush(g_ct_line_a, g_ct_line_e);
}

static u32 flush_lines_ct(u32 a, u32 len)
{
    g_ct_line_a = a;
    g_ct_line_e = a + len;
    register u32 r0 __asm__("r0") = (u32)ct_lines_callback;
    __asm__ volatile("svc 0x80" : "+r"(r0) :
                     : "r1", "r2", "r3", "r12", "cc", "memory");
    return r0;
}

static u32 flush_lines_local(u32 a, u32 len)
{
    g_line_a = a;
    g_line_e = a + len;
    register u32 r0 __asm__("r0") = (u32)icache_lines_callback;
    __asm__ volatile("svc 0x80" : "+r"(r0) :
                     : "r1", "r2", "r3", "r12", "cc", "memory");
    return r0;
}

/* SVC 0x93 InvalidateInstructionCacheRange: implemented by Luma3DS and by emulators (Azahar),
 * which do not have Luma's 0x80 backdoor. */
static void invalidate_icache_range(u32 addr, u32 len)
{
    register u32 r0 __asm__("r0") = addr;
    register u32 r1 __asm__("r1") = len;
    __asm__ volatile("svc 0x93" : "+r"(r0) : "r"(r1) : "r2", "r3", "r12", "memory");
}

/* Luma3DS answers GetSystemInfo(0x10000, 0) with its version; stock kernels and emulators fail it. */
static bool g_luma;
static bool detect_luma(void)
{
    s64 version = 0;
    return R_SUCCEEDED(svcGetSystemInfo(&version, 0x10000, 0)) && version != 0;
}

/* Compile thread (new_dynarec.c: compiles while core 0 interprets). It writes code on its own
 * core: there the lines are cleaned from the D-cache (coherent between cores and with the
 * New 3DS L2), and core 0, which runs the code, invalidates its own I-cache before it runs
 * anything that thread compiled (ctr_jit_thread_absorb). */
static Thread g_ct;
static int g_ct_core, g_ct_prio;        /* where it runs; core 2: below the video worker */
static LightEvent g_ct_go, g_ct_done;
static void (*g_ct_work)(void);
static volatile bool g_ct_in;           /* the compile thread is compiling */
int ctr64_jit_thread_want;              /* on-demand compiles on the thread too (dev switch jitthread.txt) */
unsigned long long ctr_jit_ct_flush_ticks; /* cache maintenance on the compile thread (stats.log) */
volatile int ctr_jit_thread_busy;       /* from kick until the compile is done (and its flushes) */
static volatile bool g_ct_icache;       /* it wrote code core 0 has not invalidated yet */
static u32 g_ct_lo = ~0u, g_ct_hi;      /* what it wrote (svc 0x93 path) */
unsigned int ctr64_icache_absorbs;     /* stats.log "dynarec events" */

static void ct_main(void *arg)
{
    (void)arg;
    for (;;) {
        LightEvent_Wait(&g_ct_go);
        g_ct_in = true;
        g_ct_work();
        g_ct_in = false;
        __sync_synchronize();
        ctr_jit_thread_busy = 0;
        LightEvent_Signal(&g_ct_done);
    }
}

/* core 1 for the app's threads (audio, compile, Old 3DS video): the CPU time limit raised to 80%
 * once. On hardware a second APT_SetAppCpuTimeLimit failed after the audio thread had set it
 * (2026-11-09: the compile thread never started there; Azahar accepts it), so the limit is read
 * first and set only when lower. */
int ctr64_core1_ready(void)
{
    static int known = -1;
    if (known >= 0)
        return known;
    u32 cur = 0;
    const Result g = APT_GetAppCpuTimeLimit(&cur);
    Result s1 = 0;
    known = R_SUCCEEDED(g) && cur >= 80;
    if (!known) {
        s1 = APT_SetAppCpuTimeLimit(80);
        known = R_SUCCEEDED(s1);
    }
    /* 2026-11-09 HW: still refused with the read first -> the codes, and core 2 for the rest */
    DebugMessage(M64MSG_INFO, "core 1: time limit read %08lX (%lu percent), set to 80: %08lX -> %s",
                 (unsigned long)g, (unsigned long)cur, (unsigned long)s1, known ? "usable" : "not usable");
    return known;
}

/* starts the compile thread once, on core 1 (time limit 80%); 0 when it does not run.
 * New 3DS: always, for the background precompile of remembered blocks (new_dynarec.c
 * ctr64_bg_feed; dev switch nobgjit.txt: off). On-demand compiles go to it only with the dev
 * switch jitthread.txt: on a New 3DS that made no difference the user could notice at
 * transitions (2026-10-23), and in Azahar it froze SM64 when the audio thread shared core 1
 * with it (not investigated). Old 3DS: only with jitthread.txt (core 1 runs the video). */
int ctr_jit_thread_start(void (*work)(void))
{
    static int tried, ok;
    if (tried)
        return ok;
    tried = 1;
    bool new3ds = false;
    APT_CheckNew3DS(&new3ds);
    FILE *f = fopen("sdmc:/3ds/ctr64/jitthread.txt", "r");
    ctr64_jit_thread_want = f != NULL;
    if (f)
        fclose(f);
    FILE *nb = fopen("sdmc:/3ds/ctr64/nobgjit.txt", "r");
    if (nb)
        fclose(nb);
    if (!ctr64_jit_thread_want && (!new3ds || nb))
        return 0;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_ct_core = 1;
    g_ct_prio = new3ds ? prio - 1 : prio + 1;
    FILE *c2 = fopen("sdmc:/3ds/ctr64/ctcore2.txt", "r"); /* development: core 2 path */
    if (c2)
        fclose(c2);
    if (c2 || !ctr64_core1_ready()) {
        /* New 3DS without core 1 (the 2026-11-09 HW logs): core 2, the video worker's (idle
         * 40-50% of the time), at the lowest priority there; raised while core 0 waits for it */
        if (!new3ds) {
            DebugMessage(M64MSG_INFO, "%s compile thread: no CPU time on core 1", JIT_BUILD);
            return 0;
        }
        g_ct_core = 2;
        g_ct_prio = prio + 2;
    }
    g_ct_work = work;
    LightEvent_Init(&g_ct_go, RESET_ONESHOT);
    LightEvent_Init(&g_ct_done, RESET_STICKY);
    g_ct = threadCreate(ct_main, NULL, 128 * 1024, g_ct_prio, g_ct_core, false);
    ok = g_ct != NULL;
    DebugMessage(M64MSG_INFO, "%s compile thread: %s", JIT_BUILD,
                 !ok ? "failed" : g_ct_core == 1 ? "core 1" : "core 2 (low priority)");
    return ok;
}

int ctr_jit_on_compile_thread(void)
{
    return g_ct && threadGetCurrent() == g_ct;
}

void ctr_jit_thread_kick(void)
{
    LightEvent_Clear(&g_ct_done);
    ctr_jit_thread_busy = 1;
    __sync_synchronize();
    LightEvent_Signal(&g_ct_go);
}

void ctr_jit_thread_wait(void)
{
    if (ctr_jit_thread_busy) {
        /* on core 2 it may be waiting behind the video worker: above it until done */
        const bool boost = g_ct_core == 2;
        if (boost)
            svcSetThreadPriority(threadGetHandle(g_ct), g_ct_prio - 5);
        LightEvent_Wait(&g_ct_done);
        if (boost)
            svcSetThreadPriority(threadGetHandle(g_ct), g_ct_prio);
    }
    __sync_synchronize();
}

/* core 0: invalidate what the compile thread wrote, before running it */
void ctr_jit_thread_absorb(void)
{
    if (!g_ct_icache)
        return;
    g_ct_icache = false;
    ctr64_icache_absorbs++;
    if (g_luma) {
        u32 ack = invalidate_local_icache();
        if (ack != ICACHE_ACK)
            jit_fail("local I-cache callback", ack);
    } else {
        invalidate_icache_range(g_ct_lo, g_ct_hi - g_ct_lo);
    }
    g_ct_lo = ~0u;
    g_ct_hi = 0;
}

void ctr_jit_flush(void *start, void *end)
{
    if ((u32)end <= (u32)start)
        return;
    u32 a = (u32)start & ~31u;
    u32 len = ((u32)end - a + 31u) & ~31u;
    unsigned sequence = ++g_flush_sequence;
    bool trace = sequence <= 8;
    u64 t0 = svcGetSystemTick();
    if (g_ct_in && ctr_jit_on_compile_thread()) {
        /* compile thread: D-cache here, I-cache on core 0 later */
        if (g_line_flush) {
            if (flush_lines_ct(a, len) != ICACHE_ACK)
                jit_fail("local cache line callback", 0);
        } else {
            Result rc = svcFlushProcessDataCache(CUR_PROCESS_HANDLE, a, len);
            if (R_FAILED(rc))
                jit_fail("process D-cache flush", (u32)rc);
        }
        if (a < g_ct_lo) g_ct_lo = a;
        if (a + len > g_ct_hi) g_ct_hi = a + len;
        g_ct_icache = true;
        ctr_jit_ct_flush_ticks += svcGetSystemTick() - t0;
        return;
    }
    if (g_luma) {
        s32 core = svcGetProcessorID();
        if (core != 0)
            jit_fail("JIT thread left core 0", (u32)core);
    }
    if (g_line_flush) {
        u32 ack = flush_lines_local(a, len);
        if (ack != ICACHE_ACK)
            jit_fail("local cache line callback", ack);
        ctr_jit_flush_calls++;
        ctr_jit_flush_bytes += len;
        ctr_jit_flush_ticks += svcGetSystemTick() - t0;
        return;
    }
    if (trace)
        DebugMessage(M64MSG_INFO, "%s flush %u: %08lX + %lu, before D-cache",
                     JIT_BUILD, sequence, (unsigned long)a, (unsigned long)len);
    Result rc = svcFlushProcessDataCache(CUR_PROCESS_HANDLE, a, len);
    if (R_FAILED(rc))
        jit_fail("process D-cache flush", (u32)rc);
    if (trace)
        DebugMessage(M64MSG_INFO, "%s flush %u: D-cache OK, before I-cache",
                     JIT_BUILD, sequence);
    if (g_luma) {
        u32 ack = invalidate_local_icache();
        if (ack != ICACHE_ACK)
            jit_fail("local I-cache callback", ack);
    } else {
        invalidate_icache_range(a, len);
    }
    if (trace)
        DebugMessage(M64MSG_INFO, "%s flush %u: local I-cache OK", JIT_BUILD, sequence);
    ctr_jit_flush_calls++;
    ctr_jit_flush_bytes += len;
    ctr_jit_flush_ticks += svcGetSystemTick() - t0;
}

/* The by-line path, checked on this console before use: 64 stubs spread over 8 KB, each run
 * (so the I-cache holds them), rewritten and flushed one line range at a time, run again. A
 * stale line returns the old value; the kernel path is then kept. */
static bool line_flush_selftest(void *base)
{
    enum { N = 64, STEP = 128 };
    static u32 saved[N * STEP / 4];
    volatile u32 *code = (volatile u32 *)base;
    for (u32 i = 0; i < N * STEP / 4; i++)
        saved[i] = code[i];
    bool ok = true;
    g_line_flush = true;
    for (int pass = 0; pass < 2 && ok; pass++) {
        for (u32 i = 0; i < N; i++) {
            volatile u32 *c = code + i * STEP / 4;
            c[0] = 0xE3A00000 | ((i + pass * 100) & 0xFF); /* mov r0, #value */
            c[1] = 0xE12FFF1E;                            /* bx lr */
            ctr_jit_flush((void *)c, (void *)(c + 2));
        }
        for (u32 i = 0; i < N && ok; i++) {
            int (*probe)(void) = (int (*)(void))(code + i * STEP / 4);
            ok = probe() == (int)((i + pass * 100) & 0xFF);
        }
    }
    for (u32 i = 0; i < N * STEP / 4; i++)
        code[i] = saved[i];
    g_line_flush = false;
    ctr_jit_flush(base, (char *)base + N * STEP);
    if (!ok)
        DebugMessage(M64MSG_WARNING, "%s by-line cache maintenance failed its self-test", JIT_BUILD);
    return ok;
}

void ctr_jit_init(void *base, unsigned int size)
{
    DebugMessage(M64MSG_INFO, "%s init: cache %p, %u bytes", JIT_BUILD, base, size);
    {   /* dev switch spnocheck.txt: stack-relative stores skip the compiled-code check (new_dynarec.c) */
        extern int ctr64_sp_nocheck;
        FILE *sc = fopen("sdmc:/3ds/ctr64/spnocheck.txt", "r");
        ctr64_sp_nocheck = sc != NULL;
        if (sc)
            fclose(sc);
    }
    if (((u32)base & 4095u) || (size & 4095u) || size < 4096)
        jit_fail("cache alignment/size", size);

    /* libco switches stacks on this OS thread; both JIT emission and execution
     * stay here. Local invalidation is only sufficient with this affinity. */
    /* The CIA exheader already pins this thread to core 0. The stock kernel's
     * SetThreadAffinityMask is unimplemented; verify the existing bitmask. */
    g_luma = detect_luma();
    FILE *st = fopen("sdmc:/3ds/ctr64/jitstress.txt", "r");
    if (st) {
        ctr_jit_stress = 20;
        fclose(st);
    }
    {   /* dev switch fastcompile.txt: optional compiler passes to skip (bits, new_dynarec.c pass 5),
         * instead of the menu's Fast compile (main.c) */
        FILE *fc = fopen("sdmc:/3ds/ctr64/fastcompile.txt", "r");
        if (fc) {
            if (fscanf(fc, "%d", &ctr64_fast_compile_dev) != 1)
                ctr64_fast_compile_dev = 15;
            fclose(fc);
        }
    }
    if (!ctr_jit_blockmap_file) {
        ctr_jit_blockmap_file = fopen("sdmc:/3ds/ctr64/blockmap.txt", "r") ? fopen("sdmc:/3ds/ctr64/blockmap.log", "w") : NULL;
        if (ctr_jit_blockmap_file) /* written at quit: big writes (small ones took minutes on the SD card) */
            setvbuf((FILE *)ctr_jit_blockmap_file, NULL, _IOFBF, 512 * 1024);
    }
    {   /* dev switch pclog.txt: N64 PC per event, 1M samples (new_dynarec.c, written at quit) */
        extern unsigned int *ctr64_pclog, ctr64_pclog_max;
        FILE *pl = fopen("sdmc:/3ds/ctr64/pclog.txt", "r");
        if (pl) {
            fclose(pl);
            if (!ctr64_pclog) {
                ctr64_pclog = malloc(8u << 20);
                ctr64_pclog_max = ctr64_pclog ? (8u << 20) / 4 : 0;
            }
        }
    }
    {   /* dev switch deferfps.txt: games at up to this many frames/s get deferred RDP-done waits */
        extern unsigned ctr64_gfx_defer_fps;
        FILE *df = fopen("sdmc:/3ds/ctr64/deferfps.txt", "r");
        if (df) {
            unsigned v = 0;
            if (fscanf(df, "%u", &v) == 1 && v <= 60)
                ctr64_gfx_defer_fps = v;
            fclose(df);
        }
    }
    {   /* dev switch nodefer.txt: no deferred RDP-done waits for slow games (gfx_async.c) */
        extern bool ctr64_gfx_nodefer;
        FILE *nd = fopen("sdmc:/3ds/ctr64/nodefer.txt", "r");
        if (nd) {
            ctr64_gfx_nodefer = true;
            fclose(nd);
        }
    }
    {   /* dev switch nowaitskip.txt: wait loops run every cycle (new_dynarec.c ctr64_waitloop) */
        extern int ctr64_waitskip;
        FILE *ws = fopen("sdmc:/3ds/ctr64/nowaitskip.txt", "r");
        if (ws) {
            ctr64_waitskip = 0;
            fclose(ws);
        }
    }
    {   /* dev switch jalstop.txt: blocks end after calls again (the old 3DS rule, new_dynarec.c) */
        extern int ctr64_jal_stop;
        FILE *js = fopen("sdmc:/3ds/ctr64/jalstop.txt", "r");
        if (js) {
            ctr64_jal_stop = 1;
            fclose(js);
        }
    }
    extern int ctr64_interp_tier; /* new_dynarec.c: interpret first (off); dev switch tier.txt: on */
    FILE *nt = fopen("sdmc:/3ds/ctr64/tier.txt", "r");
    if (nt) {
        ctr64_interp_tier = 1;
        fclose(nt);
    }
    DebugMessage(M64MSG_INFO, "%s I-cache path: %s", JIT_BUILD,
                 g_luma ? "Luma3DS core-local (svc 0x80)" : "svc 0x93 range");
    Result rc;
    if (g_luma) {
        u8 actual = 0;
        rc = svcGetThreadAffinityMask(&actual, CUR_THREAD_HANDLE, 4);
        if (R_FAILED(rc)) jit_fail("read JIT affinity", (u32)rc);
        if (actual != 1)
            jit_fail("verify JIT affinity", actual);
    }

    if (!g_process) {
        rc = svcDuplicateHandle(&g_process, CUR_PROCESS_HANDLE);
        if (R_FAILED(rc)) jit_fail("duplicate process handle", (u32)rc);
    }
    DebugMessage(M64MSG_INFO, "%s before executable permission", JIT_BUILD);
    rc = svcControlProcessMemory(g_process, (u32)base, 0, size, MEMOP_PROT,
                                 MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
    if (R_FAILED(rc)) jit_fail("JIT executable permission", (u32)rc);
    DebugMessage(M64MSG_INFO, "%s executable permission OK; self-test begins", JIT_BUILD);

    /* Test executable permissions AND instruction coherency on the actual
     * hardware before attempting to execute an N64 dynarec block. */
    volatile u32 *code = (volatile u32 *)base;
    u32 saved0 = code[0], saved1 = code[1];
    int (*probe)(void) = (int (*)(void))base;
    code[0] = 0xE3A0002A; /* ARM mov r0, #42 */
    code[1] = 0xE12FFF1E; /* ARM bx lr */
    ctr_jit_flush(base, (char *)base + 8);
    DebugMessage(M64MSG_INFO, "%s before self-test execution 42", JIT_BUILD);
    int value = probe();
    if (value != 42) jit_fail("execute generated code", (u32)value);
    code[0] = 0xE3A0002B; /* rewrite same cached instruction: mov r0, #43 */
    ctr_jit_flush(base, (char *)base + 8);
    DebugMessage(M64MSG_INFO, "%s before self-test execution 43", JIT_BUILD);
    value = probe();
    if (value != 43) jit_fail("execute rewritten code", (u32)value);
    code[0] = saved0;
    code[1] = saved1;
    ctr_jit_flush(base, (char *)base + 8);
    DebugMessage(M64MSG_INFO, "%s SELF-TEST PASS: executed 42 then rewritten 43", JIT_BUILD);
    if (g_luma)
        g_line_flush = line_flush_selftest(base);
    DebugMessage(M64MSG_INFO, "%s cache maintenance: %s", JIT_BUILD,
                 g_line_flush ? "core-local by line" : "kernel D-cache flush + whole I-cache");
}
