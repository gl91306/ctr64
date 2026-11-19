/* Calibration benchmark (Detailed profiler): fixed CPU, memory and GPU workloads, timed once at
 * the start of a profiled session and written into the profile as "# bench" lines:
 *   # bench <name> <microseconds> <units> <unit>
 * The same build run in Azahar writes the same lines, so tools/prof/hwcmp.py can compare the
 * console with the emulator workload by workload (CPU clock, cache misses, GPU fill rate, copy
 * bandwidth, vertex cost) and the dev Azahar's timing model can be tuned to the console. */
#include "bench.h"

#include <stdlib.h>
#include <string.h>

#include "pica/pica.h"
#include "video.h"

static FILE *g_out;
static volatile u32 g_sink;

static double to_us(u64 ticks)
{
    return (double)ticks / (SYSCLOCK_ARM11 / 1000000.0);
}

static void report(const char *name, u64 ticks, double units, const char *unit)
{
    fprintf(g_out, "# bench %s %.1f %.0f %s\n", name, to_us(ticks), units, unit);
}

/* ---- CPU and memory ------------------------------------------------------------------------ */

static void cpu_benches(void)
{
    /* integer work, no memory: the CPU clock and instruction cost */
    u64 t0 = svcGetSystemTick();
    u32 x = 1;
    for (u32 i = 0; i < 4000000; i++) {
        x = x * 1664525u + 1013904223u;
        x ^= x >> 13;
    }
    g_sink = x;
    report("cpu_alu", svcGetSystemTick() - t0, 4000000, "iterations");

    /* the profiler's clock: one kernel call per read (two per CTR64_PROF scope) */
    t0 = svcGetSystemTick();
    u64 acc = 0;
    for (u32 i = 0; i < 100000; i++)
        acc += svcGetSystemTick();
    g_sink = (u32)acc;
    report("cpu_systemtick", svcGetSystemTick() - t0, 100000, "reads");

    {   /* GLideN64's Conker vertex path (gSP.cpp): generic 1 and 4 at a time, and the fast path;
         * the video thread is drained first (the bench borrows the gSP state) */
        extern void ctr64_gfx_drain(void);
        extern int ctr64_bench_cbfd(unsigned long long ticks[3], unsigned reps);
        ctr64_gfx_drain();
        unsigned long long t[3];
        const unsigned reps = 300; /* 32 vertices each */
        const int same = ctr64_bench_cbfd(t, reps);
        report("vtx_cbfd_generic1", t[0], reps * 32.0, "vertices");
        report("vtx_cbfd_generic4", t[1], reps * 32.0, "vertices");
        report(same ? "vtx_cbfd_fast_same" : "vtx_cbfd_fast_DIFFERENT", t[2], reps * 32.0, "vertices");
    }
    {   /* GLideN64's standard vertex path (gSP.cpp): generic 4 at a time against the fast path,
         * unlit and lit (2 lights) */
        extern int ctr64_bench_std(unsigned long long ticks[4], unsigned reps);
        unsigned long long t[4];
        const unsigned reps = 300;
        const int same = ctr64_bench_std(t, reps);
        report("vtx_std_generic_unlit", t[0], reps * 32.0, "vertices");
        report("vtx_std_fast_unlit", t[1], reps * 32.0, "vertices");
        report("vtx_std_generic_lit", t[2], reps * 32.0, "vertices");
        report(same ? "vtx_std_fast_lit_same" : "vtx_std_fast_lit_DIFFERENT", t[3], reps * 32.0, "vertices");
    }

    const u32 mb = 1024 * 1024;
    u8 *a = malloc(2 * mb);
    if (a) {
        memset(a, 1, 2 * mb);
        t0 = svcGetSystemTick();
        for (int i = 0; i < 8; i++)
            memcpy(a + mb, a, mb);
        report("cpu_memcpy_heap", svcGetSystemTick() - t0, 8 * mb, "bytes");
        /* dependent random reads over 2 MB: cache misses */
        t0 = svcGetSystemTick();
        u32 idx = 0, sum = 0;
        for (u32 i = 0; i < 500000; i++) {
            idx = (idx * 1103515245u + 12345u + sum) & (2 * mb - 4);
            sum += a[idx];
        }
        g_sink = sum;
        report("cpu_random_reads", svcGetSystemTick() - t0, 500000, "reads");
        t0 = svcGetSystemTick();
        for (int i = 0; i < 8; i++)
            pica_cache_flush(a, mb);
        report("cpu_cache_flush", svcGetSystemTick() - t0, 8 * mb, "bytes");
        free(a);
    }
    u8 *l = linearAlloc(2 * mb);
    if (l) {
        memset(l, 1, 2 * mb);
        t0 = svcGetSystemTick();
        for (int i = 0; i < 8; i++)
            memcpy(l + mb, l, mb);
        report("cpu_memcpy_linear", svcGetSystemTick() - t0, 8 * mb, "bytes");
        linearFree(l);
    }
}

/* ---- GPU ----------------------------------------------------------------------------------- */

enum { TS = 256 };

/* everything recorded since the last flush runs on the GPU; returns the time until it is done */
static u64 gpu_run(void)
{
    const u64 t0 = svcGetSystemTick();
    pica_flush();
    pica_wait_idle();
    return svcGetSystemTick() - t0;
}

static void frame_close(void)
{
    pica_clear(video_screen(), pica_rgba8(0, 0, 0, 0xFF), false, 0);
    pica_frame_end(video_screen(), GFX_TOP);
    pica_wait_idle();
}

static void state_2d(const PicaTarget *t, const float proj[4][4], const void *tex, GPU_TEXCOLOR fmt)
{
    pica_bind_target(t);
    pica_viewport(0, 0, t->width, t->height);
    pica_scissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    pica_depth_test(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    pica_depth_map(-1.0f, 0.0f);
    pica_cull(GPU_CULL_NONE);
    pica_alpha_test(false, GPU_ALWAYS, 0);
    pica_blend_off();
    video_use_program(proj);
    PicaTev tev;
    pica_tev_default(&tev);
    if (tex) {
        pica_texture(0, tex, TS, TS, fmt,
                     GPU_TEXTURE_MAG_FILTER(GPU_NEAREST) | GPU_TEXTURE_MIN_FILTER(GPU_NEAREST) |
                         GPU_TEXTURE_WRAP_S(GPU_REPEAT) | GPU_TEXTURE_WRAP_T(GPU_REPEAT));
        tev.src_rgb = tev.src_alpha = GPU_TEVSOURCES(GPU_TEXTURE0, 0, 0);
    } else {
        pica_texture(0, NULL, 0, 0, GPU_RGBA8, 0);
        tev.src_rgb = tev.src_alpha = GPU_TEVSOURCES(GPU_PRIMARY_COLOR, 0, 0);
    }
    pica_tev(0, &tev);
}

/* n full-target quads (n * 65536 fragments) */
static void quads(int n)
{
    VideoBatch b;
    if (!video_batch_begin(&b, 6))
        return;
    video_batch_quad4(&b, 0, 0, TS, TS, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0, 0, 1, 1);
    for (int i = 0; i < n; i++)
        video_batch_draw(&b);
}

static void gpu_benches(void)
{
    if (pica_in_frame())
        return; /* the game's video already owns the frame: no GPU part */
    PicaTarget tv = {0}, tv2 = {0}, tl = {0};
    const bool have_v = pica_target_create(&tv, TS, TS, GPU_RB_RGBA8, true, VRAM_ALLOC_ANY);
    const bool have_v2 = pica_target_create(&tv2, TS, TS, GPU_RB_RGBA8, false, VRAM_ALLOC_ANY);
    const bool have_l = pica_target_create_linear(&tl, TS, TS, GPU_RB_RGBA8);
    const u32 tb = TS * TS * 4;
    u32 *tex = linearMemAlign(tb, 128);
    u8 *lin = linearAlloc(2 * tb);
    float proj[4][4];
    pica_mtx_ortho(proj, 0, TS, TS, 0, 0.0f, 1.0f);
    if (tex) {
        for (u32 i = 0; i < TS * TS; i++)
            tex[i] = pica_rgba8((u8)i, (u8)(i >> 8), (u8)(i * 7), 0xFF);
        pica_cache_flush(tex, tb);
    }
    if (lin) {
        memset(lin, 0x55, 2 * tb);
        pica_cache_flush(lin, 2 * tb);
    }
    u64 t;

    /* the round trip of one small command list */
    if (have_v) {
        u64 sum = 0;
        for (int i = 0; i < 8; i++) {
            pica_frame_begin();
            state_2d(&tv, proj, NULL, GPU_RGBA8);
            quads(0);
            sum += gpu_run();
            frame_close();
        }
        report("gpu_submit_latency", sum, 8, "submits");
    }
    if (have_v) {
        pica_frame_begin();
        for (int i = 0; i < 16; i++)
            pica_clear(&tv, pica_rgba8(i, 0, 0, 0xFF), false, 0);
        report("gpu_fill_vram", gpu_run(), 16.0 * tb, "bytes");
        for (int i = 0; i < 16; i++)
            pica_clear_depth(&tv, 0);
        report("gpu_fill_depth", gpu_run(), 16.0 * tb, "bytes");
        frame_close();
    }
    if (lin) {
        pica_frame_begin();
        for (int i = 0; i < 8; i++)
            pica_copy(lin + tb, lin, tb);
        report("gpu_copy_fcram_fcram", gpu_run(), 8.0 * tb, "bytes");
        if (have_v) {
            for (int i = 0; i < 8; i++)
                pica_copy(tv.color, lin, tb);
            report("gpu_copy_fcram_vram", gpu_run(), 8.0 * tb, "bytes");
            for (int i = 0; i < 8; i++)
                pica_copy_from_vram(lin, tv.color, tb);
            report("gpu_copy_vram_fcram", gpu_run(), 8.0 * tb, "bytes");
        }
        frame_close();
        if (have_v) {
            t = svcGetSystemTick();
            for (int i = 0; i < 4; i++)
                pica_untile_rgba8(tv.color, TS, TS, lin);
            report("gpu_untile_vram_fcram", svcGetSystemTick() - t, 4.0 * tb, "bytes");
        }
    }
    const double frags = 32.0 * TS * TS;
    if (have_v) {
        pica_frame_begin();
        state_2d(&tv, proj, NULL, GPU_RGBA8);
        quads(32);
        report("gpu_draw_flat_vram", gpu_run(), frags, "fragments");
        pica_blend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
        quads(32);
        report("gpu_draw_blend_vram", gpu_run(), frags, "fragments");
        pica_blend_off();
        {   /* opaque with the framebuffer reads off (PICA200 report V.1): what skipping them saves */
            const u32 noread[4] = {0, 0xF, 0, 0};
            pica_fb_access(noread);
            quads(32);
            report("gpu_draw_flat_vram_noread", gpu_run(), frags, "fragments");
            const u32 nodepth[4] = {0xF, 0xF, 0, 0};
            pica_fb_access(nodepth);
            quads(32);
            report("gpu_draw_flat_vram_nodepth", gpu_run(), frags, "fragments");
            const u32 all[4] = {0xF, 0xF, 0x3, 0x3};
            pica_fb_access(all);
        }
        pica_depth_test(true, GPU_LESS, GPU_WRITE_ALL);
        quads(32);
        report("gpu_draw_depth_vram", gpu_run(), frags, "fragments");
        if (tex) {
            state_2d(&tv, proj, tex, GPU_RGBA8);
            quads(32);
            report("gpu_draw_tex_fcram_vram", gpu_run(), frags, "fragments");
        }
        if (have_v2) {
            state_2d(&tv, proj, tv2.color, GPU_RGBA8);
            quads(32);
            report("gpu_draw_tex_vram_vram", gpu_run(), frags, "fragments");
        }
        frame_close();
    }
    if (have_l) {
        pica_frame_begin();
        state_2d(&tl, proj, NULL, GPU_RGBA8);
        quads(32);
        report("gpu_draw_flat_fcram", gpu_run(), frags, "fragments");
        if (tex) {
            state_2d(&tl, proj, tex, GPU_RGBA8);
            quads(32);
            report("gpu_draw_tex_fcram_fcram", gpu_run(), frags, "fragments");
        }
        frame_close();
    }
    /* vertex cost: 4000 one-pixel triangles drawn 8 times */
    if (have_v) {
        pica_frame_begin();
        state_2d(&tv, proj, NULL, GPU_RGBA8);
        VideoBatch b;
        if (video_batch_begin(&b, 3 * 4000)) {
            for (int i = 0; i < 4000; i++) {
                const float x = (float)(i % 250), y = (float)(i / 250);
                video_batch_vertex(&b, x, y, 0xFFFFFF, 0, 0);
                video_batch_vertex(&b, x, y + 1, 0xFFFFFF, 0, 0);
                video_batch_vertex(&b, x + 1, y + 1, 0xFFFFFF, 0, 0);
            }
            for (int i = 0; i < 8; i++)
                video_batch_draw(&b);
            report("gpu_vertices", gpu_run(), 8.0 * 3 * 4000, "vertices");
            /* the same vertices read from VRAM: is the vertex cost the FCRAM fetch? */
            const u32 vb = (u32)sizeof(VideoVertex) * 3 * 4000;
            void *vv = vramAlloc((vb + 15) & ~15u);
            if (vv) {
                pica_copy(vv, b.v, (vb + 15) & ~15u);
                pica_wait_idle(); /* the copy is not part of the time */
                pica_vertex_buffer(vv);
                for (int i = 0; i < 8; i++)
                    pica_draw_arrays(GPU_TRIANGLES, 0, 3 * 4000);
                report("gpu_vertices_vram", gpu_run(), 8.0 * 3 * 4000, "vertices");
            }
            frame_close();
            if (vv)
                vramFree(vv);
        } else {
            frame_close();
        }
        /* indexed: 4000 one-pixel-high triangles as 2000 quads sharing their edge vertices (2002
         * vertices, 12000 indices); the post-vertex cache serves the repeats */
        pica_frame_begin();
        state_2d(&tv, proj, NULL, GPU_RGBA8);
        VideoBatch g;
        u16 *ix = NULL;
        if (video_batch_begin(&g, 2 * 2001) && (ix = pica_frame_alloc(12000 * 2, 16))) {
            for (int k = 0; k <= 2000; k++) {
                const float x = (float)(k % 250), y = (float)(2 * (k / 250));
                video_batch_vertex(&g, x, y, 0xFFFFFF, 0, 0);
                video_batch_vertex(&g, x, y + 1, 0xFFFFFF, 0, 0);
            }
            for (int k = 0; k < 2000; k++) {
                u16 *q = ix + 6 * k, v = (u16)(2 * k);
                q[0] = v; q[1] = v + 1; q[2] = v + 2;
                q[3] = v + 1; q[4] = v + 3; q[5] = v + 2;
            }
            pica_vertex_buffer(g.v);
            for (int i = 0; i < 8; i++)
                pica_draw_elements(GPU_TRIANGLES, ix, 12000);
            report("gpu_vertices_indexed", gpu_run(), 8.0 * 12000, "indices");
        }
        frame_close();
    }
    if (lin)
        linearFree(lin);
    if (tex)
        linearFree(tex);
    if (have_l)
        pica_target_destroy(&tl);
    if (have_v2)
        pica_target_destroy(&tv2);
    if (have_v)
        pica_target_destroy(&tv);
}

void bench_run(FILE *out)
{
    g_out = out;
    const u64 t0 = svcGetSystemTick();
    cpu_benches();
    gpu_benches();
    fprintf(out, "# bench total %.1f 1 run\n", to_us(svcGetSystemTick() - t0));
    fflush(out);
}
