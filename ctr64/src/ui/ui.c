#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- framebuffer ------------------------------------------------------------------------------
 * The bottom LCD's framebuffer is 240 pixels per column, columns left to right, each column from
 * the bottom of the screen to the top: screen pixel (x, y) is fb[x * 240 + 239 - y]. */

static u16 *g_fb;
static int g_cx0, g_cy0, g_cx1 = UI_W, g_cy1 = UI_H;

/* ---- themes ----------------------------------------------------------------------------------- */

#define P(r, g, b) UI_RGB(0x##r, 0x##g, 0x##b)
static const UiCol k_light[UIP_COUNT] = {
    [UIP_BG_TOP] = P(F4, F4, F4),   [UIP_BG_BOT] = P(DC, DC, DC),     [UIP_STRIPE] = P(E9, E9, E9),
    [UIP_HEAD_TOP] = P(FF, FF, FF), [UIP_HEAD_BOT] = P(E2, E2, E2),   [UIP_LINE] = P(B8, B8, B8),
    [UIP_SHADOW] = P(C4, C4, C4),   [UIP_CARD] = P(FF, FF, FF),       [UIP_CARD_BOT] = P(F0, F0, F0),
    [UIP_TEXT] = P(3A, 3A, 3A),     [UIP_TEXT_DIM] = P(8A, 8A, 8A),   [UIP_VALUE] = P(1A, 84, C4),
    [UIP_SEL_FILL] = P(E4, F6, FE), [UIP_SEL_FILL_BOT] = P(D0, EE, FC), [UIP_FOOT_TOP] = P(6A, 6A, 6A),
    [UIP_FOOT_BOT] = P(46, 46, 46), [UIP_FOOT_LINE] = P(8C, 8C, 8C),  [UIP_TRACK] = P(C8, C8, C8),
    [UIP_THUMB] = P(80, 80, 80),    [UIP_KNOB] = P(EC, EC, EE),       [UIP_KNOB_SEL] = P(C8, EC, FC),
    [UIP_KNOB_ARROW] = P(A0, A0, A6), [UIP_DIVIDER] = P(E0, E0, E2), [UIP_TILE_GREY] = P(A8, A8, B0),
    [UIP_TILE_GREY_BOT] = P(7C, 7C, 86),
};
/* dark: charcoal backgrounds, raised slate cards, the same cyan accent */
static const UiCol k_dark[UIP_COUNT] = {
    [UIP_BG_TOP] = P(2A, 2B, 30),   [UIP_BG_BOT] = P(1C, 1D, 21),     [UIP_STRIPE] = P(24, 25, 2A),
    [UIP_HEAD_TOP] = P(42, 43, 49), [UIP_HEAD_BOT] = P(33, 34, 3A),   [UIP_LINE] = P(14, 14, 17),
    [UIP_SHADOW] = P(12, 12, 15),   [UIP_CARD] = P(3E, 40, 46),       [UIP_CARD_BOT] = P(35, 37, 3D),
    [UIP_TEXT] = P(EE, EE, F0),     [UIP_TEXT_DIM] = P(9C, 9E, A6),   [UIP_VALUE] = P(5C, C6, F8),
    [UIP_SEL_FILL] = P(1E, 4A, 62), [UIP_SEL_FILL_BOT] = P(18, 3C, 52), [UIP_FOOT_TOP] = P(3A, 3B, 40),
    [UIP_FOOT_BOT] = P(22, 23, 27), [UIP_FOOT_LINE] = P(50, 51, 57),  [UIP_TRACK] = P(3A, 3B, 41),
    [UIP_THUMB] = P(8A, 8C, 94),    [UIP_KNOB] = P(52, 54, 5B),       [UIP_KNOB_SEL] = P(1E, 5A, 78),
    [UIP_KNOB_ARROW] = P(B4, B6, BC), [UIP_DIVIDER] = P(50, 52, 58), [UIP_TILE_GREY] = P(70, 72, 7A),
    [UIP_TILE_GREY_BOT] = P(52, 54, 5C),
};
#undef P
UiCol g_ui_pal[UIP_COUNT];
static bool g_dark;
__attribute__((constructor)) static void theme_default(void) { memcpy(g_ui_pal, k_light, sizeof(g_ui_pal)); }

void ui_set_dark(bool dark)
{
    g_dark = dark;
    memcpy(g_ui_pal, dark ? k_dark : k_light, sizeof(g_ui_pal));
}

bool ui_is_dark(void)
{
    return g_dark;
}

/* Everything is drawn into this canvas, which keeps the whole image between draws (pages redraw
 * only what changes). ui_present copies the finished image into the bottom screen's back buffer
 * and swaps at the next VBlank: drawing straight into the buffer on screen showed every partial
 * redraw (flicker while scrolling, and each stats update during play). */
static u16 g_canvas[UI_W * UI_H] __attribute__((aligned(32)));
static PrintConsole *g_console;

void ui_set_console(PrintConsole *c)
{
    g_console = c;
    if (c && g_fb)
        c->frameBuffer = g_fb;
}

const u16 *ui_canvas(unsigned *bytes)
{
    *bytes = sizeof(g_canvas);
    return g_canvas;
}

PrintConsole *ui_console(void)
{
    return g_console;
}

void ui_init(void)
{
    if (g_fb)
        return;
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    /* start from what is on screen (the console's text, if any) */
    memcpy(g_canvas, gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL), sizeof(g_canvas));
    g_fb = g_canvas;
    if (g_console)
        g_console->frameBuffer = g_fb; /* debug text (libctru console) goes through the canvas too */
}

bool ui_ready(void) { return g_fb != NULL; }

void ui_present(void)
{
    if (!g_fb)
        return;
    /* the back buffer may still wait to be shown (two presents in one frame): it is the one the
     * LCD reads next, so let that VBlank pass first */
    if (gspIsPresentPending(GSP_SCREEN_BOTTOM))
        gspWaitForVBlank();
    u16 *back = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    memcpy(back, g_canvas, sizeof(g_canvas));
    GSPGPU_FlushDataCache(back, sizeof(g_canvas));
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
}

void ui_clip(int x, int y, int w, int h)
{
    g_cx0 = x < 0 ? 0 : x;
    g_cy0 = y < 0 ? 0 : y;
    g_cx1 = x + w > UI_W ? UI_W : x + w;
    g_cy1 = y + h > UI_H ? UI_H : y + h;
}

void ui_noclip(void) { ui_clip(0, 0, UI_W, UI_H); }

static inline u16 *px(int x, int y) { return g_fb + x * UI_H + (UI_H - 1 - y); }

/* 565 blend: a = 0..32 */
static inline u16 blend(u16 d, u16 s, u32 a)
{
    const u32 dd = (d | ((u32)d << 16)) & 0x07E0F81Fu;
    const u32 ss = (s | ((u32)s << 16)) & 0x07E0F81Fu;
    const u32 r = ((ss * a + dd * (32 - a)) >> 5) & 0x07E0F81Fu;
    return (u16)(r | (r >> 16));
}

static inline void plot(int x, int y, u16 c, u32 a)
{
    if (x < g_cx0 || y < g_cy0 || x >= g_cx1 || y >= g_cy1 || a == 0)
        return;
    u16 *p = px(x, y);
    *p = a >= 32 ? c : blend(*p, c, a);
}

static inline UiCol lerp_col(UiCol a, UiCol b, int t /* 0..32 */) { return blend(a, b, (u32)t); }

/* one horizontal run */
static void hrun(int x0, int x1, int y, u16 c)
{
    if (y < g_cy0 || y >= g_cy1)
        return;
    if (x0 < g_cx0) x0 = g_cx0;
    if (x1 > g_cx1) x1 = g_cx1;
    for (int x = x0; x < x1; x++)
        *px(x, y) = c;
}

void ui_fill(int x, int y, int w, int h, UiCol c)
{
    int x0 = x < g_cx0 ? g_cx0 : x, x1 = x + w > g_cx1 ? g_cx1 : x + w;
    int y0 = y < g_cy0 ? g_cy0 : y, y1 = y + h > g_cy1 ? g_cy1 : y + h;
    if (!g_fb || x0 >= x1 || y0 >= y1)
        return;
    for (int xx = x0; xx < x1; xx++) {
        u16 *p = px(xx, y1 - 1); /* the column runs upwards in memory */
        for (int n = y1 - y0; n > 0; n--)
            *p++ = c;
    }
}

void ui_blend_fill(int x, int y, int w, int h, UiCol c, int alpha)
{
    int x0 = x < g_cx0 ? g_cx0 : x, x1 = x + w > g_cx1 ? g_cx1 : x + w;
    int y0 = y < g_cy0 ? g_cy0 : y, y1 = y + h > g_cy1 ? g_cy1 : y + h;
    if (!g_fb || x0 >= x1 || y0 >= y1)
        return;
    for (int xx = x0; xx < x1; xx++) {
        u16 *p = px(xx, y1 - 1);
        for (int n = y1 - y0; n > 0; n--, p++)
            *p = blend(*p, c, (u32)alpha);
    }
}

void ui_vgrad(int x, int y, int w, int h, UiCol top, UiCol bottom)
{
    for (int j = 0; j < h; j++)
        hrun(x, x + w, y + j, lerp_col(top, bottom, h > 1 ? j * 32 / (h - 1) : 0));
}

/* coverage (0..32) of pixel (px, py) by a disk of radius r around (cx, cy) */
static inline u32 disk_cov(float pxc, float pyc, float cx, float cy, float r)
{
    const float dx = pxc - cx, dy = pyc - cy;
    const float d = sqrtf(dx * dx + dy * dy);
    float c = r - d + 0.5f;
    if (c <= 0.0f) return 0;
    if (c >= 1.0f) return 32;
    return (u32)(c * 32.0f + 0.5f);
}

void ui_rrect(int x, int y, int w, int h, int r, UiCol top, UiCol bottom)
{
    if (!g_fb || w <= 0 || h <= 0)
        return;
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++) {
        const u16 c = lerp_col(top, bottom, h > 1 ? j * 32 / (h - 1) : 0);
        const int yy = y + j;
        if (j >= r && j < h - r) {
            hrun(x, x + w, yy, c);
            continue;
        }
        /* a corner row: the circle centres are r in from the edges */
        const float cy = j < r ? (float)(y + r) : (float)(y + h - r);
        const float pyc = (float)yy + 0.5f;
        for (int i = 0; i < r; i++) {
            const u32 a = disk_cov((float)(x + i) + 0.5f, pyc, (float)(x + r), cy, (float)r);
            plot(x + i, yy, c, a);
            plot(x + w - 1 - i, yy, c, a);
        }
        hrun(x + r, x + w - r, yy, c);
    }
}

/* signed distance from (pxc, pyc) to a rounded rectangle (negative inside) */
static float rr_sdf(float pxc, float pyc, float x, float y, float w, float h, float r)
{
    const float hx = w * 0.5f, hy = h * 0.5f;
    float qx = fabsf(pxc - (x + hx)) - (hx - r), qy = fabsf(pyc - (y + hy)) - (hy - r);
    const float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
    const float in = (qx > qy ? qx : qy);
    return sqrtf(mx * mx + my * my) + (in < 0 ? in : 0) - r;
}

void ui_rrect_line(int x, int y, int w, int h, int r, float thick, UiCol c, int alpha)
{
    if (!g_fb || w <= 0 || h <= 0)
        return;
    const int band = (int)ceilf(thick) + 1;
    for (int j = 0; j < h; j++) {
        const int yy = y + j;
        const bool edge_row = j < band || j >= h - band || j < r || j >= h - r;
        for (int i = 0; i < w; i++) {
            if (!edge_row && i == band) {
                i = w - band - 1; /* skip the inside */
                continue;
            }
            const float d = rr_sdf((float)(x + i) + 0.5f, (float)yy + 0.5f, (float)x, (float)y, (float)w,
                                   (float)h, (float)r);
            /* stroke inside the edge: d in [-thick, 0] */
            float cov = 0.5f - fabsf(d + thick * 0.5f) + thick * 0.5f;
            if (cov <= 0.0f)
                continue;
            if (cov > 1.0f) cov = 1.0f;
            plot(x + i, yy, c, (u32)(cov * (float)alpha + 0.5f));
        }
    }
}

void ui_disk(float cx, float cy, float r, UiCol c)
{
    const int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1);
    const int y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            plot(x, y, c, disk_cov((float)x + 0.5f, (float)y + 0.5f, cx, cy, r));
}

void ui_ring(float cx, float cy, float r, float thick, UiCol c, int alpha)
{
    const int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1);
    const int y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            const float d = sqrtf(dx * dx + dy * dy) - r; /* stroke centred on r - thick/2 */
            float cov = 0.5f - fabsf(d + thick * 0.5f) + thick * 0.5f;
            if (cov <= 0.0f)
                continue;
            if (cov > 1.0f) cov = 1.0f;
            plot(x, y, c, (u32)(cov * (float)alpha + 0.5f));
        }
}

void ui_arrow(int cx, int cy, int size, int dir, UiCol c)
{
    /* triangle with apex towards dir, 4x4 supersampled */
    const float s = (float)size;
    float ax, ay, bx, by, qx, qy; /* apex, two base corners */
    switch (dir) {
    case -1: ax = cx - s * 0.5f; ay = cy; bx = cx + s * 0.4f; by = cy - s * 0.6f; qx = bx; qy = cy + s * 0.6f; break;
    case 1: ax = cx + s * 0.5f; ay = cy; bx = cx - s * 0.4f; by = cy - s * 0.6f; qx = bx; qy = cy + s * 0.6f; break;
    case -2: ax = cx; ay = cy - s * 0.5f; bx = cx - s * 0.6f; by = cy + s * 0.4f; qx = cx + s * 0.6f; qy = by; break;
    default: ax = cx; ay = cy + s * 0.5f; bx = cx - s * 0.6f; by = cy - s * 0.4f; qx = cx + s * 0.6f; qy = by; break;
    }
    const int x0 = cx - size, x1 = cx + size, y0 = cy - size, y1 = cy + size;
    const float area = (bx - ax) * (qy - ay) - (by - ay) * (qx - ax);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            int n = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    const float px_ = (float)x + (sx + 0.5f) * 0.25f, py_ = (float)y + (sy + 0.5f) * 0.25f;
                    const float w0 = ((bx - px_) * (qy - py_) - (by - py_) * (qx - px_)) / area;
                    const float w1 = ((qx - px_) * (ay - py_) - (qy - py_) * (ax - px_)) / area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 >= 0 && w1 >= 0 && w2 >= 0)
                        n++;
                }
            plot(x, y, c, (u32)(n * 2));
        }
}

/* ---- text (the system font) -------------------------------------------------------------------
 * Glyphs are decoded from the shared-font sheets, scaled with an area (box) filter and cached per
 * size, code point and quarter-pixel pen position, so letters keep their exact spacing instead of
 * snapping to whole pixels one by one. Side bearings are measured from the ink and capped: the
 * open-source font of emulators pads every glyph twice (cell and metrics), which reads as gaps
 * inside words at these sizes; Nintendo's font is unaffected (its bearings are below the cap). */

static const float k_scale[UI_SIZES] = {0.48f, 0.56f, 0.66f, 0.85f};
#define MAX_BEARING 1.25f /* texels */

typedef struct {
    u32 key;   /* code point << 2 | pen phase (quarter pixels) */
    s8 x0;     /* bitmap left edge relative to the pen's whole pixel */
    u8 w, h;
    u16 adv16; /* advance in 1/16 pixel */
    u8 *a;     /* w * h coverage 0..32 */
} Glyph;

#define CACHE_SLOTS 512
static Glyph g_cache[UI_SIZES][CACHE_SLOTS];
static bool g_font_ok, g_font_tried;
static TGLP_s *g_tglp;
static int g_line[UI_SIZES];
static float g_cap_mid = 14.0f; /* texel row halfway between cap top and baseline ('H') */

static int texel(const u8 *sheet, int x, int y);

static const u8 *glyph_cell(int gi, int *ox, int *oy)
{
    const int per = g_tglp->nRows * g_tglp->nLines, in = gi % per;
    *ox = (in % g_tglp->nRows) * (g_tglp->cellWidth + 1);
    *oy = (in / g_tglp->nRows) * (g_tglp->cellHeight + 1);
    return (const u8 *)fontGetGlyphSheetTex(NULL, gi / per);
}

static bool font_init(void)
{
    if (g_font_tried)
        return g_font_ok;
    g_font_tried = true;
    if (R_FAILED(fontEnsureMapped()))
        return false;
    g_tglp = fontGetGlyphInfo(NULL);
    if (!g_tglp || !g_tglp->sheetData)
        return false;
    const FINF_s *fi = fontGetInfo(NULL);
    for (int s = 0; s < UI_SIZES; s++)
        g_line[s] = (int)(fi->lineFeed * k_scale[s] + 0.5f);
    /* cap height from the ink of 'H' */
    int ox, oy, top = -1, bot = -1;
    const int gi = fontGlyphIndexFromCodePoint(NULL, 'H');
    const u8 *sh = glyph_cell(gi, &ox, &oy);
    for (int y = 0; y < g_tglp->cellHeight; y++)
        for (int x = 0; x < g_tglp->cellWidth; x++)
            if (texel(sh, ox + x, oy + y) > 128) {
                if (top < 0)
                    top = y;
                bot = y + 1;
                break;
            }
    if (top >= 0)
        g_cap_mid = (top + bot) * 0.5f;
    g_font_ok = true;
    return true;
}

/* sheet texel (x, y), y from the top of the sheet image: 4-bit (A4/L4) or 8-bit, PICA tiled
 * (8x8 tiles, Morton order inside), rows top-down (the font sheets are not flipped) */
static int texel(const u8 *sheet, int x, int y)
{
    const int W = g_tglp->sheetWidth;
    const int tile = (y >> 3) * (W >> 3) + (x >> 3);
    const int lx = x & 7, ly = y & 7;
    const int m = (lx & 1) | ((ly & 1) << 1) | ((lx & 2) << 1) | ((ly & 2) << 2) | ((lx & 4) << 2) | ((ly & 4) << 3);
    const int idx = tile * 64 + m;
    const u16 fmt = g_tglp->sheetFmt;
    if (fmt == GPU_A4 || fmt == GPU_L4) {
        const u8 b = sheet[idx >> 1];
        return ((idx & 1) ? (b >> 4) : (b & 15)) * 17;
    }
    if (fmt == GPU_A8 || fmt == GPU_L8)
        return sheet[idx];
    if (fmt == GPU_LA4)
        return (sheet[idx] & 15) * 17;
    return 0;
}

/* box-filter taps of the output pixel span [a, b) (texel units) over texels [lo, hi) */
static int taps(float a, float b, int lo, int hi, int *t, float *w)
{
    int n = 0;
    for (int k = (int)floorf(a); k < b && n < 6; k++) {
        if (k < lo || k >= hi)
            continue;
        const float o = (b < k + 1 ? b : k + 1) - (a > k ? a : k);
        if (o > 0) {
            t[n] = k;
            w[n++] = o;
        }
    }
    return n;
}

static Glyph *glyph(int size, u32 cp, int phase)
{
    const u32 key = cp << 2 | (u32)phase;
    unsigned h = (key * 2654435761u) >> 23;
    for (int n = 0; n < CACHE_SLOTS; n++, h = (h + 1) & (CACHE_SLOTS - 1)) {
        Glyph *g = &g_cache[size][h];
        if (g->a && g->key == key)
            return g;
        if (g->a)
            continue;
        /* render it */
        const int gi = fontGlyphIndexFromCodePoint(NULL, cp);
        const charWidthInfo_s *cw = fontGetCharWidthInfo(NULL, gi);
        int ox, oy;
        const u8 *sheet = glyph_cell(gi, &ox, &oy);
        const float s = k_scale[size];
        const int gw = cw->glyphWidth < g_tglp->cellWidth ? cw->glyphWidth : g_tglp->cellWidth;
        const int ch = g_tglp->cellHeight;
        /* ink columns */
        int il = gw, ir = 0;
        for (int x = 0; x < gw; x++)
            for (int y = 0; y < ch; y++)
                if (texel(sheet, ox + x, oy + y)) {
                    if (x < il)
                        il = x;
                    ir = x + 1;
                    break;
                }
        g->key = key;
        if (ir <= il) { /* blank (space) */
            g->a = (u8 *)malloc(1);
            if (!g->a)
                return NULL;
            g->w = g->h = 0;
            g->x0 = 0;
            g->adv16 = (u16)(cw->charWidth * s * 16.0f + 0.5f);
            return g;
        }
        float lsb = (float)(cw->left + il), rsb = (float)(cw->charWidth - cw->left - ir);
        if (lsb > MAX_BEARING) lsb = MAX_BEARING;
        if (rsb > MAX_BEARING) rsb = MAX_BEARING;
        const float inkw = (float)(ir - il);
        g->adv16 = (u16)((lsb + inkw + rsb) * s * 16.0f + 0.5f);
        const float start = phase * 0.25f + lsb * s; /* ink start, pixels from the pen's whole pixel */
        const int x0 = (int)floorf(start);
        int w = (int)ceilf(start + inkw * s) - x0, hh = (int)ceilf(ch * s);
        if (w < 1) w = 1;
        if (hh < 1) hh = 1;
        g->a = (u8 *)malloc((size_t)w * hh);
        if (!g->a)
            return NULL;
        g->x0 = (s8)x0;
        g->w = (u8)w;
        g->h = (u8)hh;
        const float inv = 1.0f / s, norm = s * s * (32.0f * 1.1f / 255.0f);
        for (int y = 0; y < hh; y++) {
            int ty[6];
            float wy[6];
            const int ny = taps(y * inv, (y + 1) * inv, 0, ch, ty, wy);
            for (int x = 0; x < w; x++) {
                int cx[6];
                float cwx[6];
                const float a = (x0 + x - start) * inv + il;
                const int ncx = taps(a, a + inv, il, ir, cx, cwx);
                float sum = 0;
                for (int j = 0; j < ny; j++) {
                    float row = 0;
                    for (int i = 0; i < ncx; i++)
                        row += cwx[i] * (float)texel(sheet, ox + cx[i], oy + ty[j]);
                    sum += row * wy[j];
                }
                int v = (int)(sum * norm + 0.5f);
                g->a[y * w + x] = (u8)(v > 32 ? 32 : v);
            }
        }
        return g;
    }
    return NULL;
}

static u32 utf8_next(const char **ps)
{
    const u8 *s = (const u8 *)*ps;
    u32 c = *s++;
    if (c >= 0xF0 && s[0] && s[1] && s[2]) {
        c = ((c & 7) << 18) | ((s[0] & 63) << 12) | ((s[1] & 63) << 6) | (s[2] & 63);
        s += 3;
    } else if (c >= 0xE0 && s[0] && s[1]) {
        c = ((c & 15) << 12) | ((s[0] & 63) << 6) | (s[1] & 63);
        s += 2;
    } else if (c >= 0xC0 && s[0]) {
        c = ((c & 31) << 6) | (s[0] & 63);
        s += 1;
    }
    *ps = (const char *)s;
    return c;
}

int ui_line_height(int size)
{
    return font_init() ? g_line[size] : 12;
}

int ui_text_y(int size, int cy)
{
    font_init();
    return cy - (int)floorf(g_cap_mid * k_scale[size] + 0.5f);
}

int ui_text_width(int size, const char *s)
{
    if (!font_init())
        return 0;
    int adv = 0;
    while (*s) {
        const Glyph *g = glyph(size, utf8_next(&s), 0);
        if (g)
            adv += g->adv16;
    }
    return (adv + 15) >> 4;
}

static int draw_text(int x, int y, int size, UiCol c, const char *s, int max_w)
{
    if (!g_fb || !font_init())
        return 0;
    const int pen0 = x * 16;
    int pen = pen0;
    while (*s) {
        const Glyph *g = glyph(size, utf8_next(&s), (pen & 15) >> 2);
        if (!g)
            continue;
        if (max_w >= 0 && ((pen - pen0 + g->adv16 + 15) >> 4) > max_w)
            break;
        const int gx = (pen >> 4) + g->x0;
        for (int j = 0; j < g->h; j++) {
            const int yy = y + j;
            if (yy < g_cy0 || yy >= g_cy1)
                continue;
            const u8 *a = g->a + j * g->w;
            for (int i = 0; i < g->w; i++)
                if (a[i])
                    plot(gx + i, yy, c, a[i]);
        }
        pen += g->adv16;
    }
    return (pen - pen0 + 15) >> 4;
}

int ui_text(int x, int y, int size, UiCol c, int align, const char *s)
{
    if (align != UI_LEFT) {
        const int w = ui_text_width(size, s);
        x -= align == UI_CENTER ? w / 2 : w;
    }
    return draw_text(x, y, size, c, s, -1);
}

int ui_text_mask(int size, const char *utf8, int max_w, u8 *out, int w, int h)
{
    enum { MAX_H = 48 };
    static u16 save[UI_W * MAX_H];
    if (!g_fb || w <= 0 || w > UI_W || h <= 0 || h > MAX_H)
        return 0;
    /* the top h rows of the first w columns: px(x, h-1) .. px(x, 0) are contiguous */
    for (int x = 0; x < w; x++)
        memcpy(&save[x * h], px(x, h - 1), h * sizeof(u16));
    const int cx0 = g_cx0, cy0 = g_cy0, cx1 = g_cx1, cy1 = g_cy1;
    ui_clip(0, 0, w, h);
    ui_fill(0, 0, w, h, 0);
    const int y = (h - ui_line_height(size)) / 2;
    const int tw = ui_text_width(size, utf8);
    if (tw <= max_w)
        ui_text((w - tw) / 2, y, size, UI_WHITE, UI_LEFT, utf8);
    else
        ui_text_fit((w - max_w) / 2, y, size, UI_WHITE, UI_LEFT, max_w, utf8);
    for (int yy = 0; yy < h; yy++)
        for (int x = 0; x < w; x++) {
            const u16 c = *px(x, yy);
            out[yy * w + x] = (u8)(((c >> 5) & 0x3F) * 255 / 63); /* green: 6 bits */
        }
    for (int x = 0; x < w; x++)
        memcpy(px(x, h - 1), &save[x * h], h * sizeof(u16));
    g_cx0 = cx0, g_cy0 = cy0, g_cx1 = cx1, g_cy1 = cy1;
    return tw < max_w ? tw : max_w;
}

int ui_textf(int x, int y, int size, UiCol c, int align, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return ui_text(x, y, size, c, align, buf);
}

int ui_text_fit(int x, int y, int size, UiCol c, int align, int max_w, const char *s)
{
    const int w = ui_text_width(size, s);
    if (w <= max_w)
        return ui_text(x, y, size, c, align, s);
    const int dots = ui_text_width(size, "...");
    if (align == UI_CENTER)
        x -= max_w / 2;
    else if (align == UI_RIGHT)
        x -= max_w;
    const int n = draw_text(x, y, size, c, s, max_w - dots);
    draw_text(x + n, y, size, c, "...", -1);
    return max_w;
}

/* ---- images ----------------------------------------------------------------------------------- */

void ui_image(const UiImage *img, int x, int y)
{
    if (!g_fb)
        return;
    const u8 *p = img->rle;
    const int total = img->w * img->h;
    int i = 0;
    while (i < total) {
        const u8 c = *p++;
        const int n = (c & 0x7F) + 1;
        if (c & 0x80) {
            const u8 v = *p++;
            if (img->alpha[v])
                for (int k = 0; k < n; k++)
                    plot(x + (i + k) % img->w, y + (i + k) / img->w, img->palette[v], img->alpha[v]);
            i += n;
        } else {
            for (int k = 0; k < n; k++, i++) {
                const u8 v = *p++;
                if (img->alpha[v])
                    plot(x + i % img->w, y + i / img->w, img->palette[v], img->alpha[v]);
            }
        }
    }
}

void ui_mask(const UiMask *m, int x, int y, UiCol c)
{
    if (!g_fb)
        return;
    const u8 *p = m->rle;
    const int total = m->w * m->h;
    for (int i = 0; i < total;) {
        const u8 r = *p++;
        const int n = (r & 0x7F) + 1;
        if (r & 0x80) {
            const u8 v = *p++;
            if (v)
                for (int k = 0; k < n; k++)
                    plot(x + (i + k) % m->w, y + (i + k) / m->w, c, v);
            i += n;
        } else {
            for (int k = 0; k < n; k++, i++) {
                const u8 v = *p++;
                if (v)
                    plot(x + i % m->w, y + i / m->w, c, v);
            }
        }
    }
}

/* ---- the screen frame --------------------------------------------------------------------------- */

void ui_background(int y, int h)
{
    for (int j = y; j < y + h; j++) {
        UiCol c = lerp_col(UI_BG_TOP, UI_BG_BOT, j * 32 / (UI_H - 1));
        if (j % 4 == 3)
            c = blend(c, UI_STRIPE, 10); /* the faint pinstripes of the system menus */
        hrun(0, UI_W, j, c);
    }
}

void ui_header(const char *title, const char *right)
{
    ui_vgrad(0, 0, UI_W, UI_HEADER_H - 1, UI_HEAD_TOP, UI_HEAD_BOT);
    ui_fill(0, UI_HEADER_H - 1, UI_W, 1, UI_LINE);
    ui_blend_fill(0, UI_HEADER_H, UI_W, 2, UI_RGB(0, 0, 0), 3);
    const int cy = UI_HEADER_H / 2;
    const int tw = ui_text_width(UI_TITLE, title);
    int rw = 0;
    if (right && *right) {
        int room = UI_W - 30 - tw;
        room = room < 90 ? 90 : room;
        rw = ui_text_fit(UI_W - 10, ui_text_y(UI_SMALL, cy), UI_SMALL, UI_TEXT_DIM, UI_RIGHT, room, right) + 10;
    }
    const int size = tw <= UI_W - 20 - rw ? UI_TITLE : UI_BODY; /* long game names: one size down */
    ui_text_fit(10, ui_text_y(size, cy), size, UI_TEXT, UI_LEFT, UI_W - 20 - rw, title);
}

int ui_footer_begin(void)
{
    const int y = UI_H - UI_FOOTER_H;
    ui_vgrad(0, y, UI_W, UI_FOOTER_H, UI_FOOT_TOP, UI_FOOT_BOT);
    ui_fill(0, y, UI_W, 1, UI_FOOT_LINE);
    return 8;
}

int ui_footer_button(int x, const char *glyph_s, const char *label, bool right)
{
    const int ty = ui_text_y(UI_BODY, UI_H - UI_FOOTER_H / 2);
    const int gw = glyph_s ? ui_text_width(UI_BODY, glyph_s) + 3 : 0;
    const int w = gw + ui_text_width(UI_BODY, label);
    if (right)
        x -= w;
    if (glyph_s)
        ui_text(x, ty, UI_BODY, UI_WHITE, UI_LEFT, glyph_s);
    ui_text(x + gw, ty, UI_BODY, UI_WHITE, UI_LEFT, label);
    return right ? x - 14 : x + w + 14;
}

/* ---- input, and the development script ----------------------------------------------------------
 * sdmc:/3ds/ctr64/uiscript.txt (development aid): one command per line, run one per ui_scan call
 * ("wait N" spends N calls): key NAME (A B X Y L R ZL ZR START SELECT UP DOWN LEFT RIGHT) presses
 * and releases a key (NAME+NAME: several at once), touch X Y taps the screen there, drag X Y DY N
 * presses at (X, Y) and moves DY pixels down over N calls, dump NAME writes the bottom screen to
 * sdmc:/3ds/ctr64/ui_NAME.tga. Lets screens be checked in an emulator without a person. */

static FILE *g_script;
static bool g_script_tried;
static int g_wait;
static u32 g_inj_up;          /* keys to release on the next call */
static bool g_inj_touch;      /* a scripted touch is down this call */
static int g_inj_tx, g_inj_ty;
static int g_drag_left, g_drag_dy; /* "drag": calls to go, movement per call */

static u32 key_by_name(const char *n)
{
    static const struct { const char *n; u32 k; } keys[] = {
        {"A", KEY_A}, {"B", KEY_B}, {"X", KEY_X}, {"Y", KEY_Y}, {"L", KEY_L}, {"R", KEY_R},
        {"ZL", KEY_ZL}, {"ZR", KEY_ZR}, {"START", KEY_START}, {"SELECT", KEY_SELECT},
        {"UP", KEY_DUP}, {"DOWN", KEY_DDOWN}, {"LEFT", KEY_DLEFT}, {"RIGHT", KEY_DRIGHT}};
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (!strcmp(n, keys[i].n))
            return keys[i].k;
    return 0;
}

void ui_scan(u32 *down, u32 *held, u32 *up)
{
    hidScanInput();
    *down = hidKeysDown();
    *held = hidKeysHeld();
    *up = hidKeysUp();
    if (!g_script_tried) {
        g_script_tried = true;
        g_script = fopen("sdmc:/3ds/ctr64/uiscript.txt", "r");
    }
    /* last call's scripted press ends now (a drag keeps the touch down) */
    *up |= g_inj_up;
    g_inj_up = 0;
    if (g_drag_left > 0) {
        g_drag_left--;
        g_inj_ty += g_drag_dy;
        *held |= KEY_TOUCH;
        return;
    }
    if (g_inj_touch)
        *up |= KEY_TOUCH;
    g_inj_touch = false;
    if (!g_script)
        return;
    if (g_wait > 0) {
        g_wait--;
        return;
    }
    char line[96], cmd[16], a[32];
    int x, y;
    while (fgets(line, sizeof(line), g_script)) {
        if (sscanf(line, "%15s", cmd) != 1)
            continue;
        if (!strcmp(cmd, "wait") && sscanf(line, "%*s %d", &x) == 1) {
            g_wait = x;
            return;
        }
        if (!strcmp(cmd, "key") && sscanf(line, "%*s %31s", a) == 1) {
            u32 k = 0; /* "key L+ZL": several at once */
            for (char *t = strtok(a, "+"); t; t = strtok(NULL, "+"))
                k |= key_by_name(t);
            *down |= k;
            *held |= k;
            g_inj_up = k;
            return;
        }
        if (!strcmp(cmd, "touch") && sscanf(line, "%*s %d %d", &x, &y) == 2) {
            *down |= KEY_TOUCH;
            *held |= KEY_TOUCH;
            g_inj_touch = true;
            g_inj_tx = x;
            g_inj_ty = y;
            return;
        }
        int dy, n;
        if (!strcmp(cmd, "drag") && sscanf(line, "%*s %d %d %d %d", &x, &y, &dy, &n) == 4 && n > 0) {
            *down |= KEY_TOUCH;
            *held |= KEY_TOUCH;
            g_inj_touch = true;
            g_inj_tx = x;
            g_inj_ty = y;
            g_drag_left = n;
            g_drag_dy = dy / n;
            return;
        }
        if (!strcmp(cmd, "dump") && sscanf(line, "%*s %31s", a) == 1) {
            char path[96];
            snprintf(path, sizeof(path), "sdmc:/3ds/ctr64/ui_%s.tga", a);
            ui_dump(path);
            continue;
        }
        if (!strcmp(cmd, "topdump") && sscanf(line, "%*s %31s", a) == 1) { /* the top screen */
            extern const void *pica_top_latest(unsigned *bytes);
            unsigned bytes = 0;
            const u8 *fb = pica_top_latest(&bytes); /* BGR8, 240x400 as the LCD scans it */
            char path[96];
            snprintf(path, sizeof(path), "sdmc:/3ds/ctr64/ui_%s.tga", a);
            FILE *f = fb && bytes >= 240 * 400 * 3 ? fopen(path, "wb") : NULL;
            if (f) {
                const u8 hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 400 & 255, 400 >> 8, 240, 0, 24, 0};
                fwrite(hdr, 1, sizeof(hdr), f);
                static u8 row[400 * 3];
                for (int y = 239; y >= 0; y--) { /* bottom-up */
                    for (int x = 0; x < 400; x++)
                        memcpy(&row[x * 3], &fb[(x * 240 + (239 - y)) * 3], 3);
                    fwrite(row, 1, sizeof(row), f);
                }
                fclose(f);
            }
            continue;
        }
    }
    fclose(g_script);
    g_script = NULL;
}

static void touch_read(int *x, int *y)
{
    if (g_inj_touch) {
        *x = g_inj_tx;
        *y = g_inj_ty;
        return;
    }
    touchPosition tp;
    hidTouchRead(&tp);
    *x = tp.px;
    *y = tp.py;
}

/* ---- touch ------------------------------------------------------------------------------------ */

void ui_touch_update(UiTouch *t, u32 down, u32 held, u32 up)
{
    t->down = (down & KEY_TOUCH) != 0;
    t->held = (held & KEY_TOUCH) != 0;
    t->up = (up & KEY_TOUCH) != 0;
    t->tap = false;
    t->dy = 0;
    if (t->down || t->held) {
        int nx, ny;
        touch_read(&nx, &ny);
        if (t->down) {
            t->sx = nx;
            t->sy = ny;
            t->dragging = false;
        } else if (t->held) {
            if (!t->dragging && (abs(nx - t->sx) > 6 || abs(ny - t->sy) > 6))
                t->dragging = true;
            if (t->dragging)
                t->dy = ny - t->y;
        }
        t->x = nx;
        t->y = ny;
    }
    if (t->up) {
        t->tap = !t->dragging;
        t->dragging = false;
    }
}

/* ---- development aid -------------------------------------------------------------------------- */

void ui_dump(const char *path)
{
    if (!g_fb)
        return;
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    const u8 hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, UI_W & 255, UI_W >> 8, UI_H & 255, UI_H >> 8, 24, 0};
    fwrite(hdr, 1, sizeof(hdr), f);
    static u8 row[UI_W * 3];
    for (int y = UI_H - 1; y >= 0; y--) { /* TGA: bottom row first */
        for (int x = 0; x < UI_W; x++) {
            const u16 c = *px(x, y);
            row[x * 3 + 0] = (u8)(((c & 31) * 255) / 31);
            row[x * 3 + 1] = (u8)((((c >> 5) & 63) * 255) / 63);
            row[x * 3 + 2] = (u8)(((c >> 11) * 255) / 31);
        }
        fwrite(row, 1, sizeof(row), f);
    }
    fclose(f);
}
