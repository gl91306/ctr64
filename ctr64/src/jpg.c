/* JPEG decoder: see jpg.h. Every read is bounded (a damaged file gives a damaged picture or an
 * error, never a crash): the entropy-coded data reads as zeros past its end, block loops run over
 * the frame's size, table indices are checked when a scan starts. */
#include "jpg.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PIXELS (64u << 20)
#define MIN_OUT 640               /* longer side of a reduced image, at least */
#define MAX_COEF_BYTES (24u << 20) /* progressive: above this, or without the memory, 1/8 size */

typedef struct {
    uint16_t fast[512];  /* first 9 bits -> length << 8 | symbol, 0: longer code */
    int32_t maxcode[18]; /* per length, left-aligned to 16 bits, exclusive */
    int32_t delta[17];   /* symbol index = code + delta */
    uint8_t sym[256];
    int count;           /* 0: not defined */
} Huff;

typedef struct {
    int id, h, v, tq;
    int bw, bh;     /* blocks, padded to whole MCUs */
    int cw, ch;     /* blocks of the image itself (scans of one component) */
    int td, ta, pred;
    int16_t *coef;  /* progressive: 64 per block (dc_only: 1) */
    uint8_t *plane; /* bw * N x bh * N */
} Comp;

typedef struct {
    const uint8_t *p, *end;
    uint32_t bits;
    int nbits;
    uint16_t qt[4][64]; /* zigzag order */
    bool qt_set[4];
    Huff dc[4], ac[4];
    Comp c[3];
    int ncomp, w, h, hmax, vmax, mcux, mcuy;
    int N;              /* a block becomes N x N pixels */
    bool progressive, dc_only, frame, scanned;
    int restart, adobe; /* adobe: color transform, -1: no marker */
    int ss, se, ah, al, eobrun;
    float T[8][8];      /* T[x][u]: N-point inverse DCT */
    const volatile bool *quit;
    const char *err;
} Dec;

static const uint8_t k_zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* ---- bits ------------------------------------------------------------------------------- */

static void fill(Dec *d)
{
    while (d->nbits <= 24) {
        uint32_t b = 0; /* at a marker or the end: zeros */
        if (d->p < d->end) {
            if (*d->p != 0xFF) {
                b = *d->p++;
            } else if (d->end - d->p >= 2 && d->p[1] == 0) {
                b = 0xFF;
                d->p += 2;
            }
        }
        d->bits |= b << (24 - d->nbits);
        d->nbits += 8;
    }
}

static int get_bits(Dec *d, int n)
{
    if (!n)
        return 0;
    if (d->nbits < n)
        fill(d);
    const uint32_t v = d->bits >> (32 - n);
    d->bits <<= n;
    d->nbits -= n;
    return (int)v;
}

/* n bits as a signed difference / coefficient */
static int get_signed(Dec *d, int n)
{
    if (!n)
        return 0;
    const int v = get_bits(d, n);
    return v < (1 << (n - 1)) ? v - (1 << n) + 1 : v;
}

static int get_sym(Dec *d, const Huff *t)
{
    if (d->nbits < 16)
        fill(d);
    const uint16_t f = t->fast[d->bits >> 23];
    if (f) {
        d->bits <<= f >> 8;
        d->nbits -= f >> 8;
        return f & 0xFF;
    }
    const int32_t code = (int32_t)(d->bits >> 16);
    int len = 10;
    while (len <= 16 && code >= t->maxcode[len])
        len++;
    if (len > 16) { /* not a code: damaged data */
        d->bits <<= 16;
        d->nbits -= 16;
        return 0;
    }
    const int i = (int)(d->bits >> (32 - len)) + t->delta[len];
    d->bits <<= len;
    d->nbits -= len;
    return i >= 0 && i < 256 ? t->sym[i] : 0;
}

static bool make_huff(Huff *t, const uint8_t *counts, const uint8_t *syms, int total)
{
    memset(t, 0, sizeof(*t));
    memcpy(t->sym, syms, (size_t)total);
    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        t->delta[len] = k - code;
        for (int i = 0; i < counts[len - 1]; i++, k++, code++)
            if (len <= 9) {
                const int first = code << (9 - len);
                for (int j = 0; j < 1 << (9 - len) && first + j < 512; j++)
                    t->fast[first + j] = (uint16_t)(len << 8 | syms[k]);
            }
        if (code > 1 << len)
            return false;
        t->maxcode[len] = code << (16 - len);
        code <<= 1;
    }
    t->count = total;
    return true;
}

/* ---- blocks ----------------------------------------------------------------------------- */

/* coefficients (natural order, dequantized) -> N x N pixels */
static void idct(const Dec *d, const int32_t *c, uint8_t *out, int stride)
{
    const int N = d->N;
    float tmp[8][8];
    bool used[8];
    for (int u = 0; u < N; u++) {
        used[u] = false;
        for (int v = 0; v < N; v++)
            used[u] |= c[v * 8 + u] != 0;
        if (used[u])
            for (int y = 0; y < N; y++) {
                float s = 0;
                for (int v = 0; v < N; v++)
                    s += d->T[y][v] * (float)c[v * 8 + u];
                tmp[y][u] = s;
            }
    }
    for (int y = 0; y < N; y++, out += stride)
        for (int x = 0; x < N; x++) {
            float s = 128.5f;
            for (int u = 0; u < N; u++)
                if (used[u])
                    s += d->T[x][u] * tmp[y][u];
            out[x] = s <= 0.0f ? 0 : s >= 255.0f ? 255 : (uint8_t)s;
        }
}

static void block_baseline(Dec *d, Comp *c, int bx, int by)
{
    int32_t blk[64] = {0};
    const uint16_t *q = d->qt[c->tq];
    c->pred = (int16_t)(c->pred + get_signed(d, get_sym(d, &d->dc[c->td]) & 15));
    blk[0] = c->pred * q[0];
    const Huff *ac = &d->ac[c->ta];
    for (int k = 1; k < 64; k++) {
        const int rs = get_sym(d, ac), s = rs & 15;
        if (!s) {
            if (rs < 0xF0)
                break;
            k += 15;
            continue;
        }
        k += rs >> 4;
        const int v = get_signed(d, s);
        if (k < 64)
            blk[k_zigzag[k]] = v * q[k];
    }
    idct(d, blk, c->plane + ((size_t)by * d->N * c->bw + bx) * d->N, c->bw * d->N);
}

static void refine(Dec *d, int16_t *co, int bit)
{
    if (get_bits(d, 1) && !(*co & bit))
        *co = (int16_t)(*co + (*co > 0 ? bit : -bit));
}

static void block_progressive(Dec *d, Comp *c, int bx, int by)
{
    int16_t *co = c->coef + ((size_t)by * c->bw + bx) * (d->dc_only ? 1 : 64);
    const int bit = 1 << d->al;
    if (d->ss == 0) {
        if (d->ah == 0) {
            c->pred = (int16_t)(c->pred + get_signed(d, get_sym(d, &d->dc[c->td]) & 15));
            co[0] = (int16_t)(c->pred * bit);
        } else if (get_bits(d, 1)) {
            co[0] |= (int16_t)bit;
        }
        return;
    }
    const Huff *ac = &d->ac[c->ta];
    int k = d->ss;
    if (d->ah == 0) {
        if (d->eobrun > 0) {
            d->eobrun--;
            return;
        }
        for (; k <= d->se; k++) {
            const int rs = get_sym(d, ac), r = rs >> 4, s = rs & 15;
            if (!s) {
                if (r < 15) {
                    d->eobrun = (1 << r) - 1 + get_bits(d, r);
                    break;
                }
                k += 15;
                continue;
            }
            k += r;
            const int v = get_signed(d, s);
            if (k < 64)
                co[k_zigzag[k]] = (int16_t)(v * bit);
        }
        return;
    }
    if (d->eobrun == 0)
        for (; k <= d->se; k++) {
            const int rs = get_sym(d, ac);
            int r = rs >> 4, v = 0;
            if (rs & 15) {
                v = get_bits(d, 1) ? bit : -bit;
            } else if (r < 15) {
                d->eobrun = (1 << r) + get_bits(d, r);
                break;
            }
            for (; k <= d->se; k++) { /* over r zeros; nonzero ones on the way get their next bit */
                int16_t *p = &co[k_zigzag[k]];
                if (*p)
                    refine(d, p, bit);
                else if (--r < 0)
                    break;
            }
            if (v && k <= d->se)
                co[k_zigzag[k]] = (int16_t)v;
        }
    if (d->eobrun > 0) {
        for (; k <= d->se; k++)
            if (co[k_zigzag[k]])
                refine(d, &co[k_zigzag[k]], bit);
        d->eobrun--;
    }
}

static void scan_reset(Dec *d)
{
    d->bits = 0;
    d->nbits = 0;
    d->eobrun = 0;
    for (int i = 0; i < d->ncomp; i++)
        d->c[i].pred = 0;
}

static bool scan(Dec *d, Comp **sc, int n)
{
    const int nx = n == 1 ? sc[0]->cw : d->mcux, ny = n == 1 ? sc[0]->ch : d->mcuy;
    scan_reset(d);
    int left = d->restart;
    for (int y = 0; y < ny; y++) {
        if (*d->quit) {
            d->err = "stopped";
            return false;
        }
        for (int x = 0; x < nx; x++) {
            if (d->restart && left-- == 0) {
                /* the next restart marker, if it is here */
                while (d->end - d->p >= 2 && d->p[0] == 0xFF && d->p[1] == 0xFF)
                    d->p++;
                if (d->end - d->p >= 2 && d->p[0] == 0xFF && (d->p[1] & 0xF8) == 0xD0)
                    d->p += 2;
                scan_reset(d);
                left = d->restart - 1;
            }
            for (int i = 0; i < n; i++) {
                Comp *c = sc[i];
                const int bw = n == 1 ? 1 : c->h, bh = n == 1 ? 1 : c->v;
                for (int v = 0; v < bh; v++)
                    for (int u = 0; u < bw; u++)
                        if (d->progressive)
                            block_progressive(d, c, x * bw + u, y * bh + v);
                        else
                            block_baseline(d, c, x * bw + u, y * bh + v);
            }
        }
    }
    return true;
}

/* ---- the file --------------------------------------------------------------------------- */

static bool fail(Dec *d, const char *why)
{
    d->err = why;
    return false;
}

static int u16be(const uint8_t *p) { return p[0] << 8 | p[1]; }

static bool read_frame(Dec *d, const uint8_t *p, int n)
{
    if (d->frame)
        return fail(d, "more than one picture");
    if (n < 6 || p[0] != 8)
        return fail(d, "not 8 bits per color");
    d->h = u16be(p + 1);
    d->w = u16be(p + 3);
    d->ncomp = p[5];
    if (!d->w || !d->h || (uint32_t)d->w * (uint32_t)d->h > MAX_PIXELS)
        return fail(d, "size not supported");
    if ((d->ncomp != 1 && d->ncomp != 3) || n < 6 + 3 * d->ncomp)
        return fail(d, "not gray or RGB color (CMYK?)");
    for (int i = 0; i < d->ncomp; i++) {
        Comp *c = &d->c[i];
        c->id = p[6 + i * 3];
        c->h = p[7 + i * 3] >> 4;
        c->v = p[7 + i * 3] & 15;
        c->tq = p[8 + i * 3] & 3;
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4)
            return fail(d, "bad sampling");
        if (d->ncomp == 1)
            c->h = c->v = 1;
        d->hmax = c->h > d->hmax ? c->h : d->hmax;
        d->vmax = c->v > d->vmax ? c->v : d->vmax;
    }
    d->mcux = (d->w + 8 * d->hmax - 1) / (8 * d->hmax);
    d->mcuy = (d->h + 8 * d->vmax - 1) / (8 * d->vmax);
    const int longer = d->w > d->h ? d->w : d->h;
    for (d->N = 8; d->N > 1 && longer * (d->N / 2) / 8 >= MIN_OUT; d->N /= 2)
        ;
    size_t blocks = 0;
    for (int i = 0; i < d->ncomp; i++) {
        Comp *c = &d->c[i];
        c->bw = d->mcux * c->h;
        c->bh = d->mcuy * c->v;
        c->cw = ((d->w * c->h + d->hmax - 1) / d->hmax + 7) / 8;
        c->ch = ((d->h * c->v + d->vmax - 1) / d->vmax + 7) / 8;
        blocks += (size_t)c->bw * c->bh;
    }
    d->dc_only = d->progressive && (d->N == 1 || blocks * 128 > MAX_COEF_BYTES);
    for (bool ok = !d->progressive; !ok;) {
        ok = true;
        for (int i = 0; i < d->ncomp; i++) {
            free(d->c[i].coef);
            d->c[i].coef = calloc((size_t)d->c[i].bw * d->c[i].bh, d->dc_only ? 2 : 128);
            ok &= d->c[i].coef != NULL;
        }
        if (!ok && d->dc_only)
            return fail(d, "too large for the memory");
        d->dc_only |= !ok;
    }
    if (d->dc_only)
        d->N = 1;
    for (int i = 0; i < d->ncomp; i++) {
        Comp *c = &d->c[i];
        const size_t bytes = (size_t)c->bw * c->bh * d->N * d->N;
        c->plane = malloc(bytes);
        if (!c->plane)
            return fail(d, "too large for the memory");
        memset(c->plane, 128, bytes);
    }
    for (int x = 0; x < d->N; x++)
        for (int u = 0; u < d->N; u++)
            d->T[x][u] = 0.5f * (u ? 1.0f : 0.70710678f) * cosf((float)((2 * x + 1) * u) * 3.14159265f / (float)(2 * d->N));
    d->frame = true;
    return true;
}

static bool read_tables(Dec *d, int marker, const uint8_t *p, int n)
{
    while (n > 0) {
        const int t = p[0] & 15, k = p[0] >> 4;
        p++;
        n--;
        if (t > 3)
            return fail(d, "bad table");
        if (marker == 0xDB) {
            const int bytes = k ? 128 : 64;
            if (n < bytes)
                return fail(d, "cut off");
            for (int i = 0; i < 64; i++)
                d->qt[t][i] = (uint16_t)(k ? u16be(p + i * 2) : p[i]);
            d->qt_set[t] = true;
            p += bytes;
            n -= bytes;
        } else {
            int total = 0;
            if (n < 16)
                return fail(d, "cut off");
            for (int i = 0; i < 16; i++)
                total += p[i];
            if (total > 256 || n < 16 + total)
                return fail(d, "cut off");
            if (!make_huff(k ? &d->ac[t] : &d->dc[t], p, p + 16, total))
                return fail(d, "bad table");
            p += 16 + total;
            n -= 16 + total;
        }
    }
    return true;
}

static bool read_scan(Dec *d, const uint8_t *p, int n)
{
    if (!d->frame)
        return fail(d, "no picture header");
    const int ns = n > 0 ? p[0] : 0;
    if (ns < 1 || ns > d->ncomp || n < 4 + 2 * ns)
        return fail(d, "bad scan");
    d->ss = p[1 + 2 * ns];
    d->se = p[2 + 2 * ns];
    d->ah = p[3 + 2 * ns] >> 4;
    d->al = p[3 + 2 * ns] & 15;
    if (!d->progressive) {
        d->ss = d->ah = d->al = 0;
        d->se = 63;
    }
    if (d->ss > d->se || d->se > 63 || d->al > 13 || (d->progressive && d->ss == 0 && d->se != 0) || (d->ss && ns != 1))
        return fail(d, "bad scan");
    Comp *sc[3];
    for (int i = 0; i < ns; i++) {
        sc[i] = NULL;
        for (int k = 0; k < d->ncomp; k++)
            if (d->c[k].id == p[1 + i * 2])
                sc[i] = &d->c[k];
        for (int k = 0; k < i && sc[i]; k++)
            if (sc[k] == sc[i])
                sc[i] = NULL;
        if (!sc[i])
            return fail(d, "bad scan");
        sc[i]->td = p[2 + i * 2] >> 4;
        sc[i]->ta = p[2 + i * 2] & 15;
        if (sc[i]->td > 3 || sc[i]->ta > 3)
            return fail(d, "bad scan");
        if ((d->ss == 0 && d->ah == 0 && !d->dc[sc[i]->td].count) || ((d->ss || !d->progressive) && !d->ac[sc[i]->ta].count) ||
            (!d->progressive && !d->qt_set[sc[i]->tq]))
            return fail(d, "tables missing");
    }
    d->scanned = true;
    return d->dc_only && d->ss ? true : scan(d, sc, ns); /* details a 1/8 size picture does not need */
}

static bool read_file(Dec *d)
{
    if (d->end - d->p < 4 || d->p[0] != 0xFF || d->p[1] != 0xD8)
        return fail(d, "not a JPG");
    d->p += 2;
    while (d->end - d->p >= 2) {
        if (d->p[0] != 0xFF || d->p[1] == 0xFF || d->p[1] == 0 || (d->p[1] & 0xF8) == 0xD0) {
            d->p++; /* the rest of a scan */
            continue;
        }
        const int m = d->p[1];
        if (m == 0xD9)
            break;
        d->p += 2;
        if (m == 0x01 || m == 0xD8)
            continue;
        if (d->end - d->p < 2)
            break;
        const int n = u16be(d->p) - 2;
        const uint8_t *s = d->p + 2;
        if (n < 0 || n > d->end - s)
            break;
        d->p = s + n;
        bool ok = true;
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            d->progressive = m == 0xC2;
            ok = read_frame(d, s, n);
        } else if (m == 0xC4 || m == 0xDB) {
            ok = read_tables(d, m, s, n);
        } else if (m >= 0xC3 && m <= 0xCF && m != 0xC8 && m != 0xCC) {
            ok = fail(d, "this kind of JPG is not supported");
        } else if (m == 0xDD) {
            d->restart = n >= 2 ? u16be(s) : 0;
        } else if (m == 0xEE && n >= 12 && !memcmp(s, "Adobe", 5)) {
            d->adobe = s[11];
        } else if (m == 0xDA) {
            ok = read_scan(d, s, n);
        }
        if (!ok)
            return false;
    }
    return d->scanned ? true : fail(d, "cut off");
}

/* progressive: the collected coefficients -> pixels */
static bool finish(Dec *d)
{
    for (int i = 0; i < d->ncomp; i++) {
        Comp *c = &d->c[i];
        const uint16_t *q = d->qt[c->tq];
        if (!d->qt_set[c->tq])
            return fail(d, "tables missing");
        for (int by = 0; by < c->bh; by++) {
            if (*d->quit)
                return fail(d, "stopped");
            for (int bx = 0; bx < c->bw; bx++) {
                const size_t b = (size_t)by * c->bw + bx;
                int32_t blk[64] = {0};
                if (d->dc_only)
                    blk[0] = c->coef[b] * q[0];
                else
                    for (int k = 0; k < 64; k++)
                        blk[k_zigzag[k]] = c->coef[b * 64 + k_zigzag[k]] * q[k];
                idct(d, blk, c->plane + ((size_t)by * d->N * c->bw + bx) * d->N, c->bw * d->N);
            }
        }
        free(c->coef);
        c->coef = NULL;
    }
    return true;
}

static uint8_t clamp8(float v) { return v <= 0.0f ? 0 : v >= 255.0f ? 255 : (uint8_t)(v + 0.5f); }

static uint8_t *to_rgba(Dec *d, int w, int h)
{
    uint8_t *out = malloc((size_t)w * h * 4), *o = out;
    if (!out) {
        fail(d, "too large for the memory");
        return NULL;
    }
    const bool rgb = d->ncomp == 3 && (d->adobe == 0 || (d->adobe < 0 && d->c[0].id == 'R' && d->c[1].id == 'G' && d->c[2].id == 'B'));
    for (int y = 0; y < h; y++) {
        const uint8_t *row[3];
        for (int i = 0; i < d->ncomp; i++)
            row[i] = d->c[i].plane + (size_t)(y * d->c[i].v / d->vmax) * d->c[i].bw * d->N;
        for (int x = 0; x < w; x++, o += 4) {
            uint8_t v[3];
            for (int i = 0; i < d->ncomp; i++)
                v[i] = row[i][x * d->c[i].h / d->hmax];
            o[3] = 255;
            if (d->ncomp == 1) {
                o[0] = o[1] = o[2] = v[0];
            } else if (rgb) {
                memcpy(o, v, 3);
            } else {
                const float l = v[0], cb = (float)v[1] - 128.0f, cr = (float)v[2] - 128.0f;
                o[0] = clamp8(l + 1.402f * cr);
                o[1] = clamp8(l - 0.344136f * cb - 0.714136f * cr);
                o[2] = clamp8(l + 1.772f * cb);
            }
        }
    }
    return out;
}

uint8_t *jpg_decode(const uint8_t *data, size_t n, int *w, int *h, const volatile bool *quit, const char **err)
{
    Dec *d = calloc(1, sizeof(*d));
    uint8_t *out = NULL;
    *err = "too large for the memory";
    if (!d)
        return NULL;
    d->p = data;
    d->end = data + n;
    d->adobe = -1;
    d->quit = quit;
    if (read_file(d) && (!d->progressive || finish(d))) {
        *w = (d->w * d->N + 7) / 8;
        *h = (d->h * d->N + 7) / 8;
        out = to_rgba(d, *w, *h);
    }
    *err = d->err;
    for (int i = 0; i < 3; i++) {
        free(d->c[i].coef);
        free(d->c[i].plane);
    }
    free(d);
    return out;
}
