#include "rom_browser.h"

#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "boxart.h"
#include "menu.h"
#include "rom_title.h"
#include "settings.h"
#include "ui/icons_img.h"
#include "ui/ui.h"

void ctr64_time_mark(const char *what); /* main.c: timing.log */
void ctr64_time_flush(void);

#define MAX_ROMS 200
#define ROW_H 34  /* row pitch: a 30-pixel card and a 4-pixel gap */
#define CARD_H 30
#define LIST_Y (UI_HEADER_H + 6)
#define LIST_H (UI_H - UI_FOOTER_H - 4 - LIST_Y)

typedef struct {
    char file[128];
    char title[80];
    char tags[32];
    char id[5]; /* cartridge ID (box art) */
} Rom;
static Rom g_roms[MAX_ROMS];
static int g_count;
static char g_loaded_title[80];

static bool is_rom(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".z64") || !strcasecmp(dot, ".n64") || !strcasecmp(dot, ".v64"));
}

static int cmp_roms(const void *a, const void *b)
{
    const Rom *x = (const Rom *)a, *y = (const Rom *)b;
    const int c = strcasecmp(x->title, y->title);
    return c ? c : strcasecmp(x->file, y->file);
}

/* the letter a game is filed under in the list: A-Z, anything else '#' */
static char rom_letter(int i)
{
    const char c = g_roms[i].title[0];
    if (c >= 'a' && c <= 'z')
        return (char)(c - 'a' + 'A');
    return c >= 'A' && c <= 'Z' ? c : '#';
}

/* left / right on the game list: the first game of the previous / next letter (wrapping) */
static int letter_jump(int sel, int dir)
{
    if (g_count <= 1)
        return sel;
    if (dir > 0) {
        int i = sel;
        while (i + 1 < g_count && rom_letter(i + 1) == rom_letter(sel))
            i++;
        return (i + 1) % g_count;
    }
    int i = sel;
    while (i > 0 && rom_letter(i - 1) == rom_letter(sel))
        i--; /* the start of this letter */
    i = (i + g_count - 1) % g_count; /* the last game of the previous letter */
    while (i > 0 && rom_letter(i - 1) == rom_letter(i))
        i--;
    return i;
}

static void draw_cart_icon(int x, int y, bool sel)
{
    ui_rrect(x, y, 24, 24, 7, sel ? UI_RGB(0x46, 0xC8, 0xF6) : UI_TILE_GREY,
             sel ? UI_RGB(0x14, 0x9C, 0xDC) : UI_TILE_GREY_BOT);
    ui_mask(&k_icon_cart, x + 2, y + 2, UI_WHITE);
}

static void draw_row(int i, int y, bool sel)
{
    const int x = 8, w = UI_W - 8 - 14;
    if (sel) {
        ui_rrect(x, y, w, CARD_H, 7, UI_SEL_FILL, UI_SEL_FILL_BOT);
        ui_rrect_line(x - 1, y - 1, w + 2, CARD_H + 2, 8, 2.5f, UI_SEL, 32);
    } else {
        ui_rrect(x, y + 1, w, CARD_H, 7, UI_SHADOW, UI_SHADOW);
        ui_rrect(x, y, w, CARD_H, 7, UI_CARD, UI_CARD_BOT);
    }
    draw_cart_icon(x + 6, y + (CARD_H - 24) / 2, sel);
    const char *title = g_roms[i].title, *tags = g_roms[i].tags;
    const int cy = y + CARD_H / 2;
    int tw = 0;
    if (tags[0])
        tw = ui_text_fit(x + w - 10, ui_text_y(UI_SMALL, cy), UI_SMALL, UI_TEXT_DIM, UI_RIGHT, 90, tags) + 8;
    ui_text_fit(x + 38, ui_text_y(UI_BODY, cy), UI_BODY, UI_TEXT, UI_LEFT, w - 48 - tw, title);
}

static void draw_list(int sel, int scroll)
{
    ui_clip(0, LIST_Y - 4, UI_W, LIST_H + 8);
    ui_background(LIST_Y - 4, LIST_H + 8);
    if (!g_count) {
        ui_noclip();
        ui_rrect(20, 70, UI_W - 40, 80, 10, UI_CARD, UI_CARD_BOT);
        ui_text(UI_W / 2, 86, UI_BODY, UI_TEXT, UI_CENTER, "No games found.");
        ui_text(UI_W / 2, 108, UI_SMALL, UI_TEXT_DIM, UI_CENTER, "Copy .z64 / .n64 / .v64 files to");
        ui_text(UI_W / 2, 124, UI_SMALL, UI_TEXT_DIM, UI_CENTER, "sdmc:/3ds/ctr64/roms");
        return;
    }
    ui_clip(0, LIST_Y - 3, UI_W, LIST_H + 6);
    const int first = scroll / ROW_H;
    for (int i = first; i < g_count; i++) {
        const int y = LIST_Y + i * ROW_H - scroll;
        if (y > LIST_Y + LIST_H)
            break;
        draw_row(i, y, i == sel);
    }
    ui_noclip();
    /* scroll bar */
    const int content = g_count * ROW_H;
    if (content > LIST_H) {
        const int bx = UI_W - 9, bh = LIST_H * LIST_H / content;
        const int by = LIST_Y + (LIST_H - bh) * scroll / (content - LIST_H);
        ui_rrect(bx, LIST_Y, 4, LIST_H, 2, UI_TRACK, UI_TRACK);
        ui_rrect(bx, by, 4, bh, 2, UI_THUMB, UI_THUMB);
    }
}

/* footer buttons (x ranges, for touch): Start, Settings, Light/Dark; Quit at the right */
static int g_foot_x[4];

static void draw_frame(void)
{
    char right[32];
    snprintf(right, sizeof(right), g_count == 1 ? "1 game" : "%d games", g_count);
    ui_header("ctr64 " CTR64_VERSION, right);
    ui_footer_begin();
    g_foot_x[0] = 0;
    g_foot_x[1] = ui_footer_button(8, UI_GLYPH_A, "Start", false);
    g_foot_x[2] = ui_footer_button(g_foot_x[1], UI_GLYPH_X, "Settings", false);
    g_foot_x[3] = ui_footer_button(g_foot_x[2], UI_GLYPH_Y, ui_is_dark() ? "Light" : "Dark", false);
    ui_footer_button(UI_W - 8, NULL, "START  Quit", true);
}

RomBrowserResult rom_browser_pick(const char *dir, char *out, size_t outsz)
{
    int sel = 0, scroll = 0;
    DIR *d = opendir(dir);
    g_count = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && g_count < MAX_ROMS)
            if (is_rom(e->d_name)) {
                Rom *r = &g_roms[g_count++];
                char path[400];
                snprintf(r->file, sizeof(r->file), "%.127s", e->d_name);
                snprintf(path, sizeof(path), "%s/%s", dir, r->file);
                rom_title_read_id(path, r->title, sizeof(r->title), r->tags, sizeof(r->tags), r->id);
            }
        closedir(d);
    }
    ctr64_time_mark("game list: ROM headers read");
    qsort(g_roms, g_count, sizeof(g_roms[0]), cmp_roms);
    /* the top screen: box art of the selected game */
    boxart_begin();
    for (int i = 0; i < g_count; i++) {
        char path[400];
        snprintf(path, sizeof(path), "%s/%s", dir, g_roms[i].file);
        boxart_add(path, g_roms[i].title, g_roms[i].id, strstr(g_roms[i].tags, "hack") != NULL);
    }
    boxart_start();
    ctr64_time_mark("game list: box art ready");

    consoleClear();
    ui_init();
    ui_background(0, UI_H);
    draw_frame();
    UiTouch touch = {0};
    hidSetRepeatParameters(18, 5);
    ctr64_time_mark("game list shown");
    ctr64_time_flush();
    const int max_scroll = g_count * ROW_H > LIST_H ? g_count * ROW_H - LIST_H : 0;

    for (bool redraw = true; aptMainLoop(); gspWaitForVBlank()) {
        u32 down, held, up;
        ui_scan(&down, &held, &up);
        const u32 rep = hidKeysDownRepeat() | (down & (KEY_DUP | KEY_DDOWN));
        ui_touch_update(&touch, down, held, up);
        boxart_frame();
        if (down & KEY_START) {
            boxart_end();
            return ROM_BROWSER_QUIT;
        }
        bool tap_settings = false, tap_theme = false;
        if (touch.tap && ui_hit(touch.x, touch.y, 0, UI_H - UI_FOOTER_H, UI_W, UI_FOOTER_H)) {
            tap_settings = touch.x >= g_foot_x[1] && touch.x < g_foot_x[2];
            tap_theme = touch.x >= g_foot_x[2] && touch.x < g_foot_x[3];
            if (tap_settings || tap_theme)
                touch.tap = false;
        }
        if ((down & KEY_X) || tap_settings) { /* the settings, without starting a game */
            menu_settings(SETTINGS_PATH);
            ui_set_dark(g_settings.dark_mode);
            ui_background(0, UI_H);
            draw_frame();
            boxart_redraw();
            hidSetRepeatParameters(18, 5);
            touch = (UiTouch){0};
            redraw = true;
        }
        if ((down & KEY_Y) || tap_theme) { /* dark mode on / off (also in Settings: Video) */
            g_settings.dark_mode = !g_settings.dark_mode;
            ui_set_dark(g_settings.dark_mode);
            settings_save(SETTINGS_PATH);
            ui_background(0, UI_H);
            draw_frame();
            boxart_redraw();
            redraw = true;
        }
        if (g_count && (down & KEY_A)) {
            snprintf(out, outsz, "%s/%s", dir, g_roms[sel].file);
            boxart_end();
            return ROM_BROWSER_PICKED;
        }
        if (g_count && (rep & (KEY_DDOWN | KEY_DUP | KEY_CPAD_DOWN | KEY_CPAD_UP | KEY_DLEFT | KEY_DRIGHT |
                               KEY_CPAD_LEFT | KEY_CPAD_RIGHT))) {
            /* D-pad left / right: by letter. The Circle Pad moves one game whichever way it is
             * pushed (right/down next, left/up previous): a letter jump from a sideways push felt
             * unexpected. */
            if (rep & (KEY_DLEFT | KEY_DRIGHT))
                sel = letter_jump(sel, (rep & KEY_DRIGHT) ? 1 : -1);
            else
                sel = (sel + ((rep & (KEY_DDOWN | KEY_CPAD_DOWN | KEY_CPAD_RIGHT)) ? 1 : g_count - 1)) % g_count;
            if (sel * ROW_H < scroll)
                scroll = sel * ROW_H;
            if (sel * ROW_H + CARD_H > scroll + LIST_H)
                scroll = sel * ROW_H + CARD_H - LIST_H;
            boxart_select(sel);
            redraw = true;
        }
        if (touch.dragging && touch.dy && max_scroll) {
            scroll -= touch.dy;
            scroll = scroll < 0 ? 0 : scroll > max_scroll ? max_scroll : scroll;
            redraw = true;
        }
        if (touch.tap && g_count) {
            if (ui_hit(touch.x, touch.y, 0, UI_H - UI_FOOTER_H, g_foot_x[1], UI_FOOTER_H)) {
                snprintf(out, outsz, "%s/%s", dir, g_roms[sel].file);
                boxart_end();
                return ROM_BROWSER_PICKED;
            }
            if (ui_hit(touch.x, touch.y, UI_W / 2, UI_H - UI_FOOTER_H, UI_W / 2, UI_FOOTER_H)) {
                boxart_end();
                return ROM_BROWSER_QUIT;
            }
            if (ui_hit(touch.x, touch.y, 0, LIST_Y, UI_W, LIST_H)) {
                const int i = (touch.y - LIST_Y + scroll) / ROW_H;
                const int in_row = (touch.y - LIST_Y + scroll) % ROW_H;
                if (i < g_count && in_row < CARD_H) {
                    if (i == sel) { /* a second tap on the selected game starts it */
                        snprintf(out, outsz, "%s/%s", dir, g_roms[sel].file);
                        boxart_end();
                        return ROM_BROWSER_PICKED;
                    }
                    sel = i;
                    boxart_select(sel);
                    redraw = true;
                }
            }
        }
        if (!redraw)
            continue;
        redraw = false;
        draw_list(sel, scroll);
        ui_present();
    }
    boxart_end();
    return ROM_BROWSER_QUIT;
}

static void loading_card(const char *label, const char *title)
{
    ui_rrect(20, 78, UI_W - 40, 74, 10, UI_SHADOW, UI_SHADOW);
    ui_rrect(20, 77, UI_W - 40, 74, 10, UI_CARD, UI_CARD_BOT);
    ui_text(UI_W / 2, ui_text_y(UI_SMALL, 96), UI_SMALL, UI_TEXT_DIM, UI_CENTER, label);
    ui_text_fit(UI_W / 2, ui_text_y(UI_TITLE, 120), UI_TITLE, UI_TEXT, UI_CENTER, UI_W - 60, title);
}

void rom_browser_loading(const char *path)
{
    char tags[32];
    rom_title_read(path, g_loaded_title, sizeof(g_loaded_title), tags, sizeof(tags));
    const char *title = g_loaded_title;
    ui_init();
    consoleClear();
    ui_background(0, UI_H);
    ui_header("ctr64", NULL);
    loading_card("Loading", title);
    ui_present();
}

/* First start of a streamed game: the whole file is read once (its MD5); a bar on the loading card */
void rom_browser_loading_progress(int pct)
{
    const int x = 44, y = 138, w = UI_W - 88, h = 6;
    ui_background(70, 90);
    loading_card("First start: reading the game", g_loaded_title);
    ui_rrect(x, y, w, h, 3, UI_TRACK, UI_TRACK);
    if (pct > 0)
        ui_rrect(x, y, w * (pct > 100 ? 100 : pct) / 100 < h ? h : w * (pct > 100 ? 100 : pct) / 100, h, 3, UI_SEL, UI_SEL);
    ui_present();
    /* UI tests (uiscript.txt): a picture of the bar half way */
    static bool dumped;
    if (!dumped && pct >= 50) {
        dumped = true;
        FILE *t = fopen("sdmc:/3ds/ctr64/uiscript.txt", "r");
        if (t) {
            fclose(t);
            ui_dump("sdmc:/3ds/ctr64/ui_progress.tga");
        }
    }
}

/* The game could not be started (unreadable file, not an N64 ROM, or too large for the memory
 * of this console). true: back to the game list (A), false: exit (START). */
bool rom_browser_error(const char *path)
{
    if (!g_loaded_title[0]) {
        char tags[32];
        rom_title_read(path, g_loaded_title, sizeof(g_loaded_title), tags, sizeof(tags));
    }
    ui_init();
    consoleClear();
    ui_background(0, UI_H);
    ui_header("ctr64", NULL);
    ui_rrect(16, 58, UI_W - 32, 112, 10, UI_SHADOW, UI_SHADOW);
    ui_rrect(16, 57, UI_W - 32, 112, 10, UI_CARD, UI_CARD_BOT);
    ui_text(UI_W / 2, ui_text_y(UI_SMALL, 76), UI_SMALL, UI_TEXT_DIM, UI_CENTER, "Could not start");
    ui_text_fit(UI_W / 2, ui_text_y(UI_TITLE, 100), UI_TITLE, UI_TEXT, UI_CENTER, UI_W - 56, g_loaded_title);
    ui_text(UI_W / 2, ui_text_y(UI_SMALL, 128), UI_SMALL, UI_TEXT_DIM, UI_CENTER,
            "The file could not be read, is not an N64 ROM,");
    ui_text(UI_W / 2, ui_text_y(UI_SMALL, 146), UI_SMALL, UI_TEXT_DIM, UI_CENTER,
            "or does not fit in memory.");
    ui_footer_begin();
    ui_footer_button(8, UI_GLYPH_A, "Game list", false);
    ui_footer_button(UI_W - 8, NULL, "START  Quit", true);
    ui_present();
    while (aptMainLoop()) {
        u32 down, held, up;
        ui_scan(&down, &held, &up);
        if (down & KEY_A)
            return true;
        if (down & KEY_START)
            return false;
        gspWaitForVBlank();
    }
    return false;
}

const char *rom_browser_title(void)
{
    return g_loaded_title[0] ? g_loaded_title : NULL;
}
