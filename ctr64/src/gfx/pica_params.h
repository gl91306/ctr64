#pragma once

/* Values the PICA backend assigns to GLideN64's graphics:: parameter constants (the OpenGL
 * backend uses GL enums). Shared by pica_Parameters.cpp (definitions) and the backend. */

namespace pica_param {
enum : unsigned {
    /* colorFormat */
    CF_RGB = 1, CF_RGBA, CF_RG, CF_RED, CF_DEPTH, CF_LUMINANCE,
    /* internalcolorFormat */
    IF_NOCOLOR = 20, IF_RGB8, IF_RGBA8, IF_RGBA4, IF_RGB5_A1, IF_RG, IF_R16F, IF_DEPTH, IF_RG32F,
    IF_LUMINANCE, IF_COLOR_INDEX8,
    /* datatype */
    DT_UNSIGNED_BYTE = 40, DT_UNSIGNED_SHORT, DT_UNSIGNED_INT, DT_FLOAT, DT_US_565, DT_US_5551, DT_US_4444,
    /* textureTarget */
    TT_2D = 60, TT_2D_MS, TT_RENDERBUFFER, TT_EXTERNAL,
    /* bufferTarget */
    BT_FRAMEBUFFER = 70, BT_DRAW, BT_READ,
    /* bufferAttachment */
    BA_COLOR0 = 80, BA_COLOR1, BA_COLOR2, BA_COLOR3, BA_COLOR4, BA_DEPTH,
    /* enable */
    EN_BLEND = 100, EN_CULL_FACE, EN_DEPTH_TEST, EN_DEPTH_CLAMP, EN_CLIP_DISTANCE0, EN_DITHER,
    EN_POLYGON_OFFSET_FILL, EN_SCISSOR_TEST,
    /* textureParameters */
    TP_NEAREST = 120, TP_LINEAR, TP_NEAREST_MIP_NEAREST, TP_LINEAR_MIP_NEAREST, TP_CLAMP, TP_REPEAT, TP_MIRROR,
    /* cullMode */
    CM_FRONT = 140, CM_BACK, CM_FRONT_AND_BACK,
    /* compare */
    CMP_LEQUAL = 150, CMP_LESS, CMP_ALWAYS,
    /* blend */
    BL_ZERO = 160, BL_ONE, BL_SRC_ALPHA, BL_DST_ALPHA, BL_ONE_MINUS_SRC_ALPHA, BL_CONSTANT_ALPHA,
    BL_ONE_MINUS_CONSTANT_ALPHA, BL_SRC1_COLOR, BL_ONE_MINUS_SRC1_COLOR, BL_SRC1_ALPHA, BL_ONE_MINUS_SRC1_ALPHA,
    /* drawmode */
    DM_TRIANGLES = 180, DM_TRIANGLE_STRIP, DM_TRIANGLE_FAN, DM_LINES,
    /* blitMask */
    BM_COLOR = 190, BM_DEPTH, BM_STENCIL,
    /* image units / access modes (unused on PICA) */
    IU_DEPTH_Z = 200, IU_DEPTH_DELTA_Z, IU_DEPTH_Z_COPY, IU_DEPTH_DELTA_Z_COPY,
    AM_READ_ONLY = 210, AM_WRITE_ONLY, AM_READ_WRITE,
};

/* texture units: Tex[0..1] map to PICA units 0..1; the rest are shader-only features */
enum : unsigned { TU_TEX0 = 0, TU_TEX1 = 1, TU_DEPTH = 2, TU_ZLUT = 3, TU_PALETTE = 4, TU_MS0 = 5, TU_MS1 = 6 };
} // namespace pica_param
