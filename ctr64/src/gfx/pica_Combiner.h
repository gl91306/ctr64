#pragma once

/* N64 color combiner -> PICA200 TEV compiler.
 *
 * Each N64 cycle computes, per channel, (A - B) * C + D. A cycle becomes one to four TEV stages
 * per channel (rgb and alpha chains run in the same stages). The TEV has one constant per
 * stage; the stage constant's rgb serves the rgb op and its alpha the alpha op, so PRIM, ENV,
 * K4/K5... never collide between channels. Constants are small expressions of combiner inputs
 * (e.g. (1 - B*C)/2), folded on the CPU per draw. Texture unit 2 samples a constant 0.5 texture,
 * a second constant every stage can use; with it the general case is computed exactly in a
 * half-range domain ((X + 1) / 2) and expanded by a final x2 stage. An op that needs two
 * different constants preloads one into PREVIOUS. (The combiner buffer's initial color is no
 * second constant: the buffer input lags one stage, so stage 0 reads zero.) In 2-cycle mode the first cycle's result is COMBINED: the first stage of cycle 2 reads it as
 * PREVIOUS, later stages from the combiner buffer, which the last stage of cycle 1 writes.
 *
 * Programs are compiled once per CombinerKey; constants are evaluated from gDP per draw. */

#include <Types.h>

namespace pica_cmb {

constexpr int MAX_STAGES = 6;

/* A constant: a combiner input or a fixed expression of inputs, evaluated per channel. */
struct KRef {
    enum : u8 { K_NONE, K_IN, K_HALF, K_HALF_1M_PROD } op = K_NONE;
    u8 g1 = 0, g2 = 0; /* G_GCI_* */
    bool operator==(const KRef &o) const { return op == o.op && g1 == o.g1 && g2 == o.g2; }
    bool operator!=(const KRef &o) const { return !(*this == o); }
};

struct Stage {
    u8 func_rgb, func_a;
    u8 src_rgb[3], src_a[3];
    u8 op_rgb[3], op_a[3];
    u8 scale_rgb, scale_a; /* GPU_TEVSCALE: 0 = x1, 1 = x2 */
    KRef k_rgb, k_a;       /* this stage's constant */
};

struct Program {
    Stage st[MAX_STAGES];
    u8 n = 0;
    u8 buf_rgb_mask = 0, buf_a_mask = 0; /* stages that write the combiner buffer */
    bool uses_tex[2] = {false, false};
    bool uses_half = false; /* texture unit 2 must sample the constant 0.5 texture */
    bool uses_shade = false;
    bool lossy = false; /* something had to be approximated (diagnostics) */
};

/* Compiles the combiner for `mux` in `cycle_type` (G_CYC_*), with GLideN64's parameter
 * corrections (1-cycle: TEXEL1 -> TEXEL0, COMBINED -> 0; 2-cycle: COMBINED -> 0.5 in cycle 1,
 * TEXEL0 <-> TEXEL1 in cycle 2). */
void compile(u64 mux, u32 cycle_type, Program &out);

/* TEV constant (0xAABBGGRR) with `k_rgb` in rgb and `k_a` in alpha, from the current gDP state. */
u32 constant(const KRef &k_rgb, const KRef &k_a);

} // namespace pica_cmb
