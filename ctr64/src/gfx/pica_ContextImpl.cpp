/* GLideN64 -> PICA200 backend.
 *
 * Contract with GLideN64 (mirrors the OpenGL backend so the front half needs no changes):
 *   - Coordinates are GL-style: framebuffer row 0 is the bottom, NDC y = +1 is the top, texture
 *     t = 0 is row 0. PICA textures are addressed the same way, but the PICA writes window row y
 *     of a render target to memory row (storage height - 1 - y). So every draw negates clip y
 *     and mirrors viewport and scissor against the storage height; the result is a GL-layout
 *     image that samples back exactly as GLideN64 expects (and GL's winding is preserved).
 *   - GLideN64's vertex shader position math (viewport transform, quarter-pixel snap, screen-
 *     space modes, the 640x640 virtual viewport) is applied on the CPU while packing vertices;
 *     the PICA vertex shader (shaders/gfx.v.pica) only passes data through.
 *   - GL clip depth [-w, w] is remapped to PICA's [-w, 0] as z' = -(z + w) / 2, which with the
 *     depth map at scale -1 reproduces GL's 0..1 window depth exactly.
 *   - Textures have a logical (GL) size and power-of-two storage; texture coordinates are scaled
 *     by logical/storage at pack time. Storage starts in linear memory and moves to VRAM the
 *     first time the texture becomes a render target.
 *
 * M2 scope: geometry, depth, culling, blending, frame buffers and presentation. Combiners are
 * reduced to shade (or primitive) color; texturing and the TEV combiner compiler come in M3. */

#include "pica_ContextImpl.h"

#include <arm_acle.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <unordered_map>
#include <vector>

/* libctru and GLideN64 both typedef u8..s64, with different underlying types for 32 bits
 * (uint32_t is unsigned long on ARM, GLideN64 uses unsigned int). libctru's names are renamed
 * while its headers are parsed; values convert implicitly between the two. */
#define u8 ctru_u8
#define u16 ctru_u16
#define u32 ctru_u32
#define u64 ctru_u64
#define s8 ctru_s8
#define s16 ctru_s16
#define s32 ctru_s32
#define s64 ctru_s64
#define vu8 ctru_vu8
#define vu16 ctru_vu16
#define vu32 ctru_vu32
#define vu64 ctru_vu64
#define vs8 ctru_vs8
#define vs16 ctru_vs16
#define vs32 ctru_vs32
#define vs64 ctru_vs64
#include <3ds.h>
extern "C" {
#include "pica/pica.h"
#include "video.h"
}
#include "ctru_abi.h"
#include "Ctr64Prof.h"
extern "C" {
#include "prof.h"
}
#undef u8
#undef u16
#undef u32
#undef u64
#undef s8
#undef s16
#undef s32
#undef s64
#undef vu8
#undef vu16
#undef vu32
#undef vu64
#undef vs8
#undef vs16
#undef vs32
#undef vs64

#include <Combiner.h>
#include <CombinerKey.h>
#include <Config.h>
#include <DisplayWindow.h>
#include <GBI.h>
#include <Graphics/ColorBufferReader.h>
#include <Graphics/CombinerProgram.h>
#include <Graphics/Context.h>
#include <Graphics/FramebufferTextureFormats.h>
#include <Graphics/Parameters.h>
#include <Graphics/PixelBuffer.h>
#include <Graphics/ShaderProgram.h>
#include <GraphicsDrawer.h>
#include <RSP.h>
#include <FrameBuffer.h>
#include <Textures.h>
#include <gDP.h>
#include <gSP.h>

#include "pica_params.h"
#include "pica_Combiner.h"
#include "debug_crumb.h"
/* all of this runs on the video worker: its own crumb too (DEV hang diagnosis) */
#undef CRUMB
#define CRUMB(s) (ctr64_gret[1] = ctr64_gret[0], ctr64_gret[0] = __builtin_return_address(0), ctr64_gcrumb = ctr64_crumb = (s))
extern "C" bool ctr64_render_skip; /* frameskip.c */
extern "C" unsigned ctr64_frames_healed; /* frameskip.c: skipped buffers given the last picture */
extern "C" bool ctr64_frameskip_present(bool stale);
extern "C" bool ctr64_frame_cleared, ctr64_frame_fbtex, ctr64_frame_drew; /* frameskip.c */
extern "C" unsigned ctr64_fb_copies;
bool g_ctr64ReadRgba16; /* ColorBufferToRDRAM: read 16-bit RGBA5551 (GLideN64's converter) instead of RGBA8 bytes */
bool g_ctr64ReadPmBlue; /* with it: Paper Mario's subscreen fix, blue 1..0xFB raised by 4 first */
extern "C" const char *ctr64_allocfail_where; /* last failed allocation, for stats.log */
extern "C" unsigned ctr64_tex_evict(unsigned bytes); /* GLideN64 Textures.cpp: frees cached textures */
extern "C" unsigned ctr64_gfx_evicted; /* gpuAlloc calls that evicted textures (gln64_ctr.c) */
extern "C" unsigned ctr64_allocfail_bytes;

using namespace graphics;
using namespace pica_param;

extern "C" unsigned int ctr64_gfx_stats[8]; /* core_compat/gln64_ctr.c */
extern "C" unsigned int ctr64_gfx_fcram[2]; /* core_compat/gln64_ctr.c: draws to FCRAM color/depth */
extern "C" bool pica_gputime; /* DEV ONLY gputime.txt (pica.c) */
extern int ctr64_stash; /* gSP.cpp: triangle batching (dev switch nostash.txt) */
extern int ctr64_flat_stash; /* gSP.cpp: flat-shaded triangles stashed (dev switch noflatstash.txt) */
extern int ctr64_std_fast; /* gSP.cpp: standard vertex fast path (dev switch nostdvtx.txt) */
extern "C" unsigned int ctr64_gfx_vram_moves[2]; /* KB moved into / out of VRAM (keepInVram) */
extern "C" unsigned int ctr64_gfx_far_draws; /* draws clipped at the far w limit (GB_WFAR) */
extern "C" void ctr64_time_mark(const char *what); /* main.c: timing.log */
/* timing.log: a graphics step that took over 20 ms (frame buffer storage made or moved) */
struct SlowStep {
    const char *what;
    u32 kb;
    u64 t0 = svcGetSystemTick();
    SlowStep(const char *w, u32 k) : what(w), kb(k) {}
    ~SlowStep()
    {
        const double ms = (double)(svcGetSystemTick() - t0) * 1000.0 / SYSCLOCK_ARM11;
        if (ms > 20.0) {
            char line[96];
            snprintf(line, sizeof(line), "gfx: %s %lu KB took %.0f ms", what, (unsigned long)kb, ms);
            ctr64_time_mark(line);
        }
    }
};
extern "C" unsigned int ctr64_gfx_fb_reads[2];   /* reads of render targets (copies to RDRAM): [0] VRAM, [1] FCRAM */
extern "C" unsigned int ctr64_gfx_reused;
extern "C" unsigned int ctr64_game_starts; /* core_host.c */
extern "C" void ctr64_log_line(const char *line); /* core_host.c: a line to core.log */
extern "C" char ctr64_rt_mode[64]; /* stats.log: the frame buffer storage this game runs with (init) */
extern "C" unsigned int ctr64_gfx_shadow[6]; /* shadow copies for sampling (shadowOf): count, KB; [2] copyByTransfer; [3] GPU copies; [4] draws (shadowDraw) */
extern "C" unsigned int ctr64_video_frames;
/* bytes of texture / render target storage held by the backend: [0] linear, [1] VRAM,
 * [2] the largest single linear allocation, [3] storage pending release */
extern "C" unsigned int ctr64_gfx_mem[4];
enum { ST_TRI, ST_RECT, ST_CLEAR, ST_SWAP, ST_DROP, ST_FBO, ST_ALLOC_FAIL, ST_SUBDIV };
extern "C" unsigned long long ctr64_gfx_ticks[3]; /* [0] draw calls, [1] display lists, [2] present */
extern "C" int ctr64_prof_on; /* main.c: fine-grained timers on (Debug info) */

/* Accumulates the system ticks spent in a backend draw call into ctr64_gfx_ticks[0]. */
struct DrawTimer { /* only with profiling on: a tick read is a kernel call */
    unsigned long long t0 = ctr64_prof_on ? svcGetSystemTick() : 0;
    ~DrawTimer()
    {
        if (t0)
            ctr64_gfx_ticks[0] += svcGetSystemTick() - t0;
    }
};

extern "C" bool ctr64_crc_nomemo; /* gln64_ctr.c; dev switch nocrcmemo.txt */
extern "C" bool ctr64_fbcheck_full; /* gln64_ctr.c; dev switch fbcheckfull.txt */

extern bool ctr64_fb_is16; /* FrameBuffer.cpp: the texture being made is a 16-bit N64 color buffer */
/* Frame buffers of 16-bit N64 buffers in 16 bits instead of 32 (as on the N64, though without its
 * dithering: gradients get coarser steps), half the memory and half the bytes per copy. Used only
 * by a game whose targets do not fit the VRAM: once more than k_fb16_kb were moved into it in each
 * of two runs of 64 presents, for the rest of the game; buffers there are change at their next
 * full clear or at the next draw into them (convert16: a GPU copy), new ones are made so. Mario Tennis with anti-aliasing (three 768x480 buffers and a
 * depth buffer), HW 2026-11-22: 31 -> 47 VI/s where 70 MB/s were moved. Dev switches: fb16.txt = from
 * the start, nofb16.txt = never. */
static bool g_fb16, g_nofb16;
static u32 g_fb16_presents, g_fb16_moved, g_fb16_runs; /* KB moved since the run began */
static const u32 k_fb16_kb = 12 * 1024;
namespace {

/* ---- textures ------------------------------------------------------------------------------ */

struct Tex {
    u16 w = 0, h = 0;   /* logical size */
    u16 tw = 0, th = 0; /* power-of-two storage */
    GPU_TEXCOLOR fmt = GPU_RGBA8;
    u8 bpp = 4;
    bool n16 = false; /* color buffer of a 16-bit N64 buffer (g_fb16) */
    bool depth = false; /* D24S8 storage (depth renderbuffer / texture) */
    bool vram = false;
    void *data = nullptr;
    u32 param = GPU_TEXTURE_MAG_FILTER(GPU_NEAREST) | GPU_TEXTURE_MIN_FILTER(GPU_NEAREST) |
                GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_EDGE) | GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_EDGE);
    u32 used_frame = 0; /* pica frame number in which the GPU last read or wrote it */
    bool dirty = false; /* contents changed since last bound: clear the GPU texture cache */
    bool lazy = false;  /* storage not allocated until first use (render targets) */
    bool rt = false;    /* attached to a frame buffer: prefer VRAM */
    bool skipped = false; /* draws into it were dropped by frameskip since it was last drawn */
    bool drawn = true;    /* drawn or written since the window last presented it */
    bool ever = false;    /* has an image: drawn, cleared or written at least once */
    u64 copy_fence = 0;   /* a queued GPU copy into it (stagedUpdate) is done at this fence */
    u32 draw_frame = 0;   /* pica frame of the last draw into it (VRAM placement, keepInVram) */
    u32 ndraw = 0;        /* draws since the last present */
    u32 ndraw_last = ~0u; /* draws in the last present interval it was drawn in (~0: none yet) */
    u32 born = 0;         /* frame its storage was set up in (init2DTexture) */
    u16 th_full = 0;      /* short storage (materialize): th is less than the target needs, this much */
    u16 rows_used = 0;    /* rows drawn to (from GL row 0); 0xFFFF: unknown */
    u32 zclean = 0;       /* depth buffers: texels cleared with nothing drawn since */
    u32 cleared = 0;      /* the last clear's color, if it was cleared since its storage was set up */
    u32 gen = 0;          /* contents generation: bumped with dirty (shadow copies, shadowOf) */
    u32 nshadow = 0;      /* shadow copies of it in this present interval (rtlog.txt) */
    u32 sh_gen = ~0u;     /* shadowOf: samples of contents generation sh_gen, of the one before */
    u16 sh_samples = 0, sh_last = 0;
    u32 trim_gen = ~0u;   /* the draw at this gen took its rows out of the shadows (noteDrawnRows) */
};

struct Fbo {
    u32 color = 0, depth = 0;
    PicaTarget rt{};
    bool valid = false;
};

u8 g_morton[64];

void init_morton()
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            g_morton[y * 8 + x] = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
                                  ((x & 4) << 2) | ((y & 4) << 3);
}

inline u32 tiled_index(u32 x, u32 y, u32 tw)
{
    return ((((y >> 3) * (tw >> 3)) + (x >> 3)) << 6) + g_morton[((y & 7) << 3) + (x & 7)];
}

/* Texel (x, y) in GL convention (y = 0 is t = 0). PICA textures and render targets keep tiled
 * row 0 at the top (t = 1), so GL row y is tiled row th - 1 - y - for textures uploaded here
 * and for images the GPU renders alike, which keeps render-to-texture GL-consistent. */
inline u32 gl_index(u32 x, u32 y, u32 tw, u32 th)
{
    return tiled_index(x, th - 1 - y, tw);
}

u16 pot(u32 v)
{
    u16 p = 8;
    while (p < v && p < 1024)
        p <<= 1;
    return p;
}

/* Storage width of a frame buffer texture or renderbuffer: the width rounded up to 32 texels.
 * With anti-aliasing a 640x480 target then takes 1.25 MB instead of 2 MB: the targets of a frame
 * fit in VRAM (2026-11-09: MK64 menus moved 4 MB per frame in and out of VRAM, OoT's pause drew
 * into an FCRAM depth buffer; HW profile4 with AA: 40-70 MB/s of target moves, MK GPU 3.2 -> 23.6
 * ms per frame). The PICA renders to any width (multiple of 8) but samples only power-of-two
 * widths on hardware (video_npot_selftest): there such a texture is sampled through a shadow
 * copy (shadowOf). The height stays a power of two: the rows of every target keep their place,
 * and a depth buffer still matches the color buffers it served before. */
bool g_slowtex; /* dev switch slowtex.txt: texture uploads by the old per-texel paths (A/B) */
bool g_noguard; /* dev switch noguard.txt: no guard band clipping (A/B) */
bool g_noshadelin; /* dev switch noshadelin.txt: no screen-linear shade subdivision (A/B) */
bool g_novramflush; /* dev switch novramflush.txt: no cache flush after CPU texel writes into VRAM */
bool g_novpz;      /* dev switch novpz.txt: the N64 viewport's z scale and translation not applied */
bool g_noproj;     /* dev switch noproj.txt: screen-linear shade by subdivision, not projective draws */
bool g_noalphaover; /* dev switch noalphaover.txt: alpha blended with the colour's factors (setBlending) */
bool g_noidxdraw;  /* dev switch noidxdraw.txt: indexed triangle lists drawn as plain lists (A/B) */

bool g_nobanks;
bool g_potfb; /* dev switch potfb.txt: power-of-two frame buffer storage always (as before 2026-11-10) */
bool g_potheight; /* dev switch potheight.txt: power-of-two heights at exact widths too (A/B) */
bool g_lruold; /* dev switch lruold.txt: the VRAM target drawn longest ago always gives way (A/B) */
bool g_noheavy; /* dev switch noheavy.txt: no switch to power-of-two widths (g_shadow_heavy, A/B) */
bool g_fullshadow; /* dev switch fullshadow.txt: shadow copies take every row (shadowOf) */
bool g_noalpha1;   /* dev switch noalpha1.txt: alpha compare on the output alpha (setupCombiner) */
bool g_noshdraw;   /* dev switch noshdraw.txt: shadow copies by the transfer engine only (shadowDraw) */
bool g_shcheck;    /* dev switch shcheck.txt (Azahar): each shadow draw checked against a transfer copy */
extern char g_shdraw_report[48]; /* shadowDrawTest result, for stats.log */
bool g_shazflush;  /* dev switch shazflush.txt (Azahar): source rows written back before a shadow draw */
bool g_shflip;     /* dev switch shflip.txt: shadow draws with the texture rows the other way round */
bool g_nodrawnrows; /* dev switch nodrawnrows.txt: a draw makes its target's shadows stale (noteDrawnRows) */

/* Hardware (no non-power-of-two sampling): frame buffers at exact widths only with anti-aliasing,
 * where that keeps the 2x targets in VRAM, at the price of a shadow copy per sampled frame buffer.
 * At 1x power-of-two storage fits and needs none. Hardware A/B (2026-11-10, profiles 3dslogs/10,
 * same scenes): OoT AA 97.2% exact vs 89.2% power-of-two, MK 88.7 vs 89.1, SSB 96.0 vs 97.2 (its
 * CPU freezes dominate). Decided when a frame buffer is created (the game's settings, AA
 * included, are final then; not always at context init). stats.log "frame buffer storage". */
/* Exact widths cost a shadow copy each time a frame buffer is sampled. A game that does that
 * every frame (Mario Golf: ~1.3 MB per frame, 40% speed in its menus on HW, 100% with
 * power-of-two storage) gets power-of-two storage for the frame buffers created from then on:
 * set once at least one shadow copy per two frames over 120 frames, for the rest of the game
 * (ContextImplPica::init clears it). Rebuilding the frame buffers that exist then was tried
 * (Goemon's diary screen samples the frame it draws: 7 copies of 1.25 MB per frame, HW 23 VI/s):
 * the 2 MB power-of-two buffers no longer fit the VRAM, 2 MB per frame moved there and 20-30
 * MB/s in gameplay that moved none (Azahar 2026-11-18): not done. */
static bool g_shadow_heavy;
static u32 g_shadow_win_start, g_shadow_win_count;
static void note_shadow_copy()
{
    const u32 now = pica_frame_number();
    if (now - g_shadow_win_start >= 120) {
        g_shadow_win_start = now;
        g_shadow_win_count = 0;
    }
    if (++g_shadow_win_count >= 60 && !g_noheavy)
        g_shadow_heavy = true;
}

static u32 g_rt_pot_bytes; /* the targets drawn lately, at power-of-two sizes (present) */
static bool pot_storage()
{
    if (video_npot_ok)
        return false;
    const bool aa = config.frameBufferEmulation.enable && config.frameBufferEmulation.nativeResFactor > 1;
    /* power-of-two targets only if those in use fit the VRAM so (Mario Tennis's do not: its depth
     * buffer grew to 2 MB and a color buffer was moved every frame) */
    const bool pot_choice = !aa || g_potfb || (g_shadow_heavy && g_rt_pot_bytes <= 5120 * 1024);
    if (aa)
        snprintf(ctr64_rt_mode, sizeof(ctr64_rt_mode), "%s%s%s",
                 pot_choice ? "power-of-two widths" : "exact widths + shadow copies", g_shdraw_report,
                 g_fb16 ? " + 16 bit" : "");
    return pot_choice;
}

u16 fb_width(u32 v)
{
    if (v < 64 || v > 1024 || pot_storage())
        return pot(v);
    return (u16)((v + 31) & ~31u);
}

/* Storage height of a target stored fb_width(width) wide. At an exact width, sampled through its
 * shadow anyway, the rows are exact too: Mario Tennis's 768x480 targets take 1440 KB each, not
 * 1536, and its three color buffers and depth buffer fit the two 3 MB halves of the VRAM (one was
 * moved out and in per frame, HW 2026-11-22: 7 of 49 ms); PAL games' 640x580 took 1024 rows. */
u16 fb_height(u16 tw, u32 h)
{
    if (g_potheight || (tw & (tw - 1)) == 0 || h < 64 || h > 1024)
        return pot(h);
    return (u16)((h + 7) & ~7u);
}

/* Without non-power-of-two sampling (hardware) a texture stored at such a width is sampled
 * through a power-of-two shadow copy (shadowOf); the size texture coordinates refer to. The rows
 * are stored bottom up, so a shadow has its unused rows first. */
inline bool needs_shadow(const Tex &t)
{
    return !video_npot_ok && (t.tw & (t.tw - 1)) != 0;
}
inline u32 sample_w(const Tex &t)
{
    return needs_shadow(t) ? pot(t.tw) : t.tw;
}
inline u32 sample_h(const Tex &t)
{
    return needs_shadow(t) ? pot(t.th) : t.th;
}

/* GL source texel -> 0xRRGGBBAA */
u32 fetch_rgba8(const u8 *src, u32 i, u32 format, u32 type)
{
    switch (type) {
    case DT_US_4444: {
        u16 v = reinterpret_cast<const u16 *>(src)[i];
        u32 r = (v >> 12) & 15, g = (v >> 8) & 15, b = (v >> 4) & 15, a = v & 15;
        return (r * 17) << 24 | (g * 17) << 16 | (b * 17) << 8 | (a * 17);
    }
    case DT_US_5551: {
        u16 v = reinterpret_cast<const u16 *>(src)[i];
        u32 r = (v >> 11) & 31, g = (v >> 6) & 31, b = (v >> 1) & 31;
        return ((r << 3) | (r >> 2)) << 24 | ((g << 3) | (g >> 2)) << 16 | ((b << 3) | (b >> 2)) << 8 |
               ((v & 1) ? 0xFF : 0);
    }
    case DT_US_565: {
        u16 v = reinterpret_cast<const u16 *>(src)[i];
        u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        return ((r << 3) | (r >> 2)) << 24 | ((g << 2) | (g >> 4)) << 16 | ((b << 3) | (b >> 2)) << 8 | 0xFF;
    }
    default:
        break;
    }
    switch (format) {
    case CF_RGB: {
        const u8 *p = src + i * 3;
        return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | 0xFF;
    }
    case CF_RED:
    case CF_LUMINANCE: {
        u8 l = src[i];
        return u32(l) << 24 | u32(l) << 16 | u32(l) << 8 | 0xFF;
    }
    case CF_RG: {
        const u8 *p = src + i * 2;
        return u32(p[0]) << 24 | u32(p[1]) << 16 | 0xFF;
    }
    default: {
        const u8 *p = src + i * 4;
        return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3];
    }
    }
}

/* ---- texel codecs: 0xRRGGBBAA <-> PICA storage formats (byte layouts as the PICA reads them) -- */

inline u32 expand5(u32 v) { return (v << 3) | (v >> 2); }

void store_texel(Tex &t, u32 idx, u32 rgba)
{
    const u32 r = rgba >> 24, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF, a = rgba & 0xFF;
    u8 *p = static_cast<u8 *>(t.data);
    switch (t.fmt) {
    case GPU_RGBA4:
        reinterpret_cast<u16 *>(p)[idx] = (u16)((r >> 4) << 12 | (g >> 4) << 8 | (b >> 4) << 4 | a >> 4);
        break;
    case GPU_RGBA5551:
        reinterpret_cast<u16 *>(p)[idx] = (u16)((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (a >> 7));
        break;
    case GPU_RGB8:
        p[idx * 3 + 0] = (u8)b;
        p[idx * 3 + 1] = (u8)g;
        p[idx * 3 + 2] = (u8)r;
        break;
    case GPU_LA8:
        p[idx * 2 + 0] = (u8)a;
        p[idx * 2 + 1] = (u8)r;
        break;
    case GPU_LA4:
        p[idx] = (u8)((r >> 4) << 4 | a >> 4);
        break;
    default:
        reinterpret_cast<u32 *>(p)[idx] = rgba;
        break;
    }
}

u32 load_texel(const Tex &t, u32 idx)
{
    const u8 *p = static_cast<const u8 *>(t.data);
    switch (t.fmt) {
    case GPU_RGBA4: {
        const u32 v = reinterpret_cast<const u16 *>(p)[idx];
        return ((v >> 12) * 17) << 24 | (((v >> 8) & 15) * 17) << 16 | (((v >> 4) & 15) * 17) << 8 | (v & 15) * 17;
    }
    case GPU_RGBA5551: {
        const u32 v = reinterpret_cast<const u16 *>(p)[idx];
        return expand5(v >> 11) << 24 | expand5((v >> 6) & 31) << 16 | expand5((v >> 1) & 31) << 8 | ((v & 1) ? 255 : 0);
    }
    case GPU_RGB8:
        return (u32)p[idx * 3 + 2] << 24 | (u32)p[idx * 3 + 1] << 16 | (u32)p[idx * 3] << 8 | 0xFF;
    case GPU_LA8: {
        const u32 l = p[idx * 2 + 1];
        return l << 24 | l << 16 | l << 8 | p[idx * 2];
    }
    case GPU_LA4: {
        const u32 l = (p[idx] >> 4) * 17;
        return l << 24 | l << 16 | l << 8 | (p[idx] & 15) * 17;
    }
    default:
        return reinterpret_cast<const u32 *>(p)[idx];
    }
}

u8 bytes_per_texel(GPU_TEXCOLOR f)
{
    switch (f) {
    case GPU_RGBA8: return 4;
    case GPU_RGB8: return 3;
    case GPU_LA4: return 1;
    default: return 2;
    }
}

/* Whether a texel survives the format exactly. */
bool texel_fits(GPU_TEXCOLOR f, u32 rgba)
{
    const u32 r = rgba >> 24, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF, a = rgba & 0xFF;
    switch (f) {
    case GPU_LA4: return r == g && g == b && r % 17 == 0 && a % 17 == 0;
    case GPU_LA8: return r == g && g == b;
    case GPU_RGBA5551:
        return expand5(r >> 3) == r && expand5(g >> 3) == g && expand5(b >> 3) == b && (a == 0 || a == 255);
    case GPU_RGBA4: return r % 17 == 0 && g % 17 == 0 && b % 17 == 0 && a % 17 == 0;
    case GPU_RGB8: return a == 255;
    default: return true;
    }
}

/* The smallest PICA format that stores every texel exactly (N64 textures are mostly 4/8/16-bit
 * intensity, IA or RGBA5551 data that GLideN64 expands to RGBA8888). */
GPU_TEXCOLOR pick_format(const std::vector<u32> &px)
{
    static const GPU_TEXCOLOR order[] = {GPU_LA4, GPU_LA8, GPU_RGBA5551, GPU_RGBA4, GPU_RGB8};
    bool ok[5] = {true, true, true, true, true};
    int left = 5;
    for (u32 v : px) {
        for (int i = 0; i < 5; i++)
            if (ok[i] && !texel_fits(order[i], v)) {
                ok[i] = false;
                left--;
            }
        if (!left)
            return GPU_RGBA8;
    }
    for (int i = 0; i < 5; i++)
        if (ok[i])
            return order[i];
    return GPU_RGBA8;
}

/* ---- bulk texel paths (uploads of whole images: frame buffer copies, N64 textures) ----------
 * Format-specialized loops: one conversion pass over the source, then tiled stores with the
 * Morton offsets of each storage row precomputed. */

/* GL source pixels -> 0xRRGGBBAA, one specialized loop per source layout */
void fetch_all(const u8 *src, u32 n, u32 format, u32 type, u32 *out)
{
    if (type == DT_US_4444 || type == DT_US_5551 || type == DT_US_565) {
        for (u32 i = 0; i < n; i++)
            out[i] = fetch_rgba8(src, i, format, type);
        return;
    }
    if (format == CF_RGBA || (format != CF_RGB && format != CF_RED && format != CF_LUMINANCE && format != CF_RG)) {
        /* bytes R, G, B, A -> 0xRRGGBBAA: a byte swap of the little-endian word */
        const u32 *s = reinterpret_cast<const u32 *>(src);
        for (u32 i = 0; i < n; i++)
            out[i] = __builtin_bswap32(s[i]);
        return;
    }
    for (u32 i = 0; i < n; i++)
        out[i] = fetch_rgba8(src, i, format, type);
}

bool fits_all(GPU_TEXCOLOR f, const u32 *px, u32 n)
{
    if (f == GPU_RGBA8)
        return true;
    for (u32 i = 0; i < n; i++)
        if (!texel_fits(f, px[i]))
            return false;
    return true;
}

template <GPU_TEXCOLOR F> inline void put_texel(u8 *p, u32 idx, u32 rgba);
template <> inline void put_texel<GPU_RGBA8>(u8 *p, u32 idx, u32 v) { reinterpret_cast<u32 *>(p)[idx] = v; }
template <> inline void put_texel<GPU_RGB8>(u8 *p, u32 idx, u32 v)
{
    u8 *d = p + idx * 3;
    d[0] = (u8)(v >> 8); d[1] = (u8)(v >> 16); d[2] = (u8)(v >> 24);
}
template <> inline void put_texel<GPU_RGBA5551>(u8 *p, u32 idx, u32 v)
{
    reinterpret_cast<u16 *>(p)[idx] = (u16)((v >> 16 & 0xF800) | (v >> 13 & 0x07C0) | (v >> 10 & 0x003E) | (v >> 7 & 1));
}
template <> inline void put_texel<GPU_RGBA4>(u8 *p, u32 idx, u32 v)
{
    reinterpret_cast<u16 *>(p)[idx] = (u16)((v >> 16 & 0xF000) | (v >> 12 & 0x0F00) | (v >> 8 & 0x00F0) | (v >> 4 & 0x000F));
}
template <> inline void put_texel<GPU_LA8>(u8 *p, u32 idx, u32 v)
{
    p[idx * 2] = (u8)v;
    p[idx * 2 + 1] = (u8)(v >> 24);
}
template <> inline void put_texel<GPU_LA4>(u8 *p, u32 idx, u32 v) { p[idx] = (u8)((v >> 24 & 0xF0) | (v >> 4 & 0x0F)); }

/* px (w x h, GL rows) -> texture rect at (x0, y0) */
template <GPU_TEXCOLOR F> void write_rect(Tex &t, const u32 *px, u32 x0, u32 y0, u32 w, u32 h)
{
    u8 *p = static_cast<u8 *>(t.data);
    const u32 tiles_x = t.tw >> 3;
    const u32 xe = x0 >= t.tw ? 0 : (w < t.tw - x0 ? w : t.tw - x0);
    for (u32 y = 0; y < h && y0 + y < t.th; y++) {
        const u32 r = t.th - 1 - (y0 + y); /* storage row (row 0 = top) */
        const u32 rowbase = ((r >> 3) * tiles_x) << 6;
        const u8 *m = &g_morton[(r & 7) << 3];
        const u32 *s = px + (size_t)y * w;
        for (u32 x = 0; x < xe; x++) {
            const u32 tx = x0 + x;
            put_texel<F>(p, rowbase + ((tx >> 3) << 6) + m[tx & 7], s[x]);
        }
    }
}

/* ---- whole-texture uploads (init2DTexture): the hot path of new screens ----------------------
 * MK64's menu screens load ~100-200 textures in one frame: on hardware 60-80 ms of the video
 * thread per screen change went to fetch_all + pick_format + write_rect + pad_edges (Azahar VI
 * 1284 of the MK menus: 97 ms fetch/pick, 107 ms swizzle of 255 ms texture loads). Same output,
 * byte for byte: one branch-free scan for the format, and the storage written tile by tile in
 * its own order (2x2 quads = 4 consecutive texels), the power-of-two padding taken from clamped
 * source rows/columns instead of a second pass. */

/* fetch_all for the 16-bit sources, without the per-texel switch */
void fetch_all_fast(const u8 *src, u32 n, u32 format, u32 type, u32 *out)
{
    const u16 *s = reinterpret_cast<const u16 *>(src);
    switch (type) {
    case DT_US_5551:
        for (u32 i = 0; i < n; i++) {
            const u32 v = s[i];
            const u32 r = (v >> 11) & 31, g = (v >> 6) & 31, b = (v >> 1) & 31;
            out[i] = ((r << 3) | (r >> 2)) << 24 | ((g << 3) | (g >> 2)) << 16 | ((b << 3) | (b >> 2)) << 8 |
                     (0u - (v & 1)) >> 24;
        }
        return;
    case DT_US_4444:
        for (u32 i = 0; i < n; i++) {
            const u32 v = s[i];
            /* nibbles 0xRGBA -> bytes 0xR0G0B0A0, each byte n * 17 = n << 4 | n */
            const u32 e = (v & 0xF000) << 16 | (v & 0x0F00) << 12 | (v & 0x00F0) << 8 | (v & 0x000F) << 4;
            out[i] = e | e >> 4;
        }
        return;
    case DT_US_565:
        for (u32 i = 0; i < n; i++) {
            const u32 v = s[i];
            const u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            out[i] = ((r << 3) | (r >> 2)) << 24 | ((g << 2) | (g >> 4)) << 16 | ((b << 3) | (b >> 2)) << 8 | 0xFF;
        }
        return;
    default:
        fetch_all(src, n, format, type, out);
    }
}

/* pick_format in one branch-free pass. Per texel 0xRRGGBBAA:
 *   gray (LA8)      r == g == b          (v ^ v >> 8) & 0x00FFFF00 == 0
 *   nibbles (RGBA4) every byte n * 17    (v ^ v >> 4) & 0x0F0F0F0F == 0
 *   LA4             gray and nibbles
 *   RGBA5551        r, g, b == expand5(c >> 3), i.e. c & 7 == c >> 5:  (v ^ v >> 5) & 0x07070700 == 0,
 *                   and a == 0 or 255:  (a + 1) & 0xFE == 0
 *   RGB8            a == 255 */
GPU_TEXCOLOR pick_format_fast(const u32 *px, u32 n)
{
    u32 gray = 0, nib = 0, c5 = 0, a5 = 0, aand = 0xFF;
    for (u32 i = 0; i < n;) {
        const u32 end = n - i > 256 ? i + 256 : n;
        for (; i < end; i++) {
            const u32 v = px[i];
            gray |= v ^ (v >> 8);
            nib |= v ^ (v >> 4);
            c5 |= v ^ (v >> 5);
            a5 |= v + 1;
            aand &= v;
        }
        if ((gray & 0x00FFFF00) && (nib & 0x0F0F0F0F) && ((c5 & 0x07070700) || (a5 & 0xFE)) && (aand & 0xFF) != 0xFF)
            return GPU_RGBA8; /* nothing smaller fits any more */
    }
    gray &= 0x00FFFF00;
    nib &= 0x0F0F0F0F;
    if (!gray && !nib)
        return GPU_LA4;
    if (!gray)
        return GPU_LA8;
    if (!(c5 & 0x07070700) && !(a5 & 0xFE))
        return GPU_RGBA5551;
    if (!nib)
        return GPU_RGBA4;
    if ((aand & 0xFF) == 0xFF)
        return GPU_RGB8;
    return GPU_RGBA8;
}

template <GPU_TEXCOLOR F> inline u32 conv_texel(u32 v);
template <> inline u32 conv_texel<GPU_RGBA8>(u32 v) { return v; }
template <> inline u32 conv_texel<GPU_RGB8>(u32 v) { return v; }
template <> inline u32 conv_texel<GPU_RGBA5551>(u32 v)
{
    return (v >> 16 & 0xF800) | (v >> 13 & 0x07C0) | (v >> 10 & 0x003E) | (v >> 7 & 1);
}
template <> inline u32 conv_texel<GPU_RGBA4>(u32 v)
{
    return (v >> 16 & 0xF000) | (v >> 12 & 0x0F00) | (v >> 8 & 0x00F0) | (v >> 4 & 0x000F);
}
template <> inline u32 conv_texel<GPU_LA8>(u32 v) { return (v & 0xFF) | (v >> 24) << 8; }
template <> inline u32 conv_texel<GPU_LA4>(u32 v) { return (v >> 24 & 0xF0) | (v >> 4 & 0x0F); }

/* one 2x2 quad: texels (x, r), (x+1, r), (x, r+1), (x+1, r+1) of the storage, at o (4 texels);
 * RAW: the values are already in the storage format (16-bit formats only) */
template <GPU_TEXCOLOR F, bool RAW = false> inline void put_quad(u8 *o, u32 t0, u32 t1, u32 t2, u32 t3)
{
    if (F == GPU_RGBA8) {
        u32 *d = reinterpret_cast<u32 *>(o);
        d[0] = t0; d[1] = t1; d[2] = t2; d[3] = t3;
    } else if (F == GPU_RGB8) {
        const u32 t[4] = {t0, t1, t2, t3};
        for (int k = 0; k < 4; k++) {
            o[k * 3 + 0] = (u8)(t[k] >> 8);
            o[k * 3 + 1] = (u8)(t[k] >> 16);
            o[k * 3 + 2] = (u8)(t[k] >> 24);
        }
    } else if (F == GPU_LA4) {
        *reinterpret_cast<u32 *>(o) =
            conv_texel<F>(t0) | conv_texel<F>(t1) << 8 | conv_texel<F>(t2) << 16 | conv_texel<F>(t3) << 24;
    } else if (RAW) {
        u32 *d = reinterpret_cast<u32 *>(o);
        d[0] = t0 | t1 << 16;
        d[1] = t2 | t3 << 16;
    } else {
        u32 *d = reinterpret_cast<u32 *>(o);
        d[0] = conv_texel<F>(t0) | conv_texel<F>(t1) << 16;
        d[1] = conv_texel<F>(t2) | conv_texel<F>(t3) << 16;
    }
}

/* the whole storage (tw x th) from px (w x h GL rows, stride = row length in texels >= w):
 * texels past the right/bottom edge repeat the last column/row (= pad_edges) */
template <GPU_TEXCOLOR F, bool RAW = false, typename T = u32> void write_full(Tex &t, const T *px, u32 w, u32 h, u32 stride)
{
    /* storage quad q of a tile: x = q bits 0, 2 -> x bits 1, 2; y = q bits 1, 3 -> y bits 1, 2
     * (g_morton: x0 y0 x1 y1 x2 y2 from the lowest bit) */
    static const u8 qx[16] = {0, 2, 0, 2, 4, 6, 4, 6, 0, 2, 0, 2, 4, 6, 4, 6};
    static const u8 qy[16] = {0, 0, 2, 2, 0, 0, 2, 2, 4, 4, 6, 6, 4, 4, 6, 6};
    const u32 bpp = F == GPU_RGBA8 ? 4 : F == GPU_RGB8 ? 3 : F == GPU_LA4 ? 1 : 2;
    u8 *o = static_cast<u8 *>(t.data);
    const u32 tiles_x = t.tw >> 3, tiles_y = t.th >> 3;
    const bool xpad = w < t.tw;
    for (u32 ty = 0; ty < tiles_y; ty++) {
        const T *row[8];
        for (u32 ry = 0; ry < 8; ry++) {
            u32 y = t.th - 1 - (ty * 8 + ry); /* GL row of storage row ty * 8 + ry */
            if (y >= h)
                y = h - 1;
            row[ry] = px + (size_t)y * stride;
        }
        for (u32 tx = 0; tx < tiles_x; tx++) {
            const u32 x0 = tx * 8;
            if (!xpad || x0 + 8 <= w) {
                for (u32 q = 0; q < 16; q++) {
                    const u32 x = x0 + qx[q];
                    const T *a = row[qy[q]], *b = row[qy[q] + 1];
                    put_quad<F, RAW>(o, a[x], a[x + 1], b[x], b[x + 1]);
                    o += 4 * bpp;
                }
            } else {
                for (u32 q = 0; q < 16; q++) {
                    u32 xa = x0 + qx[q], xb = xa + 1;
                    if (xa >= w)
                        xa = w - 1;
                    if (xb >= w)
                        xb = w - 1;
                    const T *a = row[qy[q]], *b = row[qy[q] + 1];
                    put_quad<F, RAW>(o, a[xa], a[xb], b[xa], b[xb]);
                    o += 4 * bpp;
                }
            }
        }
    }
}

void write_full_any(Tex &t, const u32 *px, u32 w, u32 h, u32 stride)
{
    switch (t.fmt) {
    case GPU_RGB8: write_full<GPU_RGB8>(t, px, w, h, stride); break;
    case GPU_RGBA5551: write_full<GPU_RGBA5551>(t, px, w, h, stride); break;
    case GPU_RGBA4: write_full<GPU_RGBA4>(t, px, w, h, stride); break;
    case GPU_LA8: write_full<GPU_LA8>(t, px, w, h, stride); break;
    case GPU_LA4: write_full<GPU_LA4>(t, px, w, h, stride); break;
    default: write_full<GPU_RGBA8>(t, px, w, h, stride); break;
    }
}

/* write_rect with the same result (texel (x0 + x, y0 + y) = px[y * w + x], x < tw - x0, y < th - y0;
 * texels outside the rect untouched), tile by tile: tiles inside the rect by 2x2 quads in storage
 * order, edge tiles texel by texel (OoT's pause menu with AA updates a 320x240 rect of a 640x512 frame
 * buffer texture every frame: 11.9 ms per frame on hardware by write_rect) */
template <GPU_TEXCOLOR F> void write_rect_fast(Tex &t, const u32 *px, u32 x0, u32 y0, u32 w, u32 h)
{
    static const u8 qx[16] = {0, 2, 0, 2, 4, 6, 4, 6, 0, 2, 0, 2, 4, 6, 4, 6};
    static const u8 qy[16] = {0, 0, 2, 2, 0, 0, 2, 2, 4, 4, 6, 6, 4, 4, 6, 6};
    if (x0 >= t.tw || y0 >= t.th || !w || !h)
        return;
    const u32 xe = w < t.tw - x0 ? w : t.tw - x0; /* columns written */
    const u32 he = h < t.th - y0 ? h : t.th - y0; /* rows written */
    const u32 bpp = F == GPU_RGBA8 ? 4 : F == GPU_RGB8 ? 3 : F == GPU_LA4 ? 1 : 2;
    u8 *p = static_cast<u8 *>(t.data);
    const u32 tiles_x = t.tw >> 3;
    const u32 r_lo = t.th - (y0 + he), r_hi = t.th - 1 - y0; /* storage rows */
    const u32 c_lo = x0, c_hi = x0 + xe - 1;
    for (u32 tr = r_lo >> 3; tr <= (r_hi >> 3); tr++) {
        for (u32 tc = c_lo >> 3; tc <= (c_hi >> 3); tc++) {
            u8 *o = p + ((tr * tiles_x + tc) << 6) * bpp;
            const bool inside = tr * 8 >= r_lo && tr * 8 + 7 <= r_hi && tc * 8 >= c_lo && tc * 8 + 7 <= c_hi;
            if (inside) {
                for (u32 q = 0; q < 16; q++) {
                    const u32 r = tr * 8 + qy[q], x = tc * 8 + qx[q] - x0;
                    const u32 *a = px + (size_t)(t.th - 1 - r - y0) * w + x; /* storage row r */
                    const u32 *b = px + (size_t)(t.th - 2 - r - y0) * w + x; /* storage row r + 1 */
                    put_quad<F>(o + q * 4 * bpp, a[0], a[1], b[0], b[1]);
                }
            } else {
                for (u32 i = 0; i < 64; i++) {
                    const u32 lx = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4);
                    const u32 ly = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4);
                    const u32 r = tr * 8 + ly, x = tc * 8 + lx;
                    if (r < r_lo || r > r_hi || x < c_lo || x > c_hi)
                        continue;
                    put_texel<F>(o, i, px[(size_t)(t.th - 1 - r - y0) * w + (x - x0)]);
                }
            }
        }
    }
}

/* Replicate the right and bottom edges into the power-of-two padding, so that clamping at the
 * storage edge matches clamping at the logical edge (non-power-of-two clamp tiles). */
template <u32 BPP> void pad_edges_bpp(Tex &t)
{
    struct Unit { u8 b[BPP]; };
    Unit *p = static_cast<Unit *>(t.data);
    for (u32 y = 0; y < t.h; y++) {
        const Unit v = p[gl_index(t.w - 1, y, t.tw, t.th)];
        for (u32 x = t.w; x < t.tw; x++)
            p[gl_index(x, y, t.tw, t.th)] = v;
    }
    for (u32 y = t.h; y < t.th; y++)
        for (u32 x = 0; x < t.tw; x++)
            p[gl_index(x, y, t.tw, t.th)] = p[gl_index(x, t.h - 1, t.tw, t.th)];
}

void pad_edges(Tex &t)
{
    switch (t.bpp) {
    case 1: pad_edges_bpp<1>(t); break;
    case 2: pad_edges_bpp<2>(t); break;
    case 3: pad_edges_bpp<3>(t); break;
    default: pad_edges_bpp<4>(t); break;
    }
}

/* ---- programs ------------------------------------------------------------------------------- */

enum class Kind { Combiner, Copy, Clear, Text, Noop };

class ContextImplPica;
ContextImplPica *g_ctx;

void set_current_program(const CombinerProgram *p, Kind k);

class PicaCombinerProgram : public CombinerProgram {
public:
    PicaCombinerProgram(const CombinerKey &key, u32 inputs) : m_key(key), m_inputs(inputs)
    {
        pica_cmb::compile(key.getMux(), key.getCycleType(), m_tev);
    }
    const pica_cmb::Program &tev() const { return m_tev; }
    void activate() override { set_current_program(this, Kind::Combiner); }
    /* GLideN64 calls update() (not activate()) before each draw; the GL backend binds there too. */
    void update(bool) override { set_current_program(this, Kind::Combiner); }
    const CombinerKey &getKey() const override { return m_key; }
    bool usesTexture() const override
    {
        return (m_inputs & ((1u << G_GCI_TEXEL0) | (1u << G_GCI_TEXEL0_ALPHA) | (1u << G_GCI_TEXEL1) |
                            (1u << G_GCI_TEXEL1_ALPHA))) != 0;
    }
    bool usesTile(u32 t) const override
    {
        return t == 0 ? (m_inputs & ((1u << G_GCI_TEXEL0) | (1u << G_GCI_TEXEL0_ALPHA))) != 0
                      : (m_inputs & ((1u << G_GCI_TEXEL1) | (1u << G_GCI_TEXEL1_ALPHA))) != 0;
    }
    bool usesShade() const override
    {
        return (m_inputs & ((1u << G_GCI_SHADE) | (1u << G_GCI_SHADE_ALPHA))) != 0;
    }
    bool usesLOD() const override { return (m_inputs & (1u << G_GCI_LOD_FRACTION)) != 0; }
    bool usesHwLighting() const override { return false; }
    bool getBinaryForm(std::vector<char> &) override { return false; }

private:
    CombinerKey m_key;
    u32 m_inputs;
    pica_cmb::Program m_tev;
};

class PicaSpecialProgram : public ShaderProgram {
public:
    explicit PicaSpecialProgram(Kind k) : m_kind(k) {}
    void activate() override { set_current_program(this, m_kind); }

private:
    Kind m_kind;
};

class PicaTexrectDrawerProgram : public TexrectDrawerShaderProgram {
public:
    void activate() override { set_current_program(this, Kind::Copy); }
    void setTextureSize(u32, u32) override {}
    void setEnableAlphaTest(int) override {}
};

class PicaTextProgram : public TextDrawerShaderProgram {
public:
    void activate() override { set_current_program(this, Kind::Text); }
    void setTextColor(float *) override {}
};

/* Same input analysis as the GLSL backend (glsl_CombinerProgramBuilder.cpp), including its
 * first/second-cycle parameter corrections. */
u32 correct_first(u32 p) { return p == G_GCI_TEXEL1 ? G_GCI_TEXEL0 : p == G_GCI_TEXEL1_ALPHA ? G_GCI_TEXEL0_ALPHA : p; }
u32 correct_first_2cyc(u32 p) { return p == G_GCI_COMBINED ? G_GCI_HALF : p; }
u32 correct_second(u32 p)
{
    switch (p) {
    case G_GCI_TEXEL0: return G_GCI_TEXEL1;
    case G_GCI_TEXEL1: return G_GCI_TEXEL0;
    case G_GCI_TEXEL0_ALPHA: return G_GCI_TEXEL1_ALPHA;
    case G_GCI_TEXEL1_ALPHA: return G_GCI_TEXEL0_ALPHA;
    default: return p;
    }
}

u32 collect_inputs(const CombinerStage &st, u32 (*fix)(u32))
{
    u32 in = 0;
    for (u32 i = 0; i < st.numOps; i++) {
        const CombinerOp &op = st.op[i];
        u32 p[3] = {op.param1, op.param2, op.param3};
        for (u32 k = 0; k < 3; k++) {
            u32 v = fix ? fix(p[k]) : p[k];
            if (v < 32)
                in |= 1u << v;
        }
    }
    return in;
}

/* ---- frame buffer readback ------------------------------------------------------------------ */

/* One GPU readback: the rows of a render target untiled into a linear buffer, and the format
 * GLideN64 wants them in */
struct ReadJob {
    s32 x0 = 0, y0 = 0;
    u32 w = 0, h = 0, hh = 0, ww = 0, s0 = 0, s1 = 0, tw = 0, th = 0, bpp = 4;
    bool rgba16 = false, pm_blue = false;
};

/* A linear buffer the GPU untiles into, and the read queued into it (if any) */
struct AsyncReadback {
    u32 *lin = nullptr;
    size_t bytes = 0;
    u64 fence = 0;
    bool pending = false; /* queued, not yet waited for */
    bool valid = false;   /* lin holds (or will hold) job */
    ReadJob job;
    bool reserve(size_t need);
    void finish();
    ~AsyncReadback();
};

class PicaColorBufferReader : public ColorBufferReader {
public:
    explicit PicaColorBufferReader(CachedTexture *tex) : ColorBufferReader(tex) {}
    AsyncReadback m_async; /* copyToRDRAM double-buffer mode */
    void cleanUp() override {}
    /* The base class would read into its own two buffers (allocated per frame buffer, which the
     * patched constructor no longer does) and then copy the first w*h*bytes onto themselves, as
     * stride = width and the formats are integer here: the texels are returned as read, from one
     * buffer for all readers (they are used one at a time). */
    const u8 *readPixels(s32 x0, s32 y0, u32 width, u32 height, u32 size, bool sync) override;

private:
    const u8 *_readPixels(const ReadColorBufferParams &params, u32 &heightOffset, u32 &stride) override;
};

class PicaPixelReadBuffer : public PixelReadBuffer {
public:
    /* the zeroed storage is made on first use (1.25 MB for the depth copy, which many games
     * never do: heap the Old 3DS's 80 MB mode cannot spare) */
    explicit PicaPixelReadBuffer(size_t size) : m_size(size) {}
    void readPixels(s32, s32, u32, u32, Parameter, Parameter) override {}
    void *getDataRange(u32 offset, u32 range) override
    {
        if (offset + range > m_size)
            return nullptr;
        if (m_data.empty())
            m_data.resize(m_size, 0);
        return m_data.data() + offset;
    }
    void closeReadBuffer() override {}
    void bind() override {}
    void unbind() override {}

private:
    size_t m_size;
    std::vector<u8> m_data;
};

struct PicaFbTexFormats : public FramebufferTextureFormats {
    PicaFbTexFormats()
    {
        colorInternalFormat = internalcolorFormat::RGBA8;
        colorFormat = colorFormat::RGBA;
        colorType = datatype::UNSIGNED_BYTE;
        colorFormatBytes = 4;
        monochromeInternalFormat = internalcolorFormat::RGBA8;
        monochromeFormat = colorFormat::RGBA;
        monochromeType = datatype::UNSIGNED_BYTE;
        monochromeFormatBytes = 4;
        depthInternalFormat = internalcolorFormat::DEPTH;
        depthFormat = colorFormat::DEPTH;
        depthType = datatype::UNSIGNED_INT;
        depthFormatBytes = 4;
        depthImageInternalFormat = internalcolorFormat::RGBA8;
        depthImageFormat = colorFormat::RGBA;
        depthImageType = datatype::UNSIGNED_BYTE;
        depthImageFormatBytes = 4;
        lutInternalFormat = internalcolorFormat::RGBA8;
        lutFormat = colorFormat::RGBA;
        lutType = datatype::UNSIGNED_BYTE;
        lutFormatBytes = 4;
        fontInternalFormat = internalcolorFormat::RGBA8;
        fontFormat = colorFormat::RGBA;
        fontType = datatype::UNSIGNED_BYTE;
        fontFormatBytes = 4;
    }
};

/* ---- vertex format -------------------------------------------------------------------------- */

struct GVtx {
    float x, y, z, w;
    u8 c[4];
    float s0, t0, s1, t1;
};

const PicaAttrib k_layout[4] = {
    {GPU_FLOAT, 4},
    {GPU_UNSIGNED_BYTE, 4},
    {GPU_FLOAT, 2},
    {GPU_FLOAT, 2},
};

inline u8 to_u8(float v)
{
    /* round to nearest and clamp to 0..255 (float->int conversion truncates; USAT clamps) */
    return (u8)__usat((int)(v * 255.0f + 0.5f), 8);
}

/* floorf without the libm call (VFPv2 has no rounding instruction); |v| < 2^31 */
inline float fast_floor(float v)
{
    const float t = (float)(int)v;
    return t > v ? t - 1.0f : t;
}

/* The N64 viewport's z: the RDP's depth is z / w * vscale + vtrans, 511 and 511 (of 1023) for the
 * whole range. Games use it to move what they draw in depth: Conker's shadow has vtrans 507, which
 * lifts it 256/65536 over the ground its polygons lie under (the shadow flickered: the desktop
 * build does this in the fragment shader, uDepthScale). zp = -(z * a + w * b). */
struct ViewportZ {
    float a = 0.5f, b = 0.5f;
    bool set = false; /* not the default */
    ViewportZ()
    {
        const float k = 1024.0f / 1022.0f, vs = gSP.viewport.vscale[2], vt = gSP.viewport.vtrans[2];
        if (g_novpz || (fabsf(vs - 511.0f / 1024.0f) < 1.5f / 1024.0f && fabsf(vt - 511.0f / 1024.0f) < 1.5f / 1024.0f))
            return;
        a = vs * k;
        b = vt * k;
        set = true;
    }
};

/* The N64's far plane (1022 of 1023) lies just in front of a cleared Z buffer; with the far plane at
 * the clear value, geometry exactly on it failed the depth test (the backgrounds of WipeOut 64's
 * menus). */
constexpr float k_depth_scale = -(1.0f - 1.0f / 32768.0f);

/* Decals (ZMODE_DEC) use GL polygon offset (factor -3, units -3): offset = -3 * max depth slope
 * - 3 * r. The PICA's depth offset is a per-draw constant, so the slope term is computed on the
 * CPU from the draw's triangles (poly_offset_for_draw); this is the floor for flat or
 * screen-aligned geometry (r taken as 2^-16, the PICA interpolating depth with less than 24 bits). */
const float k_poly_offset = -3.0f / 65536.0f;
constexpr float k_poly_factor = -3.0f;

constexpr u32 WINDOW_W = 400, WINDOW_H = 240;
constexpr u32 TEX_WINDOW_COLOR = 0xFFFF0001u, TEX_WINDOW_DEPTH = 0xFFFF0002u;

/* ---- the context ---------------------------------------------------------------------------- */

struct TexXform;

class ContextImplPica : public graphics::ContextImpl {
public:
    /* lifecycle */
    void init() override;
    void destroy() override;

    /* state */
    void setClampMode(ClampMode mode) override { m_clamp = mode; }
    ClampMode getClampMode() override { return m_clamp; }
    void enable(EnableParam p, bool on) override;
    u32 isEnabled(EnableParam p) override;
    void cullFace(CullModeParam mode) override { m_cullMode = u32(mode); }
    void enableDepthWrite(bool on) override { m_depthWrite = on; }
    void setDepthCompare(CompareParam mode) override { m_depthFunc = u32(mode); }
    void setViewport(s32 x, s32 y, s32 w, s32 h) override { m_vp[0] = x; m_vp[1] = y; m_vp[2] = w; m_vp[3] = h; }
    void setScissor(s32 x, s32 y, s32 w, s32 h) override { m_sc[0] = x; m_sc[1] = y; m_sc[2] = w; m_sc[3] = h; }
    /* GLideN64's legacy blending gives alpha the colour's factors: a see-through draw then lowers
     * the stored alpha (a*a + dst*(1-a)), where the N64 keeps coverage (an opaque pixel stays
     * opaque). Gauntlet Legends' HUD is drawn into a buffer that is copied to the screen with an
     * alpha test of 0.94: the 49% glow over the portrait made that area fail it (a black box).
     * Alpha is composited "over" instead: src + dst * (1 - src). Dev switch noalphaover.txt: off. */
    void setBlending(BlendParam s, BlendParam d) override
    {
        if (u32(s) == BL_SRC_ALPHA && u32(d) == BL_ONE_MINUS_SRC_ALPHA && !g_noalphaover)
            setBlendingSeparate(s, d, BlendParam(BL_ONE), d);
        else
            setBlendingSeparate(s, d, s, d);
    }
    void setBlendingSeparate(BlendParam sc, BlendParam dc, BlendParam sa, BlendParam da) override
    {
        m_blend[0] = u32(sc); m_blend[1] = u32(dc); m_blend[2] = u32(sa); m_blend[3] = u32(da);
    }
    void setBlendColor(f32 r, f32 g, f32 b, f32 a) override { m_blendColor = pica_const_color(to_u8(r), to_u8(g), to_u8(b), to_u8(a)); }
    void clearColorBuffer(f32 r, f32 g, f32 b, f32 a) override;
    void clearDepthBuffer() override;
    void setPolygonOffset(f32, f32) override {}

    /* textures */
    ObjectHandle createTexture(Parameter) override;
    void deleteTexture(ObjectHandle name) override;
    void init2DTexture(const Context::InitTextureParams &p) override;
    void update2DTexture(const Context::UpdateTextureDataParams &p) override;
    void setTextureParameters(const Context::TexParameters &p) override;
    void bindTexture(const Context::BindTextureParameters &p) override;
    void setTextureUnpackAlignment(s32 a) override { m_unpack = a; }
    s32 getTextureUnpackAlignment() const override { return m_unpack; }
    s32 getMaxTextureSize() const override { return 1024; }
    f32 getMaxAnisotropy() const override { return 0.0f; }
    void bindImageTexture(const Context::BindImageTextureParameters &) override {}
    u32 convertInternalTextureFormat(u32 f) const override { return f; }
    void textureBarrier() override {}

    /* frame buffers */
    FramebufferTextureFormats *getFramebufferTextureFormats() override { return new PicaFbTexFormats; }
    ObjectHandle createFramebuffer() override;
    void deleteFramebuffer(ObjectHandle name) override;
    void bindFramebuffer(BufferTargetParam target, ObjectHandle name) override;
    void addFrameBufferRenderTarget(const Context::FrameBufferRenderTarget &p) override;
    ObjectHandle createRenderbuffer() override { return createTexture(Parameter()); }
    void initRenderbuffer(const Context::InitRenderbufferParams &p) override;
    bool blitFramebuffers(const Context::BlitFramebuffersParams &) override { return false; }
    void setDrawBuffers(u32) override {}
    PixelReadBuffer *createPixelReadBuffer(size_t size) override { return new PicaPixelReadBuffer(size); }
    ColorBufferReader *createColorBufferReader(CachedTexture *tex) override { return new PicaColorBufferReader(tex); }

    /* programs */
    bool isCombinerProgramBuilderObsolete() override { return false; }
    void resetCombinerProgramBuilder() override {}
    CombinerProgram *createCombinerProgram(Combiner &color, Combiner &alpha, const CombinerKey &key) override;
    bool saveShadersStorage(const Combiners &) override { return false; }
    bool loadShadersStorage(Combiners &) override { return false; }
    ShaderProgram *createDepthFogShader() override { return new PicaSpecialProgram(Kind::Noop); }
    TexrectDrawerShaderProgram *createTexrectDrawerDrawShader() override { return new PicaTexrectDrawerProgram; }
    ShaderProgram *createTexrectDrawerClearShader() override { return new PicaSpecialProgram(Kind::Clear); }
    ShaderProgram *createTexrectUpscaleCopyShader() override { return new PicaSpecialProgram(Kind::Copy); }
    ShaderProgram *createTexrectColorAndDepthUpscaleCopyShader() override { return nullptr; }
    ShaderProgram *createTexrectDownscaleCopyShader() override { return new PicaSpecialProgram(Kind::Copy); }
    ShaderProgram *createTexrectColorAndDepthDownscaleCopyShader() override { return nullptr; }
    ShaderProgram *createGammaCorrectionShader() override { return new PicaSpecialProgram(Kind::Copy); }
    ShaderProgram *createFXAAShader() override { return new PicaSpecialProgram(Kind::Copy); }
    TextDrawerShaderProgram *createTextDrawerShader() override { return new PicaTextProgram; }
    void resetShaderProgram() override { m_program = nullptr; m_kind = Kind::Noop; }

    /* drawing */
    void drawTriangles(const Context::DrawTriangleParameters &p) override;
    void drawRects(const Context::DrawRectParameters &p) override;
    void clipRect(GVtx *v, u32 n);
    void drawLine(f32, SPVertex *) override {}
    f32 getMaxLineWidth() override { return 1.0f; }

    /* misc */
    bool isSupported(SpecialFeatures) const override { return false; }
    s32 getMaxMSAALevel() override { return 0; }
    bool isError() const override { return false; }
    bool isFramebufferError() const override { return false; }

    /* backend internals */
    void setProgram(const CombinerProgram *p, Kind k) { m_program = p; m_kind = k; }
    void present();
    void endVI();
    bool skipDraw(bool clear = false);
    void healSkipped(Tex &c);
    void healForRead(Tex *t) {
        /* the game reads the buffer back (Pokemon Stadium snapshots its screen to draw it again as
         * the entry selection background, GitHub #4): a buffer that missed a frame gets the last
         * picture shown first, else the snapshot keeps the stale picture for good */
        if (t && t->skipped) {
            healSkipped(*t);
            t->skipped = false;
        }
    }
    const u8 *readTexture(CachedTexture *tex, s32 x0, s32 y0, u32 w, u32 h, std::vector<u8> &out);
    bool depthToRdram(u32 fbo, u32 w, u32 h, u32 sc, u32 key, u16 *dst);
    const u8 *readTextureAsync(CachedTexture *tex, s32 x0, s32 y0, u32 w, u32 h, AsyncReadback &a,
                               std::vector<u8> &out);

private:
    Tex *tex(u32 id)
    {
        /* a few lookups per draw, mostly of the same ids: the last two found are kept (map nodes
         * do not move; m_tex.erase/clear forget them) */
        if (id == m_texHit[0].id && id)
            return m_texHit[0].t;
        if (id == m_texHit[1].id && id)
            return m_texHit[1].t;
        auto it = m_tex.find(id);
        if (it == m_tex.end())
            return nullptr;
        m_texHit[1] = m_texHit[0];
        m_texHit[0] = TexHit{id, &it->second};
        return &it->second;
    }
    struct TexHit { u32 id; Tex *t; };
    TexHit m_texHit[2] = {{0, nullptr}, {0, nullptr}};
    void forgetTexHits() { m_texHit[0] = m_texHit[1] = TexHit{0, nullptr}; }
    Fbo &drawFbo() { return m_drawFbo == 0 ? m_window : m_fbo[m_drawFbo]; }
    /* The window is stored the way round the LCD is (240x400, turned by 90 degrees), so the display
     * transfer reads it as it is: no draw of it into a screen target first. Draws into it are
     * turned: clip space (x, y) -> (y, -x), viewport and scissor with them. nowinrot.txt: off. */
    bool turned() const { return m_winRot && m_drawFbo == 0; }
    void turnVertices(GVtx *v, u32 n) const
    {
        if (!turned())
            return;
        for (u32 i = 0; i < n; i++) {
            const float x = v[i].x;
            v[i].x = v[i].y;
            v[i].y = -x;
        }
    }
    bool m_winRot = true;
    void allocStorage(Tex &t, bool vram, bool reuse = false, bool zero = true);
    void moveToVram(u32 id, bool copy = true);
    void gtDraw(int kind);
    bool evictToFcram(u32 id);
    void keepInVram(Fbo &f, int use);
    /* sizes of color targets made and deleted within a frame after a few draws, as of late, and
     * the rows those drew to */
    struct Brief {
        u16 w = 0, h = 0, rows = 0;
        u32 frame = 0;
    };
    Brief m_brief[4];
    u32 m_briefNext = 0;
    const Brief *brief(const Tex &t) const
    {
        if (t.depth || t.ndraw_last != ~0u || t.ndraw > 4)
            return nullptr;
        for (const Brief &b : m_brief)
            if (b.w == t.w && b.h == t.h && b.frame + 4 >= pica_frame_number())
                return &b;
        return nullptr;
    }
    u16 m_drawTop = 0xFFFF; /* rows the coming draw reaches (drawRects); 0xFFFF: unknown */
    void growStorage(u32 id);
    bool inFlight(const Tex &t) const;
    void releaseStorage(Tex &t);
    void collectPending(bool all);
    void *gpuAlloc(u32 bytes, bool vram_first, bool &is_vram, bool depth = false);
    void storageChanged(u32 id);
    void ensureStorage(u32 id);
    void writeTexels(Tex &t, const u32 *px, u32 x0, u32 y0, u32 w, u32 h);
    bool stagedUpdate(Tex &t, const u32 *px, u32 x0, u32 y0, u32 w, u32 h);
    void rebuildFbo(Fbo &f);
    bool applyState(bool depth_allowed);
    /* drawTriangles: the render mode (othermode low word) of triangles drawn into the N64 Z
     * buffer's address, and the VIs since; 0 = none (Pilotwings 64 shadow) */
    u32 m_zPassMode = 0;
    u32 m_zPassAge = 0;
    void bindUnit(int unit, bool n64_filter = false);
    bool setupCombiner(bool rect);
    enum { USE_DRAW, USE_COLOR_CLEARED, USE_DEPTH_ONLY }; /* how materialize's caller uses the FBO */
    void materialize(Fbo &f, int use = USE_DRAW);
    float poly_offset_for_draw(const GVtx *v, const u16 *idx, u32 count, GPU_Primitive_t prim);
    GVtx *linearizeShade(GVtx *v, u32 &n, const u16 *in, u32 nin, bool fog_alpha, u16 *&idx, u32 &nidx);
    const char *stateString()
    {
        static char b[200];
        const Tex *t0 = tex(m_unit[0]), *t1 = tex(m_unit[1]);
        snprintf(b, sizeof(b), "depth %d func %u write %d poff %d blend %d %u/%u fbo %u(%ux%u) t0 %u:%ux%u%s fmt%d t1 %u:%ux%u%s",
                 m_enDepth, m_depthFunc, m_depthWrite, m_enPolyOffset, m_enBlend, m_blend[0], m_blend[1], m_drawFbo, (unsigned)drawFbo().rt.width, (unsigned)drawFbo().rt.height,
                 m_unit[0], t0 ? t0->w : 0, t0 ? t0->h : 0, t0 && t0->data ? "" : "(null)", t0 ? (int)t0->fmt : -1,
                 m_unit[1], t1 ? t1->w : 0, t1 ? t1->h : 0, t1 && t1->data ? "" : "(null)");
        return b;
    }
    void texture_xforms(TexXform tx[2]);
    struct TileRect {
        u16 c0, c1, r0, r1; /* tile columns [c0, c1), tile rows [r0, r1) of the storage */
    };
    void *shadowOf(u32 id, Tex &t, bool &copied, TileRect need, const void *avoid);
    bool shadowDraw(void *dst, u16 sw, const Tex &t, TileRect need, u16 th16 = 0);
    void convert16(u32 id);
    int shadowDrawTest();
    int m_shdrawState = 0; /* shadowDraw self-test: 0 not run, 1 running, 2 passed, 3 failed, 4 no memory yet */
    u32 m_shdrawRetry = 0, m_shdrawTries = 0;
    u16 *m_quadIdx = nullptr; /* 0 1 2, 1 3 2 per quad, for shadowDraw */
    u32 m_shadowUse = 0;                              /* shadowOf LRU clock */
    const void *m_shadowBound[2] = {nullptr, nullptr}; /* shadow each unit samples in this draw */
    /* texture rectangles: the s and t ranges each unit samples (drawRects -> bindUnit), so a
     * shadow copy takes only those tiles */
    bool m_rowHint[2] = {false, false};
    float m_rowS[2][2], m_rowT[2][2];
    bool copyByTransfer(const Context::DrawRectParameters &p);
    bool m_xferCopy = true; /* copyByTransfer (dev switch noxfercopy.txt: off) */
    void *m_xferTemp[2] = {nullptr, nullptr};
    u32 m_xferTempBytes[2] = {0, 0};
    struct Shadow {
        void *p;
        u16 tw, th;
        u8 bpp;
        u32 owner, gen;
        const void *src;
        u8 nv; /* parts copied for (owner, gen, src) */
        TileRect v[8];
        u32 last_use;
    };
    std::vector<Shadow> m_shadows; /* one per storage size: the GPU copies in queue order */
    void ensureFrame()
    {
        if (!pica_in_frame()) {
            pica_frame_begin();
            collectPending(false);
        }
    }

    std::unordered_map<u32, Tex> m_tex;
    std::vector<u32> m_scratch; /* upload staging, reused (0xRRGGBBAA texels) */
    std::vector<GVtx> m_shadeScratch; /* linearizeShade() output, reused */
    std::vector<u16> m_shadeIdx;      /* and its indices */
    u32 m_shadeFrame = 0, m_shadeTris = 0; /* subdivided triangles this frame (budget) */
    std::vector<GVtx> m_idxScratch;   /* drawTriangles: indexed vertices before the expansion */
    std::vector<u32> m_idxUsed;       /* drawTriangles: m_idxGen = vertex referred to by this draw */
    std::vector<u16> m_idxRemap;      /* drawTriangles: its place in the compacted vertices */
    u32 m_idxGen = 0;
    std::unordered_map<u32, Fbo> m_fbo;
    u32 m_nextTex = 1, m_nextFbo = 1;
    Fbo m_window;
    u32 m_drawFbo = 0, m_readFbo = 0;
    u32 m_unit[2] = {0, 0};
    struct Pending {
        void *ptr;
        bool vram;
        u32 frame;
        u32 bytes;
    };
    static u32 storageBytes(const Tex &t) { return (u32)t.tw * t.th * (t.depth ? 4 : t.bpp); }
    void freeNow(void *p, bool vram, u32 bytes);
    std::vector<Pending> m_pending; /* storage freed while the GPU may still use it */

    ClampMode m_clamp = ClampMode::ClippingEnabled;
    bool m_enBlend = false, m_enCull = false, m_enDepth = false, m_enScissor = false, m_enPolyOffset = false;
    bool m_enDepthClamp = false;
    bool m_vramLru = true; /* keepInVram (dev switch novramlru.txt: off) */
    FILE *m_rtLog = nullptr; /* RTLOG dev switch rtlog.txt: render targets per present */
    u32 m_cullMode = CM_BACK;
    bool m_depthWrite = true;
    u32 m_depthFunc = CMP_LESS;
    s32 m_vp[4] = {0, 0, WINDOW_W, WINDOW_H};
    s32 m_sc[4] = {0, 0, WINDOW_W, WINDOW_H};
    u32 m_blend[4] = {BL_ONE, BL_ZERO, BL_ONE, BL_ZERO};
    u32 m_blendColor = 0;
    s32 m_unpack = 4;

    const CombinerProgram *m_program = nullptr;
    Kind m_kind = Kind::Noop;
    float m_cover = 0; /* frame buffer area drawn this frame, in pixels (noteCoverage) */
    void noteCoverage(const GVtx *v, u32 n);
    void noteDrawnRows(const GVtx *v, u32 n);
    void trimShadows(u32 id, Tex &c);
    bool m_showStale = false; /* the window copied a frame buffer with dropped draws (skipDraw) */
    bool m_showSame = false;  /* the window copied the buffer on screen, unchanged since */
    u32 m_winSrc = 0, m_winCopies = 0; /* this present's window copies */
    u32 m_shown = 0;          /* buffer on screen when the last present showed one copy */
    bool m_winClear = false;  /* a window clear waiting for the window's first draw kept */
    u32 m_winClearColor = 0;
    void clearTarget(Fbo &f, u32 rgba);

    shaderProgram_s m_shader{};
    u32 *m_half = nullptr; /* 8x8 texture of 0.5 for the combiner (texture unit 2) */
};

void set_current_program(const CombinerProgram *p, Kind k)
{
    if (g_ctx)
        g_ctx->setProgram(p, k);
}

/* ---- lifecycle ------------------------------------------------------------------------------ */

void ContextImplPica::init()
{
    CRUMB("gfx: init");
    ctr64_time_mark("gfx: init");
    SlowStep slow("init", 0);
    g_ctx = this;
    g_shadow_heavy = false;
    g_shadow_win_start = g_shadow_win_count = 0;
    init_morton();

    shaderProgramInit(&m_shader);
    DVLB_s *lib = video_shader_library();
    if (shaderProgramSetVsh(&m_shader, ctru_dvle(lib, 1)) < 0)
        printf("ctr64: gfx.v.pica missing from the shader library\n");
    /* its "proj" bool uniform (pica_projective); bool registers are uniform table entries
     * 0x78.., which shaderInstanceGetUniformLocation returns less 0x10 */
    pica_proj_bool_bit = -1;
    if (m_shader.vertexShader) {
        const int loc = shaderInstanceGetUniformLocation(m_shader.vertexShader, "proj");
        if (loc >= 0x68 && loc < 0x78)
            pica_proj_bool_bit = loc - 0x68;
    }

    m_half = static_cast<u32 *>(linearMemAlign(8 * 8 * 4, 0x80));
    if (m_half) {
        for (int i = 0; i < 64; i++)
            m_half[i] = 0x80808080;
        pica_cache_flush(m_half, 8 * 8 * 4);
    }

    if (FILE *rl = fopen("sdmc:/3ds/ctr64/rtlog.txt", "r")) {
        fclose(rl);
        m_rtLog = fopen("sdmc:/3ds/ctr64/rtlog.log", "w");
    }
    if (FILE *nx = fopen("sdmc:/3ds/ctr64/noxfercopy.txt", "r")) {
        fclose(nx);
        m_xferCopy = false;
    }
    if (FILE *pf = fopen("sdmc:/3ds/ctr64/potfb.txt", "r")) {
        fclose(pf);
        g_potfb = true;
    }
    if (FILE *ph = fopen("sdmc:/3ds/ctr64/potheight.txt", "r")) {
        fclose(ph);
        g_potheight = true;
    }
    if (FILE *nh = fopen("sdmc:/3ds/ctr64/lruold.txt", "r")) {
        fclose(nh);
        g_lruold = true;
    }
    if (FILE *nh = fopen("sdmc:/3ds/ctr64/noheavy.txt", "r")) {
        fclose(nh);
        g_noheavy = true;
    }
    if (FILE *ff = fopen("sdmc:/3ds/ctr64/fullshadow.txt", "r")) {
        fclose(ff);
        g_fullshadow = true;
    }
    if (FILE *fa = fopen("sdmc:/3ds/ctr64/noalpha1.txt", "r")) {
        fclose(fa);
        g_noalpha1 = true;
    }
    if (FILE *fd = fopen("sdmc:/3ds/ctr64/nodrawnrows.txt", "r")) {
        fclose(fd);
        g_nodrawnrows = true;
    }
    if (FILE *fs = fopen("sdmc:/3ds/ctr64/nobanks.txt", "r")) {
        fclose(fs);
        g_nobanks = true;
    }
    {   /* dev switch edclear.txt: the early depth buffer cleared at each target switch, as before */
        FILE *fs = fopen("sdmc:/3ds/ctr64/edclear.txt", "r");
        pica_early_clear = fs != nullptr;
        if (fs)
            fclose(fs);
    }
    g_fb16 = g_nofb16 = false;
    g_fb16_presents = g_fb16_moved = g_fb16_runs = 0;
    if (FILE *fs = fopen("sdmc:/3ds/ctr64/fb16.txt", "r")) {
        fclose(fs);
        g_fb16 = true;
    }
    if (FILE *fs = fopen("sdmc:/3ds/ctr64/nofb16.txt", "r")) {
        fclose(fs);
        g_nofb16 = true;
    }
    if (FILE *fs = fopen("sdmc:/3ds/ctr64/noshdraw.txt", "r")) {
        fclose(fs);
        g_noshdraw = true;
    }
    if (FILE *fs = fopen("sdmc:/3ds/ctr64/shcheck.txt", "r")) {
        fclose(fs);
        g_shcheck = true;
    }
    if (FILE *fs = fopen("sdmc:/3ds/ctr64/shazflush.txt", "r")) {
        fclose(fs);
        g_shazflush = true;
    }
    if (FILE *fs = fopen("sdmc:/3ds/ctr64/shflip.txt", "r")) {
        fclose(fs);
        g_shflip = true;
    }
    g_noidxdraw = false;
    if (FILE *ni = fopen("sdmc:/3ds/ctr64/noidxdraw.txt", "r")) {
        fclose(ni);
        g_noidxdraw = true;
    }
    pica_direct_arrays = false;
    if (FILE *da = fopen("sdmc:/3ds/ctr64/directarrays.txt", "r")) {
        fclose(da);
        pica_direct_arrays = true;
    }
    g_novramflush = false;
    if (FILE *nf = fopen("sdmc:/3ds/ctr64/novramflush.txt", "r")) {
        fclose(nf);
        g_novramflush = true;
    }
    g_noproj = false;
    if (FILE *np = fopen("sdmc:/3ds/ctr64/noproj.txt", "r")) {
        fclose(np);
        g_noproj = true;
    }
    g_noalphaover = false;
    if (FILE *nao = fopen("sdmc:/3ds/ctr64/noalphaover.txt", "r")) {
        fclose(nao);
        g_noalphaover = true;
    }
    g_noshadelin = false;
    if (FILE *ns = fopen("sdmc:/3ds/ctr64/noshadelin.txt", "r")) {
        fclose(ns);
        g_noshadelin = true;
    }
    g_noguard = false;
    if (FILE *ng = fopen("sdmc:/3ds/ctr64/noguard.txt", "r")) {
        fclose(ng);
        g_noguard = true;
    }
    if (FILE *st = fopen("sdmc:/3ds/ctr64/slowtex.txt", "r")) {
        fclose(st);
        g_slowtex = true;
    }
    if (FILE *cm = fopen("sdmc:/3ds/ctr64/nocrcmemo.txt", "r")) {
        fclose(cm);
        ctr64_crc_nomemo = true;
    }
    if (FILE *fc = fopen("sdmc:/3ds/ctr64/fbcheckfull.txt", "r")) {
        fclose(fc);
        ctr64_fbcheck_full = true;
    }
    if (FILE *fs = fopen("sdmc:/3ds/ctr64/noflatstash.txt", "r")) { /* gSP.cpp: flat triangles drawn at once */
        fclose(fs);
        ctr64_flat_stash = 0;
    }
    if (FILE *ns = fopen("sdmc:/3ds/ctr64/nostash.txt", "r")) { /* gSP.cpp: no triangle batching */
        fclose(ns);
        ctr64_stash = 0;
    }
    if (FILE *sv = fopen("sdmc:/3ds/ctr64/nostdvtx.txt", "r")) { /* gSP.cpp: generic vertex path */
        fclose(sv);
        ctr64_std_fast = 0;
    }
    if (FILE *no = fopen("sdmc:/3ds/ctr64/novpz.txt", "r")) {
        fclose(no);
        g_novpz = true;
    }
    if (FILE *nv = fopen("sdmc:/3ds/ctr64/novramlru.txt", "r")) {
        fclose(nv);
        m_vramLru = false;
    }
    /* GLideN64's default frame buffer: the 400x240 window, presented to the top LCD. With frame
     * buffer emulation it only receives the finished frame once per present: in FCRAM when
     * anti-aliasing needs the VRAM for the game's 2x targets (keepInVram). */
    /* VRAM with anti-aliasing too: the window is drawn every frame (GLideN64's copy of the shown
     * buffer, scaled) and read by every present; in FCRAM that copy draw cost ~7 ms of GPU time a
     * frame (HW, MK64 race with anti-aliasing: 82% -> 95% in VRAM). Dev switch nowinvram.txt: the
     * old rule (FCRAM when anti-aliasing needs the VRAM for the game's 2x targets). */
    bool win_vram = true;
    if (FILE *wv = fopen("sdmc:/3ds/ctr64/nowinvram.txt", "r")) {
        fclose(wv);
        win_vram = !(m_vramLru && config.frameBufferEmulation.enable && config.frameBufferEmulation.nativeResFactor > 1);
    }
    Tex &c = m_tex[TEX_WINDOW_COLOR];
    if (FILE *wr = fopen("sdmc:/3ds/ctr64/nowinrot.txt", "r")) {
        fclose(wr);
        m_winRot = false;
    }
    c.w = WINDOW_W; c.h = WINDOW_H;
    c.tw = m_winRot ? WINDOW_H : 512;
    c.th = m_winRot ? WINDOW_W : 256;
    allocStorage(c, win_vram);
    Tex &d = m_tex[TEX_WINDOW_DEPTH];
    d.w = WINDOW_W; d.h = WINDOW_H; d.tw = c.tw; d.th = c.th; d.depth = true;
    /* With anti-aliasing and frame buffer emulation the window depth buffer goes to FCRAM (the
     * window only receives copies there): its 512 KB of VRAM kept the game's second 2x color
     * buffer out (MK64, SSB: one 1.2 MB buffer moved into and one out of VRAM each frame; MK64
     * 94.5 -> 99.7% on HW). Star Fox 64's edge strips, seen with this on 2026-11-17, came from CPU
     * texel writes into a VRAM color buffer that were not flushed from the data cache
     * (writeTexels). Dev switches: winzvram.txt = in VRAM, nowinz.txt = no window depth buffer. */
    bool winz = true, winz_vram = win_vram;
    if (config.frameBufferEmulation.enable && config.frameBufferEmulation.nativeResFactor > 1) {
        winz_vram = false;
        if (FILE *wz = fopen("sdmc:/3ds/ctr64/winzvram.txt", "r")) {
            fclose(wz);
            winz_vram = win_vram;
        }
        if (FILE *wz = fopen("sdmc:/3ds/ctr64/nowinz.txt", "r")) {
            fclose(wz);
            winz = false;
        }
    }
    if (winz)
        allocStorage(d, winz_vram);
    m_window.color = TEX_WINDOW_COLOR;
    m_window.depth = TEX_WINDOW_DEPTH;
    rebuildFbo(m_window);
}

void ContextImplPica::destroy()
{
    CRUMB("gfx: destroy");
    pica_flush();
    pica_wait_idle();
    collectPending(true);
    for (auto &kv : m_tex) {
        Tex &t = kv.second;
        if (t.data)
            freeNow(t.data, t.vram, storageBytes(t));
    }
    m_tex.clear();
    forgetTexHits();
    m_fbo.clear();
    if (m_half)
        linearFree(m_half);
    m_half = nullptr;
    if (m_quadIdx)
        linearFree(m_quadIdx);
    m_quadIdx = nullptr;
    pica_forget_program(&m_shader); /* a later context may be allocated at the same address */
    shaderProgramFree(&m_shader);
    if (g_ctx == this)
        g_ctx = nullptr;
}

/* ---- state ---------------------------------------------------------------------------------- */

void ContextImplPica::enable(EnableParam p, bool on)
{
    CRUMB("gfx: enable");
    switch (u32(p)) {
    case EN_BLEND: m_enBlend = on; break;
    case EN_CULL_FACE: m_enCull = on; break;
    case EN_DEPTH_TEST: m_enDepth = on; break;
    case EN_SCISSOR_TEST: m_enScissor = on; break;
    case EN_POLYGON_OFFSET_FILL: m_enPolyOffset = on; break;
    case EN_DEPTH_CLAMP: m_enDepthClamp = on; break;
    default: break;
    }
}

u32 ContextImplPica::isEnabled(EnableParam p)
{
    CRUMB("gfx: isEnabled");
    switch (u32(p)) {
    case EN_BLEND: return m_enBlend;
    case EN_CULL_FACE: return m_enCull;
    case EN_DEPTH_TEST: return m_enDepth;
    case EN_SCISSOR_TEST: return m_enScissor;
    case EN_POLYGON_OFFSET_FILL: return m_enPolyOffset;
    case EN_DEPTH_CLAMP: return m_enDepthClamp;
    default: return 0;
    }
}

void ContextImplPica::clearColorBuffer(f32 r, f32 g, f32 b, f32 a)
{
    CRUMB("gfx: clearColorBuffer");
    if (m_drawFbo != 0) /* an N64 frame buffer, not the window GLideN64 clears at each present */
        ctr64_frame_cleared = true;
    m_enScissor = true;
    const u32 rgba = pica_rgba8(to_u8(r), to_u8(g), to_u8(b), to_u8(a));
    if (m_drawFbo == 0) { /* done with the first window draw kept (skipDraw) or at the present */
        m_winClear = true;
        m_winClearColor = rgba;
        return;
    }
    if (!skipDraw(true))
        clearTarget(drawFbo(), rgba);
}

/* The whole buffer (the fill engine ignores the scissor). The GL backend re-enables scissor after
 * a clear, which clearColorBuffer does: not here, a deferred window clear runs at a later draw. */
void ContextImplPica::clearTarget(Fbo &f, u32 rgba)
{
    if (Tex *t = g_fb16 ? tex(f.color) : nullptr)
        if (t->n16 && t->rt && t->fmt == GPU_RGBA8) { /* its contents go now: 16 bits from here on */
            releaseStorage(*t);
            t->fmt = GPU_RGBA5551;
            t->bpp = 2;
            t->th = t->th_full ? t->th_full : t->th;
            t->th_full = 0;
            t->lazy = true;
            storageChanged(f.color);
        }
    materialize(f, USE_COLOR_CLEARED);
    if (!f.valid)
        return;
    ensureFrame();
    ctr64_gfx_stats[ST_CLEAR]++;
    pica_clear(&f.rt, rgba, false, 0);
    if (Tex *t = tex(f.color)) {
        t->cleared = rgba;
        t->dirty = true;
        t->gen++;
        t->used_frame = pica_frame_number();
    }
}

void ContextImplPica::clearDepthBuffer()
{
    CRUMB("gfx: clearDepthBuffer");
    if (skipDraw(true))
        return;
    Fbo &f = drawFbo();
    materialize(f, USE_DEPTH_ONLY); /* the color buffer moves at its next use: often a full clear */
    if (!f.valid || !f.rt.depth)
        return;
    ensureFrame();
    /* Mario Tennis's VS screen clears it in 225 strips per frame; each is a clear of all of it
     * here, 2 ms (HW 2026-11-22: 6 VI/s) */
    Tex *d = tex(f.depth);
    const u32 area = (u32)f.rt.width * f.rt.height;
    if (!d || d->data != f.rt.depth || d->zclean < area) {
        pica_clear_depth(&f.rt, 0xFFFFFF);
        if (d && d->data == f.rt.depth)
            d->zclean = area;
    }
    m_enScissor = true;
}

/* ---- textures ------------------------------------------------------------------------------- */

ObjectHandle ContextImplPica::createTexture(Parameter)
{
    CRUMB("gfx: createTexture");
    u32 id = m_nextTex++;
    m_tex[id] = Tex();
    return ObjectHandle(id);
}

void ContextImplPica::deleteTexture(ObjectHandle name)
{
    CRUMB("gfx: deleteTexture");
    u32 id = u32(name);
    Tex *t = tex(id);
    if (!t || id >= TEX_WINDOW_COLOR)
        return;
    if (t->rt && !t->depth && t->ndraw && t->born == pica_frame_number() && t->rows_used) {
        Brief *b = nullptr;
        for (Brief &x : m_brief)
            if (x.w == t->w && x.h == t->h)
                b = &x;
        if (t->ndraw > 4 || t->rows_used == 0xFFFF) {
            if (b)
                b->w = 0;
        } else {
            if (!b || b->frame + 4 < t->born) {
                b = b ? b : &m_brief[m_briefNext++ % 4];
                *b = Brief{t->w, t->h, 0, t->born};
            }
            b->frame = t->born;
            b->rows = std::max(b->rows, t->rows_used);
        }
    }
    releaseStorage(*t);
    m_tex.erase(id);
    forgetTexHits();
    for (auto &u : m_unit)
        if (u == id)
            u = 0;
    storageChanged(id); /* a frame buffer it was attached to drops the freed storage */
}

/* ---- GPU memory lifetime ---------------------------------------------------------------------
 * The GPU runs up to a frame behind the CPU (two command slots), so texture memory the GPU may
 * still read or write is never reused in place:
 *   - freed storage goes to m_pending and is released once the last frame that used it is done
 *     (frame n is complete when frame n + 2 begins);
 *   - CPU updates to a texture still in flight go to fresh storage ("orphaning"), except for
 *     render targets, whose GPU-written contents must be kept: those wait for the GPU. */

bool ContextImplPica::inFlight(const Tex &t) const
{
    return t.data && t.used_frame + 2 > pica_frame_number();
}

extern "C" void *ctr64_xlinear_alloc(u32 size, u32 align);

/* libctru's linear allocator treats a pool with no free block left as never initialised: the
 * next linearAlloc re-creates it over the whole linear heap and hands out memory that is in use
 * (the screens, command lists, textures). Seen with anti-aliasing in the Mario Kart 64 menus:
 * hundreds of small textures took the last bytes, then garbage on both screens and a crash.
 * Every allocation here leaves this much free. */
static const u32 k_linear_margin = 256 * 1024;

static void *linear_alloc_safe(u32 bytes, u32 align)
{
    if ((u64)bytes + k_linear_margin + align > linearSpaceFree())
        return nullptr;
    return linearMemAlign(bytes, align);
}
extern "C" bool ctr64_xlinear_free(void *p);

void ContextImplPica::freeNow(void *p, bool vram, u32 bytes)
{
    ctr64_gfx_mem[vram ? 1 : 0] -= bytes;
    if (vram)
        vramFree(p);
    else if (!ctr64_xlinear_free(p))
        linearFree(p);
}

void ContextImplPica::releaseStorage(Tex &t)
{
    if (!t.data)
        return;
    const u32 bytes = storageBytes(t);
    if (inFlight(t)) {
        m_pending.push_back({t.data, t.vram, t.used_frame, bytes});
        ctr64_gfx_mem[3] += bytes;
    } else {
        freeNow(t.data, t.vram, bytes);
    }
    t.data = nullptr;
    t.vram = false;
}

void ContextImplPica::collectPending(bool all)
{
    const u32 now = pica_frame_number();
    size_t keep = 0;
    for (size_t i = 0; i < m_pending.size(); i++) {
        Pending &p = m_pending[i];
        if (all || p.frame + 2 <= now) {
            freeNow(p.ptr, p.vram, p.bytes);
            ctr64_gfx_mem[3] -= p.bytes;
        } else
            m_pending[keep++] = p;
    }
    m_pending.resize(keep);
}

/* VRAM is two halves (A, B) with a bus each: a color and a depth buffer in different halves are
 * read and written at the same time. Depth buffers go to B when there is room, color to A. Dev
 * switch nobanks.txt: wherever there is room first. */

static void *vram_alloc(u32 bytes, bool depth)
{
    if (g_nobanks)
        return vramAlloc(bytes);
    void *p = vramAllocAt(bytes, depth ? VRAM_ALLOC_B : VRAM_ALLOC_A);
    return p ? p : vramAlloc(bytes);
}

void *ContextImplPica::gpuAlloc(u32 bytes, bool vram_first, bool &is_vram, bool depth)
{
    const char *where = ctr64_crumb;
    /* a size that failed after a full GPU drain this frame fails again without another drain
     * (a game that asks again every draw would otherwise wait for the GPU hundreds of times) */
    static u32 fail_frame = ~0u, fail_bytes = ~0u;
    bool drained = fail_frame == pica_frame_number() && bytes >= fail_bytes;
    for (int attempt = 0; attempt < 8; attempt++) {
        void *p = vram_first ? vram_alloc(bytes, depth) : nullptr;
        is_vram = p != nullptr;
        if (!p)
            p = linear_alloc_safe(bytes, 0x80); /* the PICA can also render to and sample FCRAM */
        if (!p)
            p = ctr64_xlinear_alloc(bytes, 0x80); /* main.c: New 3DS reserve (anti-aliasing) */
        if (p) {
            ctr64_gfx_mem[is_vram ? 1 : 0] += bytes;
            if (!is_vram && bytes > ctr64_gfx_mem[2])
                ctr64_gfx_mem[2] = bytes;
            if (bytes >= fail_bytes)
                fail_frame = ~0u; /* memory came back */
            return p;
        }
        /* out of memory: drop least recently used textures from GLideN64's cache (Textures.cpp),
         * then let the GPU finish and release everything pending */
        const bool evicted = ctr64_tex_evict(bytes) != 0;
        if (evicted)
            ctr64_gfx_evicted++;
        if (drained && !evicted)
            break; /* nothing new to free: one GPU drain per attempt that can help */
        drained = true;
        CTR64_PROF(29);
        pica_flush();
        pica_wait_idle();
        collectPending(true);
    }
    if (bytes < fail_bytes || fail_frame != pica_frame_number()) {
        fail_frame = pica_frame_number();
        fail_bytes = bytes;
    }
    ctr64_gfx_stats[ST_ALLOC_FAIL]++;
    { /* development aid: with memlog.txt, the GPU memory held at the first failure -> memlog.log */
        static bool done;
        FILE *ml;
        if (!done && (ml = fopen("sdmc:/3ds/ctr64/memlog.txt", "r"))) {
            fclose(ml);
            done = true;
            if ((ml = fopen("sdmc:/3ds/ctr64/memlog.log", "w"))) {
                fprintf(ml, "fail %u bytes at %s, linear free %u\n", bytes, where, (unsigned)linearSpaceFree());
                for (auto &kv : m_tex)
                    if (kv.second.data)
                        fprintf(ml, "tex %u %ux%u store %ux%u bpp %u depth %d vram %d rt %d lazy %d used %u drawn_frame %u bytes %u\n",
                                kv.first, kv.second.w, kv.second.h, kv.second.tw, kv.second.th, kv.second.bpp, kv.second.depth,
                                kv.second.vram, kv.second.rt, kv.second.lazy, kv.second.used_frame, kv.second.draw_frame, storageBytes(kv.second));
                for (auto &p : m_pending)
                    fprintf(ml, "pending %u vram %d frame %u\n", p.bytes, p.vram, p.frame);
                fclose(ml);
            }
        }
    }
    ctr64_allocfail_where = where;
    ctr64_allocfail_bytes = bytes;
    return nullptr;
}

void ContextImplPica::allocStorage(Tex &t, bool vram, bool reuse, bool zero)
{
    CRUMB("gfx: allocStorage");
    t.zclean = 0;
    const u32 bytes = storageBytes(t);
    if (reuse) {
        /* Storage of the same size released in the last frames (still pending: the GPU may be
         * reading it) is taken over instead of allocating anew: the GPU runs its queue in order,
         * so it is done with the old contents before it zero-fills them for the new owner, and
         * the CPU never touches it. OoT's pause menu with anti-aliasing re-creates a 2 MB frame
         * buffer texture every frame: each allocation then found no room until the GPU drained
         * (HW profile 2026-11-09: 845 ms of allocStorage waits in its slow seconds). */
        int best = -1;
        for (size_t i = 0; i < m_pending.size(); i++)
            if (m_pending[i].bytes == bytes && (best < 0 || (m_pending[i].vram && !m_pending[best].vram) ||
                                                (m_pending[i].vram == m_pending[best].vram && vram == m_pending[i].vram &&
                                                 m_pending[i].frame < m_pending[best].frame)))
                best = (int)i;
        if (best >= 0 && (m_pending[best].vram || !vram || vramSpaceFree() < bytes)) {
            t.data = m_pending[best].ptr;
            t.vram = m_pending[best].vram;
            ctr64_gfx_mem[3] -= bytes;
            m_pending.erase(m_pending.begin() + best);
            pica_fill_zero(t.data, bytes);
            t.used_frame = 0;
            t.dirty = true;
            t.gen++;
            ctr64_gfx_reused++;
            return;
        }
    }
    t.data = gpuAlloc(bytes, vram, t.vram, t.depth);
    if (t.data && !t.vram) {
        if (zero) {
            memset(t.data, 0, bytes);
            pica_cache_flush(t.data, bytes);
        }
    } else if (t.data) {
        pica_fill_zero(t.data, bytes); /* VRAM holds what the last app left (SM64's first frame) */
    }
    t.used_frame = 0;
    t.dirty = true;
    t.gen++;
}

void ContextImplPica::storageChanged(u32 id)
{
    if (m_window.color == id || m_window.depth == id)
        rebuildFbo(m_window);
    for (auto &kv : m_fbo)
        if (kv.second.color == id || kv.second.depth == id)
            rebuildFbo(kv.second);
}

void ContextImplPica::moveToVram(u32 id, bool copy)
{
    CRUMB("gfx: moveToVram");
    Tex *t = tex(id);
    if (!t || t->vram || !t->data)
        return;
    const u32 bytes = storageBytes(*t);
    void *v = vram_alloc(bytes, t->depth);
    if (!v)
        return; /* keep rendering to FCRAM */
    ctr64_gfx_mem[1] += bytes;
    /* a GPU copy, queued after the rendering so far (a CPU store to VRAM faults on hardware);
     * the old storage stays allocated until that copy has run (in flight, freed 2 frames on) */
    if (copy)
        pica_copy(v, t->data, bytes);
    t->used_frame = pica_frame_number();
    releaseStorage(*t);
    t->data = v;
    t->vram = true;
    t->dirty = true;
    t->gen++;
    storageChanged(id);
}

/* Render targets in FCRAM are much slower to draw into than in VRAM. With anti-aliasing a
 * 640x480 target takes 2 MB (1024x512 storage) and the 6 MB of VRAM holds only two of a game's
 * color buffers + depth buffer, so most drawing went to FCRAM (2026-11-09, SSB intro in Azahar:
 * every draw had an FCRAM depth buffer, 65% an FCRAM color buffer; none at 1x). The targets drawn
 * now are kept in VRAM: a target not in VRAM displaces the one drawn longest ago (not in this
 * frame), which a GPU copy moves to FCRAM. VRAM is only ever written by the GPU, in queue order,
 * so the displaced storage is reusable at once. Dev switch novramlru.txt: off. */
bool ContextImplPica::evictToFcram(u32 id)
{
    Tex *t = tex(id);
    if (!t || !t->vram || !t->data)
        return false;
    const u32 bytes = storageBytes(*t);
    void *l = linear_alloc_safe(bytes, 0x80);
    if (!l)
        l = ctr64_xlinear_alloc(bytes, 0x80);
    if (!l)
        return false;
    ctr64_gfx_mem[0] += bytes;
    pica_cache_flush(l, bytes); /* no dirty CPU lines left to overwrite what the GPU writes */
    pica_copy_from_vram(l, t->data, bytes); /* after everything drawn into it so far */
    freeNow(t->data, true, bytes);
    t->data = l;
    t->vram = false;
    t->dirty = true;
    t->gen++;
    t->used_frame = pica_frame_number();
    ctr64_gfx_vram_moves[1] += bytes >> 10;
    storageChanged(id);
    return true;
}

void ContextImplPica::keepInVram(Fbo &f, int use)
{
    SlowStep slow("keepInVram", 0);
    const u32 now = pica_frame_number();
    const u32 ids[2] = {f.depth, f.color}; /* the depth buffer first: read and written by every draw */
    for (int k = 0; k < (use == USE_DEPTH_ONLY ? 1 : 2); k++) {
        Tex *t = tex(ids[k]);
        if (!t || !t->data)
            continue;
        t->draw_frame = now;
        if (use == USE_DRAW)
            t->ndraw++;
        if (t->vram || !t->rt || !m_vramLru)
            continue;
        const bool copy = !(k == 1 && use == USE_COLOR_CLEARED); /* a full clear replaces the contents */
        const u32 bytes = storageBytes(*t);
        /* A color target drawn at most twice per present (GLideN64's native-resolution copies of
         * a 2x frame buffer for the RDRAM: one copy draw each) is not worth the VRAM another needs:
         * it is drawn in FCRAM, even when VRAM is free now (MK64 with anti-aliasing: three 1x copy
         * targets next to three 2x color buffers kept the VRAM one target short, 1.6 MB moved per
         * frame, Azahar 2026-11-10) */
        if (!t->depth && t->ndraw_last <= 2)
            continue;
        /* and one that lasts only a few draws stays where it is (materialize gives it VRAM if there
         * is room) */
        if (k == 1 && brief(*t))
            continue;
        moveToVram(ids[k], copy);
        while (!t->vram) {
            /* the VRAM target drawn longest ago, not in this frame and not this FBO's; among
             * those, rarely drawn targets first */
            u32 victim = 0, oldest = now;
            bool victim_rare = false;
            for (auto &kv : m_tex) {
                const Tex &v = kv.second;
                const bool rare = !v.depth && v.ndraw_last <= 2;
                /* a rare target drawn in this frame too: it is drawn in FCRAM from here on (MK64
                 * menus with AA: its three 1x copy targets, one drawn per frame, kept the VRAM one
                 * 2x buffer short, 1.6 MB moved per frame) */
                if (!v.vram || !v.rt || !v.data || kv.first == f.color || kv.first == f.depth ||
                    (v.draw_frame >= now && !rare))
                    continue;
                /* the one drawn longest ago if it is out of use (4 frames), else the one drawn last:
                 * of buffers drawn in turn (triple buffering) with one place too few, the next
                 * needed ones stay (Mario Tennis match, HW 2026-11-22: 51 -> 54 VI/s). Nothing else
                 * is kept out of the VRAM for it: the window buffer in FCRAM took 9 ms more per
                 * frame, small targets and the ball trail's buffer 5 ms. */
                const bool better = rare != victim_rare || !victim ? rare || !victim
                                    : g_lruold || v.draw_frame + 4 < now || oldest + 4 < now ? v.draw_frame < oldest
                                                                                             : v.draw_frame > oldest;
                if (better) {
                    oldest = v.draw_frame;
                    victim = kv.first;
                    victim_rare = rare;
                }
            }
            if (!victim || !evictToFcram(victim))
                break; /* all drawn in this frame: this one stays in FCRAM */
            moveToVram(ids[k], copy);
            t = tex(ids[k]);
        }
        if (t->vram)
            ctr64_gfx_vram_moves[0] += copy ? bytes >> 10 : 0, g_fb16_moved += copy ? bytes >> 10 : 0;
    }
}

void ContextImplPica::init2DTexture(const Context::InitTextureParams &p)
{
    CTR64_PROF(25);
    CRUMB("gfx: init2DTexture");
    const u32 id = u32(p.handle);
    Tex *t = tex(id);
    if (!t || p.mipMapLevel != 0)
        return;
    releaseStorage(*t);
    t->w = (u16)p.width;
    t->h = (u16)p.height;
    t->tw = pot(p.width);
    t->th = pot(p.height);
    t->depth = u32(p.internalFormat) == IF_DEPTH;
    t->lazy = false;
    if (p.data && !t->depth && u32(p.dataType) == DT_US_5551 && !g_slowtex && p.width && p.height) {
        /* RGBA5551 source (95% of the MK64 menu textures): unless it is all gray (LA8/LA4 would
         * be smaller), pick_format chooses RGBA5551 and the stored values are the source values:
         * copied into the tiles as they are (same bytes as the general path below) */
        CTR64_PROF(30);
        const u16 *src = static_cast<const u16 *>(p.data);
        const u32 n = (u32)p.width * p.height;
        u32 i = 0;
        while (i < n && !((src[i] ^ (src[i] >> 5)) & 0x07FE)) /* r == g == b: gray */
            i++;
        if (i < n) {
            t->fmt = GPU_RGBA5551;
            t->bpp = 2;
            {
                CTR64_PROF(26);
                allocStorage(*t, false, false, false);
            }
            storageChanged(id);
            if (!t->data)
                return;
            {
                CTR64_PROF(27);
                write_full<GPU_RGBA5551, true, u16>(*t, src, p.width, p.height, p.width);
            }
            t->drawn = t->ever = true;
            {
                CTR64_PROF(31);
                pica_cache_flush(t->data, storageBytes(*t));
            }
            t->dirty = true;
            t->gen++;
            return;
        }
    }
    if (p.data && !t->depth) {
        /* N64 texture data: store it in the smallest format that keeps every texel exact */
        std::vector<u32> &px = m_scratch;
        /* a bogus size (e.g. a negative tile width, see GLideN64 Textures.cpp) must cost one
         * missing texture, not an abort: the 3DS heap cannot take what a PC shrugs off */
        try {
            px.resize((size_t)p.width * p.height);
        } catch (const std::bad_alloc &) {
            FILE *lf = fopen("sdmc:/3ds/ctr64/texwarn.log", "a");
            if (lf) {
                fprintf(lf, "init2DTexture %ux%u: no memory, texture skipped\n", (unsigned)p.width, (unsigned)p.height);
                fclose(lf);
            }
            return;
        }
        {
            CTR64_PROF(30);
            fetch_all_fast(static_cast<const u8 *>(p.data), (u32)px.size(), u32(p.format), u32(p.dataType), px.data());
            t->fmt = g_slowtex ? pick_format(px) : pick_format_fast(px.data(), (u32)px.size());
        }
        t->bpp = bytes_per_texel(t->fmt);
        {
            CTR64_PROF(26);
            allocStorage(*t, false, false, g_slowtex); /* every byte is written below */
        }
        storageChanged(id);
        if (!t->data)
            return;
        CTR64_PROF(27);
        if (g_slowtex || !p.width || !p.height) {
            writeTexels(*t, px.data(), 0, 0, p.width, p.height);
            return;
        }
        switch (t->fmt) {
        case GPU_RGB8: write_full<GPU_RGB8>(*t, px.data(), p.width, p.height, p.width); break;
        case GPU_RGBA5551: write_full<GPU_RGBA5551>(*t, px.data(), p.width, p.height, p.width); break;
        case GPU_RGBA4: write_full<GPU_RGBA4>(*t, px.data(), p.width, p.height, p.width); break;
        case GPU_LA8: write_full<GPU_LA8>(*t, px.data(), p.width, p.height, p.width); break;
        case GPU_LA4: write_full<GPU_LA4>(*t, px.data(), p.width, p.height, p.width); break;
        default: write_full<GPU_RGBA8>(*t, px.data(), p.width, p.height, p.width); break;
        }
        t->drawn = t->ever = true;
        {
            CTR64_PROF(31);
            pica_cache_flush(t->data, storageBytes(*t));
        }
        t->dirty = true;
        t->gen++;
        return;
    }
    switch (u32(p.internalFormat)) {
    case IF_RGBA4: t->fmt = GPU_RGBA4; break;
    case IF_RGB5_A1: t->fmt = GPU_RGBA5551; break;
    default: t->fmt = GPU_RGBA8; break;
    }
    t->n16 = ctr64_fb_is16 && !t->depth;
    if (t->n16 && g_fb16)
        t->fmt = GPU_RGBA5551;
    t->bpp = t->depth ? 4 : bytes_per_texel(t->fmt);
    t->tw = fb_width(p.width);
    t->th = fb_height(t->tw, p.height);
    t->born = pica_frame_number();
    t->th_full = t->rows_used = 0;
    t->cleared = 0;
    /* No data: a render target or a texture filled later. Storage is allocated on first use,
     * so targets GLideN64 creates but never draws to (e.g. the 640x580 TexrectDrawer buffer
     * at native resolution) cost nothing. */
    t->lazy = true;
    storageChanged(id);
}

void ContextImplPica::ensureStorage(u32 id)
{
    Tex *t = tex(id);
    if (!t || t->data || !t->lazy)
        return;
    SlowStep slow(t->rt ? "frame buffer storage" : "texture storage", storageBytes(*t) >> 10);
    allocStorage(*t, t->rt, true);
    t->lazy = !t->data;
    storageChanged(id);
}

void ContextImplPica::writeTexels(Tex &t, const u32 *px, u32 x0, u32 y0, u32 w, u32 h)
{
    t.drawn = t.ever = true;
    if (g_slowtex) {
        switch (t.fmt) {
        case GPU_RGB8: write_rect<GPU_RGB8>(t, px, x0, y0, w, h); break;
        case GPU_RGBA5551: write_rect<GPU_RGBA5551>(t, px, x0, y0, w, h); break;
        case GPU_RGBA4: write_rect<GPU_RGBA4>(t, px, x0, y0, w, h); break;
        case GPU_LA8: write_rect<GPU_LA8>(t, px, x0, y0, w, h); break;
        case GPU_LA4: write_rect<GPU_LA4>(t, px, x0, y0, w, h); break;
        default: write_rect<GPU_RGBA8>(t, px, x0, y0, w, h); break;
        }
    } else {
        switch (t.fmt) {
        case GPU_RGB8: write_rect_fast<GPU_RGB8>(t, px, x0, y0, w, h); break;
        case GPU_RGBA5551: write_rect_fast<GPU_RGBA5551>(t, px, x0, y0, w, h); break;
        case GPU_RGBA4: write_rect_fast<GPU_RGBA4>(t, px, x0, y0, w, h); break;
        case GPU_LA8: write_rect_fast<GPU_LA8>(t, px, x0, y0, w, h); break;
        case GPU_LA4: write_rect_fast<GPU_LA4>(t, px, x0, y0, w, h); break;
        default: write_rect_fast<GPU_RGBA8>(t, px, x0, y0, w, h); break;
        }
    }
    const bool padded = x0 == 0 && y0 == 0 && w >= t.w && h >= t.h && (t.w < t.tw || t.h < t.th);
    if (padded)
        pad_edges(t);
    {
        /* flush the rows of 8x8 tiles written (storage rows run bottom-up). VRAM too: CPU stores
         * there go through the data cache like any other (a VRAM color buffer updated from RDRAM
         * without this showed stale garbage tiles on hardware: Star Fox 64's edge strips) */
        const u32 r_lo = y0 + h >= t.th ? 0 : t.th - (y0 + h), r_hi = y0 >= t.th ? 0 : t.th - 1 - y0;
        const u32 tile_row_bytes = (u32)(t.tw >> 3) * 64 * t.bpp;
        u8 *f0 = static_cast<u8 *>(t.data);
        u32 fn = storageBytes(t);
        if (!padded && h != 0) {
            f0 += (r_lo >> 3) * tile_row_bytes;
            fn = ((r_hi >> 3) - (r_lo >> 3) + 1) * tile_row_bytes;
        }
        if (t.vram && g_novramflush)
            ;
        else if (t.vram) /* the kernel call only (a refusal must not switch pica_cache_flush to GSP) */
            svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)f0, fn);
        else
            pica_cache_flush(f0, fn);
    }
    t.dirty = true;
    t.gen++;
}

/* Updates the 8x8 tiles that the rect (x0, y0, w, h) touches through a staging block: the texels
 * are written into the block (tile rows of only the touched tiles) and a GPU copy into the
 * texture, with a row gap for the untouched tiles, is queued after the draws recorded so far, so
 * the CPU never waits for the GPU (with anti-aliasing each wait was ~7 ms, 50 per second in the
 * OoT pause menu). Partly covered edge tiles are first read from the texture (the GPU only reads
 * it). false: not possible here (padded update, no memory) - the caller waits instead. */
bool ContextImplPica::stagedUpdate(Tex &t, const u32 *px, u32 x0, u32 y0, u32 w, u32 h)
{
    if (t.vram || t.depth || !h || !w || y0 >= t.th || x0 >= t.tw)
        return false;
    if (x0 == 0 && y0 == 0 && w >= t.w && h >= t.h && (t.w < t.tw || t.h < t.th))
        return false; /* edge padding writes the whole texture */
    CRUMB("gfx: stagedUpdate"); /* waits from here on are this path's (stats.log "waits") */
    if (x0 + w > t.tw)
        return false; /* rows of px are w long: clipping w here would shear them (the caller's
                         path writes the clipped rect with the right stride) */
    if (y0 + h > t.th)
        h = t.th - y0;
    const u32 r_lo = t.th - (y0 + h), r_hi = t.th - 1 - y0; /* storage rows */
    const u32 tr0 = r_lo >> 3, ntr = (r_hi >> 3) - tr0 + 1;
    const u32 tx0 = x0 >> 3, ntx = ((x0 + w + 7) >> 3) - tx0;
    const u32 tile_bytes = 64u * t.bpp;
    const u32 run = ntx * tile_bytes;                  /* touched bytes per tile row */
    const u32 row_bytes = (u32)(t.tw >> 3) * tile_bytes;
    if (run % 16 || (row_bytes - run) % 16)
        return false;
    const u32 bytes = ntr * run;
    bool v = false;
    u8 *stage = static_cast<u8 *>(gpuAlloc(bytes, false, v));
    if (!stage)
        return false;
    u8 *dst = static_cast<u8 *>(t.data) + tr0 * row_bytes + tx0 * tile_bytes;
    const bool partial = (x0 & 7) || ((x0 + w) & 7) || (r_lo & 7) || ((r_hi + 1) & 7);
    if (partial) {
        /* the edge tiles are read from the texture: an earlier staged copy must have landed.
         * (Aligned updates never wait: copies run in queue order. On hardware that copy sits
         * behind a frame of drawing, ~18 ms with anti-aliasing.) */
        pica_wait_fence(t.copy_fence);
        pica_cache_invalidate(dst, (ntr - 1) * row_bytes + run);
        for (u32 r = 0; r < ntr; r++)
            memcpy(stage + r * run, dst + r * row_bytes, run);
    }
    /* the copy below rewrites the texture behind the cache: drop any cached lines there */
    pica_cache_invalidate(dst, (ntr - 1) * row_bytes + run);
    Tex s = t;
    s.tw = (u16)(ntx * 8);
    s.data = stage - tr0 * run; /* storage tile row tr lands at stage + (tr - tr0) * run */
    if (x0 == 0 && y0 == 0 && w == t.tw && h == t.th && !g_slowtex)
        write_full_any(s, px, w, h, w); /* the whole texture (frame buffer copies, AA): tiled writer */
    else
        writeTexels(s, px, x0 - tx0 * 8, y0, w, h); /* flushes the stage */
    pica_cache_flush(stage, bytes);
    pica_copy_gap(dst, stage, bytes, run / 16, (row_bytes - run) / 16);
    t.copy_fence = pica_fence();
    t.used_frame = pica_frame_number();
    t.dirty = true;
    t.gen++;
    t.drawn = t.ever = true;
    m_pending.push_back({stage, false, pica_frame_number(), bytes});
    ctr64_gfx_mem[3] += bytes;
    return true;
}

/* Textures up to this size are orphaned when updated while in flight; larger ones wait. */
const u32 k_orphan_max_bytes = 1024 * 1024;

void ContextImplPica::update2DTexture(const Context::UpdateTextureDataParams &p)
{
    CRUMB("gfx: update2DTexture");
    ctr64_fb_copies++;
    CTR64_PROF(15);
    const u32 id = u32(p.handle);
    Tex *t = tex(id);
    if (!t || !p.data || p.mipMapLevel != 0 || t->depth)
        return;
    if (!t->data && t->lazy && !t->rt && needs_shadow(*t)) {
        /* a texture without data that turns out to be filled by the CPU, not drawn into
         * (GLideN64's RDRAM frame buffer copy: OoT's pause menu, 640x480 with AA): sampled with a
         * 1.25 MB shadow copy per draw on hardware at its exact width (5 per frame, ~6 MB of GPU
         * copies per frame). Power-of-two storage instead: sampled as it is. */
        t->tw = pot(t->w);
    }
    ensureStorage(id);
    if (!t->data)
        return;
    std::vector<u32> &px = m_scratch;
    px.resize((size_t)p.width * p.height);
    Ctr64ProfScope fetch_scope(36);
    fetch_all_fast(static_cast<const u8 *>(p.data), (u32)px.size(), u32(p.format), u32(p.dataType), px.data());
    const bool fits = fits_all(t->fmt, px.data(), (u32)px.size());
    fetch_scope.end();
    const bool full = p.x == 0 && p.y == 0 && p.width >= t->w && p.height >= t->h;
    if (inFlight(*t) || !fits) {
        if (fits && (t->vram || storageBytes(*t) >= k_orphan_max_bytes)) {
            /* a render target, whose GPU-written contents must survive, or a texture too big to
             * double up (the RDRAM frame buffer copy, updated every frame in the OoT pause menu:
             * a second 2 MB block rarely exists in linear memory). A big texture is updated by a
             * GPU copy queued after the draws that read it; otherwise wait for the GPU. */
            {
                CTR64_PROF(37);
                if (!t->vram && stagedUpdate(*t, px.data(), p.x, p.y, p.width, p.height))
                    return;
            }
            Ctr64ProfScope wait_scope(38);
            pica_flush();
            pica_wait_idle();
        } else {
            /* new storage (keeping the old for frames still in flight), widened to RGBA8 if the
             * update does not fit the current format */
            Tex n = *t;
            if (!fits) {
                n.fmt = GPU_RGBA8;
                n.bpp = 4;
            }
            n.data = gpuAlloc(storageBytes(n), t->vram, n.vram);
            if (!n.data)
                return;
            if (!full) {
                if (t->vram || inFlight(*t)) {
                    pica_flush();
                    pica_wait_idle(); /* read back what the GPU may still be writing */
                }
                const u32 count = (u32)t->tw * t->th;
                if (n.fmt == t->fmt)
                    memcpy(n.data, t->data, storageBytes(n));
                else
                    for (u32 i = 0; i < count; i++)
                        store_texel(n, i, load_texel(*t, i));
            }
            releaseStorage(*t);
            *t = n;
            t->used_frame = 0;
            storageChanged(id);
        }
    }
    Ctr64ProfScope write_scope(39);
    if (full && !t->vram && !g_slowtex) {
        /* the whole image (GLideN64's RDRAM frame buffer copies: OoT's pause menu, 320x240 per
         * frame, was 11.9 ms on hardware by writeTexels): the tiled writer of init2DTexture, same
         * bytes (texels past the logical size come from the edge, as pad_edges did) */
        const u32 w = p.width < t->w ? p.width : t->w, h = p.height < t->h ? p.height : t->h;
        switch (t->fmt) {
        case GPU_RGB8: write_full<GPU_RGB8>(*t, px.data(), w, h, p.width); break;
        case GPU_RGBA5551: write_full<GPU_RGBA5551>(*t, px.data(), w, h, p.width); break;
        case GPU_RGBA4: write_full<GPU_RGBA4>(*t, px.data(), w, h, p.width); break;
        case GPU_LA8: write_full<GPU_LA8>(*t, px.data(), w, h, p.width); break;
        case GPU_LA4: write_full<GPU_LA4>(*t, px.data(), w, h, p.width); break;
        default: write_full<GPU_RGBA8>(*t, px.data(), w, h, p.width); break;
        }
        t->drawn = t->ever = true;
        pica_cache_flush(t->data, storageBytes(*t));
        t->dirty = true;
        t->gen++;
        return;
    }
    writeTexels(*t, px.data(), p.x, p.y, p.width, p.height);
}

void ContextImplPica::setTextureParameters(const Context::TexParameters &p)
{
    CRUMB("gfx: setTextureParameters");
    Tex *t = tex(u32(p.handle));
    u32 unit = u32(p.textureUnitIndex);
    if (unit < 2)
        m_unit[unit] = u32(p.handle);
    if (!t)
        return;
    auto filt = [](Parameter v, GPU_TEXTURE_FILTER_PARAM cur) {
        if (!v.isValid())
            return cur;
        u32 f = u32(v);
        return (f == TP_LINEAR || f == TP_LINEAR_MIP_NEAREST) ? GPU_LINEAR : GPU_NEAREST;
    };
    auto wrap = [](Parameter v, GPU_TEXTURE_WRAP_PARAM cur) {
        if (!v.isValid())
            return cur;
        u32 w = u32(v);
        return w == TP_REPEAT ? GPU_REPEAT : w == TP_MIRROR ? GPU_MIRRORED_REPEAT : GPU_CLAMP_TO_EDGE;
    };
    GPU_TEXTURE_FILTER_PARAM mag = (t->param & GPU_TEXTURE_MAG_FILTER(1)) ? GPU_LINEAR : GPU_NEAREST;
    GPU_TEXTURE_FILTER_PARAM min = (t->param & GPU_TEXTURE_MIN_FILTER(1)) ? GPU_LINEAR : GPU_NEAREST;
    GPU_TEXTURE_WRAP_PARAM ws = (GPU_TEXTURE_WRAP_PARAM)((t->param >> 12) & 3);
    GPU_TEXTURE_WRAP_PARAM wt = (GPU_TEXTURE_WRAP_PARAM)((t->param >> 8) & 3);
    t->param = GPU_TEXTURE_MAG_FILTER(filt(p.magFilter, mag)) | GPU_TEXTURE_MIN_FILTER(filt(p.minFilter, min)) |
               GPU_TEXTURE_WRAP_S(wrap(p.wrapS, ws)) | GPU_TEXTURE_WRAP_T(wrap(p.wrapT, wt));
}

void ContextImplPica::bindTexture(const Context::BindTextureParameters &p)
{
    CRUMB("gfx: bindTexture");
    u32 unit = u32(p.textureUnitIndex);
    if (unit < 2)
        m_unit[unit] = u32(p.texture);
}

/* ---- frame buffers -------------------------------------------------------------------------- */

ObjectHandle ContextImplPica::createFramebuffer()
{
    CRUMB("gfx: createFramebuffer");
    u32 id = m_nextFbo++;
    m_fbo[id] = Fbo();
    return ObjectHandle(id);
}

void ContextImplPica::deleteFramebuffer(ObjectHandle name)
{
    CRUMB("gfx: deleteFramebuffer");
    u32 id = u32(name);
    m_fbo.erase(id);
    if (m_drawFbo == id)
        m_drawFbo = 0;
    if (m_readFbo == id)
        m_readFbo = 0;
}

void ContextImplPica::bindFramebuffer(BufferTargetParam target, ObjectHandle name)
{
    CRUMB("gfx: bindFramebuffer");
    u32 id = u32(name);
    if (id != 0 && m_fbo.find(id) == m_fbo.end())
        id = 0;
    u32 t = u32(target);
    ctr64_gfx_stats[ST_FBO]++;
    if (t == BT_FRAMEBUFFER || t == BT_DRAW)
        m_drawFbo = id;
    if (t == BT_FRAMEBUFFER || t == BT_READ)
        m_readFbo = id;
}

void ContextImplPica::rebuildFbo(Fbo &f)
{
    CRUMB("gfx: rebuildFbo");
    Tex *c = tex(f.color);
    f.valid = c && c->data && !c->depth;
    if (!f.valid)
        return;
    memset(&f.rt, 0, sizeof(f.rt));
    f.rt.color = c->data;
    f.rt.width = c->tw;
    f.rt.height = c->th;
    f.rt.color_fmt = c->fmt == GPU_RGBA4 ? GPU_RB_RGBA4 : c->fmt == GPU_RGBA5551 ? GPU_RB_RGBA5551 : GPU_RB_RGBA8;
    f.rt.depth_fmt = GPU_RB_DEPTH24_STENCIL8;
    Tex *d = tex(f.depth);
    /* The PICA has one frame buffer size; a depth buffer is usable if it is at least as large. */
    if (d && d->data && d->depth && (u32)d->tw * d->th >= (u32)c->tw * c->th)
        f.rt.depth = d->data;
}

void ContextImplPica::addFrameBufferRenderTarget(const Context::FrameBufferRenderTarget &p)
{
    CRUMB("gfx: addFrameBufferRenderTarget");
    u32 id = u32(p.bufferHandle);
    Fbo *f = id == 0 ? &m_window : (m_fbo.count(id) ? &m_fbo[id] : nullptr);
    if (!f)
        return;
    u32 tid = u32(p.textureHandle);
    if (Tex *t = tex(tid))
        t->rt = true;
    if (u32(p.attachment) == BA_DEPTH) {
        f->depth = tid;
    } else if (u32(p.attachment) == BA_COLOR0) {
        f->color = tid;
    }
    moveToVram(tid); /* rebuilds the FBOs that use it if the storage moves */
    rebuildFbo(*f);
}

void ContextImplPica::initRenderbuffer(const Context::InitRenderbufferParams &p)
{
    CRUMB("gfx: initRenderbuffer");
    Tex *t = tex(u32(p.handle));
    if (!t)
        return;
    releaseStorage(*t);
    t->w = (u16)p.width;
    t->h = (u16)p.height;
    t->tw = fb_width(p.width);
    t->th = fb_height(t->tw, p.height);
    t->depth = true;
    t->bpp = 4;
    t->rt = true;
    t->lazy = true; /* allocated when first drawn to */
    storageChanged(u32(p.handle));
}

/* ---- programs ------------------------------------------------------------------------------- */

CombinerProgram *ContextImplPica::createCombinerProgram(Combiner &color, Combiner &alpha, const CombinerKey &key)
{
    u32 inputs = 0;
    if (key.getCycleType() != G_CYC_2CYCLE) {
        inputs |= collect_inputs(alpha.stage[0], correct_first);
        inputs |= collect_inputs(color.stage[0], correct_first);
    } else {
        inputs |= collect_inputs(alpha.stage[0], correct_first_2cyc);
        inputs |= collect_inputs(color.stage[0], correct_first_2cyc);
        if (alpha.numStages == 2)
            inputs |= collect_inputs(alpha.stage[1], correct_second);
        if (color.numStages == 2)
            inputs |= collect_inputs(color.stage[1], correct_second);
    }
    return new PicaCombinerProgram(key, inputs);
}

/* ---- drawing -------------------------------------------------------------------------------- */

GPU_BLENDFACTOR blend_factor(u32 f)
{
    switch (f) {
    case BL_ZERO: return GPU_ZERO;
    case BL_ONE: return GPU_ONE;
    case BL_SRC_ALPHA: return GPU_SRC_ALPHA;
    case BL_DST_ALPHA: return GPU_DST_ALPHA;
    case BL_ONE_MINUS_SRC_ALPHA: return GPU_ONE_MINUS_SRC_ALPHA;
    case BL_CONSTANT_ALPHA: return GPU_CONSTANT_ALPHA;
    case BL_ONE_MINUS_CONSTANT_ALPHA: return GPU_ONE_MINUS_CONSTANT_ALPHA;
    default: return GPU_ONE; /* dual-source factors are never requested (feature reported off) */
    }
}

/* A target with short storage (materialize) is drawn to or read beyond it: all of it now. */
void ContextImplPica::growStorage(u32 id)
{
    Tex *t = tex(id);
    if (!t || !t->th_full)
        return;
    const u16 th = t->th;
    const u32 bytes = storageBytes(*t);
    t->th = t->th_full;
    t->th_full = 0;
    if (!t->data) {
        t->lazy = true;
        storageChanged(id);
        return;
    }
    const u32 all = storageBytes(*t);
    bool vram = false;
    void *n = gpuAlloc(all, true, vram);
    if (!n) { /* no memory at all: keeps what it has */
        t->th_full = t->th;
        t->th = th;
        return;
    }
    if (!vram)
        pica_cache_flush(n, all); /* no dirty CPU lines left to overwrite what the GPU writes */
    PicaTarget rt;
    memset(&rt, 0, sizeof(rt));
    rt.color = n;
    rt.width = t->tw;
    rt.height = t->th;
    rt.color_fmt = t->fmt == GPU_RGBA4 ? GPU_RB_RGBA4 : t->fmt == GPU_RGBA5551 ? GPU_RB_RGBA5551 : GPU_RB_RGBA8;
    ensureFrame();
    pica_clear(&rt, t->cleared, false, 0);
    /* rows are stored bottom up: what there is goes to the end */
    u8 *to = static_cast<u8 *>(n) + (all - bytes);
    if (t->vram)
        pica_copy_from_vram(to, t->data, bytes);
    else
        pica_copy(to, t->data, bytes);
    t->used_frame = pica_frame_number();
    const u16 full = t->th;
    t->th = th;
    releaseStorage(*t);
    t->th = full;
    t->data = n;
    t->vram = vram;
    t->used_frame = pica_frame_number();
    t->dirty = true;
    t->gen++;
    storageChanged(id);
}

/* A frame buffer's 32-bit storage becomes 16-bit (g_fb16), its picture with it. In FCRAM at first:
 * the old storage has the VRAM until the GPU is done with it (keepInVram moves it in then). */
void ContextImplPica::convert16(u32 id)
{
    Tex *t = tex(id);
    if (!t || !t->n16 || !t->rt || t->fmt != GPU_RGBA8 || t->th_full)
        return;
    if (t->data) {
        Tex n = *t;
        n.fmt = GPU_RGBA5551;
        n.bpp = 2;
        n.vram = false;
        n.data = gpuAlloc(storageBytes(n), false, n.vram);
        if (!n.data)
            return;
        pica_cache_flush(n.data, storageBytes(n)); /* no dirty CPU lines of its last owner */
        ensureFrame();
        if (!shadowDraw(n.data, t->tw, *t, TileRect{0, (u16)(t->tw / 8), 0, (u16)(t->th / 8)}, t->th)) {
            freeNow(n.data, false, storageBytes(n));
            return;
        }
        t->used_frame = pica_frame_number();
        releaseStorage(*t);
        t->data = n.data;
        t->vram = false;
        t->used_frame = pica_frame_number();
        t->dirty = true;
        t->gen++;
    }
    t->fmt = GPU_RGBA5551;
    t->bpp = 2;
    storageChanged(id);
}

void ContextImplPica::materialize(Fbo &f, int use)
{
    if (g_fb16 && use == USE_DRAW)
        convert16(f.color);
    /* Short storage: a target like those made and deleted within a frame after a few draws gets
     * the rows those used (with some to spare) and the rest only once something reaches there.
     * Goemon's diary screen makes 12 targets per frame for a few rows each; whole ones (512x480)
     * moved the game's own targets out of the VRAM and in again, 14 MB per frame, and cost their
     * clears (HW 2026-11-22: 26 VI/s). */
    if (Tex *c = tex(f.color)) {
        if (use == USE_DRAW)
            c->rows_used = std::max(c->rows_used, m_drawTop);
        if (!c->data && c->lazy && c->rt && !c->th_full && !needs_shadow(*c)) {
            if (const Brief *b = brief(*c)) {
                const u16 th = (u16)std::max<u32>(pot(b->rows + 8u), 64);
                if (th < c->th) {
                    c->th_full = c->th;
                    c->th = th;
                }
            }
        }
        if (c->th_full && (use == USE_DEPTH_ONLY || (use == USE_DRAW && m_drawTop > c->th)))
            growStorage(f.color);
    }
    ensureStorage(f.color);
    /* after the switch to power-of-two storage (g_shadow_heavy) a new color buffer can be wider
     * than the depth buffer made before it, which rebuildFbo would then leave off: the depth
     * buffer gets the color buffer's width (its contents go once; games clear it per frame) */
    if (g_shadow_heavy) {
        Tex *c = tex(f.color), *d = tex(f.depth);
        if (c && d && d->depth && (c->tw > d->tw || c->th > d->th) && c->w <= d->w && c->h <= d->h) {
            releaseStorage(*d);
            d->tw = std::max(c->tw, d->tw);
            d->th = std::max(c->th, d->th);
            d->lazy = true;
            storageChanged(f.depth);
        }
    }
    ensureStorage(f.depth);
    keepInVram(f, use);
}

bool ContextImplPica::applyState(bool depth_allowed)
{
    CRUMB("gfx: applyState");
    Fbo &f = drawFbo();
    materialize(f);
    if (!f.valid)
        return false;
    if (m_enCull && m_cullMode == CM_FRONT_AND_BACK)
        return false;

    ensureFrame();
    pica_bind_target(&f.rt);
    pica_bind_program(&m_shader);
    pica_vertex_layout(k_layout, 4, sizeof(GVtx));
    if (turned())
        pica_viewport((u16)m_vp[1], (u16)(WINDOW_W - m_vp[0] - m_vp[2]), (u16)m_vp[3], (u16)m_vp[2]);
    else
        pica_viewport((u16)m_vp[0], (u16)m_vp[1], (u16)m_vp[2], (u16)m_vp[3]);

    /* GL clips drawing to the frame buffer's logical size; with scissor on, to the scissor box */
    Tex *c = tex(f.color);
    s32 x0 = 0, y0 = 0, x1 = c->w, y1 = c->h;
    if (m_enScissor) {
        x0 = m_sc[0] > 0 ? m_sc[0] : 0;
        y0 = m_sc[1] > 0 ? m_sc[1] : 0;
        x1 = m_sc[0] + m_sc[2] < c->w ? m_sc[0] + m_sc[2] : c->w;
        y1 = m_sc[1] + m_sc[3] < c->h ? m_sc[1] + m_sc[3] : c->h;
        if (x1 <= x0 || y1 <= y0)
            return false;
    }
    if (turned())
        pica_scissor(GPU_SCISSOR_NORMAL, (u16)y0, (u16)(WINDOW_W - x1), (u16)y1, (u16)(WINDOW_W - x0));
    else
        pica_scissor(GPU_SCISSOR_NORMAL, (u16)x0, (u16)y0, (u16)x1, (u16)y1);

    const bool depth = depth_allowed && m_enDepth && f.rt.depth;
    const GPU_TESTFUNC func = m_depthFunc == CMP_LEQUAL ? GPU_LEQUAL : m_depthFunc == CMP_LESS ? GPU_LESS : GPU_ALWAYS;
    pica_depth_test(depth, depth ? func : GPU_ALWAYS, GPU_WRITE_COLOR | ((depth && m_depthWrite) ? GPU_WRITE_DEPTH : 0));
    if (depth && m_depthWrite)
        if (Tex *d = tex(f.depth))
            d->zclean = 0;
    pica_depth_map(k_depth_scale, (depth && m_enPolyOffset) ? k_poly_offset : 0.0f);

    /* Measured against GLideN64's GL output: for the same window coordinates the PICA's cull
     * modes select the opposite winding to GL's (GL culls back = clockwise faces here). */
    pica_cull(!m_enCull ? GPU_CULL_NONE : m_cullMode == CM_FRONT ? GPU_CULL_BACK_CCW : GPU_CULL_FRONT_CCW);

    if (m_enBlend) {
        pica_blend(GPU_BLEND_ADD, GPU_BLEND_ADD, blend_factor(m_blend[0]), blend_factor(m_blend[1]),
                   blend_factor(m_blend[2]), blend_factor(m_blend[3]));
        pica_blend_color(m_blendColor);
    } else {
        pica_blend_off();
    }

    if (c) {
        c->dirty = true; /* rendered to: sampling it later must refetch */
        trimShadows(f.color, *c);
        c->gen++;
        c->used_frame = pica_frame_number();
    }
    if (Tex *d = tex(f.depth))
        d->used_frame = pica_frame_number();
    return true;
}

void ContextImplPica::bindUnit(int unit, bool n64_filter)
{
    CRUMB("gfx: bindUnit");
    m_shadowBound[unit] = nullptr;
    ensureStorage(m_unit[unit]); /* sampling a target never drawn to: zeroed storage */
    Tex *t = tex(m_unit[unit]);
    if (!t || !t->data || t->depth) {
        pica_texture(unit, NULL, 0, 0, GPU_RGBA8, 0);
        return;
    }
    u32 param = t->param;
    if (n64_filter) {
        /* GLideN64 keeps N64 textures at GL_NEAREST and filters in the fragment shader when
         * the RDP's texture filter is not point sampling (uTextureFilterMode): use the PICA's
         * bilinear filter for that. */
        const bool bilerp = gDP.otherMode.cycleType != G_CYC_COPY && gDP.otherMode.textureFilter != G_TF_POINT;
        const GPU_TEXTURE_FILTER_PARAM f = bilerp ? GPU_LINEAR : GPU_NEAREST;
        param = (param & ~(GPU_TEXTURE_MAG_FILTER(1) | GPU_TEXTURE_MIN_FILTER(1))) | GPU_TEXTURE_MAG_FILTER(f) |
                GPU_TEXTURE_MIN_FILTER(f);
        /* N64 clamp applies at the tile's bounds, the mask wraps inside them (GLideN64's
         * clampWrapMirror). GLideN64 asks for CLAMP_TO_EDGE whenever clamp is on; with a tile
         * larger than the mask period the texture repeats instead (SSB's wallpaper, table). */
        const CachedTexture *ct = textureCache().current[unit];
        const gDPTile *tile = gSP.textureTile[unit];
        if (ct && tile && ct->frameBufferTexture == CachedTexture::fbNone && tile->textureMode == TEXTUREMODE_NORMAL &&
            gDP.otherMode.cycleType != G_CYC_COPY) {
            if (ct->clampS && ct->maskS) {
                u32 tileWidth = ((tile->lrs - tile->uls) & 0x03FF) + 1;
                if (tile->size > ct->size)
                    tileWidth <<= tile->size - ct->size;
                if (tileWidth > (1u << tile->originalMaskS))
                    param = (param & ~GPU_TEXTURE_WRAP_S(3)) |
                            GPU_TEXTURE_WRAP_S(ct->mirrorS ? GPU_MIRRORED_REPEAT : GPU_REPEAT);
            }
            if (ct->clampT && ct->maskT) {
                const u32 tileHeight = ((tile->lrt - tile->ult) & 0x03FF) + 1;
                if (tileHeight > (1u << tile->originalMaskT))
                    param = (param & ~GPU_TEXTURE_WRAP_T(3)) |
                            GPU_TEXTURE_WRAP_T(ct->mirrorT ? GPU_MIRRORED_REPEAT : GPU_REPEAT);
            }
        }
    }
    if (n64_filter && t->rt)
        ctr64_frame_fbtex = true; /* a frame buffer read as a texture (frameskip.c) */
    bool copied = false;
    if (needs_shadow(*t)) {
        /* the tiles a texture rectangle samples (2 texels of margin for filtering); anything else
         * takes all of them. Past an edge: clamped to the edge, or (repeat) anywhere. */
        TileRect need = {0, (u16)(t->tw / 8), 0, (u16)(t->th / 8)};
        if (m_rowHint[unit] && !g_fullshadow) {
            float w = t->w, h = t->h;
            if (m_kind != Kind::Copy && m_kind != Kind::Text) {
                const CachedTexture *ct = textureCache().current[unit];
                if (!ct || ct->frameBufferTexture == CachedTexture::fbNone || t->w != ct->width)
                    h = -1.0f; /* coordinates in the cache's scale (drawRects): not mapped here */
                else
                    w = ct->width, h = ct->height;
            }
            auto range = [](float lo, float hi, float size, bool clamp, u32 tiles, bool flip, u16 &a, u16 &b) {
                if (!clamp && (lo < 0.0f || hi > 1.0f))
                    return;
                lo = lo < 0.0f ? 0.0f : lo > 4.0f ? 4.0f : lo;
                hi = hi < 0.0f ? 0.0f : hi > 4.0f ? 4.0f : hi;
                int p0 = (int)(lo * size) - 2, p1 = (int)(hi * size + 1.0f) + 2;
                if (flip) { /* t runs up from the last storage row (stored bottom row first) */
                    const int n = (int)tiles * 8, q0 = n - p1;
                    p1 = n - p0;
                    p0 = q0;
                }
                u32 t0 = p0 < 0 ? 0 : (u32)p0 / 8, t1 = p1 < 1 ? 1 : (u32)(p1 + 7) / 8;
                if (t1 > tiles)
                    t1 = tiles;
                if (t0 >= t1)
                    t0 = t1 - 1; /* all past the far edge: the last tiles */
                a = (u16)t0;
                b = (u16)t1;
            };
            if (h > 0.0f) {
                const bool clamp_s = (param & GPU_TEXTURE_WRAP_S(3)) == GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_EDGE);
                const bool clamp_t = (param & GPU_TEXTURE_WRAP_T(3)) == GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_EDGE);
                range(m_rowS[unit][0], m_rowS[unit][1], w, clamp_s, t->tw / 8, false, need.c0, need.c1);
                range(m_rowT[unit][0], m_rowT[unit][1], h, clamp_t, t->th / 8, true, need.r0, need.r1);
            }
        }
        void *sh = shadowOf(m_unit[unit], *t, copied, need, unit ? m_shadowBound[0] : nullptr);
        m_shadowBound[unit] = sh;
        if (!sh) {
            pica_texture(unit, NULL, 0, 0, GPU_RGBA8, 0);
            return;
        }
        pica_texture(unit, sh, pot(t->tw), pot(t->th), t->fmt, param);
    } else {
        pica_texture(unit, t->data, t->tw, t->th, t->fmt, param);
    }
    if (t->dirty || copied) {
        pica_texture_flush_cache();
        t->dirty = false;
    }
    t->used_frame = pica_frame_number();
}

/* The power-of-two shadow of a texture stored at a non-power-of-two width, up to date: a GPU copy
 * of each 8-row strip of tiles into the wider strip of the shadow (tiles keep their place, the
 * columns past the storage width stay zero, as in power-of-two storage). One shadow per storage
 * size, shared: the GPU runs copies and draws in queue order, so a copy for another texture
 * comes after the draws that sampled the previous one. */
/* Only the tile rows a texture rectangle samples are copied (tr0..tr1, see bindUnit), the rest
 * when a later draw needs them: Goemon's diary screen samples strips of the frame it is drawing,
 * 7 full copies (1.25 MB each) per frame at 23 VI/s on HW. Dev switch fullshadow.txt: always all. */
static const u16 k_shadow_parts = 8;

/* Shadows are kept per texture, up to k_shadows_per_size of one size (least recently used goes).
 * One shared shadow per size took turns between two textures sampled by the same draw (Mario
 * Tennis's ball trail: unit 0 the frame before, unit 1 the frame drawn): both units then read
 * the second copy, and the unchanged one was copied again 32 times per frame (1.5 MB each). The
 * shadow the other unit of this draw uses (avoid) is never taken. */
static const unsigned k_shadows_per_size = 3;

void *ContextImplPica::shadowOf(u32 id, Tex &t, bool &copied, TileRect need, const void *avoid)
{
    const u16 sw = pot(t.tw), sh = pot(t.th);
    Shadow *s = nullptr, *lru = nullptr;
    unsigned nsize = 0;
    for (Shadow &x : m_shadows) {
        if (x.tw != sw || x.th != sh || x.bpp != t.bpp)
            continue;
        nsize++;
        if (x.owner == id && x.p != avoid) {
            s = &x;
            break;
        }
        if (x.p != avoid && (!lru || (s32)(x.last_use - lru->last_use) < 0))
            lru = &x;
    }
    if (!s && lru && nsize >= k_shadows_per_size)
        s = lru;
    if (!s) {
        const u32 bytes = (u32)sw * sh * t.bpp;
        void *p = linear_alloc_safe(bytes, 0x80);
        if (!p)
            p = ctr64_xlinear_alloc(bytes, 0x80);
        if (!p) {
            if (!lru)
                return nullptr;
            s = lru;
        } else {
            memset(p, 0, bytes);
            pica_cache_flush(p, bytes);
            ctr64_gfx_mem[0] += bytes;
            m_shadows.push_back(Shadow{p, sw, sh, t.bpp, 0, 0, nullptr, 0, {}, 0});
            s = &m_shadows.back();
        }
    }
    s->last_use = ++m_shadowUse;
    const u16 cols = t.tw / 8, rows = t.th / 8;
    if (need.c1 > cols)
        need.c1 = cols;
    if (need.r1 > rows)
        need.r1 = rows;
    if (need.c1 <= need.c0 || need.r1 <= need.r0)
        need = {0, cols, 0, rows};
    const bool same = s->owner == id && s->gen == t.gen && s->src == t.data;
    /* Each copy is a GPU command between draws. A version sampled many times (Mario Tennis reads
     * a 4x32 grid of points from one frame: 128 samples) is copied whole, as soon as the one
     * before had more than k_shadow_parts samples; one with few (Goemon: 3-4 strips) in parts. */
    if (t.sh_gen != t.gen) {
        /* decaying: Mario Tennis samples each frame 128 times, then once more for the window */
        t.sh_last = t.sh_samples > t.sh_last / 2 ? t.sh_samples : t.sh_last / 2;
        t.sh_samples = 0;
        t.sh_gen = t.gen;
    }
    t.sh_samples += t.sh_samples < 0xFFFF;
    if (same)
        for (u32 i = 0; i < s->nv; i++)
            if (need.c0 >= s->v[i].c0 && need.c1 <= s->v[i].c1 && need.r0 >= s->v[i].r0 && need.r1 <= s->v[i].r1)
                return s->p;
    if (t.sh_samples > k_shadow_parts || t.sh_last > k_shadow_parts)
        need = {0, cols, 0, rows};
    if (same && s->nv) {
        /* current but for some rows (a draw since, noteDrawnRows): only the rows of the need
         * no kept rect covers across its columns (heavy sampling: all such rows, full width;
         * Mario Tennis draws its trail band 8 rows at a time and samples each version) */
        int lo = need.r1, hi = (int)need.r0 - 1;
        for (int r = need.r0; r < need.r1; r++) {
            bool cov = false;
            for (u32 i = 0; i < s->nv && !cov; i++)
                cov = s->v[i].r0 <= r && r < s->v[i].r1 && s->v[i].c0 <= need.c0 && need.c1 <= s->v[i].c1;
            if (!cov) {
                lo = r < lo ? r : lo;
                hi = r > hi ? r : hi;
            }
        }
        if (hi >= lo) {
            need.r0 = (u16)lo;
            need.r1 = (u16)(hi + 1);
        }
    }
    /* one strided copy: the needed tiles of each tile row (a tile row is its tiles in order) */
    const u32 run = (u32)t.tw * 8 * t.bpp, out = (u32)sw * 8 * t.bpp, tile = 64u * t.bpp;
    const u32 chunk = (u32)(need.c1 - need.c0) * tile, nrows = need.r1 - need.r0;
    if (shadowDraw(s->p, sw, t, need)) {
        ctr64_gfx_shadow[4]++;
    } else {
        pica_copy_rect(static_cast<u8 *>(s->p) + (need.r0 + (sh - t.th) / 8) * out + need.c0 * tile,
                       static_cast<const u8 *>(t.data) + need.r0 * run + need.c0 * tile, chunk * nrows, chunk / 16,
                       (run - chunk) / 16, (out - chunk) / 16);
        ctr64_gfx_shadow[3]++;
    }
    ctr64_gfx_shadow[1] += (chunk * nrows) >> 10;
    if (!same) {
        s->nv = 0;
        t.nshadow++;
        note_shadow_copy();
        ctr64_gfx_shadow[0]++;
    }
    if (need.c0 == 0 && need.c1 == cols && need.r0 == 0 && need.r1 == rows)
        s->nv = 0;
    if (s->nv == sizeof(s->v) / sizeof(s->v[0])) /* full: the oldest goes */
        memmove(&s->v[0], &s->v[1], sizeof(s->v) - sizeof(s->v[0])), s->nv--;
    s->v[s->nv++] = need;
    s->owner = id;
    s->gen = t.gen;
    s->src = t.data;
    copied = true;
    return s->p;
}

/* A shadow copy drawn instead of copied: a transfer-engine copy ends the command list there and
 * goes to the GPU as a job of its own (pica_copy_rect: submit_commands), ~2300 per second in
 * Mario Tennis's trail scene and ~670 in Goemon's diary screen, which stayed at 16 and 22 VI/s on
 * hardware however few bytes each copied (2026-11-18). A draw stays in the command list.
 * The PICA samples only power-of-two widths, but the storage of tile rows is regular: with k the
 * largest power of two dividing the tiles per row (768 = 96 tiles: k = 32), the storage read as
 * a texture 8k texels wide has parts = tiles / k texture tile rows per storage tile row, in
 * order. Each needed (storage tile row, part) is one quad of 8 rows into the shadow (a render
 * target pot(tw) wide), texel for pixel with nearest sampling: the same bytes as the copy. At
 * most 1024 texture rows per draw (128 / parts storage tile rows). The gfx program needs no
 * uniforms (positions in clip space), so only the state layer's registers change, and
 * pica_state_save/restore put them back for the draw being set up. RGBA8 only. Dev switches:
 * noshdraw.txt off; shcheck.txt compares each with a transfer copy (shcheck.log); shflip.txt. */
static const u32 k_shdraw_quads = 4096;
static const u32 k_shdraw_max_bytes = 192 * 1024; /* larger copies: by transfer */

char g_shdraw_report[48] = "";

/* Before the first shadow draw: one of a 768x64 texture of known texels (written by the CPU, so
 * Azahar reads it from memory too), whole and a part of it, read back and compared word for word;
 * rows the other way round (shflip) if not. Shadow draws stay off unless one passes. stats.log
 * "frame buffer storage". Returns 2 passed, 3 failed. */
int ContextImplPica::shadowDrawTest()
{
    const u16 tw = 768, th = 64, sw = 1024;
    const u32 sbytes = (u32)tw * th * 4, dbytes = (u32)sw * th * 4;
    /* where the game's textures go too: the linear heap, else the New 3DS reserve (Banjo-Tooie
     * with anti-aliasing: the linear heap had no 450 KB left) */
    u32 *src = static_cast<u32 *>(linear_alloc_safe(sbytes, 0x80));
    if (!src)
        src = static_cast<u32 *>(ctr64_xlinear_alloc(sbytes, 0x80));
    u32 *dst = static_cast<u32 *>(linear_alloc_safe(dbytes, 0x80));
    if (!dst)
        dst = static_cast<u32 *>(ctr64_xlinear_alloc(dbytes, 0x80));
    int result = 3;
    u32 bad = 0;
    if (src && dst) {
        for (u32 i = 0; i < sbytes / 4; i++)
            src[i] = (i * 2654435761u) ^ 0x5A5A5A5Au;
        pica_cache_flush(src, sbytes);
        Tex tt;
        tt.w = tt.tw = tw;
        tt.h = tt.th = th;
        tt.fmt = GPU_RGBA8;
        tt.bpp = 4;
        tt.data = src;
        const bool keep_flip = g_shflip;
        for (int flip = 0; flip < 2 && result != 2; flip++) {
            g_shflip = flip ? !keep_flip : keep_flip;
            bad = 0;
            for (int part = 0; part < 2; part++) {
                const TileRect need = part ? TileRect{10, 40, 3, 6} : TileRect{0, 96, 0, 8};
                memset(dst, 0, dbytes);
                pica_cache_flush(dst, dbytes);
                if (!shadowDraw(dst, sw, tt, need)) {
                    bad = ~0u;
                    break;
                }
                pica_flush(); /* the draw is still in the command list */
                pica_wait_idle();
                pica_cache_invalidate(dst, dbytes);
                for (u32 r = 0; r < th / 8u; r++)
                    for (u32 c = 0; c < sw / 8u; c++) {
                        const bool in = c >= need.c0 && c < need.c1 && r >= need.r0 && r < need.r1;
                        for (u32 m = 0; m < 64; m++) {
                            const u32 want = in ? src[(r * (tw / 8u) + c) * 64 + m] : 0;
                            const u32 got = dst[(r * (sw / 8u) + c) * 64 + m];
                            if (got != want && g_shcheck) { /* shtest.log: where the texel came from */
                                static FILE *tl;
                                static int nl;
                                if (!tl)
                                    tl = fopen("sdmc:/3ds/ctr64/shtest.log", "w");
                                if (tl && nl < 60) {
                                    long at = -1;
                                    for (u32 i = 0; i < sbytes / 4 && at < 0; i++)
                                        if (src[i] == got)
                                            at = (long)i;
                                    fprintf(tl, "flip %d part %d r %lu c %lu m %lu: got %08lx want %08lx; got = src word %ld (tile %ld m %ld)\n",
                                            flip, part, (unsigned long)r, (unsigned long)c, (unsigned long)m,
                                            (unsigned long)got, (unsigned long)want, at, at < 0 ? -1 : at / 64, at < 0 ? -1 : at % 64);
                                    fflush(tl);
                                    nl++;
                                }
                            }
                            bad += got != want;
                        }
                    }
            }
            if (!bad)
                result = 2;
        }
        if (result != 2)
            g_shflip = keep_flip;
        if (result == 2)
            snprintf(g_shdraw_report, sizeof(g_shdraw_report), " + shadow draws%s", g_shflip ? " (flipped)" : "");
        else
            snprintf(g_shdraw_report, sizeof(g_shdraw_report), " (shadow draw test: %lu bad)", (unsigned long)bad);
    } else {
        snprintf(g_shdraw_report, sizeof(g_shdraw_report), " (shadow draw test: no memory)");
        result = 4; /* tried again later (shadowDraw) */
    }
    if (src && !ctr64_xlinear_free(src))
        linearFree(src);
    if (dst && !ctr64_xlinear_free(dst))
        linearFree(dst);
    return result;
}

/* th16: dst is storage of sw x th16 in 16 bits that takes the whole texture (convert16) */
bool ContextImplPica::shadowDraw(void *dst, u16 sw, const Tex &t, TileRect need, u16 th16)
{
    const u32 bpp = t.bpp;
    if (g_noshdraw || (t.fmt != GPU_RGBA8 && t.fmt != GPU_RGBA5551) || !m_shader.vertexShader || sw > 1024 || t.th > 1024)
        return false;
    /* the test needs ~450 KB of linear memory at once: Banjo-Tooie had none free at its first
     * copy, and the draws stayed off for the session (82 transfer jobs a second). Without the
     * memory it is tried again every 2 s, up to 30 times. */
    if (m_shdrawState == 0 || (m_shdrawState == 4 && pica_frame_number() >= m_shdrawRetry)) {
        m_shdrawState = 1;
        m_shdrawState = shadowDrawTest();
        if (m_shdrawState == 4) {
            m_shdrawRetry = pica_frame_number() + 120;
            if (++m_shdrawTries >= 30)
                m_shdrawState = 3;
        }
    }
    if (m_shdrawState == 3 || m_shdrawState == 4)
        return false;
    /* big copies stay transfers: drawing 1.5 MB into the shadow (FCRAM) took longer on hardware
     * than the transfer job (Mario Tennis whole-frame copies, 2026-11-19: 50 -> 40 VI/s) */
    if (!th16 && m_shdrawState == 2 && (u32)(need.c1 - need.c0) * (need.r1 - need.r0) * 64u * bpp > k_shdraw_max_bytes)
        return false;
    const u32 tiles = t.tw / 8;
    u32 k = tiles & (0u - tiles);
    if (k > 128)
        k = 128;
    if (!tiles)
        return false;
    const u32 parts = tiles / k, W = 8 * k;
    if (parts > 128)
        return false;
    const u32 rpc = 128 / parts; /* storage tile rows per draw */
    const u32 pa = need.c0 / k, pb = (need.c1 - 1) / k; /* parts touched */
    const u32 rows = need.r1 - need.r0, per = pb - pa + 1, nq = rows * per, draws = (rows + rpc - 1) / rpc;
    if (!rows || nq > k_shdraw_quads)
        return false;
    if (!m_quadIdx) {
        m_quadIdx = static_cast<u16 *>(linearMemAlign(k_shdraw_quads * 6 * 2, 0x80));
        if (!m_quadIdx)
            return false;
        for (u32 q = 0; q < k_shdraw_quads; q++) {
            u16 *i = m_quadIdx + q * 6;
            const u16 b = (u16)(q * 4);
            i[0] = b, i[1] = (u16)(b + 1), i[2] = (u16)(b + 2), i[3] = (u16)(b + 1), i[4] = (u16)(b + 3), i[5] = (u16)(b + 2);
        }
        pica_cache_flush(m_quadIdx, k_shdraw_quads * 6 * 2);
    }
    const u32 vbytes = nq * 4 * sizeof(GVtx);
    /* this draw's commands and the 4 KB the draw being set up reserved (pica_reserve) */
    if (!pica_room(4096 + 2048 + draws * 512, vbytes + 64))
        return false;
    GVtx *v = static_cast<GVtx *>(pica_frame_alloc(vbytes, 16));
    if (!v)
        return false;

    const u16 sh = th16 ? th16 : pot(t.th); /* the shadow's rows: storage row r of the source is its row r + sh - th */
    const float sx = 2.0f / sw, sy = 2.0f / sh;
    u32 q = 0;
    for (u32 r = need.r0; r < need.r1; r++) {
        const u32 base = need.r0 + (r - need.r0) / rpc * rpc;          /* first row of its draw */
        const u32 n_in = need.r1 - base < rpc ? need.r1 - base : rpc;  /* rows of its draw */
        const u32 htex = pot(n_in * parts * 8);
        const float yt = (float)(t.th - 8 * r) * sy - 1.0f, yb = (float)(t.th - 8 * r - 8) * sy - 1.0f;
        for (u32 p = pa; p <= pb; p++, q++) {
            const u32 ca = need.c0 > p * k ? need.c0 : p * k, cb = need.c1 < (p + 1) * k ? need.c1 : (p + 1) * k;
            const float x0 = (float)(8 * ca) * sx - 1.0f, x1 = (float)(8 * cb) * sx - 1.0f;
            const float s0 = (float)(8 * (ca - p * k)) / W, s1 = (float)(8 * (cb - p * k)) / W;
            const u32 j0 = ((r - base) * parts + p) * 8;
            float tt = (float)j0 / htex, tb = (float)(j0 + 8) / htex;
            if (g_shflip)
                tt = 1.0f - tt, tb = 1.0f - tb;
            GVtx *o = v + q * 4;
            const float xs[4] = {x0, x1, x0, x1}, ys[4] = {yt, yt, yb, yb};
            const float ss[4] = {s0, s1, s0, s1}, ts[4] = {tt, tt, tb, tb};
            for (int i = 0; i < 4; i++) {
                o[i].x = xs[i];
                o[i].y = ys[i];
                o[i].z = -0.5f;
                o[i].w = 1.0f;
                o[i].c[0] = o[i].c[1] = o[i].c[2] = o[i].c[3] = 0xFF;
                o[i].s0 = ss[i];
                o[i].t0 = ts[i];
                o[i].s1 = o[i].t1 = 0.0f;
            }
        }
    }

    if (g_shazflush) {
        /* Azahar only: its surface cache does not see the frame buffer it keeps on the host
         * through a texture of another width and reads stale memory; a transfer of the rows
         * writes them back first (the hardware reads memory) */
        static u8 *fl;
        const u32 run0 = (u32)t.tw * 8 * bpp, fb = run0 * (t.th / 8);
        static u32 fl_bytes;
        if (fl_bytes < fb) {
            if (fl)
                linearFree(fl);
            fl = static_cast<u8 *>(linearMemAlign(fb, 0x80));
            fl_bytes = fl ? fb : 0;
        }
        (void)fl;
        /* Azahar writes a host surface back when the CPU touches its pages */
        pica_flush();
        pica_wait_idle();
        const u8 *p0 = static_cast<const u8 *>(t.data) + need.r0 * run0;
        pica_cache_invalidate(p0, run0 * rows);
        volatile u32 sink = 0;
        for (u32 o = 0; o < run0 * rows; o += 4096)
            sink += *reinterpret_cast<const volatile u32 *>(p0 + o);
        (void)sink;
    }
    pica_state_save();
    PicaTarget rt{};
    rt.color = dst;
    rt.depth = nullptr;
    rt.width = sw;
    rt.height = sh;
    rt.color_fmt = th16 || t.fmt == GPU_RGBA5551 ? GPU_RB_RGBA5551 : GPU_RB_RGBA8;
    pica_bind_target(&rt); /* flushes what the draws so far left in the framebuffer cache */
    pica_viewport(0, 0, sw, sh);
    pica_scissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    pica_depth_test(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    pica_depth_map(-1.0f, 0.0f);
    pica_cull(GPU_CULL_NONE);
    pica_alpha_test(false, GPU_ALWAYS, 0);
    pica_blend_off();
    pica_bind_program(&m_shader);
    pica_vertex_layout(k_layout, 4, sizeof(GVtx));
    PicaTev tev;
    pica_tev_default(&tev);
    tev.src_rgb = tev.src_alpha = GPU_TEVSOURCES(GPU_TEXTURE0, 0, 0);
    pica_tev(0, &tev);
    pica_tev_default(&tev);
    for (int i = 1; i < 6; i++)
        pica_tev(i, &tev);
    pica_texture(1, NULL, 0, 0, GPU_RGBA8, 0);
    pica_texture(2, NULL, 0, 0, GPU_RGBA8, 0);
    const u32 run = (u32)t.tw * 8 * bpp;
    for (u32 d = 0; d < draws; d++) {
        const u32 r0 = need.r0 + d * rpc, n_in = need.r1 - r0 < rpc ? need.r1 - r0 : rpc;
        const u32 htex = pot(n_in * parts * 8);
        pica_texture(0, static_cast<const u8 *>(t.data) + r0 * run, (u16)W, (u16)htex, t.fmt,
                     GPU_TEXTURE_MAG_FILTER(GPU_NEAREST) | GPU_TEXTURE_MIN_FILTER(GPU_NEAREST) |
                         GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_EDGE) | GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_EDGE));
        pica_texture_flush_cache(); /* the source was drawn into */
        pica_projective(false);
        pica_vertex_buffer(v + d * rpc * per * 4);
        pica_draw_elements(GPU_TRIANGLES, m_quadIdx, n_in * per * 6);
    }
    pica_state_restore();

    if (g_shcheck && !th16) { /* Azahar: the same rectangle by the transfer engine, compared */
        static u8 *chk;
        static u32 chk_bytes;
        const u32 out = (u32)sw * 8 * bpp, bytes = out * (t.th / 8), dr = (sh - t.th) / 8u * out;
        if (chk_bytes < 2 * bytes) {
            if (chk)
                linearFree(chk);
            chk = static_cast<u8 *>(linearMemAlign(2 * bytes, 0x80));
            chk_bytes = chk ? 2 * bytes : 0;
        }
        if (chk) {
            const u32 tile = 64u * bpp, chunk = (u32)(need.c1 - need.c0) * tile;
            pica_copy_rect(chk + need.r0 * out + need.c0 * tile,
                           static_cast<const u8 *>(t.data) + need.r0 * run + need.c0 * tile, chunk * rows, chunk / 16,
                           (run - chunk) / 16, (out - chunk) / 16);
            /* the draw's result read back the same way (Azahar keeps drawn surfaces on the host
             * until a transfer reads them) */
            u8 *const got = chk + bytes;
            pica_copy_rect(got + need.r0 * out + need.c0 * tile, static_cast<const u8 *>(dst) + dr + need.r0 * out + need.c0 * tile,
                           chunk * rows, chunk / 16, (out - chunk) / 16, (out - chunk) / 16);
            pica_wait_idle();
            pica_cache_invalidate(chk, bytes);
            pica_cache_invalidate(got, bytes);
            u32 bad = 0, fr = 0, fi = 0;
            for (u32 r = need.r0; r < need.r1; r++) {
                const u32 *a = reinterpret_cast<const u32 *>(chk + r * out + need.c0 * tile);
                const u32 *b = reinterpret_cast<const u32 *>(got + r * out + need.c0 * tile);
                for (u32 i = 0; i < chunk / 4; i++)
                    if (a[i] != b[i]) {
                        if (!bad)
                            fr = r, fi = i;
                        bad++;
                    }
            }
            static FILE *lf;
            static u32 n, nbad;
            if (!lf)
                lf = fopen("sdmc:/3ds/ctr64/shcheck.log", "w");
            n++;
            nbad += bad != 0;
            if (lf && ((bad && nbad < 200) || n % 200 == 1)) {
                fprintf(lf, "draw %u (bad so far %u): %ux%u tw %u parts %u rect %u-%u %u-%u: %u words differ, first r %u i %u\n",
                        (unsigned)n, (unsigned)nbad, (unsigned)sw, (unsigned)t.th, (unsigned)t.tw, (unsigned)parts,
                        need.c0, need.c1, need.r0, need.r1, (unsigned)bad, (unsigned)fr, (unsigned)fi);
                if (bad && nbad < 4) { /* the first tile row of words: transfer copy, draw, source */
                    const u32 *ca = reinterpret_cast<const u32 *>(chk + fr * out + need.c0 * tile);
                    const u32 *cb = reinterpret_cast<const u32 *>(got + fr * out + need.c0 * tile);
                    pica_cache_invalidate(t.data, run * (t.th / 8));
                    const u32 *cs = reinterpret_cast<const u32 *>(static_cast<const u8 *>(t.data) + fr * run + need.c0 * tile);
                    for (int w = 0; w < 3; w++) {
                        const u32 *x = w == 0 ? ca : w == 1 ? cb : cs;
                        fprintf(lf, "  %s", w == 0 ? "copy" : w == 1 ? "draw" : "src ");
                        for (int i = 0; i < 16; i++)
                            fprintf(lf, " %08lx", (unsigned long)x[i]);
                        fputc('\n', lf);
                    }
                    /* where does the draw's first word occur in the source tile row? */
                    const u32 *row = reinterpret_cast<const u32 *>(static_cast<const u8 *>(t.data) + fr * run);
                    int at = -1;
                    for (u32 i = 0; i < run / 4 && at < 0; i++)
                        if (row[i] == cb[0])
                            at = (int)i;
                    fprintf(lf, "  draw word 0 found at src row word %d\n", at);
                }
                fflush(lf);
            }
        }
    }
    return true;
}

/* Texture coordinate transform of one tile (UTextureParamsFast + calcTexCoord + the
 * fragment shader's +0.0001), followed by logical -> storage scaling for the PICA texture. */
struct TexXform {
    float ss = 1, st = 1, os = 0, ot = 0, cs = 1, ct = 1, rs = 1, rt = 1;
    /* (((v * ss - os) * cs) + 0.0001) * rs, folded to v * as + bs by bake() */
    float as = 1, bs = 0.0001f, at = 1, bt = 0.0001f;
    void bake()
    {
        as = ss * cs * rs;
        bs = (0.0001f - os * cs) * rs;
        at = st * ct * rt;
        bt = (0.0001f - ot * ct) * rt;
    }
    float s(float v) const { return v * as + bs; }
    float t(float v) const { return v * at + bt; }
};

/* GLideN64's fog usage for the current draw (UFog): 1 = fog factor in shade alpha, 2 = in rgb */
int fog_usage()
{
    if (RSP.LLE)
        return 0;
    int usage = (gSP.geometryMode & G_FOG) ? 1 : 0;
    if (GBI.getMicrocodeType() == F3DAM) {
        const s16 fogMode = ((gSP.geometryMode >> 13) & 9) + 0xFFF8;
        usage = fogMode == 0 ? 1 : fogMode > 0 ? 2 : usage;
    }
    return usage;
}

/* Development aid: sdmc:/3ds/ctr64/drawlog.txt holds "from [to]" swap numbers; every draw of those
 * frames is
 * written to sdmc:/3ds/ctr64/drawlog.log with its combiner, state and window-space bounding box. */
u32 g_swap_count = 1; /* the swap that will present the draws being made */
FILE *draw_log()
{
    static int from = -2, to = -1;
    static FILE *f;
    if (from == -2) {
        from = -1;
        if (FILE *c = fopen("sdmc:/3ds/ctr64/drawlog.txt", "r")) {
            if (fscanf(c, "%d %d", &from, &to) < 1)
                from = -1;
            if (to < from)
                to = from;
            fclose(c);
        }
    }
    if ((int)g_swap_count < from || (int)g_swap_count > to) {
        if (f && (int)g_swap_count > to) {
            fclose(f);
            f = nullptr;
            from = to = -1;
        }
        return nullptr;
    }
    if (!f)
        f = fopen("sdmc:/3ds/ctr64/drawlog.log", "w");
    return f;
}

void log_draw(const char *what, const CombinerProgram *prog, int kind, const GVtx *v, u32 n, const s32 vp[4],
              const char *state)
{
    FILE *f = draw_log();
    if (!f)
        return;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (u32 i = 0; i < n; i++) {
        if (v[i].w <= 0.0f)
            continue;
        /* NDC -> window pixels of the current viewport (GL: y up) */
        const float x = vp[0] + (v[i].x / v[i].w + 1.0f) * 0.5f * vp[2];
        const float y = vp[1] + (v[i].y / v[i].w + 1.0f) * 0.5f * vp[3];
        x0 = x < x0 ? x : x0; x1 = x > x1 ? x : x1;
        y0 = y < y0 ? y : y0; y1 = y > y1 ? y : y1;
    }
    unsigned long long mux = kind == 0 && prog ? prog->getKey().getMux() : 0;
    fprintf(f, "%s kind %d mux %016llx cyc %u n %u bbox %.0f,%.0f-%.0f,%.0f fog %d othermode %08x %s prim %.2f,%.2f,%.2f,%.2f env %.2f,%.2f,%.2f,%.2f\n",
            what, kind, mux, (unsigned)gDP.otherMode.cycleType, (unsigned)n, x0, y0, x1, y1, fog_usage(),
            (unsigned)gDP.otherMode.l, state, gDP.primColor.r, gDP.primColor.g, gDP.primColor.b, gDP.primColor.a,
            gDP.envColor.r, gDP.envColor.g, gDP.envColor.b, gDP.envColor.a);
    /* addresses: color image, depth image, and texture 0 (fb: a frame buffer texture) */
    const CachedTexture *t0 = textureCache().current[0];
    fprintf(f, "  z %g w %g clamp %d non %d zbuf %d\n", (double)v[0].z, (double)v[0].w, g_ctx ? (int)g_ctx->getClampMode() : -1, (int)GBI.isNoN(),
            (int)((gSP.geometryMode & G_ZBUFFER) != 0));
    fprintf(f, "  ci %08x zi %08x t0 %08x fb %d\n", (unsigned)gDP.colorImage.address, (unsigned)gDP.depthImageAddress,
            t0 ? (unsigned)t0->address : 0u, t0 ? (int)t0->frameBufferTexture : -1);
    if (t0 && gSP.textureTile[0]) {
        const gDPTile *tl = gSP.textureTile[0];
        float s0 = 1e9f, s1 = -1e9f, q0 = 1e9f, q1 = -1e9f;
        for (u32 i = 0; i < n; i++) {
            s0 = v[i].s0 < s0 ? v[i].s0 : s0; s1 = v[i].s0 > s1 ? v[i].s0 : s1;
            q0 = v[i].t0 < q0 ? v[i].t0 : q0; q1 = v[i].t0 > q1 ? v[i].t0 : q1;
        }
        fprintf(f, "  tex %ux%u scale %g,%g off %g,%g clamp %d,%d mask %d,%d mirror %d,%d | tile uls %u lrs %u ult %u lrt %u "
                   "masks %u maskt %u cs %u ct %u ms %u mt %u shifts %u | s %g..%g t %g..%g\n",
                t0->width, t0->height, t0->scaleS, t0->scaleT, t0->offsetS, t0->offsetT, t0->clampS, t0->clampT,
                t0->maskS, t0->maskT, t0->mirrorS, t0->mirrorT, (unsigned)tl->uls, (unsigned)tl->lrs,
                (unsigned)tl->ult, (unsigned)tl->lrt, (unsigned)tl->masks, (unsigned)tl->maskt, (unsigned)tl->clamps,
                (unsigned)tl->clampt, (unsigned)tl->mirrors, (unsigned)tl->mirrort, (unsigned)tl->shifts, s0, s1, q0, q1);
        bool odd = false;
        for (u32 i = 0; i < n; i++)
            if (v[i].w <= 0.0f || v[i].z < -v[i].w || v[i].z > 0.0f || fabsf(v[i].x) > 4.0f * v[i].w)
                odd = true;
        if (odd)
            fprintf(f, "  odd vertices\n");
        if (t0->frameBufferTexture != CachedTexture::fbNone || odd) {
            fprintf(f, "  fbtex size %u fmt %u tile size %u fmt %u\n", (unsigned)t0->size, (unsigned)t0->format,
                    (unsigned)tl->size, (unsigned)tl->format);
            for (u32 i = 0; i < n && i < 12; i++)
                fprintf(f, "  v %g %g %g %g c %u %u %u %u st %g %g\n", v[i].x, v[i].y, v[i].z, v[i].w, v[i].c[0],
                        v[i].c[1], v[i].c[2], v[i].c[3], v[i].s0, v[i].t0);
        }
    }
}

/* Guard band. The hardware rasterizer drops triangles that reach far outside the render target
 * (Azahar draws them): Blast Corps' ground triangles reach 3x the screen width; at 1x (640 px)
 * they draw, with anti-aliasing (1280 px) whole patches of ground were missing on HW only
 * (2026-11-17). A triangle with a vertex outside the window band [GB_MIN, GB_MAX] (or w <= 0) is
 * clipped against that band (and w > 0) here, in clip space with all attributes interpolated;
 * the others go as they are. Returns a flat triangle list in frame memory, or null when no
 * triangle needed it. */
static const float GB_MIN = -256.0f, GB_MAX = 1016.0f, GB_W = 1e-5f;
/* Far limit for w. The N64's RSP computes clip coordinates in s15.16, so its w never exceeds
 * 32768; GLideN64's float transform can make far more. Iggy's Reckin' Balls, in the frame its
 * main menu appears, has triangles from w ~ 2 to w ~ 1e7..3e11 (points thrown to a screen
 * corner), and such a command list hung the HW GPU in about one boot in six (2026-11-21; the
 * same frame without them ran). Triangles are clipped at this w like at the guard band. */
static const float GB_WFAR = 32768.0f;
enum { GB_PLANES = 6 };
struct ClipV {
    float p[4], c[4], st[4];
};
static inline ClipV to_clip(const GVtx &g)
{
    return ClipV{{g.x, g.y, g.z, g.w}, {(float)g.c[0], (float)g.c[1], (float)g.c[2], (float)g.c[3]},
                 {g.s0, g.t0, g.s1, g.t1}};
}
static inline GVtx from_clip(const ClipV &c)
{
    GVtx g;
    g.x = c.p[0]; g.y = c.p[1]; g.z = c.p[2]; g.w = c.p[3];
    for (int i = 0; i < 4; i++) {
        const float x = c.c[i] + 0.5f;
        g.c[i] = (u8)(x < 0.0f ? 0.0f : x > 255.0f ? 255.0f : x);
    }
    g.s0 = c.st[0]; g.t0 = c.st[1]; g.s1 = c.st[2]; g.t1 = c.st[3];
    return g;
}
/* plane k: d = A*x + B*y + C*w >= 0 inside */
static void guard_planes(const s32 vp[4], float pl[GB_PLANES][3])
{
    /* window x = vp0 + (x/w + 1) / 2 * vp2 <= GB_MAX  <=>  x <= ax * w */
    const float axh = 2.0f * (GB_MAX - vp[0]) / (vp[2] ? vp[2] : 1) - 1.0f;
    const float axl = 2.0f * (GB_MIN - vp[0]) / (vp[2] ? vp[2] : 1) - 1.0f;
    const float ayh = 2.0f * (GB_MAX - vp[1]) / (vp[3] ? vp[3] : 1) - 1.0f;
    const float ayl = 2.0f * (GB_MIN - vp[1]) / (vp[3] ? vp[3] : 1) - 1.0f;
    const float t[GB_PLANES][3] = {{-1, 0, axh}, {1, 0, -axl}, {0, -1, ayh}, {0, 1, -ayl}, {0, 0, 1}, {0, 0, -1}};
    memcpy(pl, t, sizeof t);
}
static inline float guard_dist(const float pl[GB_PLANES][3], int k, const float *p)
{
    return pl[k][0] * p[0] + pl[k][1] * p[1] + pl[k][2] * p[3] - (k == 4 ? GB_W : 0.0f) + (k == 5 ? GB_WFAR : 0.0f);
}
/* a vertex outside the band (or w <= 0, or w > GB_WFAR) */
static inline bool guard_out(const float pl[GB_PLANES][3], const GVtx &g)
{
    const float p[4] = {g.x, g.y, g.z, g.w};
    for (int k = 0; k < GB_PLANES; k++)
        if (guard_dist(pl, k, p) < 0.0f)
            return true;
    return false;
}
GVtx *guard_clip(GVtx *v, u32 &count, const void *elements, u32 nidx, bool bytes, const s32 vp[4])
{
    float pl[GB_PLANES][3];
    guard_planes(vp, pl);
    auto dist = [&](int k, const float *p) { return guard_dist(pl, k, p); };
    const u32 ntri = (nidx ? nidx : count) / 3;
    auto vidx = [&](u32 i) -> u32 {
        if (!nidx)
            return i;
        return bytes ? static_cast<const u8 *>(elements)[i] : static_cast<const u16 *>(elements)[i];
    };
    /* quick test: any vertex outside the band? */
    bool any = false;
    for (u32 i = 0; i < count && !any; i++)
        any = guard_out(pl, v[i]);
    if (!any)
        return nullptr;
    /* worst case: a triangle clipped by 6 planes is a polygon of 9 vertices = 7 triangles */
    GVtx *out = static_cast<GVtx *>(pica_frame_alloc(ntri * 21 * sizeof(GVtx), 16));
    if (!out)
        return nullptr;
    u32 n = 0;
    for (u32 t = 0; t < ntri; t++) {
        const GVtx &a = v[vidx(t * 3)], &b = v[vidx(t * 3 + 1)], &c = v[vidx(t * 3 + 2)];
        ClipV poly[2][9];
        int m = 3, cur = 0;
        poly[0][0] = to_clip(a);
        poly[0][1] = to_clip(b);
        poly[0][2] = to_clip(c);
        bool clipped = false;
        for (int k = 0; k < GB_PLANES && m >= 3; k++) {
            float d[9];
            bool out_any = false;
            for (int i = 0; i < m; i++) {
                d[i] = dist(k, poly[cur][i].p);
                out_any |= d[i] < 0.0f;
            }
            if (!out_any)
                continue;
            clipped = true;
            int m2 = 0;
            for (int i = 0; i < m && m2 < 9; i++) {
                const int j = (i + 1) % m;
                const ClipV &P = poly[cur][i], &Q = poly[cur][j];
                if (d[i] >= 0.0f)
                    poly[cur ^ 1][m2++] = P;
                if ((d[i] >= 0.0f) != (d[j] >= 0.0f) && m2 < 9) {
                    const float s = d[i] / (d[i] - d[j]);
                    ClipV &R = poly[cur ^ 1][m2++];
                    for (int e = 0; e < 4; e++) {
                        R.p[e] = P.p[e] + (Q.p[e] - P.p[e]) * s;
                        R.c[e] = P.c[e] + (Q.c[e] - P.c[e]) * s;
                        R.st[e] = P.st[e] + (Q.st[e] - P.st[e]) * s;
                    }
                }
            }
            m = m2;
            cur ^= 1;
        }
        if (!clipped) {
            out[n++] = a;
            out[n++] = b;
            out[n++] = c;
            continue;
        }
        for (int i = 1; i + 1 < m; i++) {
            out[n++] = from_clip(poly[cur][0]);
            out[n++] = from_clip(poly[cur][i]);
            out[n++] = from_clip(poly[cur][i + 1]);
        }
    }
    count = n;
    return out;
}

/* Adds a draw's screen bounding box to the area drawn this frame; once that reaches 90% of the
 * frame buffer, the frame counts as redrawing its whole image (frameskip.c). SM64 never clears,
 * its skybox covers the screen; Paper Mario's name entry only redraws what changes. */
/* Shadows of a frame buffer that is drawn into between samples. Every draw starts a new
 * contents generation (applyState); a shadow copied at gen G holds the contents before the draw
 * made at G (copies are queued before that draw). Here, at the end of that draw, the rows it can
 * touch are taken out of the gen-G shadows; at the next generation (trimShadows) they stay
 * current with the rest, so the next sample copies only what changed. Mario Tennis' court
 * effect draws a strip and samples the band above it ~35 times a frame: 3 tile rows each copied
 * before, 1 now. Dev switches fullshadow.txt, nodrawnrows.txt: off. */
void ContextImplPica::noteDrawnRows(const GVtx *v, u32 n)
{
    if (m_drawFbo == 0)
        return;
    const u32 id = drawFbo().color;
    Tex *c = tex(id);
    if (!c)
        return;
    c->trim_gen = ~0u;
    if (c->sh_gen == ~0u || !needs_shadow(*c))
        return; /* never sampled through a shadow */
    /* rows unknown: this generation's shadows miss this draw entirely (a sample before the next
     * generation starts, as drawTriangles makes, must copy again) */
    auto stale = [&]() {
        for (Shadow &x : m_shadows)
            if (x.owner == id && x.gen == c->gen)
                x.gen = c->gen - 1;
    };
    if (g_fullshadow || g_nodrawnrows || !c->w || !c->h || !v || !n) {
        stale();
        return;
    }
    float y0 = 1e9f, y1 = -1e9f;
    for (u32 i = 0; i < n; i++) {
        if (v[i].w <= 0.0f) { /* crosses the eye plane */
            stale();
            return;
        }
        const float y = v[i].y / v[i].w;
        y0 = y < y0 ? y : y0;
        y1 = y > y1 ? y : y1;
    }
    /* window pixels (GL: y up), then storage rows (stored bottom row first: row = th - y) */
    const float p0 = m_vp[1] + (y0 + 1.0f) * 0.5f * m_vp[3], p1 = m_vp[1] + (y1 + 1.0f) * 0.5f * m_vp[3];
    const int th = c->th, rows = th / 8;
    /* pixel rows whose centers the primitive can cover, half a pixel wider each way (the GPU
     * snaps vertices to 1/16 px and anti-aliasing scales: a tight bound missed edge pixels in
     * Goemon's diary screen); 8-row strips on tile boundaries (Mario Tennis) stay one tile row */
    const int a = (int)floorf(p0), b = (int)ceilf(p1);
    const int s0 = th - b, s1 = th - a;
    int d0 = s0 < 0 ? 0 : s0 / 8, d1 = s1 <= 0 ? 0 : (s1 + 7) / 8;
    d1 = d1 > rows ? rows : d1;
    d0 = d0 > d1 ? d1 : d0;
    for (Shadow &x : m_shadows) {
        if (x.owner != id || x.src != c->data || x.gen != c->gen)
            continue;
        TileRect keep[8];
        u32 nk = 0;
        for (u32 i = 0; i < x.nv; i++) {
            const TileRect r = x.v[i];
            if ((int)r.r1 <= d0 || (int)r.r0 >= d1) {
                if (nk < 8) keep[nk++] = r;
                continue;
            }
            if ((int)r.r0 < d0 && nk < 8)
                keep[nk++] = TileRect{r.c0, r.c1, r.r0, (u16)d0};
            if ((int)r.r1 > d1 && nk < 8)
                keep[nk++] = TileRect{r.c0, r.c1, (u16)d1, r.r1};
        }
        memcpy(x.v, keep, sizeof(TileRect) * nk);
        x.nv = (u8)nk;
    }
    c->trim_gen = c->gen;
}

/* applyState, about to start the next generation of target `id`: if the one draw of this
 * generation took its rows out of the shadows, the shadows of this generation carry over. */
void ContextImplPica::trimShadows(u32 id, Tex &c)
{
    if (c.trim_gen != c.gen)
        return;
    for (Shadow &x : m_shadows)
        if (x.owner == id && x.src == c.data && x.gen == c.gen)
            x.gen = c.gen + 1;
}

void ContextImplPica::noteCoverage(const GVtx *v, u32 n)
{
    if (m_drawFbo == 0)
        return;
    /* a draw sampling a frame buffer (Jet Force Gemini's round view, motion blur) composes an
     * image drawn earlier: skipping the next frame would leave that source empty */
    if (!ctr64_frame_fbtex && m_program && m_kind == Kind::Combiner) {
        TextureCache &cache = textureCache();
        for (u32 t = 0; t < 2; t++)
            if (m_program->usesTile(t) && cache.current[t] &&
                cache.current[t]->frameBufferTexture != CachedTexture::fbNone)
                ctr64_frame_fbtex = true;
    }
    if (ctr64_frame_cleared)
        return;
    const Tex *c = tex(drawFbo().color);
    if (!c || !c->w || !c->h)
        return;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (u32 i = 0; i < n; i++) {
        if (v[i].w <= 0.0f)
            return; /* crosses the eye plane: leave it out */
        const float rw = 1.0f / v[i].w;
        const float x = v[i].x * rw, y = v[i].y * rw;
        x0 = x < x0 ? x : x0; x1 = x > x1 ? x : x1;
        y0 = y < y0 ? y : y0; y1 = y > y1 ? y : y1;
    }
    const float W = c->w, H = c->h;
    auto px = [&](float ndc, float o, float s, float lim) {
        const float p = o + (ndc + 1.0f) * 0.5f * s;
        return p < 0.0f ? 0.0f : p > lim ? lim : p;
    };
    const float w = px(x1, m_vp[0], m_vp[2], W) - px(x0, m_vp[0], m_vp[2], W);
    const float h = px(y1, m_vp[1], m_vp[3], H) - px(y0, m_vp[1], m_vp[3], H);
    m_cover += w * h;
    if (m_cover >= 0.9f * W * H)
        ctr64_frame_cleared = true;
}

/* Frameskip: in a skipped frame the draws into N64 frame buffers are dropped and the buffers
 * marked. The window, GLideN64's copy of the displayed buffer, is always drawn: the N64 shows a
 * buffer drawn in an earlier frame (double or triple buffering), so whether a present shows
 * a fresh image depends on the buffer it copies, not on whether its own frame was skipped. A
 * present copying a marked buffer is dropped (present()). Returns whether to drop this draw. */
bool ContextImplPica::skipDraw(bool clear)
{
    const Tex *src = !clear && m_kind == Kind::Copy ? tex(m_unit[0]) : nullptr;
    if (m_drawFbo == 0) {
        /* the window keeps the image last presented, so a frame dump shows the screen */
        if ((src && src->skipped) || m_showStale) {
            m_showStale = true;
            return true;
        }
        if (src) {
            m_winSrc = m_unit[0];
            /* the image on screen again (SM64 presents every VI, draws every other) */
            if (++m_winCopies == 1 && m_winSrc == m_shown && !src->drawn) {
                m_showSame = true;
                return true;
            }
        }
        if (m_showSame)
            return true;
        if (m_winClear) {
            m_winClear = false;
            clearTarget(m_window, m_winClearColor);
        }
        return false;
    }
    Tex *c = tex(drawFbo().color);
    ctr64_frame_drew = true;
    /* only buffers that have been on screen: an off-screen image drawn once and reused (Pokemon
     * Stadium's entry selection panels, GitHub #4) stayed broken until the screen was left
     * when its frame was skipped; nor a buffer without an image yet: a new buffer whose first
     * frame was skipped stayed blank where the game only draws over it (the same screen: the
     * background flickered white/black) */
    const FrameBuffer *cur = frameBufferList().getCurrent();
    if (ctr64_render_skip && cur && cur->m_isMainBuffer && c && c->ever) {
        if (c)
            c->skipped = true;
        return true;
    }
    if (c) {
        if (c->skipped && !(src && src->skipped))
            healSkipped(*c);
        c->skipped = src && src->skipped; /* a copy of a marked buffer is as stale */
        c->drawn = c->ever = true;
    }
    return false;
}

/* A buffer that missed a frame is drawn again: it still holds the picture from before the
 * skipped frame. A game that then redraws only what changes (Pokemon Stadium's entry selection
 * after its panels slid in, GitHub #4) left it at the half-slid picture, shown every other frame
 * until the screen was left. It gets the last picture shown first (the other buffer of a double-
 * buffered game, a frame apart); a full redraw simply covers it. One GPU copy per skipped buffer. */
void ContextImplPica::healSkipped(Tex &c)
{
    const Tex *s = tex(m_shown);
    if (!s || s == &c || s->skipped || !s->data || !c.data || s->depth || c.depth || s->tw != c.tw ||
        s->th != c.th || s->bpp != c.bpp || s->fmt != c.fmt || s->th_full || c.th_full)
        return;
    const u32 bytes = storageBytes(c);
    if (s->vram)
        pica_copy_from_vram(c.data, s->data, bytes);
    else
        pica_copy(c.data, s->data, bytes);
    c.used_frame = pica_frame_number();
    c.rows_used = s->rows_used;
    c.dirty = true;
    c.gen++;
    ctr64_frames_healed++;
}

void ContextImplPica::texture_xforms(TexXform tx[2])
{
    TextureCache &cache = textureCache();
    for (u32 t = 0; t < 2; t++) {
        TexXform &x = tx[t];
        const gDPTile *tile = gSP.textureTile[t];
        CachedTexture *ct = cache.current[t];
        if (!tile || !ct)
            continue;
        float fuls = 0, fult = 0;
        if (tile->textureMode != TEXTUREMODE_BGIMAGE && tile->textureMode != TEXTUREMODE_FRAMEBUFFER_BG) {
            fuls = tile->fuls;
            fult = tile->fult;
            if (tile->frameBufferAddress > 0 && frameBufferList().getBuffer(tile->frameBufferAddress)) {
                if (tile->masks > 0 && tile->clamps == 0)
                    fuls = float(tile->uls % (1 << tile->masks));
                if (tile->maskt > 0 && tile->clampt == 0)
                    fult = float(tile->ult % (1 << tile->maskt));
            }
        }
        f32 shiftS = 1.0f, shiftT = 1.0f;
        getTextureShiftScale(t, cache, shiftS, shiftT);
        x.ss = shiftS;
        x.st = shiftT;
        /* (v*shift - texOffset + cacheOffset) * cacheScale */
        x.os = fuls - ct->offsetS;
        x.ot = fult - ct->offsetT;
        x.cs = ct->scaleS;
        x.ct = ct->scaleT;
        /* texel u of the cached texture is storage texel u: normalise by the storage size. (Not
         * w/tw: with a wrap mask GLideN64's cache scale is 1/pow2(width), e.g. 1/128 for the
         * 80-texel strips of the SM64 title background.) */
        if (Tex *p = tex(m_unit[t]))
            if (p->tw && x.cs > 0.0f && x.ct > 0.0f) {
                if (ct->frameBufferTexture != CachedTexture::fbNone) {
                    /* a frame buffer: cache scale = render scale / width, so u * cs * width is the
                     * storage texel (twice u when rendering at 2x) */
                    x.rs = float(ct->width) / sample_w(*p);
                    x.rt = float(ct->height) / sample_h(*p);
                } else {
                    x.rs = 1.0f / (x.cs * sample_w(*p));
                    x.rt = 1.0f / (x.ct * sample_h(*p));
                }
            }
    }
    tx[0].bake();
    tx[1].bake();
}

bool ContextImplPica::setupCombiner(bool rect)
{
    CRUMB("gfx: setupCombiner");
    m_shadowBound[0] = m_shadowBound[1] = nullptr;
    (void)rect;
    PicaTev tev[pica_cmb::MAX_STAGES];
    for (auto &t : tev)
        pica_tev_default(&t);
    u8 buf_rgb = 0, buf_a = 0;
    u32 buf_color = 0xFFFFFFFF;
    bool alpha_test = false, alpha_greater = false;
    u8 alpha_ref = 0;

    switch (m_kind) {
    case Kind::Copy:
    case Kind::Text:
        bindUnit(0);
        pica_texture(1, NULL, 0, 0, GPU_RGBA8, 0);
        pica_texture(2, NULL, 0, 0, GPU_RGBA8, 0);
        tev[0].src_rgb = tev[0].src_alpha = GPU_TEVSOURCES(GPU_TEXTURE0, 0, 0);
        break;
    case Kind::Clear:
        pica_texture(0, NULL, 0, 0, GPU_RGBA8, 0);
        pica_texture(1, NULL, 0, 0, GPU_RGBA8, 0);
        pica_texture(2, NULL, 0, 0, GPU_RGBA8, 0);
        tev[0].src_rgb = tev[0].src_alpha = GPU_TEVSOURCES(GPU_CONSTANT, 0, 0);
        tev[0].color = 0;
        break;
    default: {
        const pica_cmb::Program &pr = static_cast<const PicaCombinerProgram *>(m_program)->tev();
        int n = pr.n;
        for (int i = 0; i < n; i++) {
            const pica_cmb::Stage &s = pr.st[i];
            PicaTev &t = tev[i];
            t.src_rgb = GPU_TEVSOURCES(s.src_rgb[0], s.src_rgb[1], s.src_rgb[2]);
            t.src_alpha = GPU_TEVSOURCES(s.src_a[0], s.src_a[1], s.src_a[2]);
            t.op_rgb = GPU_TEVOPERANDS(s.op_rgb[0], s.op_rgb[1], s.op_rgb[2]);
            t.op_alpha = GPU_TEVOPERANDS(s.op_a[0], s.op_a[1], s.op_a[2]);
            t.func_rgb = s.func_rgb;
            t.func_alpha = s.func_a;
            t.scale_rgb = s.scale_rgb;
            t.scale_alpha = s.scale_a;
            t.color = pica_cmb::constant(s.k_rgb, s.k_a);
        }
        /* GLSL backend's uFbMonochrome mode 1: an 8-bit frame buffer sampled as a texture reads
         * its red channel into all four (vec4(name.r)), alpha included. Blast Corps renders its
         * light pools into 8-bit buffers that way; read as RGBA their black became opaque black
         * squares on the ground. Every operand on that unit selects R instead. (Mode 2/3,
         * luminance, and the fixed alpha 0.825 for imageRead off are not done.) */
        for (u32 u = 0; u < 2; u++) {
            const CachedTexture *ct = textureCache().current[u];
            if (!pr.uses_tex[u] || !ct || ct->frameBufferTexture == CachedTexture::fbNone ||
                ct->size != G_IM_SIZ_8b)
                continue;
            const u8 src = u ? GPU_TEXTURE1 : GPU_TEXTURE0;
            for (int i = 0; i < n; i++) {
                const pica_cmb::Stage &s = pr.st[i];
                u8 orgb[3], oa[3];
                for (int k = 0; k < 3; k++) {
                    orgb[k] = s.src_rgb[k] == src ? (u8)(GPU_TEVOP_RGB_SRC_R | (s.op_rgb[k] & 1)) : s.op_rgb[k];
                    oa[k] = s.src_a[k] == src ? (u8)(GPU_TEVOP_A_SRC_R | (s.op_a[k] & 1)) : s.op_a[k];
                }
                tev[i].op_rgb = GPU_TEVOPERANDS(orgb[0], orgb[1], orgb[2]);
                tev[i].op_alpha = GPU_TEVOPERANDS(oa[0], oa[1], oa[2]);
            }
        }
        buf_rgb = pr.buf_rgb_mask;
        buf_a = pr.buf_a_mask;
        pr.uses_tex[0] ? bindUnit(0, true) : pica_texture(0, NULL, 0, 0, GPU_RGBA8, 0);
        pr.uses_tex[1] ? bindUnit(1, true) : pica_texture(1, NULL, 0, 0, GPU_RGBA8, 0);
        /* unit 2: the constant 0.5 the compiler uses as a second per-stage constant */
        if (pr.uses_half)
            pica_texture(2, m_half, 8, 8, GPU_RGBA8,
                         GPU_TEXTURE_MAG_FILTER(GPU_NEAREST) | GPU_TEXTURE_MIN_FILTER(GPU_NEAREST));
        else
            pica_texture(2, NULL, 0, 0, GPU_RGBA8, 0);

        /* Fragment-shader tail of the GLSL backend (legacy blending): coverage as alpha, fog. */
        const u32 cycle = gDP.otherMode.cycleType;
        const bool cvg_alpha = cycle <= G_CYC_2CYCLE && gDP.otherMode.alphaCvgSel && !gDP.otherMode.cvgXAlpha;
        const bool fog = config.generalEmulation.enableLegacyBlending != 0 && fog_usage() == 1;
        if ((cvg_alpha || fog) && n < pica_cmb::MAX_STAGES) {
            PicaTev &t = tev[n++];
            if (fog) {
                t.src_rgb = GPU_TEVSOURCES(GPU_CONSTANT, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
                t.op_rgb = GPU_TEVOPERANDS(GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
                t.func_rgb = GPU_INTERPOLATE;
            }
            if (cvg_alpha)
                t.src_alpha = GPU_TEVSOURCES(GPU_CONSTANT, 0, 0);
            t.color = pica_const_color(to_u8(gDP.fogColor.r), to_u8(gDP.fogColor.g), to_u8(gDP.fogColor.b), 255);
        }

        /* Alpha compare (UAlphaTestInfo + ShaderAlphaTest); cvgXAlpha discards below 0.125.
         * The RDP's threshold compare writes a pixel only when its alpha is GREATER than the
         * blend colour's alpha (angrylion: comb_alpha > threshold), so with a threshold of 0 it
         * still drops alpha-0 pixels. GLideN64 discards only alpha < threshold, i.e. nothing at
         * 0: Gauntlet Legends' HUD portrait, drawn twice without blending and cut out by the
         * test alone, became a black box (its second, 35x33 layer is transparent around the face). */
        float ref = -1.0f;
        bool greater = false;
        if (cycle == G_CYC_COPY) {
            if (gDP.otherMode.alphaCompare & G_AC_THRESHOLD)
                ref = 0.5f;
        } else if (cycle != G_CYC_FILL && (gDP.otherMode.alphaCompare & G_AC_THRESHOLD)) {
            ref = gDP.blendColor.a;
            greater = true;
            if (cvg_alpha) {
                if (0.125f < ref)
                    return false; /* the test compares the constant 0.125: every fragment fails */
                ref = -1.0f;
                greater = false;
            }
        }
        if (cycle <= G_CYC_2CYCLE && gDP.otherMode.cvgXAlpha && ref < 0.125f) {
            ref = 0.125f;
            greater = false;
        }
        if (ref > 0.0f || (greater && ref >= 0.0f)) {
            alpha_test = true;
            alpha_greater = greater;
            alpha_ref = to_u8(ref);
        }
        /* GLideN64 (ShaderAlphaTest) compares alpha1, cycle 1's alpha, not the output alpha;
         * the PICA tests the TEV output. When cycle 2 computes another alpha and the blender
         * does not read the source alpha, cycle 2 passes alpha1 through instead (the alpha
         * written to the buffer is then alpha1). Gauntlet Legends' HUD gauges: alpha1 = the
         * IA4 gauge shape, alpha2 = a 1-bit RGBA16 mask that is 1 everywhere -> opaque black
         * boxes over the heart and score. */
        if (alpha_test && pr.alpha2_differs && !cvg_alpha && !g_noalpha1) {
            const auto reads_src_alpha = [](u32 b) { return b == BL_SRC_ALPHA || b == BL_ONE_MINUS_SRC_ALPHA; };
            if (!m_enBlend || !(reads_src_alpha(m_blend[0]) || reads_src_alpha(m_blend[1]) ||
                                reads_src_alpha(m_blend[2]) || reads_src_alpha(m_blend[3]))) {
                for (int i = pr.cycle2_first; i < pr.n; i++) {
                    tev[i].src_alpha = GPU_TEVSOURCES(GPU_PREVIOUS, 0, 0);
                    tev[i].op_alpha = GPU_TEVOPERANDS(GPU_TEVOP_A_SRC_ALPHA, 0, 0);
                    tev[i].func_alpha = GPU_REPLACE;
                    tev[i].scale_alpha = GPU_TEVSCALE_1;
                }
            }
        }
        break;
    }
    }
    for (int i = 0; i < pica_cmb::MAX_STAGES; i++)
        pica_tev(i, &tev[i]);
    pica_tev_buffer(buf_rgb, buf_a, buf_color);
    pica_alpha_test(alpha_test, alpha_greater ? GPU_GREATER : GPU_GEQUAL, alpha_ref);
    return true;
}


void ContextImplPica::drawTriangles(const Context::DrawTriangleParameters &p)
{
    DrawTimer timer;
    CRUMB("gfx: drawTriangles");
    const u32 mode = u32(p.mode);
    if (mode == DM_LINES || !p.vertices || !p.verticesCount || m_kind == Kind::Noop || skipDraw())
        return;
    {
        CTR64_PROF(32);
        /* Triangles drawn into the game's own Z buffer (color image = depth image address).
         * Pilotwings 64 draws the glider's shadow there (blended black: the Z values under the
         * shadow move nearer), then the shadow mesh into the frame in the same render mode,
         * with a depth test against that Z buffer which then fails over the ground. Without N64
         * depth compare GLideN64 gives the Z address a separate color buffer, the Z buffer
         * never changes, and the second pass drew a large dark shape over the ground and out to
         * the horizon over the sea. Both passes are dropped (the second: triangles in the same
         * depth-tested, non-writing render mode in that game frame), as the PC build shows the
         * scene with N64 depth compare. */
        if (config.frameBufferEmulation.N64DepthCompare == 0 && gDP.otherMode.depthCompare &&
            !gDP.otherMode.depthUpdate) {
            if (gDP.colorImage.address == gDP.depthImageAddress && gDP.depthImageAddress != 0) {
                m_zPassMode = gDP.otherMode.l;
                m_zPassAge = 0;
                return;
            }
            if (m_zPassMode && gDP.otherMode.l == m_zPassMode)
                return;
        }
        if (!applyState(true)) {
            ctr64_gfx_stats[ST_DROP]++;
            return;
        }
        if (!setupCombiner(false))
            return;
    }
    ctr64_gfx_stats[ST_TRI]++;
    {   /* draws into FCRAM render targets (stats.log "fcram draws"): slower than VRAM */
        const Fbo &f = drawFbo();
        const Tex *c = tex(f.color), *d = tex(f.depth);
        if (c && !c->vram) ctr64_gfx_fcram[0]++;
        if (f.rt.depth && d && !d->vram) ctr64_gfx_fcram[1]++;
    }
    /* Indexed triangle lists (renderAndDrawTriangles: each kept vertex once): the vertices the
     * triangles use are converted once each, into the pool in the order of first use, and drawn
     * with the triangles' indices remapped to them (the GPU's post-vertex cache; a vertex is
     * written once instead of once per triangle). Dev switch noidxdraw.txt: converted into
     * m_idxScratch and expanded into a plain list in the pool (below), as before. */
    const bool indexedIn = p.elementsCount && p.elements && u32(p.elementsType) == DT_UNSIGNED_SHORT &&
                           mode == DM_TRIANGLES;
    const bool compact = indexedIn && !g_noidxdraw && p.verticesCount <= 0xFFFF;
    const bool expand = indexedIn && !compact;
    GVtx *v;
    u16 *compactIdx = nullptr;
    u32 compactCount = 0;
    if (compact) {
        pica_reserve(4096, p.verticesCount * sizeof(GVtx) + p.elementsCount * 2 + 256);
        if (m_idxUsed.size() < p.verticesCount) {
            m_idxUsed.resize(p.verticesCount, 0u);
            m_idxRemap.resize(p.verticesCount);
        }
        if (++m_idxGen == 0) {
            std::fill(m_idxUsed.begin(), m_idxUsed.end(), 0u);
            m_idxGen = 1;
        }
        const u16 *e = static_cast<const u16 *>(p.elements);
        for (u32 i = 0; i < p.elementsCount; i++)
            if (m_idxUsed[e[i]] != m_idxGen) {
                m_idxUsed[e[i]] = m_idxGen;
                m_idxRemap[e[i]] = u16(compactCount++);
            }
        v = static_cast<GVtx *>(pica_frame_alloc(compactCount * sizeof(GVtx), 16));
        compactIdx = static_cast<u16 *>(pica_frame_alloc(p.elementsCount * 2, 16));
        if (!v || !compactIdx)
            return;
        for (u32 i = 0; i < p.elementsCount; i++)
            compactIdx[i] = m_idxRemap[e[i]];
    } else if (expand) {
        pica_reserve(4096, p.elementsCount * sizeof(GVtx) + 256);
        if (m_idxScratch.size() < p.verticesCount) {
            m_idxScratch.resize(p.verticesCount);
            m_idxUsed.resize(p.verticesCount, 0u);
        }
        v = m_idxScratch.data();
        /* only the vertices the elements refer to are converted (the others: culled triangles) */
        if (++m_idxGen == 0) {
            std::fill(m_idxUsed.begin(), m_idxUsed.end(), 0u);
            m_idxGen = 1;
        }
        const u16 *e = static_cast<const u16 *>(p.elements);
        for (u32 i = 0; i < p.elementsCount; i++)
            m_idxUsed[e[i]] = m_idxGen;
    } else {
        pica_reserve(4096, p.verticesCount * sizeof(GVtx) + p.elementsCount * 2 + 256);
        v = static_cast<GVtx *>(pica_frame_alloc(p.verticesCount * sizeof(GVtx), 16));
        if (!v)
            return;
    }

    /* GLideN64 vertex shader math (VertexShaderTriangle + VertexShaderEnd), per vertex */
    const float vs0 = gSP.viewport.vscale[0], vs1 = -gSP.viewport.vscale[1];
    const float vt0 = gSP.viewport.vtrans[0], vt1 = gSP.viewport.vtrans[1];
    const bool native = config.frameBufferEmulation.nativeResFactor == 1 && config.video.multisampling == 0;
    const float offset = (config.generalEmulation.enableInaccurateTextureCoordinates == 0 && native) ? 0.5f : 0.0f;
    const int fogUsage = fog_usage();
    const float fogMul = gSP.fog.multiplierf, fogOff = gSP.fog.offsetf;
    const bool clampZ = m_clamp != ClampMode::ClippingEnabled;
    /* microcodes without near clipping (NoN) clip at the far plane still, which the GPU does
     * (Pilotwings: the far part of an ocean triangle floated in the sky, clamped instead) */
    const bool clampFar = m_clamp == ClampMode::NoClipping;
    TexXform tx[2];
    texture_xforms(tx);
    const bool persp = (RSP.LLE || GBI.isTexturePersp()) ? gDP.otherMode.texturePersp != 0 : true;
    const float tscale_s = gSP.texture.scales, tscale_t = gSP.texture.scalet;

    const ViewportZ vpz;
    bool wPos = true; /* every converted vertex in front of the eye (projective draws) */
    Ctr64ProfScope vtx_scope(33);
    for (u32 i = 0; i < p.verticesCount; i++) {
        if ((expand || compact) && m_idxUsed[i] != m_idxGen)
            continue;
        const SPVertex &s = i < p.verticesBase ? p.vertices[i] : p.vertices2[i - p.verticesBase];
        GVtx &o = compact ? v[m_idxRemap[i]] : v[i];
        float x = s.x, y = s.y, z = s.z, w = s.w;
        const u32 mod = s.modify;

        if (mod & MODIFY_XY) {
            x *= w;
            y *= w;
        } else {
            x = x * vs0 + vt0 * w;
            y = y * vs1 + vt1 * w;
            x = fast_floor(x * 4.0f) * 0.25f;
            y = fast_floor(y * 4.0f) * 0.25f;
        }
        if (mod & MODIFY_Z)
            z *= w;
        x += offset * w;
        y += offset * w;
        o.x = (x - 0.5f * SCREEN_SIZE_DIM * w) / (0.5f * SCREEN_SIZE_DIM);
        o.y = (y - 0.5f * SCREEN_SIZE_DIM * w) / (0.5f * SCREEN_SIZE_DIM);
        float zp = vpz.set ? -(z * vpz.a + w * vpz.b) : -0.5f * (z + w);
        if (clampZ) {
            if (zp > 0.0f) zp = 0.0f;
            if (zp < -w && (clampFar || zp >= -1.0005f * w)) zp = -w; /* at the plane (skies): kept, rounding or not */
        }
        o.z = zp;
        o.w = w;
        if (!(w > 1e-4f)) /* 1/w in the shader: no infinities (float24) */
            wPos = false;

        const float *col = p.flatColors ? &s.flat_r : &s.r;
        o.c[0] = to_u8(col[0]); o.c[1] = to_u8(col[1]); o.c[2] = to_u8(col[2]); o.c[3] = to_u8(col[3]);
        if (fogUsage) {
            float fp = (s.z < -s.w && !(mod & MODIFY_Z)) ? (-fogMul + fogOff) : (s.z / s.w * fogMul + fogOff);
            u8 f8 = to_u8(fp < 0.0f ? 0.0f : fp > 1.0f ? 1.0f : fp);
            if (fogUsage == 1)
                o.c[3] = f8;
            else
                o.c[0] = o.c[1] = o.c[2] = f8;
        }
        float ts = s.s * tscale_s, tt = s.t * tscale_t;
        if (!persp && !(mod & MODIFY_ST)) {
            ts *= 0.5f;
            tt *= 0.5f;
        }
        o.s0 = tx[0].s(ts); o.t0 = tx[0].t(tt);
        o.s1 = tx[1].s(ts); o.t1 = tx[1].t(tt);
    }

    vtx_scope.end();
    /* Any vertex outside the guard band? Tested here on the converted vertices (each once):
     * the band's planes are linear in clip space, and every vertex made below (index expansion,
     * shade subdivision: points between two vertices with w > 0) lies inside when these do, so
     * guard_clip need not look at the many more vertices drawn. */
    bool guardAny = false;
    if (mode == DM_TRIANGLES && !g_noguard) {
        float pl[GB_PLANES][3];
        guard_planes(m_vp, pl);
        if (compact) {
            for (u32 i = 0; i < compactCount && !guardAny; i++)
                guardAny = guard_out(pl, v[i]);
        } else {
            for (u32 i = 0; i < p.verticesCount && !guardAny; i++)
                if (!expand || m_idxUsed[i] == m_idxGen)
                    guardAny = guard_out(pl, v[i]);
        }
        if (guardAny) { /* stats.log: draws cut at the far w limit (GB_WFAR) */
            const u32 nv = compact ? compactCount : p.verticesCount;
            for (u32 i = 0; i < nv; i++)
                if ((!expand || m_idxUsed[i] == m_idxGen) && v[i].w > GB_WFAR) {
                    ctr64_gfx_far_draws++;
                    break;
                }
        }
    }
    u32 count = compact ? compactCount : p.verticesCount;
    const void *elements = compact ? compactIdx : p.elements;
    u32 elementsCount = p.elementsCount;
    if (expand) { /* room reserved above (no pica_reserve between: it may restart the pool) */
        const u16 *e = static_cast<const u16 *>(elements);
        GVtx *flat = static_cast<GVtx *>(pica_frame_alloc(elementsCount * sizeof(GVtx), 16));
        if (!flat)
            return;
        for (u32 i = 0; i < elementsCount; i++)
            flat[i] = v[e[i]];
        v = flat;
        count = elementsCount;
        elements = nullptr;
        elementsCount = 0;
    }
    bool idxBytes = u32(p.elementsType) == DT_UNSIGNED_BYTE && !compact;
    u16 *idxPool = compactIdx; /* indices already in frame memory (compacted, shade subdivision) */
    /* Screen-linear shade (see linearizeShade): a draw without texture unit 1 is drawn projective
     * instead (pica_projective: the GPU interpolates colour linearly on screen, exactly, and
     * texture 0 stays perspective-correct), with no subdivision. */
    bool proj = false;
    /* not for decals (polygon offset): the shader's z / w in 24-bit floats loses the few depth
     * units the offset gives them, so shadows on the ground z-fought into fragments (Mario
     * Tennis on the court, Conker; Azahar and HW 2026-11-18) */
    if (mode == DM_TRIANGLES && m_kind == Kind::Combiner && m_program && !g_noshadelin && !g_noproj && wPos &&
        !(m_enDepth && m_enPolyOffset) &&
        pica_proj_bool_bit >= 0 && m_program->usesShade() &&
        !static_cast<const PicaCombinerProgram *>(m_program)->tev().uses_tex[1])
        proj = true;
    if (!proj && (!(elementsCount && elements) || idxPool) && mode == DM_TRIANGLES && m_kind == Kind::Combiner &&
        m_program && !g_noshadelin && m_program->usesShade()) {
        CTR64_PROF(34);
        CRUMB("gfx: tri shade");
        u16 *si;
        u32 sn;
        v = linearizeShade(v, count, idxPool, idxPool ? elementsCount : 0,
                           fogUsage == 1 && config.generalEmulation.enableLegacyBlending != 0, si, sn);
        if (si) {
            elements = idxPool = si;
            elementsCount = sn;
            idxBytes = false;
        }
    }
    CTR64_PROF(35);

    if (guardAny) {
        CRUMB("gfx: tri guard");
        u32 n_idx = (elementsCount && elements) ? elementsCount : 0;
        if (GVtx *c = guard_clip(v, count, n_idx ? elements : nullptr, n_idx, idxBytes, m_vp)) {
            idxPool = nullptr;
            v = c;
            elements = nullptr;
            elementsCount = 0;
            if (!count)
                return; /* every triangle outside the band: nothing to draw */
            for (u32 i = 0; i < count && proj; i++)
                proj = c[i].w > 1e-4f;
        }
    }

    if (draw_log())
        log_draw("tri", m_program, (int)m_kind, v, count, m_vp, stateString());
    noteCoverage(v, count);
    noteDrawnRows(v, count);
    pica_vertex_buffer(v);

    GPU_Primitive_t prim_type = mode == DM_TRIANGLE_STRIP ? GPU_TRIANGLE_STRIP : mode == DM_TRIANGLE_FAN ? GPU_TRIANGLE_FAN : GPU_TRIANGLES;
    u16 *idx = idxPool;
    if (elementsCount && elements && !idx) {
        idx = static_cast<u16 *>(pica_frame_alloc(elementsCount * 2, 16));
        if (!idx)
            return;
        if (idxBytes) {
            const u8 *e = static_cast<const u8 *>(elements);
            for (u32 i = 0; i < elementsCount; i++)
                idx[i] = e[i];
        } else {
            memcpy(idx, elements, elementsCount * 2);
        }
    }
    if (m_enPolyOffset && m_enDepth)
        pica_depth_map(k_depth_scale, poly_offset_for_draw(v, idx, idx ? elementsCount : count, prim_type));
    turnVertices(v, count);
    pica_projective(proj);
    CRUMB("gfx: tri draw");
    if (idx)
        pica_draw_elements(prim_type, idx, elementsCount);
    else
        pica_draw_arrays(prim_type, 0, count);
    gtDraw(0);
}

/* ---- screen-linear shade -------------------------------------------------------------------
 * The N64 RDP interpolates shade linearly in screen space, and so does GLideN64 with clipping
 * enabled (vShadeColorNoperspective). The PICA interpolates vertex colours perspective-
 * correctly, which differs visibly on large triangles spanning a wide depth range (floors near
 * the camera). Such triangles are subdivided at screen-space edge midpoints until the w ratio in
 * each piece is small enough: position, depth and texture coordinates are interpolated in clip
 * space (t = wa / (wa + wb) projects to the screen midpoint), shade by the plain average, and the
 * fog factor, which GL interpolates perspective-correctly, in clip space.
 * Along an edge with w ratio r the two weights differ by at most (r - 1) / (2 (r + 1)), so the
 * shade error is bounded by that times the largest channel difference between the vertices. */
const float k_shade_max_error = 1.5f; /* colour levels (of 255) */
const int k_shade_max_depth = 4;
/* A piece this small on screen (pixels of the render target, both directions) is not split: the
 * shade error inside it is a level or two over a few pixels. Without it every floor triangle of
 * Donkey Kong 64's intro went to 256 pieces (HW: 1 M triangles/s, 60-70 ms/frame, 25% speed). */
const float k_shade_min_px = 16.0f;
/* at most this many triangles a frame come out of the subdivision; past it draws go as they are */
const u32 k_shade_frame_budget = 6000;

struct ShadeView { float hw, hh; }; /* half the viewport in pixels */

inline GVtx screen_midpoint(const GVtx &a, const GVtx &b, bool fog_alpha)
{
    const float t = a.w / (a.w + b.w);
    GVtx m;
    m.x = a.x + (b.x - a.x) * t;
    m.y = a.y + (b.y - a.y) * t;
    m.z = a.z + (b.z - a.z) * t;
    m.w = a.w + (b.w - a.w) * t;
    m.s0 = a.s0 + (b.s0 - a.s0) * t;
    m.t0 = a.t0 + (b.t0 - a.t0) * t;
    m.s1 = a.s1 + (b.s1 - a.s1) * t;
    m.t1 = a.t1 + (b.t1 - a.t1) * t;
    for (int k = 0; k < 3; k++)
        m.c[k] = (u8)((a.c[k] + b.c[k] + 1) >> 1);
    m.c[3] = fog_alpha ? (u8)(a.c[3] + (b.c[3] - a.c[3]) * t + 0.5f) : (u8)((a.c[3] + b.c[3] + 1) >> 1);
    return m;
}

/* Whether perspective-correct shade can be off by more than k_shade_max_error over the triangle
 * (alpha ignored when it carries the fog factor, which GL interpolates perspective-correctly too). */
inline bool shade_needs_split(const GVtx &a, const GVtx &b, const GVtx &c, bool fog_alpha, const ShadeView &sv)
{
    const float lo = std::min(a.w, std::min(b.w, c.w)), hi = std::max(a.w, std::max(b.w, c.w));
    if (lo <= 0.0f)
        return false;
    int dc = 0;
    for (int k = 0, n = fog_alpha ? 3 : 4; k < n; k++) {
        const int mn = std::min(a.c[k], std::min(b.c[k], c.c[k])), mx = std::max(a.c[k], std::max(b.c[k], c.c[k]));
        dc = std::max(dc, mx - mn);
    }
    if (dc * (hi - lo) <= 2.0f * k_shade_max_error * (hi + lo))
        return false;
    const float xa = a.x / a.w * sv.hw, xb = b.x / b.w * sv.hw, xc = c.x / c.w * sv.hw;
    const float ya = a.y / a.w * sv.hh, yb = b.y / b.w * sv.hh, yc = c.y / c.w * sv.hh;
    const float ex = std::max(xa, std::max(xb, xc)) - std::min(xa, std::min(xb, xc));
    const float ey = std::max(ya, std::max(yb, yc)) - std::min(ya, std::min(yb, yc));
    return ex > k_shade_min_px || ey > k_shade_min_px;
}

/* Splits a triangle at its screen-space edge midpoints (recursively, while shade_needs_split):
 * the pieces share the vertices their parent made (the same
 * values, so the same pieces in the same order as the flat list); about a third of the vertices
 * to write, and the GPU's post-vertex cache */
void subdivide_for_shade_idx(u32 ia, u32 ib, u32 ic, int depth, bool fog_alpha, const ShadeView &sv,
                             std::vector<GVtx> &ov, std::vector<u16> &oi)
{
    const GVtx a = ov[ia], b = ov[ib], c = ov[ic]; /* copies: ov grows below */
    if (depth == 0 || !shade_needs_split(a, b, c, fog_alpha, sv)) {
        oi.push_back(u16(ia));
        oi.push_back(u16(ib));
        oi.push_back(u16(ic));
        return;
    }
    const u32 iab = (u32)ov.size();
    ov.push_back(screen_midpoint(a, b, fog_alpha));
    ov.push_back(screen_midpoint(b, c, fog_alpha));
    ov.push_back(screen_midpoint(c, a, fog_alpha));
    const u32 ibc = iab + 1, ica = iab + 2;
    subdivide_for_shade_idx(ia, iab, ica, depth - 1, fog_alpha, sv, ov, oi);
    subdivide_for_shade_idx(iab, ib, ibc, depth - 1, fog_alpha, sv, ov, oi);
    subdivide_for_shade_idx(ica, ibc, ic, depth - 1, fog_alpha, sv, ov, oi);
    subdivide_for_shade_idx(iab, ibc, ica, depth - 1, fog_alpha, sv, ov, oi);
}

/* Replaces v[0..n) (a triangle list) by its subdivision when any triangle needs it; returns the
 * vertex buffer to draw and its count, and in idx/nidx the triangles' indices into it (in frame
 * memory; null when the list is drawn as it is). */
GVtx *ContextImplPica::linearizeShade(GVtx *v, u32 &n, const u16 *in, u32 nin, bool fog_alpha, u16 *&idx,
                                      u32 &nidx)
{
    idx = nullptr;
    nidx = 0;
    const u32 frame = pica_frame_number();
    if (m_shadeFrame != frame) {
        m_shadeFrame = frame;
        m_shadeTris = 0;
    }
    if (m_shadeTris >= k_shade_frame_budget)
        return v;
    const ShadeView sv{0.5f * m_vp[2], 0.5f * m_vp[3]};
    /* the triangles: v[0..n) in threes, or v[in[0..nin)] */
    const u32 nv3 = in ? nin : n;
    auto at = [&](u32 k) -> const GVtx & { return in ? v[in[k]] : v[k]; };
    bool any = false;
    for (u32 i = 0; i + 2 < nv3 && !any; i += 3)
        any = shade_needs_split(at(i), at(i + 1), at(i + 2), fog_alpha, sv);
    if (!any)
        return v;
    std::vector<GVtx> &ov = m_shadeScratch;
    std::vector<u16> &oi = m_shadeIdx;
    ov.clear();
    oi.clear();
    for (u32 i = 0; i + 2 < nv3; i += 3) {
        const u32 base = (u32)ov.size();
        ov.push_back(at(i));
        ov.push_back(at(i + 1));
        ov.push_back(at(i + 2));
        subdivide_for_shade_idx(base, base + 1, base + 2, k_shade_max_depth, fog_alpha, sv, ov, oi);
        if (ov.size() > 0xFFFF)
            break;
    }
    /* far more than any real draw (as a flat list): drawn as it is (a restarted pool could not
     * hold it, and v would then point into reused memory) */
    if (oi.size() * sizeof(GVtx) > 1024 * 1024 || ov.size() > 0xFFFF)
        return v;
    /* may restart the pool: v is not used below */
    pica_reserve(512, ov.size() * sizeof(GVtx) + oi.size() * 2 + 256);
    GVtx *nv = static_cast<GVtx *>(pica_frame_alloc(ov.size() * sizeof(GVtx), 16));
    u16 *ni = nv ? static_cast<u16 *>(pica_frame_alloc(oi.size() * 2, 16)) : nullptr;
    if (!nv || !ni)
        return v;
    memcpy(nv, ov.data(), ov.size() * sizeof(GVtx));
    memcpy(ni, oi.data(), oi.size() * 2);
    n = (u32)ov.size();
    idx = ni;
    nidx = (u32)oi.size();
    ctr64_gfx_stats[ST_SUBDIV] += nidx / 3;
    m_shadeTris += nidx / 3;
    return nv;
}

/* GL polygon offset for a decal draw: factor * max over its triangles of max(|dz/dx|, |dz/dy|)
 * in window space (depth 0..1 per pixel), plus the constant floor. */
float ContextImplPica::poly_offset_for_draw(const GVtx *v, const u16 *idx, u32 count, GPU_Primitive_t prim)
{
    const float hw = 0.5f * m_vp[2], hh = 0.5f * m_vp[3];
    auto win = [&](u32 i, float &x, float &y, float &z) {
        const GVtx &q = v[idx ? idx[i] : i];
        const float iw = q.w != 0.0f ? 1.0f / q.w : 0.0f;
        x = q.x * iw * hw;
        y = q.y * iw * hh;
        z = -q.z * iw; /* window depth with the depth map at scale -1 */
    };
    float slope = 0.0f;
    const u32 ntri = prim == GPU_TRIANGLES ? count / 3 : count >= 3 ? count - 2 : 0;
    for (u32 t = 0; t < ntri; t++) {
        u32 i0, i1, i2;
        if (prim == GPU_TRIANGLES) { i0 = t * 3; i1 = i0 + 1; i2 = i0 + 2; }
        else if (prim == GPU_TRIANGLE_FAN) { i0 = 0; i1 = t + 1; i2 = t + 2; }
        else { i0 = t; i1 = t + 1; i2 = t + 2; }
        float x0, y0, z0, x1, y1, z1, x2, y2, z2;
        win(i0, x0, y0, z0);
        win(i1, x1, y1, z1);
        win(i2, x2, y2, z2);
        const float ax = x1 - x0, ay = y1 - y0, az = z1 - z0;
        const float bx = x2 - x0, by = y2 - y0, bz = z2 - z0;
        const float det = ax * by - ay * bx;
        if (fabsf(det) < 1e-6f)
            continue;
        const float dzdx = fabsf((az * by - ay * bz) / det);
        const float dzdy = fabsf((ax * bz - az * bx) / det);
        const float m = dzdx > dzdy ? dzdx : dzdy;
        if (m > slope)
            slope = m;
    }
    if (slope > 0.01f) /* edge-on triangles: keep the offset bounded */
        slope = 0.01f;
    return k_poly_factor * slope + k_poly_offset;
}

void ContextImplPica::drawRects(const Context::DrawRectParameters &p)
{
    DrawTimer timer;
    CRUMB("gfx: drawRects");
    /* a fill or rectangle over the whole N64 frame buffer redraws it (frameskip.c) */
    if (p.vertices && p.verticesCount && m_kind == Kind::Combiner && m_drawFbo != 0 && !ctr64_frame_cleared) {
        if (Tex *c = tex(drawFbo().color)) {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (u32 i = 0; i < p.verticesCount; i++) {
                const RectVertex &s = p.vertices[i];
                x0 = s.x < x0 ? s.x : x0; x1 = s.x > x1 ? s.x : x1;
                y0 = s.y < y0 ? s.y : y0; y1 = s.y > y1 ? s.y : y1;
            }
            if (x0 <= 0.0f && y0 <= 0.0f && x1 >= c->w - 1 && y1 >= c->h - 8)
                ctr64_frame_cleared = true;
        }
    }
    if (!p.vertices || !p.verticesCount || m_kind == Kind::Noop || skipDraw())
        return;
    const bool special = m_kind != Kind::Combiner;
    {   /* the rows it reaches (materialize) */
        float top = 0.0f;
        for (u32 i = 0; i < p.verticesCount; i++) {
            const RectVertex &r = p.vertices[i];
            if (r.w <= 0.0f) {
                top = 1e9f;
                break;
            }
            const float n = special ? (r.y / r.w + 1.0f) * 0.5f : (r.y / r.w + 0.5f) / SCREEN_SIZE_DIM;
            top = std::max(top, (float)m_vp[1] + n * (float)m_vp[3]);
        }
        if (m_enScissor)
            top = std::min(top, (float)(m_sc[1] + m_sc[3]));
        m_drawTop = top < 2048.0f ? (u16)(top + 1.0f) : 0xFFFF;
    }
    const bool ready = applyState(!special);
    m_drawTop = 0xFFFF;
    if (!ready) {
        ctr64_gfx_stats[ST_DROP]++;
        return;
    }
    if (m_kind == Kind::Copy && copyByTransfer(p)) {
        ctr64_gfx_stats[ST_RECT]++;
        noteDrawnRows(nullptr, 0); /* rows unknown here: this generation's shadows go stale */
        return;
    }
    if (m_rtLog && p.verticesCount >= 4) { /* rtlog.txt: draws sampling a frame buffer that needs a shadow */
        const Tex *s = tex(m_unit[0]), *d = tex(drawFbo().color);
        if (s && ((s->rt && needs_shadow(*s)) || (d && !d->vram)))
            fprintf(m_rtLog, "  S kind %d src %u %ux%u/%ux%u dst %u %ux%u/%ux%u blend %d depth %d sc %d %d,%d,%d,%d vp %d,%d,%d,%d"
                             " v %.3f,%.3f %.3f,%.3f w %.3f st %.4f,%.4f %.4f,%.4f\n",
                    (int)m_kind, (unsigned)m_unit[0], s->w, s->h, s->tw, s->th, (unsigned)drawFbo().color, d ? d->w : 0,
                    d ? d->h : 0, d ? d->tw : 0, d ? d->th : 0, (int)m_enBlend, (int)m_enDepth, (int)m_enScissor,
                    (int)m_sc[0], (int)m_sc[1], (int)m_sc[2], (int)m_sc[3], m_vp[0], m_vp[1], m_vp[2], m_vp[3],
                    p.vertices[0].x, p.vertices[0].y, p.vertices[3].x, p.vertices[3].y, p.vertices[0].w,
                    p.vertices[0].s0, p.vertices[0].t0, p.vertices[3].s0, p.vertices[3].t0);
    }
    for (u32 u = 0; u < 2; u++) { /* the t range per unit, for shadow copies (bindUnit) */
        float lo = 1e30f, hi = -1e30f;
        for (u32 i = 0; i < p.verticesCount; i++) {
            const float tc = u ? p.vertices[i].t1 : p.vertices[i].t0;
            lo = tc < lo ? tc : lo;
            hi = tc > hi ? tc : hi;
        }
        m_rowT[u][0] = lo;
        m_rowT[u][1] = hi;
        lo = 1e30f, hi = -1e30f;
        for (u32 i = 0; i < p.verticesCount; i++) {
            const float sc = u ? p.vertices[i].s1 : p.vertices[i].s0;
            lo = sc < lo ? sc : lo;
            hi = sc > hi ? sc : hi;
        }
        m_rowS[u][0] = lo;
        m_rowS[u][1] = hi;
        m_rowHint[u] = true;
    }
    const bool combined = setupCombiner(true);
    m_rowHint[0] = m_rowHint[1] = false;
    if (!combined)
        return;
    ctr64_gfx_stats[ST_RECT]++;
    pica_reserve(4096, p.verticesCount * sizeof(GVtx) + 256);

    GVtx *v = static_cast<GVtx *>(pica_frame_alloc(p.verticesCount * sizeof(GVtx), 16));
    if (!v)
        return;

    /* Texture coordinates are normalized to the logical texture size; scale them into storage. */
    float ss0 = 1.0f, st0 = 1.0f, ss1 = 1.0f, st1 = 1.0f;
    if (Tex *t = tex(m_unit[0]))
        if (t->tw) { ss0 = (float)t->w / sample_w(*t); st0 = (float)t->h / sample_h(*t); }
    if (Tex *t = tex(m_unit[1]))
        if (t->tw) { ss1 = (float)t->w / sample_w(*t); st1 = (float)t->h / sample_h(*t); }
    /* N64 texture rectangles: coordinates come normalised by GLideN64's cache scale, which is
     * 1/pow2(width) with a wrap mask (MK64's title: 320-texel strips, mask 512), so texel u is at
     * u * scale; normalise by the storage size as for triangles (texture_xforms) */
    if (!special) {
        TextureCache &cache = textureCache();
        for (u32 u = 0; u < 2; u++) {
            const CachedTexture *ct = cache.current[u];
            Tex *t = tex(m_unit[u]);
            if (!ct || !t || !t->tw || ct->scaleS <= 0.0f || ct->scaleT <= 0.0f || t->w != ct->width)
                continue;
            if (ct->frameBufferTexture != CachedTexture::fbNone) { /* see texture_xforms */
                (u ? ss1 : ss0) = float(ct->width) / sample_w(*t);
                (u ? st1 : st0) = float(ct->height) / sample_h(*t);
                continue;
            }
            (u ? ss1 : ss0) = 1.0f / (ct->scaleS * sample_w(*t));
            (u ? st1 : st0) = 1.0f / (ct->scaleT * sample_h(*t));
        }
    }

    const bool native = config.frameBufferEmulation.nativeResFactor == 1 && config.video.multisampling == 0;
    const float offset = (config.generalEmulation.enableInaccurateTextureCoordinates == 0 && native) ? 0.5f : 0.0f;
    const u8 rc[4] = {to_u8(gDP.rectColor.r), to_u8(gDP.rectColor.g), to_u8(gDP.rectColor.b), to_u8(gDP.rectColor.a)};

    const ViewportZ vpz;
    for (u32 i = 0; i < p.verticesCount; i++) {
        const RectVertex &s = p.vertices[i];
        GVtx &o = v[i];
        float x = s.x, y = s.y, w = s.w;
        if (!special) {
            /* N64 pixel space through the 640x640 virtual viewport (VertexShaderEnd) */
            x = (x + offset * w - 0.5f * SCREEN_SIZE_DIM * w) / (0.5f * SCREEN_SIZE_DIM);
            y = (y + offset * w - 0.5f * SCREEN_SIZE_DIM * w) / (0.5f * SCREEN_SIZE_DIM);
        }
        o.x = x;
        o.y = y;
        o.z = vpz.set && !special ? -(s.z * vpz.a + w * vpz.b) : -0.5f * (s.z + w);
        o.w = w;
        memcpy(o.c, special ? (const u8[4]){255, 255, 255, 255} : rc, 4);
        /* + 0.0001: ShaderFragmentCorrectTexCoords */
        o.s0 = (s.s0 + 0.0001f) * ss0; o.t0 = (s.t0 + 0.0001f) * st0;
        o.s1 = (s.s1 + 0.0001f) * ss1; o.t1 = (s.t1 + 0.0001f) * st1;
    }
    clipRect(v, p.verticesCount);

    if (draw_log())
        log_draw("rect", m_program, (int)m_kind, v, p.verticesCount, m_vp, stateString());
    if (!special)
        noteCoverage(v, p.verticesCount);
    noteDrawnRows(v, p.verticesCount);
    turnVertices(v, p.verticesCount);
    pica_vertex_buffer(v);
    const u32 mode = u32(p.mode);
    pica_draw_arrays(mode == DM_TRIANGLES ? GPU_TRIANGLES : mode == DM_TRIANGLE_FAN ? GPU_TRIANGLE_FAN : GPU_TRIANGLE_STRIP,
                     0, p.verticesCount);
    gtDraw(special ? 2 : 1);
}

/* A rectangle reaching far past its target, cut down to the target (plus a margin). Mario
 * Tennis's scene-change fade is one white rectangle to about 1016x1784 window pixels with
 * anti-aliasing (2x): the PICA rasterizer on hardware drew only its top ~265 rows, the rest of
 * the screen kept unfaded (Azahar draws it all; at 1x, 508x892, hardware too). Only for an
 * axis-aligned rectangle of 4 corners with one w: its attributes are interpolated bilinearly
 * between the corners, exact for such a rectangle. */
void ContextImplPica::clipRect(GVtx *v, u32 n)
{
    if (n != 4)
        return;
    const Tex *d = tex(drawFbo().color);
    if (!d || m_vp[2] <= 0 || m_vp[3] <= 0)
        return;
    const float w = v[0].w;
    if (w <= 0.0f)
        return;
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (u32 i = 0; i < 4; i++) {
        if (v[i].w != w)
            return;
        x0 = std::min(x0, v[i].x); x1 = std::max(x1, v[i].x);
        y0 = std::min(y0, v[i].y); y1 = std::max(y1, v[i].y);
    }
    if (!(x1 > x0) || !(y1 > y0))
        return;
    /* the target plus 16 pixels around it, in clip space (x = ndc * w) */
    const float m = 16.0f;
    const float cx0 = ((-m - m_vp[0]) / m_vp[2] * 2.0f - 1.0f) * w;
    const float cx1 = (((float)(turned() ? d->w : d->tw) + m - m_vp[0]) / m_vp[2] * 2.0f - 1.0f) * w;
    const float cy0 = ((-m - m_vp[1]) / m_vp[3] * 2.0f - 1.0f) * w;
    const float cy1 = (((float)(turned() ? d->h : d->th) + m - m_vp[1]) / m_vp[3] * 2.0f - 1.0f) * w;
    if (x0 >= cx0 && x1 <= cx1 && y0 >= cy0 && y1 <= cy1)
        return;
    /* the corners: each at x0 or x1 and y0 or y1 */
    int at[2][2] = {{-1, -1}, {-1, -1}};
    for (u32 i = 0; i < 4; i++) {
        const int ix = v[i].x == x0 ? 0 : v[i].x == x1 ? 1 : -1;
        const int iy = v[i].y == y0 ? 0 : v[i].y == y1 ? 1 : -1;
        if (ix < 0 || iy < 0 || at[ix][iy] >= 0)
            return;
        at[ix][iy] = (int)i;
    }
    const float nx0 = std::max(x0, cx0), nx1 = std::min(x1, cx1);
    const float ny0 = std::max(y0, cy0), ny1 = std::min(y1, cy1);
    if (!(nx1 > nx0) || !(ny1 > ny0))
        return;
    GVtx c[2][2];
    for (int ix = 0; ix < 2; ix++)
        for (int iy = 0; iy < 2; iy++)
            c[ix][iy] = v[at[ix][iy]];
    auto lerp = [](float a, float b, float f) { return a + (b - a) * f; };
    for (int ix = 0; ix < 2; ix++)
        for (int iy = 0; iy < 2; iy++) {
            const float fx = ((ix ? nx1 : nx0) - x0) / (x1 - x0);
            const float fy = ((iy ? ny1 : ny0) - y0) / (y1 - y0);
            GVtx &o = v[at[ix][iy]];
            auto bil = [&](float GVtx::*f) {
                return lerp(lerp(c[0][0].*f, c[1][0].*f, fx), lerp(c[0][1].*f, c[1][1].*f, fx), fy);
            };
            o.x = ix ? nx1 : nx0;
            o.y = iy ? ny1 : ny0;
            o.z = bil(&GVtx::z);
            o.s0 = bil(&GVtx::s0); o.t0 = bil(&GVtx::t0);
            o.s1 = bil(&GVtx::s1); o.t1 = bil(&GVtx::t1);
        }
}

/* DEV ONLY (gputime.txt, pica.c): each draw submitted alone, tagged by kind, target memory
 * (V/F), depth test and blending, so the GPU time per kind of draw shows in stats */
void ContextImplPica::gtDraw(int kind)
{
    if (!pica_gputime)
        return;
    static char tags[3][2][2][2][16];
    static const char *const kinds[3] = {"tri", "rect", "special"};
    const Tex *c = tex(drawFbo().color);
    const int f = c && !c->vram, d = m_enDepth ? 1 : 0, b = m_enBlend ? 1 : 0;
    char *t = tags[kind][f][d][b];
    if (!t[0])
        snprintf(t, 16, "%s %s%s%s", kinds[kind], f ? "F" : "V", d ? " z" : "", b ? " b" : "");
    CRUMB(t);
    pica_flush();
}

/* A copy draw of a frame buffer at exactly half size into a tile-aligned rectangle - the window
 * copy and GLideN64's native-resolution copies with anti-aliasing - done by the transfer engine
 * (2x2 average = bilinear sampling at exactly half size, up to rounding) instead of a draw that
 * samples it: on hardware a texture stored at a non-power-of-two width is only sampled through a
 * shadow copy of the whole of it (Azahar 2026-11-10: 40-150 MB of shadow copies per 10 s, MK
 * 684 MB). Queued in order with the draws. Only for copies the sampling path would shadow;
 * anything else (blending, depth, scissor cutting into it, flips, unaligned) is drawn as before.
 * Dev switch noxfercopy.txt: off. */
bool ContextImplPica::copyByTransfer(const Context::DrawRectParameters &p)
{
    if (!m_xferCopy || p.verticesCount != 4 || m_enBlend || m_enDepth || turned())
        return false;
    CRUMB("gfx: copyByTransfer");
    Tex *s = tex(m_unit[0]);
    Tex *d = tex(drawFbo().color);
    if (!s || !d || s == d || !s->data || !d->data || s->depth || d->depth || s->fmt != d->fmt ||
        (d->fmt != GPU_RGBA8 && d->fmt != GPU_RGBA5551) || !needs_shadow(*s))
        return false;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, s0 = 1e9f, s1 = -1e9f, t0 = 1e9f, t1 = -1e9f;
    for (u32 i = 0; i < 4; i++) {
        const RectVertex &v = p.vertices[i];
        if (v.w < 0.9999f || v.w > 1.0001f)
            return false;
        x0 = std::min(x0, v.x); x1 = std::max(x1, v.x);
        y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
        s0 = std::min(s0, v.s0); s1 = std::max(s1, v.s0);
        t0 = std::min(t0, v.t0); t1 = std::max(t1, v.t0);
    }
    /* an axis-aligned rectangle, texture coordinates running the same way as the positions */
    for (u32 i = 0; i < 4; i++) {
        const RectVertex &v = p.vertices[i];
        const bool lx = v.x < 0.5f * (x0 + x1), ly = v.y < 0.5f * (y0 + y1);
        if ((v.x != x0 && v.x != x1) || (v.y != y0 && v.y != y1) || lx != (v.s0 < 0.5f * (s0 + s1)) ||
            ly != (v.t0 < 0.5f * (t0 + t1)))
            return false;
    }
    auto whole = [](float f, int &out) {
        out = (int)lroundf(f);
        return fabsf(f - (float)out) < 0.01f;
    };
    int dx0, dx1, dy0, dy1, sx0, sx1, sy0, sy1;
    if (!whole(m_vp[0] + (x0 + 1.0f) * 0.5f * m_vp[2], dx0) || !whole(m_vp[0] + (x1 + 1.0f) * 0.5f * m_vp[2], dx1) ||
        !whole(m_vp[1] + (y0 + 1.0f) * 0.5f * m_vp[3], dy0) || !whole(m_vp[1] + (y1 + 1.0f) * 0.5f * m_vp[3], dy1) ||
        !whole(s0 * s->w, sx0) || !whole(s1 * s->w, sx1) || !whole(t0 * s->h, sy0) || !whole(t1 * s->h, sy1))
        return false;
    const int w = dx1 - dx0, h = dy1 - dy0;
    /* whole storage rows of the source (the engine reads full rows), tile-aligned everywhere */
    if (w <= 0 || h <= 0 || sx0 != 0 || sx1 != s->tw || sx1 - sx0 != 2 * w || sy1 - sy0 != 2 * h || h % 8 || w % 8)
        return false;
    const int r0 = (int)s->th - sy1, q0 = (int)d->th - dy1;
    if (r0 < 0 || sy0 < 0 || r0 % 8 || dx0 < 0 || dy0 < 0 || dx1 > d->tw || q0 < 0 || q0 % 8 || dx0 % 8)
        return false;
    if (m_enScissor && (m_sc[0] > dx0 || m_sc[1] > dy0 || m_sc[0] + m_sc[2] < dx1 || m_sc[1] + m_sc[3] < dy1))
        return false;
    const u32 bpp = d->bpp, bytes = (u32)w * h * bpp;
    const bool direct = dx0 == 0 && w == d->tw;
    for (int k = 0; k < (direct ? 1 : 2); k++)
        if (m_xferTempBytes[k] < bytes) {
            if (m_xferTemp[k]) { /* rare (a larger copy): the old one may be in a queued transfer */
                pica_flush();
                pica_wait_idle();
                freeNow(m_xferTemp[k], false, m_xferTempBytes[k]);
            }
            m_xferTemp[k] = linear_alloc_safe(bytes, 0x80);
            if (!m_xferTemp[k])
                m_xferTemp[k] = ctr64_xlinear_alloc(bytes, 0x80);
            m_xferTempBytes[k] = m_xferTemp[k] ? bytes : 0;
            if (!m_xferTemp[k])
                return false;
            pica_cache_flush(m_xferTemp[k], bytes); /* no dirty CPU lines of its last owner left */
            ctr64_gfx_mem[0] += bytes;
        }
    const u8 *src = static_cast<const u8 *>(s->data) + (size_t)r0 * s->tw * bpp;
    u8 *dst = static_cast<u8 *>(d->data) + (size_t)q0 * d->tw * bpp + (size_t)(dx0 / 8) * 64 * bpp;
    pica_downscale2(src, s->tw, (u16)(2 * h), m_xferTemp[0], bpp);
    if (direct) {
        pica_tile(m_xferTemp[0], (u16)w, (u16)h, dst, bpp);
    } else {
        pica_tile(m_xferTemp[0], (u16)w, (u16)h, m_xferTemp[1], bpp);
        pica_copy_gap(dst, m_xferTemp[1], bytes, (u32)w * 8 * bpp / 16, (u32)(d->tw - w) * 8 * bpp / 16);
    }
    s->used_frame = pica_frame_number();
    ctr64_gfx_shadow[2]++;
    return true;
}

/* ---- presentation and readback -------------------------------------------------------------- */

/* Development aid: sdmc:/3ds/ctr64/dumpframes.txt lists swap numbers; at each, the window
 * buffer is written to sdmc:/3ds/ctr64/frame_<n>.tga (bottom-up rows, like the texture). */
static bool dump_wanted(u32 swap)
{
    static std::vector<u32> wanted;
    static bool loaded;
    if (!loaded) {
        loaded = true;
        if (FILE *f = fopen("sdmc:/3ds/ctr64/dumpframes.txt", "r")) {
            unsigned n;
            while (fscanf(f, "%u", &n) == 1)
                wanted.push_back(n);
            fclose(f);
        }
    }
    for (u32 n : wanted)
        if (n == swap)
            return true;
    return false;
}

static void write_tga(const Tex &t0, const char *path, bool is_turned = false)
{
    if (!t0.data || t0.depth)
        return;
    /* a VRAM target goes through a GPU copy into linear memory first: Azahar does not keep its
     * emulated VRAM up to date for CPU reads (frames dumped from VRAM came out all zero) */
    Tex t = t0;
    void *lin = nullptr;
    if (t0.vram) {
        const u32 bytes = (u32)t0.tw * t0.th * t0.bpp;
        CRUMB("dump: linearAlloc");
        lin = linearAlloc(bytes);
        if (!lin) /* the linear heap can be full (Goemon with nonpot.txt: 256 KB left): the reserve */
            lin = ctr64_xlinear_alloc(bytes, 0x80);
        if (!lin)
            return;
        pica_copy_from_vram(lin, t0.data, bytes);
        pica_flush();
        pica_wait_idle();
        GSPGPU_InvalidateDataCache(lin, bytes);
        t.data = lin;
        t.vram = false;
    }
    /* built here, written by the profiler's writer thread: SD writes on this thread stopped for
     * good mid-file after a state load on HW (prof_write_file) */
    CRUMB("dump: build");
    const unsigned size = 18 + (unsigned)t.w * t.h * 4;
    u8 *buf = (u8 *)malloc(size);
    if (!buf) {
        if (lin)
            if (!ctr64_xlinear_free(lin)) linearFree(lin);
        return;
    }
    const u8 hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, (u8)t.w, (u8)(t.w >> 8), (u8)t.h, (u8)(t.h >> 8), 32, 8};
    memcpy(buf, hdr, sizeof(hdr));
    /* TGA rows are bottom-up like GL rows */
    u8 *d = buf + 18;
    for (u32 y = 0; y < t.h; y++)
        for (u32 x = 0; x < t.w; x++, d += 4) {
            u32 v = load_texel(t, is_turned ? gl_index(y, t.th - 1 - x, t.tw, t.th) : gl_index(x, y, t.tw, t.th));
            d[0] = v >> 8; d[1] = v >> 16; d[2] = v >> 24; d[3] = v; /* BGRA */
        }
    CRUMB("dump: hand over");
    if (!prof_write_file(path, buf, size))
        free(buf);
    CRUMB("dump: flush");
    prof_files_flush(3000);
    CRUMB("dump: done");
    if (lin)
        if (!ctr64_xlinear_free(lin)) linearFree(lin);
}

void ContextImplPica::present()
{
    struct PresentTimer {
        unsigned long long t0 = svcGetSystemTick();
        ~PresentTimer() { ctr64_gfx_ticks[2] += svcGetSystemTick() - t0; }
    } timer;
    CRUMB("gfx: present");
    m_cover = 0;
    const bool stale = m_showStale, same = m_showSame && !stale;
    const u32 src = m_winSrc, copies = m_winCopies;
    m_showStale = m_showSame = false;
    m_winSrc = m_winCopies = 0;
    if (m_winClear && !stale && !same)
        clearTarget(m_window, m_winClearColor);
    m_winClear = false;
    pica_lcd_gamma((*REG.VI_STATUS & 0x08) != 0); /* VI_STATUS_GAMMA_ENABLED */
    if (!ctr64_frameskip_present(stale) || same)
        return; /* the window holds a buffer with dropped draws or the image on screen: it stays */
    m_shown = copies == 1 ? src : 0;
    if (Tex *t = tex(src))
        t->drawn = false;
    if (m_rtLog)
        fprintf(m_rtLog, "P %u:", (unsigned)pica_frame_number());
    u32 rt_pot = 0;
    for (auto &kv : m_tex) {
        Tex &t = kv.second;
        if (m_rtLog && t.rt && t.data && (t.ndraw || t.draw_frame + 1 >= pica_frame_number()))
            fprintf(m_rtLog, " %u%s %ux%u/%ux%u %s n%u s%u", (unsigned)kv.first, t.depth ? "d" : "c", t.w, t.h, t.tw, t.th,
                    t.vram ? "V" : "F", (unsigned)t.ndraw, (unsigned)t.nshadow);
        if (t.rt && t.data && t.draw_frame + 8 >= pica_frame_number())
            rt_pot += (u32)pot(t.tw) * pot(t.th) * (t.depth ? 4 : t.bpp);
        t.nshadow = 0;
        if (t.ndraw)
            t.ndraw_last = t.ndraw;
        t.ndraw = 0;
    }
    if (m_rtLog && pica_frame_number() % 64 == 0) /* the VRAM's map */
        for (auto &kv : m_tex)
            if (kv.second.vram && kv.second.data)
                fprintf(m_rtLog, "\n  V %u%s %ux%u at %06lx size %lu rt %d", (unsigned)kv.first, kv.second.depth ? "d" : "c", kv.second.tw,
                        kv.second.th, (unsigned long)((u32)kv.second.data & 0xFFFFFF), (unsigned long)storageBytes(kv.second),
                        (int)kv.second.rt);
    if (m_rtLog) {
        fprintf(m_rtLog, " | moves %u/%u KB, shadow copies %u (%u KB), transfer copies %u\n", ctr64_gfx_vram_moves[0],
                ctr64_gfx_vram_moves[1], ctr64_gfx_shadow[0], ctr64_gfx_shadow[1], ctr64_gfx_shadow[2]);
        fflush(m_rtLog);
    }
    g_rt_pot_bytes = rt_pot;
    if (!g_fb16 && !g_nofb16 && ++g_fb16_presents >= 64) { /* VRAM too small for the game's targets? */
        g_fb16_runs = g_fb16_moved > k_fb16_kb ? g_fb16_runs + 1 : 0;
        g_fb16 = g_fb16_runs >= 2;
        g_fb16_moved = 0;
        g_fb16_presents = 0;
    }
    ctr64_gfx_stats[ST_SWAP]++;
    ensureFrame();
    Tex *c = tex(TEX_WINDOW_COLOR);
    if (m_winRot) {
        video_draw_borders(&m_window.rt); /* over the black bars of a 4:3 picture */
        video_present_target(&m_window.rt);
        return;
    }
    pica_texture_flush_cache();
    video_draw_fullscreen(c->data, c->tw, c->th, GPU_RGBA8, c->w, c->h, false, true);
    video_draw_borders(video_screen());
    video_present();
}

/* End of an N64 VI (after GLideN64's VI_UpdateScreen, which presents only when there is a new
 * image). Frames are counted here, per VI, so frame dumps and input scripts line up with the
 * Windows reference build whether or not this VI presented. */
void ContextImplPica::endVI()
{
    const u32 vi = ++ctr64_video_frames;
    g_swap_count = vi + 1;
    if (m_zPassMode && ++m_zPassAge > 4) /* the second pass comes in the same game frame */
        m_zPassMode = 0;
    Tex *c = tex(TEX_WINDOW_COLOR);
    if (dump_wanted(vi)) {
        ensureFrame();
        pica_flush();
        pica_wait_idle();
        char path[64];
        snprintf(path, sizeof(path), "sdmc:/3ds/ctr64/frame_%u.tga", (unsigned)vi);
        write_tga(*c, path, m_winRot);
        { /* the top screen buffer transferred last, as the LCD shows it (BGR8, 240x400 columns):
           * frame_<n>_top.raw; tools/remote ctr64remote.py lcd_png converts it */
            unsigned bytes = 0;
            if (const void *top = pica_top_latest(&bytes)) {
                if (u8 *b = static_cast<u8 *>(malloc(bytes))) {
                    memcpy(b, top, bytes);
                    snprintf(path, sizeof(path), "sdmc:/3ds/ctr64/frame_%u_top.raw", (unsigned)vi);
                    if (!prof_write_file(path, b, bytes))
                        free(b);
                    prof_files_flush(3000);
                }
            }
        }
        static int with_fbos = -1; /* sdmc:/3ds/ctr64/dumpfbo.txt: also dump every FBO */
        if (with_fbos < 0) {
            FILE *f = fopen("sdmc:/3ds/ctr64/dumpfbo.txt", "r");
            with_fbos = f != nullptr;
            if (f)
                fclose(f);
        }
        for (auto &kv : m_fbo)
            if (Tex *t = with_fbos ? tex(kv.second.color) : nullptr) {
                snprintf(path, sizeof(path), "sdmc:/3ds/ctr64/frame_%u_fb%u_%ux%u.tga", (unsigned)vi,
                         (unsigned)kv.first, (unsigned)t->w, (unsigned)t->h);
                write_tga(*t, path);
            }
    }

    /* Detailed profiler: a screenshot of the top screen every SHOT_EVERY VIs, next to the
     * profile (prof.c writes it). The GPU untiles the window into linear memory at one VI and
     * the copy is handed over at the next, so no VI waits for the GPU. */
    if (ctr64_profiler >= 2) {
        enum { SHOT_EVERY = 180 };
        static u8 *lin;
        static u64 fence;
        static u32 lin_bytes;
        if (lin) {
            pica_wait_fence(fence); /* queued a VI ago: done by now */
            pica_cache_invalidate(lin, lin_bytes);
            /* tiled row 0 is the top (gl_index): the image's top row is tiled row th - h */
            if (m_winRot) { /* stored turned: column x of the picture is row x, bottom first */
                static std::vector<u32> up;
                up.resize(WINDOW_W * WINDOW_H);
                const u32 *l = reinterpret_cast<const u32 *>(lin);
                for (u32 y = 0; y < WINDOW_H; y++)
                    for (u32 x = 0; x < WINDOW_W; x++)
                        up[y * WINDOW_W + x] = l[x * WINDOW_H + (WINDOW_H - 1 - y)];
                prof_shot(vi - 1, reinterpret_cast<u8 *>(up.data()), WINDOW_W);
            } else
                prof_shot(vi - 1, lin + (size_t)(c->th - c->h) * c->tw * 4, c->tw);
            linearFree(lin);
            lin = nullptr;
        }
        if (vi % SHOT_EVERY == 0 && c->data && c->w == 400 && c->h == 240 && prof_shot_ready()) {
            lin_bytes = (u32)c->tw * c->th * 4;
            lin = (u8 *)linearAlloc(lin_bytes);
            if (lin) {
                ensureFrame();
                pica_untile_async(c->data, c->tw, c->th, lin, 4);
                fence = pica_fence();
            }
        }
    }

    /* Development aid: sdmc:/3ds/ctr64/freezeframe.txt holds a VI number at which the
     * emulation stops and that frame stays on screen (for emulator screenshots). */
    static int freeze_at = -2;
    if (freeze_at == -2) {
        freeze_at = -1;
        if (FILE *f = fopen("sdmc:/3ds/ctr64/freezeframe.txt", "r")) {
            if (fscanf(f, "%d", &freeze_at) != 1)
                freeze_at = -1;
            fclose(f);
        }
    }
    if ((int)vi == freeze_at)
        while (aptMainLoop()) {
            ensureFrame();
            if (m_winRot) {
                video_present_target(&m_window.rt);
                continue;
            }
            video_draw_fullscreen(c->data, c->tw, c->th, GPU_RGBA8, c->w, c->h, false, true);
            video_present();
        }
}


template <bool PmBlue>
static inline u16 to_rgba5551(u32 v) /* 0xRRGGBBAA */
{
    if (PmBlue) {
        const u32 b = (v >> 8) & 0xFF;
        if (b > 0x00 && b <= 0xFB)
            v += 0x400;
    }
    return (u16)(((v >> 16) & 0xF800) | ((v >> 13) & 0x07C0) | ((v >> 10) & 0x3E) | ((v & 0xFF) != 0));
}

/* RGBA8 render targets: the GPU untiles the 8-row strips holding GL rows y0..y0+h-1 (storage
 * rows th-y0-h..th-1-y0) into linear memory; false if this read can't take that path */
static bool plan_read(const Tex *t, s32 x0, s32 y0, u32 w, u32 h, ReadJob &j)
{
    if (!t || !t->data || t->depth || (t->fmt != GPU_RGBA8 && t->fmt != GPU_RGBA5551) || x0 < 0 || y0 < 0 || (u32)x0 >= t->tw ||
        (u32)y0 >= t->th)
        return false;
    j.x0 = x0; j.y0 = y0; j.w = w; j.h = h;
    j.tw = t->tw; j.th = t->th;
    j.bpp = t->bpp;
    j.hh = std::min<u32>(h, t->th - y0);
    j.ww = std::min<u32>(w, t->tw - x0);
    j.s0 = (t->th - y0 - j.hh) & ~7u;
    j.s1 = (t->th - y0 + 7) & ~7u;
    j.rgba16 = g_ctr64ReadRgba16;
    j.pm_blue = g_ctr64ReadPmBlue;
    return true;
}

static size_t read_bytes(const ReadJob &j)
{
    return (size_t)j.tw * (j.s1 - j.s0) * j.bpp;
}

static void queue_untile(const Tex *t, const ReadJob &j, u32 *lin)
{
    pica_untile_async(static_cast<const u8 *>(t->data) + (size_t)j.s0 * j.tw * j.bpp, j.tw, j.s1 - j.s0, lin, j.bpp);
}

/* The untiled strips in lin to GLideN64's layout (w*h, RGBA5551 or R,G,B,A bytes, rows from y0) */
static const u8 *convert_read(const u32 *lin, const ReadJob &j, std::vector<u8> &out)
{
    CTR64_PROF(21);
    const u32 w = j.w;
    if (j.bpp == 2) { /* 16-bit frame buffer: the N64's own layout (Paper Mario's blue + 4 of 255
                       * moves a 5-bit value up by less than one step: 0x1E stays, nothing to do) */
        const u16 *l = reinterpret_cast<const u16 *>(lin);
        out.resize((size_t)w * j.h * (j.rgba16 ? 2 : 4));
        for (u32 y = 0; y < j.hh; y++) {
            const u16 *src = l + (size_t)(j.th - 1 - j.y0 - y - j.s0) * j.tw + j.x0;
            if (j.rgba16) {
                u16 *dst = reinterpret_cast<u16 *>(&out[(size_t)y * w * 2]);
                memcpy(dst, src, j.ww * 2);
                std::fill(dst + j.ww, dst + w, (u16)0);
            } else {
                u32 *dst = reinterpret_cast<u32 *>(&out[(size_t)y * w * 4]);
                for (u32 x = 0; x < j.ww; x++) {
                    const u32 v = src[x];
                    dst[x] = expand5(v >> 11) | expand5((v >> 6) & 31) << 8 | expand5((v >> 1) & 31) << 16 |
                             (0u - (v & 1)) << 24;
                }
                std::fill(dst + j.ww, dst + w, 0u);
            }
        }
        std::fill(out.begin() + (size_t)j.hh * w * (j.rgba16 ? 2 : 4), out.end(), 0);
        return out.data();
    }
    if (j.rgba16) {
        out.resize((size_t)w * j.h * 2);
        for (u32 y = 0; y < j.hh; y++) {
            const u32 *src = lin + (size_t)(j.th - 1 - j.y0 - y - j.s0) * j.tw + j.x0;
            u16 *dst = reinterpret_cast<u16 *>(&out[(size_t)y * w * 2]);
            if (j.pm_blue)
                for (u32 x = 0; x < j.ww; x++)
                    dst[x] = to_rgba5551<true>(src[x]);
            else {
                u32 x = 0;
                if ((reinterpret_cast<uintptr_t>(dst) & 3) == 0) /* two pixels per word store */
                    for (; x + 2 <= j.ww; x += 2)
                        *reinterpret_cast<u32 *>(dst + x) =
                            to_rgba5551<false>(src[x]) | ((u32)to_rgba5551<false>(src[x + 1]) << 16);
                for (; x < j.ww; x++)
                    dst[x] = to_rgba5551<false>(src[x]);
            }
            std::fill(dst + j.ww, dst + w, (u16)0);
        }
        std::fill(out.begin() + (size_t)j.hh * w * 2, out.end(), 0);
        return out.data();
    }
    out.resize((size_t)w * j.h * 4);
    for (u32 y = 0; y < j.hh; y++) {
        const u32 *src = lin + (size_t)(j.th - 1 - j.y0 - y - j.s0) * j.tw + j.x0;
        u32 *dst = reinterpret_cast<u32 *>(&out[(size_t)y * w * 4]);
        for (u32 x = 0; x < j.ww; x++)
            dst[x] = __builtin_bswap32(src[x]); /* 0xRRGGBBAA -> bytes R, G, B, A */
        std::fill(dst + j.ww, dst + w, 0u);
    }
    std::fill(out.begin() + (size_t)j.hh * w * 4, out.end(), 0);
    return out.data();
}

bool AsyncReadback::reserve(size_t need)
{
    if (need <= bytes)
        return true;
    finish();
    if (lin)
        linearFree(lin);
    lin = static_cast<u32 *>(linear_alloc_safe((u32)need, 128));
    bytes = lin ? need : 0;
    if (lin) /* no dirty CPU lines of its last owner left to overwrite what the GPU writes */
        pica_cache_flush(lin, (u32)need);
    return lin != nullptr;
}

void AsyncReadback::finish()
{
    if (pending) {
        CRUMB("gfx: readback finish");
        pica_wait_fence(fence);
        pica_cache_invalidate(lin, (u32)read_bytes(job));
        pending = false;
    }
}

AsyncReadback::~AsyncReadback()
{
    finish(); /* the GPU must not write into freed memory */
    if (lin)
        linearFree(lin);
}

/* copyToRDRAM "double buffer": return the image this frame buffer held at its previous copy
 * (as GLideN64's PBO readers do) and queue this one, so the copy never waits for the GPU to
 * finish drawing. nullptr when there is no matching earlier read (GLideN64 then skips the copy). */
const u8 *ContextImplPica::readTextureAsync(CachedTexture *ct, s32 x0, s32 y0, u32 w, u32 h, AsyncReadback &a,
                                            std::vector<u8> &out)
{
    CTR64_PROF(19);
    Tex *t = ct ? tex(u32(ct->name)) : nullptr;
    if (t && t->th_full)
        growStorage(u32(ct->name)); /* short storage: all of it is wanted */
    healForRead(t);
    ReadJob j;
    if (!plan_read(t, x0, y0, w, h, j)) {
        a.finish();
        a.valid = false;
        return readTexture(ct, x0, y0, w, h, out);
    }
    ctr64_fb_copies++;
    ctr64_gfx_fb_reads[t->vram ? 0 : 1]++;
    const u8 *result = nullptr;
    if (a.pending || a.valid) {
        a.finish();
        const ReadJob &p = a.job;
        if (a.valid && p.x0 == j.x0 && p.y0 == j.y0 && p.w == j.w && p.h == j.h && p.rgba16 == j.rgba16 && p.bpp == j.bpp)
            result = convert_read(a.lin, p, out);
    }
    if (!a.reserve(read_bytes(j))) {
        a.valid = false;
        return result;
    }
    {
        CTR64_PROF(20);
        queue_untile(t, j, a.lin);
    }
    a.fence = pica_fence();
    a.job = j;
    a.pending = true;
    a.valid = true;
    return result;
}

const u8 *ContextImplPica::readTexture(CachedTexture *ct, s32 x0, s32 y0, u32 w, u32 h, std::vector<u8> &out)
{
    CTR64_PROF(19);
    CRUMB("gfx: readTexture");
    ctr64_fb_copies++;
    Tex *t = ct ? tex(u32(ct->name)) : nullptr;
    if (t && t->th_full)
        growStorage(u32(ct->name)); /* short storage: all of it is wanted */
    healForRead(t);
    const bool rgba16 = g_ctr64ReadRgba16;
    const u32 bpp = rgba16 ? 2 : 4;
    out.resize((size_t)w * h * bpp); /* zeroed below only where nothing is read */
    if (!t || !t->data || t->depth) {
        std::fill(out.begin(), out.end(), 0);
        return out.data();
    }
    ReadJob j;
    if (plan_read(t, x0, y0, w, h, j)) {
        /* one linear buffer for all synchronous reads (never destroyed: no GPU wait at exit) */
        static AsyncReadback &s_sync = *new AsyncReadback;
        if (s_sync.reserve(read_bytes(j))) {
            {
                CTR64_PROF(20);
                queue_untile(t, j, s_sync.lin);
                s_sync.fence = pica_fence();
                s_sync.job = j;
                s_sync.pending = true;
                s_sync.finish();
            }
            return convert_read(s_sync.lin, j, out);
        }
    }
    pica_flush();
    pica_wait_idle();
    std::fill(out.begin(), out.end(), 0);
    for (u32 y = 0; y < h; y++) {
        u32 ty = y0 + y;
        if (ty >= t->th)
            break;
        for (u32 x = 0; x < w; x++) {
            u32 tx = x0 + x;
            if (tx >= t->tw)
                break;
            u32 v = load_texel(*t, gl_index(tx, ty, t->tw, t->th));
            if (rgba16) {
                reinterpret_cast<u16 *>(out.data())[(size_t)y * w + x] =
                    g_ctr64ReadPmBlue ? to_rgba5551<true>(v) : to_rgba5551<false>(v);
                continue;
            }
            u8 *d = &out[((size_t)y * w + x) * 4];
            d[0] = v >> 24; d[1] = v >> 16; d[2] = v >> 8; d[3] = v;
        }
    }
    return out.data();
}

/* The depth buffer of a frame buffer into RDRAM as N64 z (GLideN64 copyDepthToRDRAM; Donkey Kong
 * 64 reads it to see whether Kong is hidden and moves its camera if so). Never waits for the frame
 * just drawn: the GPU copies it, the copy made a frame earlier is converted now. One sample per
 * 4x4 N64 pixels; with anti-aliasing (2x) the GPU copies one quarter of each tile only. */
struct DepthJob {
    u32 *lin = nullptr;
    size_t bytes = 0;
    u64 fence = 0;
    bool valid = false;
    u32 key = 0, w = 0, h = 0, sc = 0, cols = 0, s0 = 0, th = 0;
};
static DepthJob g_depth_job;

static inline u32 n64_z(u32 v) /* 24 bit depth -> the N64 16 bit format (GLideN64 DepthBufferList LUT) */
{
    const u32 i = (v & 0xFFFFFF) >> 6;
    const u32 e = (u32)__builtin_clz(~(i << 14) | 0x1000000); /* leading ones of the 18 bits, at most 7 */
    const u32 m = (i >> (6 - (e > 6 ? 6 : e))) & 0x7FF;
    return (e << 11 | m) << 2;
}

bool ContextImplPica::depthToRdram(u32 fbo, u32 w, u32 h, u32 sc, u32 key, u16 *dst)
{
    CTR64_PROF(19);
    DepthJob &j = g_depth_job;
    bool wrote = false;
    if (j.valid && j.key == key && j.w == w && j.h == h) {
        pica_wait_fence(j.fence);
        pica_cache_invalidate(j.lin, (u32)j.bytes);
        /* N64 row y = GL row y * sc = storage row th - 1 - y * sc */
        for (u32 y = 0; y < h; y += 4) {
            const u32 row = j.th - 1 - (y * j.sc + 2 * j.sc - 1), tr = row / 8 - j.s0 / 8;
            u32 line[640 / 2];
            for (u32 x = 0; x < w; x += 4) {
                const u32 px = x * j.sc + 3, tile = tr * j.cols + px / 8;
                const u32 q = ((row & 7) >> 2) * 2 + ((px & 7) >> 2);
                const u32 z = n64_z(j.lin[j.sc == 1 ? tile * 64 + q * 16 + 12 : tile * 16]);
                line[x / 2] = line[x / 2 + 1] = z << 16 | z;
            }
            for (u32 k = 0; k < 4 && y + k < h; k++)
                memcpy(dst + (size_t)(y + k) * w, line, w * 2);
        }
        wrote = true;
    }
    j.valid = false;
    auto it = m_fbo.find(fbo);
    if (it == m_fbo.end())
        return wrote;
    const Fbo &f = it->second;
    Tex *c = tex(f.color), *d = tex(f.depth);
    if (!c || !d || !c->data || !d->data || !d->depth || f.rt.depth != d->data || (sc != 1 && sc != 2) ||
        w * sc > f.rt.width || h * sc > f.rt.height || w > 640 || (w & 3))
        return wrote;
    const u32 tw = f.rt.width, th = f.rt.height, s0 = (th - h * sc) & ~7u;
    const u32 tiles = (tw / 8) * ((th - s0) / 8), bytes = tiles * (sc == 1 ? 256 : 64);
    if (j.bytes < bytes) {
        if (j.lin) {
            pica_wait_fence(j.fence); /* the GPU must not write into freed memory */
            linearFree(j.lin);
        }
        j.lin = static_cast<u32 *>(linear_alloc_safe(bytes, 128));
        j.bytes = j.lin ? bytes : 0;
        if (!j.lin)
            return wrote;
        pica_cache_flush(j.lin, bytes);
    }
    const u8 *src = static_cast<const u8 *>(d->data) + (size_t)s0 * tw * 4;
    if (sc == 1)
        pica_copy_rect(j.lin, src, bytes, bytes / 16, 0, 0);
    else /* the last 4x4 of each tile: its first texel is the tile's (4, 4) */
        pica_copy_rect(j.lin, src + 192, bytes, 4, 12, 0);
    j.fence = pica_fence();
    j.key = key; j.w = w; j.h = h; j.sc = sc;
    j.cols = tw / 8; j.s0 = s0; j.th = th;
    j.valid = true;
    return wrote;
}

std::vector<u8> g_readback; /* shared by all PicaColorBufferReaders */

const u8 *PicaColorBufferReader::readPixels(s32 x0, s32 y0, u32 width, u32 height, u32, bool sync)
{
    if (!g_ctx)
        return nullptr;
    if (!sync)
        return g_ctx->readTextureAsync(m_pTexture, x0, y0, width, height, m_async, g_readback);
    m_async.finish();
    m_async.valid = false;
    return g_ctx->readTexture(m_pTexture, x0, y0, width, height, g_readback);
}

const u8 *PicaColorBufferReader::_readPixels(const ReadColorBufferParams &params, u32 &heightOffset, u32 &stride)
{
    heightOffset = 0;
    stride = params.width;
    return g_ctx ? g_ctx->readTexture(m_pTexture, params.x0, params.y0, params.width, params.height, g_readback)
                 : nullptr;
}

} // namespace

namespace pica {

graphics::ContextImpl *createContextImpl()
{
    return new ContextImplPica;
}

void presentWindow()
{
    if (g_ctx)
        g_ctx->present();
}

} // namespace pica

/* DepthBufferToRDRAM (GLideN64): see ContextImplPica::depthToRdram */
extern "C" bool ctr64_depth_to_rdram(unsigned fbo, unsigned w, unsigned h, unsigned scale, unsigned address, unsigned short *dst)
{
    return g_ctx && g_ctx->depthToRdram(fbo, w, h, scale, address, dst);
}

/* the frame number GLideN64 code ages things by (DepthBuffer.cpp evictOld) */
extern "C" unsigned ctr64_frame_now(void)
{
    return pica_frame_number();
}

extern "C" void ctr64_gfx_end_vi(void)
{
    if (g_ctx)
        g_ctx->endVI();
}

/* GLideN64 dropped triangles of a skipped frame before reaching the backend (GraphicsDrawer) */
extern "C" void ctr64_gfx_draw_skipped(void)
{
    if (g_ctx)
        g_ctx->skipDraw();
}

namespace pica {

} // namespace pica
