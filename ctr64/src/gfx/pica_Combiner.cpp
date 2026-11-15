/* N64 color combiner -> PICA200 TEV compiler. See pica_Combiner.h for the scheme. */

#include "pica_Combiner.h"

#include <Combiner.h>
#include <GBI.h>
#include <gDP.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace pica_cmb {
namespace {

/* PICA TEV encodings (gpu/enums.h; libctru's headers cannot be included next to GLideN64's). */
enum : u8 { SRC_PRIMARY = 0x0, SRC_TEX0 = 0x3, SRC_TEX1 = 0x4, SRC_TEX2 = 0x5, SRC_BUFFER = 0xD, SRC_CONST = 0xE, SRC_PREV = 0xF };
enum : u8 { OPC_COLOR = 0, OPC_1M_COLOR = 1, OPC_ALPHA = 2, OPC_1M_ALPHA = 3 };
enum : u8 { OPA_ALPHA = 0, OPA_1M_ALPHA = 1 };
enum : u8 { F_REPLACE = 0, F_MODULATE = 1, F_ADD = 2, F_INTERPOLATE = 4, F_SUBTRACT = 5, F_MULADD = 8 };

/* GLideN64's mux decoding tables (Combiner.cpp). */
const u8 saRGB[16] = {G_GCI_COMBINED, G_GCI_TEXEL0, G_GCI_TEXEL1, G_GCI_PRIMITIVE, G_GCI_SHADE, G_GCI_ENVIRONMENT,
                      G_GCI_ONE, G_GCI_NOISE, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO,
                      G_GCI_ZERO, G_GCI_ZERO};
const u8 sbRGB[16] = {G_GCI_COMBINED, G_GCI_TEXEL0, G_GCI_TEXEL1, G_GCI_PRIMITIVE, G_GCI_SHADE, G_GCI_ENVIRONMENT,
                      G_GCI_CENTER, G_GCI_K4, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO,
                      G_GCI_ZERO, G_GCI_ZERO};
const u8 mRGB[32] = {G_GCI_COMBINED, G_GCI_TEXEL0, G_GCI_TEXEL1, G_GCI_PRIMITIVE, G_GCI_SHADE, G_GCI_ENVIRONMENT,
                     G_GCI_SCALE, G_GCI_COMBINED_ALPHA, G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA,
                     G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA, G_GCI_LOD_FRACTION, G_GCI_PRIM_LOD_FRAC, G_GCI_K5,
                     G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO,
                     G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO};
const u8 aRGB[8] = {G_GCI_COMBINED, G_GCI_TEXEL0, G_GCI_TEXEL1, G_GCI_PRIMITIVE,
                    G_GCI_SHADE, G_GCI_ENVIRONMENT, G_GCI_ONE, G_GCI_ZERO};
const u8 saA[8] = {G_GCI_COMBINED, G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA,
                   G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA, G_GCI_ONE, G_GCI_ZERO};
const u8 sbA[8] = {G_GCI_COMBINED, G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA,
                   G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA, G_GCI_ONE, G_GCI_ZERO};
const u8 mA[8] = {G_GCI_LOD_FRACTION, G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA,
                  G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA, G_GCI_PRIM_LOD_FRAC, G_GCI_ZERO};
const u8 aA[8] = {G_GCI_COMBINED, G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA,
                  G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA, G_GCI_ONE, G_GCI_ZERO};

/* ---- symbolic operands ------------------------------------------------------------------- */

enum Kind : u8 {
    K_SHADE, K_TEX0, K_TEX1, K_COMB,
    K_CONST, /* the stage constant */
    K_HALF,  /* 0.5, from texture unit 2 */
    K_PREV,  /* this channel's previous op */
    K_BUF    /* this channel's buffered temporary (written by an earlier op) */
};

struct Arg {
    u8 kind = K_CONST;
    KRef k;
    bool alpha = false; /* rgb channel reads the source's alpha */
    bool one_minus = false;
};

struct Op {
    u8 func = F_REPLACE;
    u8 n = 0;
    u8 scale = 0;
    bool write_buf = false;
    Arg a[3];
};

struct Chain {
    Op op[10];
    int n = 0;
    Op &push(u8 func, int nargs, Arg a0 = Arg(), Arg a1 = Arg(), Arg a2 = Arg())
    {
        Op &o = op[n < 10 ? n++ : 9];
        o = Op();
        o.func = func;
        o.n = (u8)nargs;
        o.a[0] = a0;
        o.a[1] = a1;
        o.a[2] = a2;
        return o;
    }
};

KRef kin(u32 g)
{
    KRef k;
    k.op = KRef::K_IN;
    k.g1 = (u8)g;
    return k;
}

Arg arg_of(u32 gci)
{
    Arg r;
    switch (gci) {
    case G_GCI_SHADE: r.kind = K_SHADE; break;
    case G_GCI_SHADE_ALPHA: r.kind = K_SHADE; r.alpha = true; break;
    case G_GCI_TEXEL0: r.kind = K_TEX0; break;
    case G_GCI_TEXEL0_ALPHA: r.kind = K_TEX0; r.alpha = true; break;
    case G_GCI_TEXEL1: r.kind = K_TEX1; break;
    case G_GCI_TEXEL1_ALPHA: r.kind = K_TEX1; r.alpha = true; break;
    case G_GCI_COMBINED: r.kind = K_COMB; break;
    case G_GCI_COMBINED_ALPHA: r.kind = K_COMB; r.alpha = true; break;
    default: r.kind = K_CONST; r.k = kin(gci); break;
    }
    return r;
}

Arg karg(KRef k)
{
    Arg r;
    r.kind = K_CONST;
    r.k = k;
    return r;
}

Arg simple(u8 kind)
{
    Arg r;
    r.kind = kind;
    return r;
}

Arg one_minus(Arg a)
{
    a.one_minus = !a.one_minus;
    return a;
}

bool is_const(const Arg &a) { return a.kind == K_CONST; }
bool is_zero(u32 g) { return g == G_GCI_ZERO; }
bool is_one(u32 g) { return g == G_GCI_ONE; }
bool varying(u32 g) { return !is_const(arg_of(g)); }

/* (A - B) * C + D for one channel, as TEV ops (TEV stages clamp every result to [0, 1]). */
void emit_cycle(u32 A, u32 B, u32 C, u32 D, Chain &ch)
{
    const Arg a = arg_of(A), b = arg_of(B), c = arg_of(C), d = arg_of(D);
    if (is_zero(C) || A == B) {
        ch.push(F_REPLACE, 1, d);
    } else if (is_zero(B)) {
        if (is_one(C))
            is_zero(D) ? ch.push(F_REPLACE, 1, a) : ch.push(F_ADD, 2, a, d);
        else if (is_one(A))
            is_zero(D) ? ch.push(F_REPLACE, 1, c) : ch.push(F_ADD, 2, c, d);
        else
            is_zero(D) ? ch.push(F_MODULATE, 2, a, c) : ch.push(F_MULADD, 3, a, c, d);
    } else if (D == B) {
        if (is_one(C))
            ch.push(F_REPLACE, 1, a);
        else
            ch.push(F_INTERPOLATE, 3, a, b, c);
    } else if (is_one(A)) {
        if (is_one(C))
            is_zero(D) ? ch.push(F_REPLACE, 1, one_minus(b)) : ch.push(F_ADD, 2, one_minus(b), d);
        else
            is_zero(D) ? ch.push(F_MODULATE, 2, one_minus(b), c) : ch.push(F_MULADD, 3, one_minus(b), c, d);
    } else if (is_zero(D)) {
        /* max(A - B, 0) * C is exact: a negative product clamps to 0 anyway */
        ch.push(F_SUBTRACT, 2, a, b);
        if (!is_one(C))
            ch.push(F_MODULATE, 2, simple(K_PREV), c);
    } else if (is_zero(A)) {
        /* D - B*C */
        if (is_one(C)) {
            ch.push(F_SUBTRACT, 2, d, b);
        } else {
            ch.push(F_MODULATE, 2, b, c);
            ch.push(F_SUBTRACT, 2, d, simple(K_PREV));
        }
    } else if (!varying(B) && !varying(D)) {
        /* (A - B)*C + D = lerp(B, A, C) + (D - B): the lerp stays in [0, 1] (exact), and the
         * signed constant D - B is added as its positive part, then subtracted as its negative
         * part (per channel only one is nonzero). No combiner buffer: COMBINED, which cycle 2
         * reads from the buffer, stays intact (Conker's characters: (SHADE - ENV)*COMBINED +
         * PRIM lost the texture and drew flat red) */
        KRef kp, kn;
        kp.op = KRef::K_POSDIFF; kp.g1 = (u8)D; kp.g2 = (u8)B;
        kn.op = KRef::K_NEGDIFF; kn.g1 = (u8)D; kn.g2 = (u8)B;
        ch.push(F_INTERPOLATE, 3, a, b, c);
        ch.push(F_ADD, 2, simple(K_PREV), karg(kp));
        ch.push(F_SUBTRACT, 2, simple(K_PREV), karg(kn));
    } else if (!varying(B) && !varying(C)) {
        /* X = A*C + D - B*C with B*C folded: compute (X + 1) / 2, then expand */
        KRef k1, k2;
        k1.op = KRef::K_HALF_1M_PROD; k1.g1 = (u8)B; k1.g2 = (u8)C; /* (1 - B*C) / 2 */
        k2.op = KRef::K_HALF; k2.g1 = (u8)C;                         /* C / 2 */
        ch.push(F_MULADD, 3, d, simple(K_HALF), karg(k1));          /* (D + 1 - B*C) / 2 */
        ch.push(F_MULADD, 3, a, karg(k2), simple(K_PREV));          /* (X + 1) / 2 */
        ch.push(F_SUBTRACT, 2, simple(K_PREV), simple(K_HALF)).scale = 1;
    } else {
        /* General: H = (A - B + 1)/2, J = (D - C + 1)/2, (X + 1)/2 = H*C + J, then expand */
        ch.push(F_INTERPOLATE, 3, a, one_minus(b), simple(K_HALF)).write_buf = true;
        ch.push(F_INTERPOLATE, 3, d, one_minus(c), simple(K_HALF));
        ch.push(F_MULADD, 3, simple(K_BUF), c, simple(K_PREV));
        ch.push(F_SUBTRACT, 2, simple(K_PREV), simple(K_HALF)).scale = 1;
    }
}

/* At most one distinct constant per op: an extra one is preloaded into PREVIOUS. */
void resolve_constants(Chain &ch, bool &lossy)
{
    Chain out;
    for (int i = 0; i < ch.n; i++) {
        Op o = ch.op[i];
        int first = -1;
        for (int k = 0; k < o.n; k++) {
            Arg &x = o.a[k];
            if (!is_const(x))
                continue;
            if (first < 0) {
                first = k;
                continue;
            }
            if (o.a[first].k == x.k)
                continue;
            /* a second, different constant */
            bool uses_prev = false;
            for (int j = 0; j < o.n; j++)
                uses_prev |= o.a[j].kind == K_PREV;
            if (!uses_prev && !o.write_buf) {
                out.push(F_REPLACE, 1, karg(x.k));
                const bool om = x.one_minus;
                x = simple(K_PREV);
                x.one_minus = om;
                continue;
            }
            x = o.a[first]; /* cannot be expressed: approximate with the other constant */
            lossy = true;
        }
        if (out.n < 10)
            out.op[out.n++] = o;
    }
    ch = out;
}

void decode(u64 mux, u32 cycle_type, u32 cc[2][4], u32 ac[2][4], int &ncycles)
{
    gDPCombine m;
    m.mux = mux;
    if (cycle_type != G_CYC_2CYCLE) {
        u32 c[4] = {saRGB[m.saRGB1], sbRGB[m.sbRGB1], mRGB[m.mRGB1], aRGB[m.aRGB1]};
        u32 a[4] = {saA[m.saA1], sbA[m.sbA1], mA[m.mA1], aA[m.aA1]};
        for (int i = 0; i < 4; i++) {
            if (c[i] == G_GCI_COMBINED || c[i] == G_GCI_COMBINED_ALPHA)
                c[i] = G_GCI_ZERO;
            if (a[i] == G_GCI_COMBINED)
                a[i] = G_GCI_ZERO;
            /* correctFirstStageParam */
            c[i] = c[i] == G_GCI_TEXEL1 ? G_GCI_TEXEL0 : c[i] == G_GCI_TEXEL1_ALPHA ? G_GCI_TEXEL0_ALPHA : c[i];
            a[i] = a[i] == G_GCI_TEXEL1 ? G_GCI_TEXEL0 : a[i] == G_GCI_TEXEL1_ALPHA ? G_GCI_TEXEL0_ALPHA : a[i];
            cc[0][i] = c[i];
            ac[0][i] = a[i];
        }
        ncycles = 1;
        return;
    }
    u32 c0[4] = {saRGB[m.saRGB0], sbRGB[m.sbRGB0], mRGB[m.mRGB0], aRGB[m.aRGB0]};
    u32 a0[4] = {saA[m.saA0], sbA[m.sbA0], mA[m.mA0], aA[m.aA0]};
    u32 c1[4] = {saRGB[m.saRGB1], sbRGB[m.sbRGB1], mRGB[m.mRGB1], aRGB[m.aRGB1]};
    u32 a1[4] = {saA[m.saA1], sbA[m.sbA1], mA[m.mA1], aA[m.aA1]};
    const bool equal = !memcmp(c0, c1, sizeof(c0)) && !memcmp(a0, a1, sizeof(a0));
    auto fix0 = [](u32 g) { return g == G_GCI_COMBINED ? (u32)G_GCI_HALF : g; };
    auto fix1 = [](u32 g) -> u32 {
        switch (g) {
        case G_GCI_TEXEL0: return G_GCI_TEXEL1;
        case G_GCI_TEXEL1: return G_GCI_TEXEL0;
        case G_GCI_TEXEL0_ALPHA: return G_GCI_TEXEL1_ALPHA;
        case G_GCI_TEXEL1_ALPHA: return G_GCI_TEXEL0_ALPHA;
        default: return g;
        }
    };
    for (int i = 0; i < 4; i++) {
        /* the COMBINED_ALPHA input of cycle 1 also reads the (nonexistent) previous cycle */
        cc[0][i] = c0[i] == G_GCI_COMBINED_ALPHA ? (u32)G_GCI_HALF : fix0(c0[i]);
        ac[0][i] = fix0(a0[i]);
        cc[1][i] = fix1(c1[i]);
        ac[1][i] = fix1(a1[i]);
    }
    ncycles = equal ? 1 : 2;
}

void encode_arg(const Arg &x, bool alpha_channel, int stage, int cycle2_first, u8 &src, u8 &op, bool &comb_buf)
{
    switch (x.kind) {
    case K_SHADE: src = SRC_PRIMARY; break;
    case K_TEX0: src = SRC_TEX0; break;
    case K_TEX1: src = SRC_TEX1; break;
    case K_CONST: src = SRC_CONST; break;
    case K_HALF: src = SRC_TEX2; break;
    case K_PREV: src = SRC_PREV; break;
    case K_BUF: src = SRC_BUFFER; break;
    case K_COMB:
        if (stage == cycle2_first) {
            src = SRC_PREV;
        } else {
            src = SRC_BUFFER;
            comb_buf = true;
        }
        break;
    }
    if (alpha_channel) {
        op = x.one_minus ? OPA_1M_ALPHA : OPA_ALPHA;
    } else {
        /* constants are pre-swizzled; the rest read alpha when the input is an _ALPHA one */
        const bool a = x.alpha && (x.kind == K_SHADE || x.kind == K_TEX0 || x.kind == K_TEX1 || x.kind == K_COMB);
        op = a ? (x.one_minus ? OPC_1M_ALPHA : OPC_ALPHA) : (x.one_minus ? OPC_1M_COLOR : OPC_COLOR);
    }
}

u8 to8(f32 v)
{
    v = v * 255.0f + 0.5f;
    return v <= 0.0f ? 0 : v >= 255.0f ? 255 : (u8)v;
}

/* value of an input for one channel: c = 0..2 for r, g, b, 3 for the alpha channel */
f32 input_value(u8 g, int c)
{
    auto pick = [c](const gDPInfo::Color &k) { return c == 0 ? k.r : c == 1 ? k.g : c == 2 ? k.b : k.a; };
    switch (g) {
    case G_GCI_PRIMITIVE: return pick(gDP.primColor);
    case G_GCI_ENVIRONMENT: return pick(gDP.envColor);
    case G_GCI_CENTER: return pick(gDP.key.center);
    case G_GCI_SCALE: return pick(gDP.key.scale);
    case G_GCI_PRIMITIVE_ALPHA: return gDP.primColor.a;
    case G_GCI_ENV_ALPHA: return gDP.envColor.a;
    case G_GCI_PRIM_LOD_FRAC: return gDP.primColor.l;
    case G_GCI_K4: return gDP.convert.k4 / 255.0f;
    case G_GCI_K5: return gDP.convert.k5 / 255.0f;
    case G_GCI_ONE: return 1.0f;
    case G_GCI_HALF: case G_GCI_NOISE: return 0.5f;
    default: return 0.0f; /* ZERO, LOD_FRACTION (no per-pixel LOD on the PICA) */
    }
}

f32 kvalue(const KRef &k, int c)
{
    switch (k.op) {
    case KRef::K_IN: return input_value(k.g1, c);
    case KRef::K_HALF: return 0.5f * input_value(k.g1, c);
    case KRef::K_HALF_1M_PROD: return 0.5f * (1.0f - input_value(k.g1, c) * input_value(k.g2, c));
    case KRef::K_POSDIFF: return std::max(input_value(k.g1, c) - input_value(k.g2, c), 0.0f);
    case KRef::K_NEGDIFF: return std::max(input_value(k.g2, c) - input_value(k.g1, c), 0.0f);
    default: return 0.0f;
    }
}

const char *gci_name(u32 g)
{
    static const char *names[] = {"COMB", "T0", "T1", "PRIM", "SHADE", "ENV", "CENTER", "SCALE", "COMB_A",
                                  "T0_A", "T1_A", "PRIM_A", "SHADE_A", "ENV_A", "LOD", "PRIM_LOD", "NOISE",
                                  "K4", "K5", "1", "0", "0.5", "HWLIGHT"};
    return g < sizeof(names) / sizeof(names[0]) ? names[g] : "?";
}

/* Development aid: every compiled combiner is appended to sdmc:/3ds/ctr64/combiners.log when
 * that file exists (create it empty to enable). */
void log_program(u64 mux, u32 cycle_type, u32 cc[2][4], u32 ac[2][4], int ncycles, const Program &pr)
{
    static int enabled = -1;
    if (enabled < 0) {
        FILE *f = fopen("sdmc:/3ds/ctr64/combiners.log", "r");
        enabled = f != nullptr;
        if (f)
            fclose(f);
    }
    if (!enabled)
        return;
    FILE *f = fopen("sdmc:/3ds/ctr64/combiners.log", "a");
    if (!f)
        return;
    fprintf(f, "mux %08x%08x cyc %u stages %u%s:", (unsigned)(mux >> 32), (unsigned)mux, (unsigned)cycle_type,
            (unsigned)pr.n, pr.lossy ? " LOSSY" : "");
    for (int c = 0; c < ncycles; c++)
        fprintf(f, "  c%d rgb (%s-%s)*%s+%s a (%s-%s)*%s+%s", c, gci_name(cc[c][0]), gci_name(cc[c][1]),
                gci_name(cc[c][2]), gci_name(cc[c][3]), gci_name(ac[c][0]), gci_name(ac[c][1]), gci_name(ac[c][2]),
                gci_name(ac[c][3]));
    fputc('\n', f);
    fclose(f);
}

} // namespace

u32 constant(const KRef &k_rgb, const KRef &k_a)
{
    u32 v = 0;
    if (k_rgb.op != KRef::K_NONE)
        v = to8(kvalue(k_rgb, 0)) | (u32)to8(kvalue(k_rgb, 1)) << 8 | (u32)to8(kvalue(k_rgb, 2)) << 16;
    if (k_a.op != KRef::K_NONE)
        v |= (u32)to8(kvalue(k_a, 3)) << 24;
    return v;
}

void compile(u64 mux, u32 cycle_type, Program &out)
{
    out = Program();
    u32 cc[2][4], ac[2][4];
    int ncycles;
    decode(mux, cycle_type, cc, ac, ncycles);

    Chain crgb[2], ca[2];
    for (int c = 0; c < ncycles; c++) {
        emit_cycle(cc[c][0], cc[c][1], cc[c][2], cc[c][3], crgb[c]);
        emit_cycle(ac[c][0], ac[c][1], ac[c][2], ac[c][3], ca[c]);
    }

    for (int c = 0; c < ncycles; c++) {
        resolve_constants(crgb[c], out.lossy);
        resolve_constants(ca[c], out.lossy);
    }

    /* schedule: both channels advance together; the shorter chain passes PREVIOUS through */
    int stage = 0, cycle2_first = -1, cycle1_last = -1;
    bool comb_buf = false, temp_buf_in_cycle2 = false;
    for (int c = 0; c < ncycles; c++) {
        if (c == 1)
            cycle2_first = stage;
        const int len = crgb[c].n > ca[c].n ? crgb[c].n : ca[c].n;
        for (int i = 0; i < len; i++) {
            if (stage >= MAX_STAGES) {
                out.lossy = true;
                break;
            }
            Stage &s = out.st[stage];
            memset(&s, 0, sizeof(s));
            s.k_rgb = s.k_a = KRef();
            for (int ch = 0; ch < 2; ch++) {
                Chain &chain = ch == 0 ? crgb[c] : ca[c];
                Op o;
                if (i < chain.n) {
                    o = chain.op[i];
                } else {
                    o.func = F_REPLACE;
                    o.n = 1;
                    o.a[0] = simple(K_PREV);
                }
                u8 *src = ch == 0 ? s.src_rgb : s.src_a;
                u8 *op = ch == 0 ? s.op_rgb : s.op_a;
                (ch == 0 ? s.func_rgb : s.func_a) = o.func;
                (ch == 0 ? s.scale_rgb : s.scale_a) = o.scale;
                if (o.write_buf) {
                    if (stage < 4)
                        (ch == 0 ? out.buf_rgb_mask : out.buf_a_mask) |= (u8)(1u << stage);
                    else
                        out.lossy = true;
                    temp_buf_in_cycle2 |= c == 1;
                }
                for (int k = 0; k < 3; k++) {
                    src[k] = SRC_PREV;
                    op[k] = 0;
                }
                for (int k = 0; k < o.n; k++) {
                    const Arg &x = o.a[k];
                    encode_arg(x, ch == 1, stage, cycle2_first, src[k], op[k], comb_buf);
                    if (x.kind == K_CONST)
                        (ch == 0 ? s.k_rgb : s.k_a) = x.k;
                    out.uses_tex[0] |= x.kind == K_TEX0;
                    out.uses_tex[1] |= x.kind == K_TEX1;
                    out.uses_shade |= x.kind == K_SHADE;
                    out.uses_half |= x.kind == K_HALF;
                }
            }
            stage++;
        }
        if (c == 0)
            cycle1_last = stage - 1;
    }
    out.n = (u8)stage;
    if (comb_buf) {
        /* written by stage k, the buffer is visible from stage k + 2: cycle 2's first stage
         * reads COMBINED as PREVIOUS instead */
        if (cycle1_last >= 0 && cycle1_last < 4 && !temp_buf_in_cycle2) {
            out.buf_rgb_mask |= (u8)(1u << cycle1_last);
            out.buf_a_mask |= (u8)(1u << cycle1_last);
        } else {
            out.lossy = true;
        }
    }
    log_program(mux, cycle_type, cc, ac, ncycles, out);
}

} // namespace pica_cmb
