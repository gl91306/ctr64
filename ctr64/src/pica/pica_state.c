#include <stdio.h>
#include "pica_internal.h"

#include <string.h>

/* Shadow of the register state this layer owns. A setter only emits commands when the
 * value differs from what the GPU already has; pica_state_reset() invalidates everything. */
static struct {
    PicaTarget target; /* by value: callers rebuild targets in place and reuse their memory */
    bool target_valid;
    bool drawn; /* draws issued since the last framebuffer flush */

    u32 viewport[5];
    u32 scissor[3];
    bool viewport_valid, scissor_valid;

    shaderProgram_s *program;

    u32 attr_format[2];
    u64 attr_perm;
    int attr_count;
    u16 stride;
    u32 vbo_offset;
    bool layout_dirty, buffer_dirty;
    bool buffers_set; /* buffers 1-11 written (zero) since the reset: only buffer 0 changes */

    u32 tev[6][5];
    bool tev_valid[6];
    u32 tev_buffer[2];
    bool tev_buffer_valid;

    u32 tex[3][5];
    u32 tex_type[3];
    u32 tex_units; /* enabled-unit bits */
    bool tex_valid[3];

    u32 access[4]; /* framebuffer color read/write, depth read/write (0x112-0x115) */
    u32 depth_color, depth_scale, depth_offset;
    u32 stencil_test, stencil_op; /* 0: off (reset value) */
    u32 cull, alpha_test, blend_func, blend_color;
    bool effect_valid;

    /* projective draws (pica_projective): requested for the next draw; what the shader's bool
     * uniform and texture unit 0's mode are set to */
    bool proj_req, proj_bool, proj_tex;
} S;

int pica_proj_bool_bit = -1; /* the vertex shader's "proj" bool uniform (pica_projective) */

/* ---- reset / framebuffer ------------------------------------------------------------------ */

static void emit_tex_config(bool clear_cache)
{
    /* Bit 12 must stay set (as citro3d does); bit 13 makes unit 2 read texcoord1 (the vertex
     * shader outputs no texcoord2); bit 16 clears the texture cache. */
    GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0xB, S.tex_units | BIT(12) | BIT(13));
    if (clear_cache)
        GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0x4, BIT(16));
}

void pica_state_reset(void)
{
    memset(&S, 0, sizeof(S));

    /* Fixed baseline that the rest of the layer assumes. */
    GPUCMD_AddWrite(GPUREG_LIGHTING_ENABLE0, 0);
    GPUCMD_AddWrite(GPUREG_LIGHTING_ENABLE1, 1);
    GPUCMD_AddWrite(GPUREG_STENCIL_TEST, 0);
    GPUCMD_AddWrite(GPUREG_STENCIL_OP, 0);
    GPUCMD_AddWrite(GPUREG_LOGIC_OP, GPU_LOGICOP_COPY);
    GPUCMD_AddMaskedWrite(GPUREG_COLOR_OPERATION, 7, 0xE40100); /* GL fragment mode + blending */
    GPUCMD_AddMaskedWrite(GPUREG_EARLYDEPTH_TEST1, 1, 0);
    GPUCMD_AddWrite(GPUREG_EARLYDEPTH_TEST2, 0);
    GPUCMD_AddWrite(GPUREG_TEXUNIT0_SHADOW, 1);
    emit_tex_config(true);

    PicaTev tev;
    pica_tev_default(&tev);
    for (int i = 0; i < 6; i++)
        pica_tev(i, &tev);
    pica_tev_buffer(0, 0, 0xFFFFFFFF);

    pica_depth_map(-1.0f, 0.0f);
    pica_depth_test(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    pica_cull(GPU_CULL_NONE);
    pica_alpha_test(false, GPU_ALWAYS, 0);
    pica_blend_off();
    pica_blend_color(0);
    pica_scissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    S.effect_valid = true; /* every effect register now holds its shadowed value */
}

bool pica_early_clear;
void pica_state_flush_framebuffer(void)
{
    if (!S.drawn)
        return;
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    if (pica_early_clear) /* early depth is never used; the clear takes ~4 us of GPU time */
        GPUCMD_AddWrite(GPUREG_EARLYDEPTH_CLEAR, 1);
    S.drawn = false;
}

void pica_bind_target(const PicaTarget *t)
{
    PICA_CHECK(!t->color || !t->width || !t->height || t->width > 1024 || t->height > 1024 || (t->width & 7) || (t->height & 7),
               "target %p %ux%u", t->color, t->width, t->height);
    static const u8 color_size_code[] = {2, 1, 0, 0, 0}; /* RGBA8, RGB8, 5551, 565, RGBA4 */
    u32 p[4];

    if (S.target_valid && !memcmp(t, &S.target, sizeof(*t)))
        return;
    pica_state_flush_framebuffer();
    S.target = *t;
    S.target_valid = true;

    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    p[0] = t->depth ? osConvertVirtToPhys(t->depth) >> 3 : 0;
    p[1] = osConvertVirtToPhys(t->color) >> 3;
    p[2] = 0x01000000 | (((u32)(t->height - 1) & 0xFFF) << 12) | (t->width & 0xFFF);
    GPUCMD_AddIncrementalWrites(GPUREG_DEPTHBUFFER_LOC, p, 3);
    GPUCMD_AddWrite(GPUREG_RENDERBUF_DIM, p[2]);
    GPUCMD_AddWrite(GPUREG_DEPTHBUFFER_FORMAT, t->depth_fmt);
    GPUCMD_AddWrite(GPUREG_COLORBUFFER_FORMAT, color_size_code[t->color_fmt] | ((u32)t->color_fmt << 16));
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_BLOCK32, 0);

    u32 depth_mask = !t->depth ? 0 : t->depth_fmt == GPU_RB_DEPTH24_STENCIL8 ? 0x3 : 0x2;
    p[0] = p[1] = 0xF;          /* color read / write */
    p[2] = p[3] = depth_mask;   /* depth read / write */
    GPUCMD_AddIncrementalWrites(GPUREG_COLORBUFFER_READ, p, 4);
    memcpy(S.access, p, sizeof(S.access));
}

/* Framebuffer access (0x112-0x115) for the following draws; bench.c only. Switching the color
 * read off is 22% cheaper per fragment on hardware (bench gpu_draw_flat_vram_noread), but the GPU
 * then writes whole 8x8 tiles: pixels a draw does not cover in a tile it touches become garbage
 * (hardware self-test 2026-11-16: 286 color and 947 depth words wrong in a 64x64 scene). So
 * games keep everything on. A change flushes the framebuffer cache first. */
void pica_fb_access(const u32 a[4])
{
    if (!memcmp(a, S.access, sizeof(S.access)))
        return;
    if (S.drawn) { /* tiles cached under the old access mode go out first */
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
        S.drawn = false;
    }
    memcpy(S.access, a, sizeof(S.access));
    GPUCMD_AddIncrementalWrites(GPUREG_COLORBUFFER_READ, (u32 *)a, 4);
}

void pica_viewport(u16 x, u16 y, u16 w, u16 h)
{
    PICA_CHECK(!w || !h || w > 2048 || h > 2048 || x > 1024 || y > 1024, "viewport %u,%u %ux%u", x, y, w, h);
    u32 v[5];
    v[0] = f32tof24(w / 2.0f);
    v[1] = f32tof31(2.0f / w) << 1;
    v[2] = f32tof24(h / 2.0f);
    v[3] = f32tof31(2.0f / h) << 1;
    v[4] = ((u32)y << 16) | x;
    if (S.viewport_valid && !memcmp(v, S.viewport, sizeof(v)))
        return;
    memcpy(S.viewport, v, sizeof(v));
    S.viewport_valid = true;
    GPUCMD_AddIncrementalWrites(GPUREG_VIEWPORT_WIDTH, v, 4);
    GPUCMD_AddWrite(GPUREG_VIEWPORT_XY, v[4]);
}

void pica_scissor(GPU_SCISSORMODE mode, u16 left, u16 top, u16 right, u16 bottom)
{
    PICA_CHECK(mode != GPU_SCISSOR_DISABLE && (right <= left || bottom <= top || right > 1024 || bottom > 1024),
               "scissor mode %d %u,%u-%u,%u", (int)mode, left, top, right, bottom);
    u32 s[3] = {mode, 0, 0};
    if (mode != GPU_SCISSOR_DISABLE) {
        s[1] = ((u32)top << 16) | left;
        s[2] = ((u32)(bottom - 1) << 16) | (u16)(right - 1);
    }
    if (S.scissor_valid && !memcmp(s, S.scissor, sizeof(s)))
        return;
    memcpy(S.scissor, s, sizeof(s));
    S.scissor_valid = true;
    GPUCMD_AddIncrementalWrites(GPUREG_SCISSORTEST_MODE, s, 3);
}

/* ---- shaders / uniforms --------------------------------------------------------------------- */

static void upload_shader_constants(const shaderInstance_s *si)
{
    /* shaderProgramConfigure uploads code and output maps but not the .constf/.consti/.constb
     * values baked into the DVLE; those are sent here. */
    for (int i = 0; i < si->numFloat24Uniforms; i++)
        GPUCMD_AddIncrementalWrites(GPUREG_VSH_FLOATUNIFORM_CONFIG, (u32 *)&si->float24Uniforms[i], 4);
    for (int i = 0; i < 4; i++)
        if (si->intUniformMask & BIT(i))
            GPUCMD_AddWrite(GPUREG_VSH_INTUNIFORM_I0 + i, si->intUniforms[i]);
    GPUCMD_AddWrite(GPUREG_VSH_BOOLUNIFORM, 0x7FFF0000 | (si->boolUniforms & si->boolUniformMask));
}

void pica_bind_program(shaderProgram_s *prog)
{
    if (prog == S.program)
        return;
    if (!prog->vertexShader) {
        printf("\x1b[24;0Hbind_program %p: no vertex shader (lr %p)\n", (void *)prog, __builtin_return_address(0));
        return;
    }
    S.program = prog;
    shaderProgramConfigure(prog, true, true);
    upload_shader_constants(prog->vertexShader);
    S.proj_bool = false; /* the program's own bool uniforms (proj off) were written */
    S.layout_dirty = true; /* input configuration must follow the program */
}

void pica_forget_program(const shaderProgram_s *prog)
{
    if (S.program == prog)
        S.program = NULL;
}

void pica_uniform4fv(int reg, const float *xyzw, int count)
{
    u32 words[16 * 4];

    while (count > 0) {
        int n = count > 16 ? 16 : count;
        for (int i = 0; i < n; i++) {
            /* The data FIFO takes components in reverse order: w, z, y, x. */
            for (int c = 0; c < 4; c++)
                memcpy(&words[i * 4 + c], &xyzw[i * 4 + (3 - c)], 4);
        }
        GPUCMD_AddWrite(GPUREG_VSH_FLOATUNIFORM_CONFIG, 0x80000000u | (u32)reg); /* float32 mode */
        GPUCMD_AddWrites(GPUREG_VSH_FLOATUNIFORM_DATA, words, n * 4);
        reg += n;
        xyzw += n * 4;
        count -= n;
    }
}

void pica_uniform_matrix(int reg, const float rows[4][4])
{
    pica_uniform4fv(reg, &rows[0][0], 4);
}

/* ---- vertex input ----------------------------------------------------------------------------- */

void pica_vertex_layout(const PicaAttrib *attribs, int count, u16 stride)
{
    u32 fmt[2] = {0, 0xFFFu << 16};
    u64 perm = 0;

    for (int i = 0; i < count; i++) {
        if (i < 8)
            fmt[0] |= GPU_ATTRIBFMT(i, attribs[i].count, attribs[i].format);
        else
            fmt[1] |= GPU_ATTRIBFMT(i - 8, attribs[i].count, attribs[i].format);
        fmt[1] = (fmt[1] & ~(0xF0000000u | BIT(i + 16))) | ((u32)i << 28);
        perm |= (u64)i << (i * 4);
    }
    if (!S.layout_dirty && count == S.attr_count && stride == S.stride &&
        !memcmp(fmt, S.attr_format, sizeof(fmt)))
        return;
    memcpy(S.attr_format, fmt, sizeof(fmt));
    S.attr_perm = perm;
    S.attr_count = count;
    S.stride = stride;
    S.layout_dirty = true;
    S.buffer_dirty = true;
}

void pica_vertex_buffer(const void *data)
{
    u32 off = osConvertVirtToPhys(data) - PICA_BUFFER_BASE_PADDR;
    if (off != S.vbo_offset)
        S.buffer_dirty = true;
    S.vbo_offset = off;
}

void pica_projective(bool on)
{
    S.proj_req = on && pica_proj_bool_bit >= 0;
}

void pica_state_prepare_draw(void)
{
    /* projective draw: the shader divides by w (screen-linear colours) and texture unit 0
     * divides its coordinates per pixel (texcoord0w); one draw only */
    if (S.proj_req != S.proj_bool) {
        const shaderInstance_s *si = S.program ? S.program->vertexShader : NULL;
        u32 b = si ? (si->boolUniforms & si->boolUniformMask) : 0;
        if (S.proj_req)
            b |= BIT(pica_proj_bool_bit);
        GPUCMD_AddWrite(GPUREG_VSH_BOOLUNIFORM, 0x7FFF0000 | b);
        S.proj_bool = S.proj_req;
    }
    if (S.tex_valid[0] && S.proj_req != S.proj_tex) {
        S.tex[0][2] = (S.tex[0][2] & ~GPU_TEXTURE_MODE(7)) | (S.proj_req ? GPU_TEXTURE_MODE(GPU_TEX_PROJECTION) : 0);
        GPUCMD_AddWrite(GPUREG_TEXUNIT0_PARAM, S.tex[0][2]);
        S.proj_tex = S.proj_req;
    }
    S.proj_req = false;
    if (S.layout_dirty) {
        GPUCMD_AddIncrementalWrites(GPUREG_ATTRIBBUFFERS_FORMAT_LOW, S.attr_format, 2);
        GPUCMD_AddMaskedWrite(GPUREG_VSH_INPUTBUFFER_CONFIG, 0xB, 0xA0000000u | (S.attr_count - 1));
        GPUCMD_AddWrite(GPUREG_VSH_NUM_ATTR, S.attr_count - 1);
        GPUCMD_AddIncrementalWrites(GPUREG_VSH_ATTRIBUTES_PERMUTATION_LOW, (u32 *)&S.attr_perm, 2);
        S.layout_dirty = false;
    }
    if (S.buffer_dirty) {
        /* Buffer 0 holds all attributes interleaved; buffers 1-11 are unused. Each draw has its
         * own vertex memory, so buffer 0 changes per draw: the rest is written once per reset. */
        u32 cfg[12 * 3];
        memset(cfg, 0, sizeof(cfg));
        cfg[0] = S.vbo_offset;
        cfg[1] = (u32)S.attr_perm;
        cfg[2] = (u32)(S.attr_perm >> 32) | ((u32)S.stride << 16) | ((u32)S.attr_count << 28);
        if (S.buffers_set) {
            GPUCMD_AddIncrementalWrites(GPUREG_ATTRIBBUFFER0_OFFSET, cfg, 3);
        } else {
            GPUCMD_AddWrite(GPUREG_ATTRIBBUFFERS_LOC, PICA_BUFFER_BASE_PADDR >> 3);
            GPUCMD_AddIncrementalWrites(GPUREG_ATTRIBBUFFER0_OFFSET, cfg, 12 * 3);
            S.buffers_set = true;
        }
        S.buffer_dirty = false;
    }
    S.drawn = true;
}

/* ---- texture combiners ------------------------------------------------------------------------ */

void pica_tev_default(PicaTev *t)
{
    memset(t, 0, sizeof(*t));
    t->src_rgb = t->src_alpha = GPU_TEVSOURCES(GPU_PREVIOUS, 0, 0);
    t->func_rgb = t->func_alpha = GPU_REPLACE;
    t->color = 0xFFFFFFFF;
}

void pica_tev(int stage, const PicaTev *t)
{
    u32 r[5];
    r[0] = t->src_rgb | ((u32)t->src_alpha << 16);
    r[1] = t->op_rgb | ((u32)t->op_alpha << 12);
    r[2] = t->func_rgb | ((u32)t->func_alpha << 16);
    r[3] = t->color;
    r[4] = t->scale_rgb | ((u32)t->scale_alpha << 16);
    if (S.tev_valid[stage] && !memcmp(r, S.tev[stage], sizeof(r)))
        return;
    memcpy(S.tev[stage], r, sizeof(r));
    S.tev_valid[stage] = true;
    /* Stages 4 and 5 live after the fog registers. */
    int slot = stage < 4 ? stage : stage + 2;
    GPUCMD_AddIncrementalWrites(GPUREG_TEXENV0_SOURCE + slot * 8, r, 5);
}

void pica_tev_buffer(u8 rgb_mask, u8 alpha_mask, u32 buffer_color)
{
    u32 b[2];
    b[0] = ((u32)(rgb_mask & 0xF) << 8) | ((u32)(alpha_mask & 0xF) << 12); /* fog mode 0 = off */
    b[1] = buffer_color;
    if (S.tev_buffer_valid && !memcmp(b, S.tev_buffer, sizeof(b)))
        return;
    memcpy(S.tev_buffer, b, sizeof(b));
    S.tev_buffer_valid = true;
    GPUCMD_AddMaskedWrite(GPUREG_TEXENV_UPDATE_BUFFER, 0x7, b[0]);
    GPUCMD_AddWrite(GPUREG_TEXENV_BUFFER_COLOR, b[1]);
}

/* ---- textures ----------------------------------------------------------------------------------- */

void pica_texture(int unit, const void *data, u16 width, u16 height, GPU_TEXCOLOR fmt, u32 param)
{
    static const u16 base_reg[3] = {GPUREG_TEXUNIT0_BORDER_COLOR, GPUREG_TEXUNIT1_BORDER_COLOR,
                                    GPUREG_TEXUNIT2_BORDER_COLOR};
    static const u16 type_reg[3] = {GPUREG_TEXUNIT0_TYPE, GPUREG_TEXUNIT1_TYPE, GPUREG_TEXUNIT2_TYPE};
    u32 units = S.tex_units;

    if (!data) {
        units &= ~BIT(unit);
        if (units != S.tex_units) {
            S.tex_units = units;
            emit_tex_config(false);
        }
        S.tex_valid[unit] = false;
        return;
    }

    /* sizes that are not powers of two but multiples of 8 are fine (video.c self-test) */
    PICA_CHECK(width < 8 || height < 8 || width > 1024 || height > 1024 || (width & 7) || (height & 7) || ((u32)data & 7),
               "texture unit %d %p %ux%u fmt %d", unit, data, width, height, (int)fmt);
    u32 r[5];
    r[0] = 0;                                   /* border color */
    r[1] = height | ((u32)width << 16);         /* dimensions */
    r[2] = param;                               /* filters, wrap, mode */
    r[3] = 0;                                   /* LOD */
    r[4] = osConvertVirtToPhys(data) >> 3;      /* address */

    bool changed = !S.tex_valid[unit] || memcmp(r, S.tex[unit], sizeof(r)) || S.tex_type[unit] != (u32)fmt;
    if (changed) {
        memcpy(S.tex[unit], r, sizeof(r));
        S.tex_type[unit] = fmt;
        S.tex_valid[unit] = true;
        GPUCMD_AddIncrementalWrites(base_reg[unit], r, 5);
        GPUCMD_AddWrite(type_reg[unit], fmt);
        if (unit == 0)
            S.proj_tex = false; /* written with the caller's (2D) mode */
    }
    units |= BIT(unit);
    if (changed || units != S.tex_units) {
        S.tex_units = units;
        emit_tex_config(changed);
    }
}

void pica_texture_flush_cache(void)
{
    emit_tex_config(true);
}

/* ---- fragment operations ------------------------------------------------------------------------ */

void pica_depth_test(bool enable, GPU_TESTFUNC func, u32 writemask)
{
    u32 v = (enable ? 1u : 0u) | ((u32)(func & 7) << 4) | (writemask << 8);
    if (S.effect_valid && v == S.depth_color)
        return;
    S.depth_color = v;
    GPUCMD_AddWrite(GPUREG_DEPTH_COLOR_MASK, v);
    GPUCMD_AddMaskedWrite(GPUREG_GAS_DELTAZ_DEPTH, 0x8, (u32)GPU_MAKEGASDEPTHFUNC(func) << 24);
}

void pica_stencil(bool enable, GPU_TESTFUNC func, u8 ref)
{
    const u32 t = enable ? 1u | ((u32)(func & 7) << 4) | (0xFFu << 8) | ((u32)ref << 16) | (0xFFu << 24) : 0;
    const u32 op = enable ? (u32)GPU_STENCIL_KEEP | ((u32)GPU_STENCIL_KEEP << 4) | ((u32)GPU_STENCIL_REPLACE << 8) : 0;
    if (t != S.stencil_test) {
        S.stencil_test = t;
        GPUCMD_AddWrite(GPUREG_STENCIL_TEST, t);
    }
    if (op != S.stencil_op) {
        S.stencil_op = op;
        GPUCMD_AddWrite(GPUREG_STENCIL_OP, op);
    }
}

void pica_depth_map(float scale, float offset)
{
    u32 s = f32tof24(scale), o = f32tof24(offset);
    if (S.effect_valid && s == S.depth_scale && o == S.depth_offset)
        return;
    S.depth_scale = s;
    S.depth_offset = o;
    u32 v[2] = {s, o};
    GPUCMD_AddWrite(GPUREG_DEPTHMAP_ENABLE, 1);
    GPUCMD_AddIncrementalWrites(GPUREG_DEPTHMAP_SCALE, v, 2);
}

void pica_cull(GPU_CULLMODE mode)
{
    if (S.effect_valid && S.cull == (u32)mode)
        return;
    S.cull = mode;
    GPUCMD_AddWrite(GPUREG_FACECULLING_CONFIG, mode & 3);
}

void pica_alpha_test(bool enable, GPU_TESTFUNC func, u8 ref)
{
    u32 v = (enable ? 1u : 0u) | ((u32)(func & 7) << 4) | ((u32)ref << 8);
    if (S.effect_valid && v == S.alpha_test)
        return;
    S.alpha_test = v;
    GPUCMD_AddWrite(GPUREG_FRAGOP_ALPHA_TEST, v);
}

void pica_blend(GPU_BLENDEQUATION eq_rgb, GPU_BLENDEQUATION eq_a, GPU_BLENDFACTOR src_rgb,
                GPU_BLENDFACTOR dst_rgb, GPU_BLENDFACTOR src_a, GPU_BLENDFACTOR dst_a)
{
    u32 v = eq_rgb | ((u32)eq_a << 8) | ((u32)src_rgb << 16) | ((u32)dst_rgb << 20) |
            ((u32)src_a << 24) | ((u32)dst_a << 28);
    if (S.effect_valid && v == S.blend_func)
        return;
    S.blend_func = v;
    GPUCMD_AddWrite(GPUREG_BLEND_FUNC, v);
}

void pica_blend_off(void)
{
    pica_blend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}

void pica_blend_color(u32 rgba)
{
    if (S.effect_valid && rgba == S.blend_color)
        return;
    S.blend_color = rgba;
    GPUCMD_AddWrite(GPUREG_BLEND_COLOR, rgba);
}

/* ---- draws -------------------------------------------------------------------------------------- */

/* DEV hang diagnosis: the last draws, dumped by pica_debug_dump when the GPU stops */
typedef struct {
    u32 seq, vi, prim, n, vbo, ibo, maxi, cmdw, proj, tex0;
} DrawRec;
static DrawRec g_dr[32];
static u32 g_dr_seq;
unsigned pica_bad_offsets; /* buffer offsets the GPU's 28-bit fields cannot hold */
static void note_draw(u32 prim, u32 n, u32 ibo, u32 maxi)
{
    extern unsigned ctr64_vi_count;
    DrawRec *r = &g_dr[g_dr_seq & 31];
    r->seq = g_dr_seq++;
    r->vi = ctr64_vi_count;
    r->prim = prim;
    r->n = n;
    r->vbo = S.vbo_offset;
    r->ibo = ibo;
    r->maxi = maxi;
    r->cmdw = gpuCmdBufOffset;
    r->proj = S.proj_bool;
    r->tex0 = S.tex[0][2];
    if (S.vbo_offset >= 0x10000000u || ibo >= 0x10000000u)
        pica_bad_offsets++;
}

void pica_state_debug(FILE *f)
{
    fprintf(f, "draws %lu bad offsets %u; last (seq vi prim n vbo ibo maxi cmdw proj tex0):\n",
            (unsigned long)g_dr_seq, pica_bad_offsets);
    for (u32 i = 0; i < 32; i++) {
        const DrawRec *r = &g_dr[(g_dr_seq + i) & 31];
        if (r->seq || r->n)
            fprintf(f, "  %lu %lu %lu %lu %08lx %08lx %lu %lu %lu %08lx\n", (unsigned long)r->seq,
                    (unsigned long)r->vi, (unsigned long)r->prim, (unsigned long)r->n, (unsigned long)r->vbo,
                    (unsigned long)r->ibo, (unsigned long)r->maxi, (unsigned long)r->cmdw,
                    (unsigned long)r->proj, (unsigned long)r->tex0);
    }
}

/* Plain vertex lists are drawn as indexed draws through a fixed 0, 1, 2... index buffer, so the
 * GPU never switches between array and indexed mode. Mixing both (indexed triangle lists with
 * plain clipped lists / rectangles) hung the hardware GPU now and then (Mario Kart 64 at its
 * first frames, 2026-11-18; never in Azahar). Dev switch directarrays.txt: real array draws. */
bool pica_direct_arrays;
static u16 *g_seq_idx;

void pica_draw_arrays(GPU_Primitive_t prim, u32 first, u32 count)
{
    if (!count)
        return; /* the hardware GPU hangs on a draw of 0 vertices (Azahar does not) */
    if (!pica_direct_arrays && first + count <= 0x10000) {
        if (!g_seq_idx) {
            g_seq_idx = linearMemAlign(0x10000 * 2, 16);
            if (g_seq_idx) {
                for (u32 i = 0; i < 0x10000; i++)
                    g_seq_idx[i] = (u16)i;
                GSPGPU_FlushDataCache(g_seq_idx, 0x10000 * 2);
            }
        }
        if (g_seq_idx) {
            pica_draw_elements(prim, g_seq_idx + first, count);
            return;
        }
    }
    pica_state_prepare_draw();
    GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 2, prim);
    GPUCMD_AddWrite(GPUREG_RESTART_PRIMITIVE, 1);
    GPUCMD_AddWrite(GPUREG_INDEXBUFFER_CONFIG, 0x80000000); /* required even without indices */
    GPUCMD_AddWrite(GPUREG_NUMVERTICES, count);
    GPUCMD_AddWrite(GPUREG_VERTEX_OFFSET, first);
    GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 1, 1);
    GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 0);
    GPUCMD_AddWrite(GPUREG_DRAWARRAYS, 1);
    GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 1);
    GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 1, 0);
    GPUCMD_AddWrite(GPUREG_VTX_FUNC, 1); /* clear the post-vertex cache */
    note_draw(prim, count, 0xFFFFFFFFu, first + count - 1);
}

void pica_draw_elements(GPU_Primitive_t prim, const u16 *indices, u32 count)
{
    if (!count)
        return; /* as pica_draw_arrays */
    PICA_CHECK((prim == GPU_TRIANGLES && count % 3) || ((u32)indices & 1) || count > 0x10000, "draw_elements prim %d count %lu", (int)prim, (unsigned long)count);
    u32 off = osConvertVirtToPhys(indices) - PICA_BUFFER_BASE_PADDR;
    bool tris = prim == GPU_TRIANGLES;

    pica_state_prepare_draw();
    GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 2, tris ? GPU_GEOMETRY_PRIM : prim);
    GPUCMD_AddWrite(GPUREG_RESTART_PRIMITIVE, 1);
    GPUCMD_AddWrite(GPUREG_INDEXBUFFER_CONFIG, off | (1u << 31)); /* 16-bit indices */
    GPUCMD_AddWrite(GPUREG_NUMVERTICES, count);
    GPUCMD_AddWrite(GPUREG_VERTEX_OFFSET, 0);
    if (tris) {
        GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 2, 0x100);
        GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 2, 0x100);
    }
    GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 0);
    GPUCMD_AddWrite(GPUREG_DRAWELEMENTS, 1);
    GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 1);
    if (tris) {
        GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 2, 0);
        GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 2, 0);
    }
    GPUCMD_AddWrite(GPUREG_VTX_FUNC, 1);
    /* written twice, as citro3d does (C3D_DrawElements): a single write after an indexed draw
     * is suspected of leaving the GPU in indexed mode now and then, which hangs the next
     * non-indexed draw (hardware: hangs at the first frame after a state load, 2026-11-18) */
    GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
    GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
    u32 maxi = 0;
    for (u32 i = 0; i < count; i++)
        if (indices[i] > maxi)
            maxi = indices[i];
    note_draw(prim, count, off, maxi);
}

/* ---- save / restore ---------------------------------------------------------------------------- */

/* A draw slipped in between a draw's state setup and the draw itself (the shadow copies of
 * ContextImplPica::shadowDraw): pica_state_save() before it, pica_state_restore() after it puts
 * back every register this layer shadows the way it was (the slipped-in draw goes out of the
 * framebuffer cache first). Vertex shader uniforms are not shadowed: the slipped-in draw must not
 * change them (same program, no uniform writes). */
static __typeof__(S) Ssave;

void pica_state_save(void)
{
    Ssave = S;
    pica_stencil(false, GPU_ALWAYS, 0); /* internal draws: no stencil (the next game draw sets it again) */
}

void pica_state_restore(void)
{
    /* target first: switching flushes what was drawn into the other one */
    if (Ssave.target_valid) {
        pica_bind_target(&Ssave.target);
    } else {
        pica_state_flush_framebuffer();
        S.target_valid = false;
    }
    if (memcmp(S.access, Ssave.access, sizeof(S.access))) {
        pica_state_flush_framebuffer();
        memcpy(S.access, Ssave.access, sizeof(S.access));
        GPUCMD_AddIncrementalWrites(GPUREG_COLORBUFFER_READ, S.access, 4);
    }

    if (Ssave.viewport_valid && (!S.viewport_valid || memcmp(S.viewport, Ssave.viewport, sizeof(S.viewport)))) {
        GPUCMD_AddIncrementalWrites(GPUREG_VIEWPORT_WIDTH, Ssave.viewport, 4);
        GPUCMD_AddWrite(GPUREG_VIEWPORT_XY, Ssave.viewport[4]);
    }
    memcpy(S.viewport, Ssave.viewport, sizeof(S.viewport));
    S.viewport_valid = Ssave.viewport_valid;
    if (Ssave.scissor_valid && (!S.scissor_valid || memcmp(S.scissor, Ssave.scissor, sizeof(S.scissor))))
        GPUCMD_AddIncrementalWrites(GPUREG_SCISSORTEST_MODE, Ssave.scissor, 3);
    memcpy(S.scissor, Ssave.scissor, sizeof(S.scissor));
    S.scissor_valid = Ssave.scissor_valid;

    if (Ssave.program && Ssave.program != S.program)
        pica_bind_program(Ssave.program);

    /* vertex input: written again at the next draw */
    memcpy(S.attr_format, Ssave.attr_format, sizeof(S.attr_format));
    S.attr_perm = Ssave.attr_perm;
    S.attr_count = Ssave.attr_count;
    S.stride = Ssave.stride;
    S.vbo_offset = Ssave.vbo_offset;
    S.layout_dirty = S.buffer_dirty = true;

    for (int i = 0; i < 6; i++) {
        if (Ssave.tev_valid[i] && (!S.tev_valid[i] || memcmp(S.tev[i], Ssave.tev[i], sizeof(S.tev[i])))) {
            const int slot = i < 4 ? i : i + 2;
            GPUCMD_AddIncrementalWrites(GPUREG_TEXENV0_SOURCE + slot * 8, Ssave.tev[i], 5);
        }
        memcpy(S.tev[i], Ssave.tev[i], sizeof(S.tev[i]));
        S.tev_valid[i] = Ssave.tev_valid[i];
    }
    if (Ssave.tev_buffer_valid &&
        (!S.tev_buffer_valid || memcmp(S.tev_buffer, Ssave.tev_buffer, sizeof(S.tev_buffer)))) {
        GPUCMD_AddMaskedWrite(GPUREG_TEXENV_UPDATE_BUFFER, 0x7, Ssave.tev_buffer[0]);
        GPUCMD_AddWrite(GPUREG_TEXENV_BUFFER_COLOR, Ssave.tev_buffer[1]);
    }
    memcpy(S.tev_buffer, Ssave.tev_buffer, sizeof(S.tev_buffer));
    S.tev_buffer_valid = Ssave.tev_buffer_valid;

    static const u16 base_reg[3] = {GPUREG_TEXUNIT0_BORDER_COLOR, GPUREG_TEXUNIT1_BORDER_COLOR,
                                    GPUREG_TEXUNIT2_BORDER_COLOR};
    static const u16 type_reg[3] = {GPUREG_TEXUNIT0_TYPE, GPUREG_TEXUNIT1_TYPE, GPUREG_TEXUNIT2_TYPE};
    for (int u = 0; u < 3; u++) {
        if (Ssave.tex_valid[u] && (!S.tex_valid[u] || memcmp(S.tex[u], Ssave.tex[u], sizeof(S.tex[u])) ||
                                   S.tex_type[u] != Ssave.tex_type[u])) {
            GPUCMD_AddIncrementalWrites(base_reg[u], Ssave.tex[u], 5);
            GPUCMD_AddWrite(type_reg[u], Ssave.tex_type[u]);
        }
        memcpy(S.tex[u], Ssave.tex[u], sizeof(S.tex[u]));
        S.tex_type[u] = Ssave.tex_type[u];
        S.tex_valid[u] = Ssave.tex_valid[u];
    }
    S.tex_units = Ssave.tex_units;
    emit_tex_config(true); /* units back, texture cache cleared (the copy wrote what is sampled next) */
    S.proj_tex = Ssave.proj_tex; /* unit 0's mode bits came back with its registers */
    S.proj_req = Ssave.proj_req; /* proj_bool stays: it is what the GPU has */

    if (Ssave.effect_valid) {
        if (S.depth_color != Ssave.depth_color) {
            GPUCMD_AddWrite(GPUREG_DEPTH_COLOR_MASK, Ssave.depth_color);
            GPUCMD_AddMaskedWrite(GPUREG_GAS_DELTAZ_DEPTH, 0x8,
                                  (u32)GPU_MAKEGASDEPTHFUNC((GPU_TESTFUNC)((Ssave.depth_color >> 4) & 7)) << 24);
        }
        if (S.depth_scale != Ssave.depth_scale || S.depth_offset != Ssave.depth_offset) {
            u32 v[2] = {Ssave.depth_scale, Ssave.depth_offset};
            GPUCMD_AddWrite(GPUREG_DEPTHMAP_ENABLE, 1);
            GPUCMD_AddIncrementalWrites(GPUREG_DEPTHMAP_SCALE, v, 2);
        }
        if (S.cull != Ssave.cull)
            GPUCMD_AddWrite(GPUREG_FACECULLING_CONFIG, Ssave.cull & 3);
        if (S.alpha_test != Ssave.alpha_test)
            GPUCMD_AddWrite(GPUREG_FRAGOP_ALPHA_TEST, Ssave.alpha_test);
        if (S.blend_func != Ssave.blend_func)
            GPUCMD_AddWrite(GPUREG_BLEND_FUNC, Ssave.blend_func);
        if (S.blend_color != Ssave.blend_color)
            GPUCMD_AddWrite(GPUREG_BLEND_COLOR, Ssave.blend_color);
    }
    S.depth_color = Ssave.depth_color;
    S.depth_scale = Ssave.depth_scale;
    S.depth_offset = Ssave.depth_offset;
    S.cull = Ssave.cull;
    S.alpha_test = Ssave.alpha_test;
    S.blend_func = Ssave.blend_func;
    S.blend_color = Ssave.blend_color;
    S.effect_valid = Ssave.effect_valid;
}
