#pragma once

/* Bottom-screen UI: a small software renderer (CPU, RGB565, drawn straight into the bottom
 * framebuffer) in the look of the 3DS system menus, with the system font. Only the menus and
 * the game list draw with it; while a game runs the bottom screen changes once a second at
 * most (the stats strip), so the emulation pays nothing for it. No GPU work: the GPU belongs to
 * the game's renderer. */

#include <3ds.h>
#include <stdbool.h>

#define UI_W 320
#define UI_H 240

typedef u16 UiCol; /* RGB565 */
#define UI_RGB(r, g, b) ((UiCol)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

/* the palette (3DS System Settings: light grays, white cards, cyan selection), or its dark
 * variant (ui_set_dark; the "Dark mode" setting) */
enum {
    UIP_BG_TOP, UIP_BG_BOT, UIP_STRIPE, UIP_HEAD_TOP, UIP_HEAD_BOT, UIP_LINE, UIP_SHADOW, UIP_CARD,
    UIP_CARD_BOT, UIP_TEXT, UIP_TEXT_DIM, UIP_VALUE, UIP_SEL_FILL, UIP_SEL_FILL_BOT, UIP_FOOT_TOP,
    UIP_FOOT_BOT, UIP_FOOT_LINE, UIP_TRACK, UIP_THUMB, UIP_KNOB, UIP_KNOB_SEL, UIP_KNOB_ARROW,
    UIP_DIVIDER, UIP_TILE_GREY, UIP_TILE_GREY_BOT, UIP_COUNT
};
extern UiCol g_ui_pal[UIP_COUNT];
void ui_set_dark(bool dark);
bool ui_is_dark(void);
#define UI_BG_TOP g_ui_pal[UIP_BG_TOP]
#define UI_BG_BOT g_ui_pal[UIP_BG_BOT]
#define UI_STRIPE g_ui_pal[UIP_STRIPE]
#define UI_HEAD_TOP g_ui_pal[UIP_HEAD_TOP]
#define UI_HEAD_BOT g_ui_pal[UIP_HEAD_BOT]
#define UI_LINE g_ui_pal[UIP_LINE]
#define UI_SHADOW g_ui_pal[UIP_SHADOW]
#define UI_CARD g_ui_pal[UIP_CARD]
#define UI_CARD_BOT g_ui_pal[UIP_CARD_BOT]
#define UI_TEXT g_ui_pal[UIP_TEXT]
#define UI_TEXT_DIM g_ui_pal[UIP_TEXT_DIM]
#define UI_VALUE g_ui_pal[UIP_VALUE]
#define UI_SEL_FILL g_ui_pal[UIP_SEL_FILL]
#define UI_SEL_FILL_BOT g_ui_pal[UIP_SEL_FILL_BOT]
#define UI_FOOT_TOP g_ui_pal[UIP_FOOT_TOP]
#define UI_FOOT_BOT g_ui_pal[UIP_FOOT_BOT]
#define UI_FOOT_LINE g_ui_pal[UIP_FOOT_LINE]
#define UI_TRACK g_ui_pal[UIP_TRACK]          /* scroll bar track */
#define UI_THUMB g_ui_pal[UIP_THUMB]          /* scroll bar thumb */
#define UI_KNOB g_ui_pal[UIP_KNOB]            /* small round arrow buttons */
#define UI_KNOB_SEL g_ui_pal[UIP_KNOB_SEL]
#define UI_KNOB_ARROW g_ui_pal[UIP_KNOB_ARROW]
#define UI_DIVIDER g_ui_pal[UIP_DIVIDER]      /* hairlines inside a card */
#define UI_TILE_GREY g_ui_pal[UIP_TILE_GREY]  /* unselected icon squares */
#define UI_TILE_GREY_BOT g_ui_pal[UIP_TILE_GREY_BOT]
/* the same in both themes */
#define UI_SEL UI_RGB(0x22, 0xB8, 0xF0)
#define UI_WHITE UI_RGB(0xFF, 0xFF, 0xFF)
#define UI_WARN UI_RGB(0xE0, 0x7A, 0x10)
#define UI_RED UI_RGB(0xD8, 0x40, 0x30)

/* text sizes */
enum { UI_SMALL, UI_BODY, UI_TITLE, UI_BIG, UI_SIZES };
enum { UI_LEFT, UI_CENTER, UI_RIGHT };

/* the system font's button symbols (UTF-8) */
#define UI_GLYPH_A "\xEE\x80\x80"      /* U+E000 */
#define UI_GLYPH_B "\xEE\x80\x81"      /* U+E001 */
#define UI_GLYPH_X "\xEE\x80\x82"      /* U+E002 */
#define UI_GLYPH_Y "\xEE\x80\x83"      /* U+E003 */
#define UI_GLYPH_L "\xEE\x80\x84"      /* U+E004 */
#define UI_GLYPH_R "\xEE\x80\x85"      /* U+E005 */
#define UI_GLYPH_DPAD "\xEE\x80\x86"   /* U+E006 */
#define UI_GLYPH_TOUCH "\xEE\x80\xBF"  /* U+E03F? (touch: used only if the font has it) */

void ui_init(void);
/* the libctru console (consoleInit's result): its text is drawn into the UI canvas */
void ui_set_console(PrintConsole *c);
PrintConsole *ui_console(void);
bool ui_ready(void);
/* the bottom screen's pixels reach the LCD (data cache flush) */
void ui_present(void);
/* clip rectangle for all drawing (the whole screen by default) */
void ui_clip(int x, int y, int w, int h);
void ui_noclip(void);

void ui_fill(int x, int y, int w, int h, UiCol c);
void ui_blend_fill(int x, int y, int w, int h, UiCol c, int alpha /* 0..32 */);
void ui_vgrad(int x, int y, int w, int h, UiCol top, UiCol bottom);
/* rounded rectangles, anti-aliased corners */
void ui_rrect(int x, int y, int w, int h, int r, UiCol top, UiCol bottom);
void ui_rrect_line(int x, int y, int w, int h, int r, float thick, UiCol c, int alpha);
void ui_disk(float cx, float cy, float r, UiCol c);
void ui_ring(float cx, float cy, float r, float thick, UiCol c, int alpha);
/* a filled triangle pointing left (dir -1) or right (1), up (-2) or down (2) */
void ui_arrow(int cx, int cy, int size, int dir, UiCol c);

/* text: x is the anchor of `align`, y the top of the line box; returns the width */
int ui_text(int x, int y, int size, UiCol c, int align, const char *utf8);
int ui_textf(int x, int y, int size, UiCol c, int align, const char *fmt, ...) __attribute__((format(printf, 6, 7)));
int ui_text_width(int size, const char *utf8);
int ui_line_height(int size);
/* the top y to pass so that capital letters are centred on the line cy */
int ui_text_y(int size, int cy);
/* text cut with "..." to fit max_w */
int ui_text_fit(int x, int y, int size, UiCol c, int align, int max_w, const char *utf8);

/* palettized run-length image (tools/ui/png2c.py) */
typedef struct {
    u16 w, h;
    u16 ncolors;
    const u16 *palette; /* RGB565; index 0 = transparent */
    const u8 *alpha;    /* per palette entry 0..32 */
    const u8 *rle;      /* runs: count-1 (1..128) | 0x80 = repeat next byte, else literal bytes */
} UiImage;
void ui_image(const UiImage *img, int x, int y);
/* one-colour image (icons): run-length alpha 0..32, same run coding; drawn in colour c */
typedef struct {
    u16 w, h;
    const u8 *rle;
} UiMask;
void ui_mask(const UiMask *m, int x, int y, UiCol c);

/* the common screen frame: background, header with a title, footer bar */
void ui_background(int y, int h);
void ui_header(const char *title, const char *right);
#define UI_HEADER_H 26
#define UI_FOOTER_H 26
/* footer buttons, drawn left to right from x; returns the next x */
int ui_footer_begin(void);
int ui_footer_button(int x, const char *glyph, const char *label, bool right);

/* touch: a press that does not move more than a few pixels is a tap (on release); one that does
 * is a drag (dy since the last update in `dy`) */
typedef struct {
    bool down, held, up; /* this frame */
    bool tap, dragging;
    int x, y;            /* current (or last) position */
    int sx, sy;          /* where the press started */
    int dy;              /* vertical movement this frame while dragging */
} UiTouch;
void ui_touch_update(UiTouch *t, u32 down, u32 held, u32 up);
/* hidScanInput plus the keys of this frame (with the development script's, see ui.c) */
void ui_scan(u32 *down, u32 *held, u32 *up);
static inline bool ui_hit(int px_, int py_, int x, int y, int w, int h)
{
    return px_ >= x && py_ >= y && px_ < x + w && py_ < y + h;
}

/* development aid: the bottom screen as a .tga */
void ui_dump(const char *path);
