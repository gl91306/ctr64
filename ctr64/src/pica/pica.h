#pragma once
#include <stdio.h>

/* ctr64 PICA200 driver layer.
 *
 * A thin, explicit layer over libctru's GPUCMD_* / GX_* calls (no citro3d, no picaGL):
 *   - two frame slots, each with its own command buffer and linear-heap scratch pool
 *   - one continuously running GX queue; per-frame fences come from completed-entry counts,
 *     so the CPU builds frame N+1 while the GPU executes frame N
 *   - register state is shadowed and only re-emitted when it changes
 *
 * All state setters must be called between pica_frame_begin() and pica_frame_end(). */

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- render targets -------------------------------------------------------------------- */

typedef struct {
    void *color;            /* VRAM, PICA 8x8-tiled */
    void *depth;            /* VRAM or NULL */
    u16 width, height;      /* allocation size; a power of two if the target is sampled */
    GPU_COLORBUF color_fmt;
    GPU_DEPTHBUF depth_fmt;
} PicaTarget;

bool pica_target_create(PicaTarget *t, u16 width, u16 height, GPU_COLORBUF color_fmt,
                        bool with_depth, vramAllocPos bank);
bool pica_target_create_linear(PicaTarget *t, u16 width, u16 height, GPU_COLORBUF color_fmt);
void pica_target_destroy(PicaTarget *t);

/* ---- device / frame ------------------------------------------------------------------- */

bool pica_init(void);
void pica_exit(void);
/* the N64 VI's gamma on the top screen (the LCD's colour lookup table) */
void pica_lcd_gamma(bool on);
/* Blocks until the GPU has finished everything queued (e.g. before freeing GPU memory). */
void pica_wait_idle(void);

void pica_frame_begin(void);
/* Ends the frame, transfers `screen` (a 240x400-style portrait target) to the LCD and swaps. */
void pica_frame_end(const PicaTarget *screen, gfxScreen_t lcd);
/* the newest top-screen image (BGR8, 240x400 as the LCD scans it), NULL before the first; for the
 * remote screenshot while paused */
const void *pica_top_latest(unsigned *bytes);

/* Per-frame GPU-visible scratch memory (vertices, indices). Valid until this slot is reused. */
void *pica_frame_alloc(size_t size, size_t align);

bool pica_in_frame(void);
/* Number of frames begun. When frame n begins, frames up to n - 2 have completed on the GPU
 * (there are two command slots), so their resources can be reused. */
u32 pica_frame_number(void);
/* Guarantees room for `cmd_bytes` of commands and `pool_bytes` of scratch memory in the current
 * frame. If the slot is full, work so far is submitted and the slot is recycled once the GPU has
 * caught up (rare: only for very long frames). Register state is preserved. */
void pica_reserve(u32 cmd_bytes, size_t pool_bytes);
/* whether cmd_bytes of commands and pool_bytes of pica_frame_alloc fit without a restart */
bool pica_room(u32 cmd_bytes, size_t pool_bytes);
/* every register the state layer shadows, saved and put back around a slipped-in draw */
void pica_state_save(void);
void pica_state_restore(void);
/* Submits everything recorded so far without ending the frame (e.g. before a readback). */
void pica_flush(void);
/* CPU cache maintenance for memory the GPU reads (flush: clean + invalidate) or wrote
 * (invalidate). Direct kernel calls; GSPGPU_FlushDataCache is an IPC to GSP, which makes the
 * same kernel call on our behalf. */
void pica_cache_flush(const void *p, u32 size);
void pica_cache_invalidate(const void *p, u32 size);
/* Readback: untiles `rows` rows (a multiple of 8, from row 0 of the tiled layout) of a
 * `width`-texel RGBA8 surface into linear memory `dst` with the GPU transfer engine, after
 * everything recorded so far, and waits for it. `dst` comes from linearAlloc. */
void pica_untile_rgba8(const void *src, u16 width, u16 rows, void *dst);
/* Queued untile without waiting; then pica_fence() names it: pica_wait_fence(f) +
 * pica_cache_invalidate(dst, ...) before reading dst */
void pica_untile_async(const void *src, u16 width, u16 rows, void *dst, u32 bpp);
u64 pica_fence(void);
void pica_wait_fence(u64 fence);
void pica_clear_depth(const PicaTarget *t, u32 depth24);
/* zeroes VRAM (or linear memory) with the fill engine, after the work already recorded */
void pica_fill_zero(void *p, u32 bytes);
/* start-up check: does GX_MemoryFill reach FCRAM (result in pica_fill_report) */
void pica_fill_selftest(void);
extern char pica_fill_report[96];
extern bool pica_early_clear;
void pica_copy(void *dst, const void *src, u32 bytes);
void pica_copy_from_vram(void *dst, const void *src, u32 bytes); /* no CPU cache flush of src */
void pica_copy_gap(void *dst, const void *src, u32 bytes, u32 run16, u32 gap16);
/* bytes in lines of run16*16 bytes, skipping in_gap16*16 bytes after each source line and
 * out_gap16*16 after each destination line */
void pica_copy_rect(void *dst, const void *src, u32 bytes, u32 run16, u32 in_gap16, u32 out_gap16);
void pica_downscale2(const void *src, u16 width, u16 rows, void *dst, u32 bpp); /* tiled -> linear, 2x2 average */
void pica_tile(const void *src, u16 width, u16 rows, void *dst, u32 bpp);        /* linear -> tiled */

/* Clears with the PSC fill engine. Correctly ordered against draws already recorded. */
void pica_clear(const PicaTarget *t, u32 rgba, bool clear_depth, u32 depth24);

typedef struct {
    u32 frames;
    float cpu_ms;        /* time between frame_begin and frame_end */
    float fence_wait_ms; /* time blocked waiting for the GPU */
    float frame_ms;      /* wall time between consecutive frame_begin calls */
    u32 cmd_bytes;       /* command bytes recorded last frame */
    u32 queue_entries;   /* GX queue entries last frame */
    u32 vblank_waits;    /* presents that had to wait for VBlank (cumulative) */
    float vblank_ms;     /* time spent in those waits (cumulative) */
    u32 fence_waits;     /* waits for the GPU (cumulative) */
    float fence_ms;      /* time spent in them (cumulative) */
} PicaStats;

const PicaStats *pica_stats(void);
void pica_stats_reset_vblank(void);
/* prints and clears GPU wait totals per breadcrumb: " where count/ms" */
void pica_wait_report(FILE *f);
/* DEV hang diagnosis (main.c watchdog): queue state, last submits and draws into f; command
 * lists, draw memory and GPU registers as files into dir (NULL: text only) */
void pica_debug_dump(FILE *f, const char *dir, bool regs_only);

/* ---- state ------------------------------------------------------------------------------ */

void pica_bind_target(const PicaTarget *t);
/* Framebuffer access (0x112-0x115: color read, color write, depth read, depth write) for the next
 * draws; binding a target resets it to everything. Bench only (pica_state.c says why). */
void pica_fb_access(const u32 a[4]);
void pica_viewport(u16 x, u16 y, u16 w, u16 h);
void pica_scissor(GPU_SCISSORMODE mode, u16 left, u16 top, u16 right, u16 bottom);

void pica_bind_program(shaderProgram_s *prog);
/* Drops the cached binding of `prog` (call before freeing it). */
void pica_forget_program(const shaderProgram_s *prog);
/* Float uniform registers c0..c95, given in natural x,y,z,w order. */
void pica_uniform4fv(int reg, const float *xyzw, int count);
void pica_uniform_matrix(int reg, const float rows[4][4]);

typedef struct {
    GPU_FORMATS format;
    u8 count;       /* components, 1..4 */
} PicaAttrib;

/* One interleaved vertex buffer; attribute i feeds shader input v<i>. */
void pica_vertex_layout(const PicaAttrib *attribs, int count, u16 stride);
void pica_vertex_buffer(const void *data);

typedef struct {
    u16 src_rgb, src_alpha;     /* GPU_TEVSOURCES(...) */
    u16 op_rgb, op_alpha;       /* GPU_TEVOPERANDS(...) */
    u16 func_rgb, func_alpha;   /* GPU_COMBINEFUNC */
    u32 color;                  /* constant, 0xAABBGGRR */
    u16 scale_rgb, scale_alpha; /* GPU_TEVSCALE */
} PicaTev;

void pica_tev_default(PicaTev *tev);          /* pass-through of PREVIOUS */
void pica_tev(int stage, const PicaTev *tev);
/* Combiner buffer: bit i of each mask makes stage i+1 (1..4) write its output into the buffer.
 * buffer_color is the buffer's initial value (0xAABBGGRR), usable as an extra constant. */
void pica_tev_buffer(u8 rgb_mask, u8 alpha_mask, u32 buffer_color);

/* Bind a texture (or a render target's color buffer) to unit 0..2; data == NULL disables. */
void pica_texture(int unit, const void *data, u16 width, u16 height, GPU_TEXCOLOR fmt, u32 param);
/* Invalidate the GPU texture cache (call when texture memory content changed). */
void pica_texture_flush_cache(void);

void pica_depth_test(bool enable, GPU_TESTFUNC func, u32 writemask /* GPU_WRITEMASK */);
void pica_depth_map(float scale, float offset);
void pica_cull(GPU_CULLMODE mode);
void pica_alpha_test(bool enable, GPU_TESTFUNC func, u8 ref);
void pica_blend(GPU_BLENDEQUATION eq_rgb, GPU_BLENDEQUATION eq_a, GPU_BLENDFACTOR src_rgb,
                GPU_BLENDFACTOR dst_rgb, GPU_BLENDFACTOR src_a, GPU_BLENDFACTOR dst_a);
void pica_blend_off(void);
void pica_blend_color(u32 rgba);

/* The next draw only: positions divided by w in the vertex shader (w = 1: colours interpolate
 * linearly on screen, as on the N64), texture unit 0 perspective-correct by its projection mode
 * (texcoord0w = 1/w). Texture units 1 and 2 would be affine. Needs pica_proj_bool_bit. */
void pica_projective(bool on);
extern int pica_proj_bool_bit;

void pica_draw_arrays(GPU_Primitive_t prim, u32 first, u32 count);
extern bool pica_direct_arrays; /* dev switch directarrays.txt (pica_state.c) */
/* indices must come from pica_frame_alloc (or other linear memory). */
void pica_draw_elements(GPU_Primitive_t prim, const u16 *indices, u32 count);

/* ---- helpers ----------------------------------------------------------------------------- */

/* PICA stores RGBA8 byte-reversed; this packs for MemoryFill/constants: 0xRRGGBBAA. */
static inline u32 pica_rgba8(u8 r, u8 g, u8 b, u8 a)
{
    return ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | a;
}

/* Constant colors for TEV / blend registers are 0xAABBGGRR. */
static inline u32 pica_const_color(u8 r, u8 g, u8 b, u8 a)
{
    return r | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24);
}

void pica_mtx_ortho(float out[4][4], float left, float right, float bottom, float top,
                    float near_, float far_);
/* Ortho rotated a quarter turn for the 3DS LCDs, which are portrait panels. */
void pica_mtx_ortho_tilt(float out[4][4], float left, float right, float bottom, float top,
                         float near_, float far_);

#ifdef __cplusplus
}
#endif
