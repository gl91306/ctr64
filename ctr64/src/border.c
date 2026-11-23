/* Side borders next to a 4:3 game picture: see border.h.
 *
 * Only the two strips are built (2 x 40x240), on the CPU, when a game starts or the setting
 * changes, into a 128x256 RGB565 texture (64 KB of linear memory; two of them, so a new one is
 * written while the GPU may still read the old). GLideN64's present draws them as two quads over
 * the black bars of its window (video_draw_borders). */
#include "border.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pica/pica.h"
#include "rom_title.h"
#include "settings.h"

#define BORDER_DIR "sdmc:/3ds/ctr64/borders"
#define ART_DIR "sdmc:/3ds/ctr64/boxart"
#define ART_MAGIC 0x41343643u /* boxart.c cache file: "C64A" */
#define ART_TEX 256
#define W 400
#define H 240
#define SW BORDER_SIDE

u8 *boxart_read_image(const char *path, int *w, int *h); /* boxart.c */
int ctr64_aspect_applied(void);                            /* GLideN64: the running game's ASPECT_* */

typedef struct {
    float r, g, b;
} Rgb;

static struct {
    u16 *tex[2];
    volatile int cur; /* tex[] shown, -1 none */
    volatile bool fresh;
    char rom[256];
    Rgb *px; /* the strips being built: [side][y][x], side 0 left, 1 right */
} G = {.cur = -1};

static inline Rgb *px(int side, int y, int x) { return &G.px[((size_t)side * H + y) * SW + x]; }
/* screen column of strip pixel x */
static inline int scr_x(int side, int x) { return side ? W - SW + x : x; }

static inline u32 morton8(u32 x, u32 y)
{
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

static inline u32 tiled(u32 x, u32 y, u32 tw)
{
    return ((y >> 3) * (tw >> 3) + (x >> 3)) * 64 + morton8(x & 7, y & 7);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static Rgb rgb_hex(u32 c) { return (Rgb){(float)(c >> 16 & 0xFF), (float)(c >> 8 & 0xFF), (float)(c & 0xFF)}; }

static Rgb mix(Rgb a, Rgb b, float t)
{
    return (Rgb){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

static Rgb scale(Rgb a, float k) { return (Rgb){a.r * k, a.g * k, a.b * k}; }

/* ROM file name without its folder and ending */
static void rom_stem(char *out, size_t n, const char *path)
{
    const char *base = strrchr(path, '/');
    snprintf(out, n, "%s", base ? base + 1 : path);
    char *dot = strrchr(out, '.');
    if (dot)
        *dot = 0;
}

/* ---- the user's picture --------------------------------------------------------------------- */

static bool from_picture(const char *stem)
{
    static const char *const exts[] = {"png", "jpg", "jpeg"};
    const char *names[2] = {stem, "default"};
    for (int k = 0; k < 2; k++)
        for (int e = 0; e < 3; e++) {
            char path[320];
            snprintf(path, sizeof(path), BORDER_DIR "/%s.%s", names[k], exts[e]);
            int w = 0, h = 0;
            u8 *img = boxart_read_image(path, &w, &h);
            if (!img)
                continue;
            /* scaled to cover the screen, centred; each screen pixel the mean of the pixels it
             * covers (one pixel when enlarging) */
            const float s = fmaxf((float)W / w, (float)H / h);
            const float ox = (w * s - W) / 2, oy = (h * s - H) / 2;
            for (int side = 0; side < 2; side++)
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < SW; x++) {
                        const int X = scr_x(side, x);
                        int x0 = (int)((X + ox) / s), x1 = (int)ceilf((X + 1 + ox) / s);
                        int y0 = (int)((y + oy) / s), y1 = (int)ceilf((y + 1 + oy) / s);
                        x0 = x0 < 0 ? 0 : x0 >= w ? w - 1 : x0;
                        y0 = y0 < 0 ? 0 : y0 >= h ? h - 1 : y0;
                        x1 = x1 <= x0 ? x0 + 1 : x1 > w ? w : x1;
                        y1 = y1 <= y0 ? y0 + 1 : y1 > h ? h : y1;
                        Rgb acc = {0, 0, 0};
                        int cnt = 0;
                        for (int sy = y0; sy < y1; sy++)
                            for (int sx = x0; sx < x1; sx++, cnt++) {
                                const u8 *p = img + ((size_t)sy * w + sx) * 4;
                                const float a = p[3] / 255.0f; /* over black */
                                acc.r += p[0] * a;
                                acc.g += p[1] * a;
                                acc.b += p[2] * a;
                            }
                        *px(side, y, x) = scale(acc, 1.0f / cnt);
                    }
            free(img);
            return true;
        }
    return false;
}

/* ---- the game's box art, blurred -------------------------------------------------------------- */

/* the box art cache file of the game (boxart.c): its own picture, its file name key (hacks),
 * its cartridge ID */
static u16 *read_art(const char *rom_path, const char *stem, int *w, int *h)
{
    char safe[256], title[80], tags[64], id[5] = {0};
    snprintf(safe, sizeof(safe), "%s", stem);
    for (char *c = safe; *c; c++) /* as boxart.c's thumb_safe */
        if (strchr("&*/:`<>?\\|\"", *c))
            *c = '_';
    rom_title_read_id(rom_path, title, sizeof(title), tags, sizeof(tags), id);
    const bool hack = strstr(tags, "hack") != NULL;
    char paths[3][320];
    int n = 0;
    snprintf(paths[n++], sizeof(paths[0]), ART_DIR "/u_%s.bin", safe);
    if (hack || !id[0])
        snprintf(paths[n++], sizeof(paths[0]), ART_DIR "/f_%.60s.bin", safe);
    if (id[0])
        snprintf(paths[n++], sizeof(paths[0]), ART_DIR "/%s.bin", id);
    u16 *tex = malloc(ART_TEX * ART_TEX * 2);
    for (int i = 0; tex && i < n; i++) {
        for (char *c = paths[i] + sizeof(ART_DIR); *c; c++) /* FAT file names, as boxart.c's keys */
            if (strchr(":*?\"<>|", *c))
                *c = '_';
        FILE *f = fopen(paths[i], "rb");
        if (!f)
            continue;
        u32 hdr[4];
        const bool ok = fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == ART_MAGIC &&
                        fread(tex, ART_TEX * ART_TEX * 2, 1, f) == 1;
        fclose(f);
        *w = hdr[1] & 0xFFFF;
        *h = hdr[1] >> 16;
        if (ok && *w > 0 && *h > 0 && *w <= ART_TEX && *h <= ART_TEX)
            return tex;
    }
    free(tex);
    return NULL;
}

#define ZOOM 1.7f
enum { CELL = 16, GX = (W + CELL - 1) / CELL, GY = H / CELL };

static bool from_box_art(const char *rom_path, const char *stem)
{
    int w = 0, h = 0;
    u16 *tex = read_art(rom_path, stem, &w, &h);
    if (!tex)
        return false;
    /* the art covering the screen, zoomed further (ZOOM) so the sides show the middle of the box
     * instead of its outer edges (logo bands, white margins), averaged over CELL x CELL pixels */
    static Rgb grid[GY][GX], tmp[GY][GX];
    static float cnt[GY][GX];
    memset(grid, 0, sizeof(grid));
    memset(cnt, 0, sizeof(cnt));
    const float s = fmaxf((float)W / w, (float)H / h) * ZOOM;
    const float ox = (w * s - W) / 2, oy = (h * s - H) / 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const u16 v = tex[tiled(x, y, ART_TEX)];
            if (!(v & 1))
                continue; /* transparent */
            const int gx = (int)((x + 0.5f) * s - ox) / CELL, gy = (int)((y + 0.5f) * s - oy) / CELL;
            if (gx < 0 || gx >= GX || gy < 0 || gy >= GY)
                continue;
            grid[gy][gx].r += (v >> 11 & 31) * (255.0f / 31);
            grid[gy][gx].g += (v >> 6 & 31) * (255.0f / 31);
            grid[gy][gx].b += (v >> 1 & 31) * (255.0f / 31);
            cnt[gy][gx] += 1;
        }
    free(tex);
    Rgb mean = {0, 0, 0};
    float total = 0;
    for (int y = 0; y < GY; y++)
        for (int x = 0; x < GX; x++) {
            mean.r += grid[y][x].r;
            mean.g += grid[y][x].g;
            mean.b += grid[y][x].b;
            total += cnt[y][x];
        }
    mean = scale(mean, total > 0 ? 1.0f / total : 0);
    for (int y = 0; y < GY; y++)
        for (int x = 0; x < GX; x++)
            grid[y][x] = cnt[y][x] > 0 ? scale(grid[y][x], 1.0f / cnt[y][x]) : mean;
    /* three passes of a 3x3 blur over the cells */
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < GY; y++)
            for (int x = 0; x < GX; x++) {
                Rgb a = {0, 0, 0};
                float k = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        const int yy = y + dy, xx = x + dx;
                        if (yy < 0 || yy >= GY || xx < 0 || xx >= GX)
                            continue;
                        const float wt = (dx ? 1.0f : 2.0f) * (dy ? 1.0f : 2.0f);
                        a.r += grid[yy][xx].r * wt;
                        a.g += grid[yy][xx].g * wt;
                        a.b += grid[yy][xx].b * wt;
                        k += wt;
                    }
                tmp[y][x] = scale(a, 1.0f / k);
            }
        memcpy(grid, tmp, sizeof(grid));
    }
    /* smooth (smoothstep-weighted bilinear) between cell centres; darker and less saturated, so
     * the picture stays what the eye goes to */
    for (int side = 0; side < 2; side++)
        for (int y = 0; y < H; y++)
            for (int x = 0; x < SW; x++) {
                const float fx = clampf((scr_x(side, x) + 0.5f) / CELL - 0.5f, 0, GX - 1.001f);
                const float fy = clampf((y + 0.5f) / CELL - 0.5f, 0, GY - 1.001f);
                const int ix = (int)fx, iy = (int)fy;
                float tx = fx - ix, ty = fy - iy;
                tx = tx * tx * (3 - 2 * tx);
                ty = ty * ty * (3 - 2 * ty);
                const Rgb top = mix(grid[iy][ix], grid[iy][ix + 1], tx);
                const Rgb bot = mix(grid[iy + 1][ix], grid[iy + 1][ix + 1], tx);
                Rgb c = mix(top, bot, ty);
                const float lum = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
                c = mix(c, (Rgb){lum, lum, lum}, 0.35f);
                *px(side, y, x) = scale(c, 0.42f);
            }
    return true;
}

/* ---- Console: dark grey plastic, an embossed D-pad (left), A and B buttons (right) ------------ */

static float hash01(u32 x, u32 y)
{
    u32 h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

/* coverage of a disc at (cx, cy) radius r for the pixel at (x, y), with a one-pixel soft edge */
static float disc(float x, float y, float cx, float cy, float r)
{
    const float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
    return clampf(r + 0.5f - d, 0, 1);
}

/* inside a plus shape (centre cx, cy; arms `len` from the centre, `half` wide on each side) */
static bool in_cross(float x, float y, float cx, float cy, float len, float half)
{
    const float dx = fabsf(x - cx), dy = fabsf(y - cy);
    return (dx <= half && dy <= len) || (dy <= half && dx <= len);
}

static void console_style(void)
{
    const Rgb top = rgb_hex(0x35373C), bottom = rgb_hex(0x222327);
    for (int side = 0; side < 2; side++)
        for (int y = 0; y < H; y++)
            for (int x = 0; x < SW; x++) {
                Rgb c = mix(top, bottom, y / (float)(H - 1));
                /* fine horizontal ridges, like the console's top, and plastic grain */
                const int r = y % 6;
                c = scale(c, r == 0 ? 0.90f : r == 1 ? 1.05f : 1.0f);
                c = scale(c, 0.97f + 0.06f * hash01((u32)scr_x(side, x), (u32)y));
                *px(side, y, x) = c;
            }
    /* D-pad: a recessed cross, shadow on its top/left inner edges, light on bottom/right */
    const float dcx = SW / 2.0f, dcy = 196, len = 11, half = 3.5f;
    for (int y = 180; y < 214; y++)
        for (int x = 0; x < SW; x++) {
            const float fx = x + 0.5f, fy = y + 0.5f;
            if (!in_cross(fx, fy, dcx, dcy, len, half))
                continue;
            Rgb c = scale(rgb_hex(0x1A1B1E), 0.95f + 0.05f * hash01(x, y));
            if (!in_cross(fx - 1.5f, fy - 1.5f, dcx, dcy, len, half))
                c = scale(c, 0.6f); /* the shadow inside the top/left edge */
            else if (!in_cross(fx + 1, fy + 1, dcx, dcy, len, half))
                c = mix(c, rgb_hex(0x4A4D54), 0.7f); /* the lit bottom/right edge */
            *px(0, y, x) = c;
        }
    /* B (green, small, upper left) and A (blue, larger, lower right), dimmed */
    static const struct {
        float cx, cy, r;
        u32 col;
    } buttons[] = {{13, 186, 5.5f, 0x2F8F4E}, {25, 202, 7.0f, 0x3557B8}};
    for (int b = 0; b < 2; b++)
        for (int y = 170; y < 216; y++)
            for (int x = 0; x < SW; x++) {
                const float fx = x + 0.5f, fy = y + 0.5f;
                const float cx = buttons[b].cx, cy = buttons[b].cy, rr = buttons[b].r;
                const float rim = disc(fx, fy, cx, cy, rr + 1.2f); /* dark ring in the shell */
                const float a = disc(fx, fy, cx, cy, rr);
                if (rim <= 0)
                    continue;
                Rgb *p = px(1, y, x);
                *p = mix(*p, rgb_hex(0x141517), rim);
                Rgb cap = scale(rgb_hex(buttons[b].col), 0.75f);
                /* light from the top left */
                const float lit = clampf(1.0f - ((fx - (cx - rr * 0.35f)) * (fx - (cx - rr * 0.35f)) +
                                                 (fy - (cy - rr * 0.35f)) * (fy - (cy - rr * 0.35f))) /
                                                    (rr * rr * 1.2f),
                                         0, 1);
                cap = mix(cap, rgb_hex(0xFFFFFF), lit * 0.28f);
                *p = mix(*p, cap, a);
            }
}

/* ---- the edge next to the picture ------------------------------------------------------------ */

/* a soft shadow towards the picture and a black line at its edge, so the picture sits in the
 * border instead of next to it */
static void inner_edge(void)
{
    for (int side = 0; side < 2; side++)
        for (int y = 0; y < H; y++)
            for (int i = 0; i < 8; i++) {
                const int x = side ? i : SW - 1 - i; /* i pixels from the picture */
                const float k = i == 0 ? 0.0f : 0.55f + 0.45f * (i / 8.0f);
                *px(side, y, x) = scale(*px(side, y, x), k);
            }
}

/* ---- the texture ----------------------------------------------------------------------------- */

static u16 to565(Rgb c)
{
    const int r = (int)clampf(c.r + 0.5f, 0, 255), g = (int)clampf(c.g + 0.5f, 0, 255), b = (int)clampf(c.b + 0.5f, 0, 255);
    return (u16)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

void border_refresh(void)
{
    if (g_settings.border == BORDER_OFF || !G.rom[0]) {
        G.cur = -1;
        return;
    }
    const int next = G.cur == 0 ? 1 : 0;
    if (!G.tex[next] && !(G.tex[next] = linearAlloc(BORDER_TW * BORDER_TH * 2)))
        return;
    G.px = malloc(sizeof(Rgb) * 2 * H * SW);
    if (!G.px)
        return;
    char stem[256];
    rom_stem(stem, sizeof(stem), G.rom);
    if (!from_picture(stem)) {
        if (g_settings.border == BORDER_ART && from_box_art(G.rom, stem))
            inner_edge();
        else {
            console_style();
            inner_edge();
        }
    }
    u16 *t = G.tex[next];
    memset(t, 0, BORDER_TW * BORDER_TH * 2);
    for (int side = 0; side < 2; side++)
        for (int y = 0; y < BORDER_TH; y++)
            for (int x = 0; x < SW; x++)
                t[tiled((side ? BORDER_RIGHT_U : 0) + x, y, BORDER_TW)] = to565(*px(side, y < H ? y : H - 1, x));
    free(G.px);
    G.px = NULL;
    pica_cache_flush(t, BORDER_TW * BORDER_TH * 2);
    G.cur = next;
    G.fresh = true;
}

void border_prepare(const char *rom_path)
{
    snprintf(G.rom, sizeof(G.rom), "%s", rom_path);
    border_refresh();
}

void border_free(void)
{
    G.cur = -1;
    for (int i = 0; i < 2; i++) {
        if (G.tex[i])
            linearFree(G.tex[i]);
        G.tex[i] = NULL;
    }
    G.rom[0] = 0;
}

const void *border_texture(bool *changed)
{
    const int c = G.cur;
    if (c < 0 || ctr64_aspect_applied() != ASPECT_43)
        return NULL;
    *changed = G.fresh;
    G.fresh = false;
    return G.tex[c];
}
