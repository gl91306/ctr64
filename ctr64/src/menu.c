#include "menu.h"

#include <3ds.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_ndsp.h"
#include "border.h"
#include "core_host.h"
#include "remote.h"
#include "rom_browser.h"
#include "settings.h"
#include "ui/icons_img.h"
#include "ui/n64pad_img.h"
#include "ui/ui.h"

extern volatile u32 ctr64_heartbeat; /* main.c watchdog */

static char g_settings_path[256];
static MenuStats g_stats;
static const char *game_title(void);
static bool g_stats_dirty = true;
static bool g_running_drawn;
static int g_slot;
static char g_status[80];
static bool g_no_game; /* menu_settings: opened from the game list, no game loaded */

int ctr64_game_fb_copy(void);         /* GLideN64: the running game's own copy mode */
int ctr64_fb_emulation_applied(void); /* GLideN64: frame buffer emulation the game started with */
int ctr64_aa_applied(void);           /* ... anti-aliasing */
int ctr64_aspect_applied(void);       /* ... screen shape (ASPECT_*) */
int ctr64_aa_available(void);         /* New 3DS: room for anti-aliasing */
int ctr64_aa_excluded(void);          /* main.c: not for the running game (memory) */

void menu_apply_settings(void)
{
    audio_ndsp_set_volume(g_settings.audio ? g_settings.volume / 100.0f : 0.0f);
    ui_set_dark(g_settings.dark_mode);
}

/* ---- the screen while the game runs -------------------------------------------------------------
 * Drawn once; afterwards only the stats card, once a second. With "Debug info" the developer
 * counters (main.c, libctru console) use the lower part of the screen. */

#define RUN_CARD_Y 34
#define RUN_CARD_H 92
#define STATS_Y 132
#define STATS_H 48
#define DEBUG_ROW 11 /* console row (8-pixel rows) where the debug window starts */

static void console_window(void)
{
    PrintConsole *con = ui_console();
    if (g_settings.debug_info) {
        consoleSetWindow(con, 0, DEBUG_ROW, 40, 30 - DEBUG_ROW);
        consoleClear();
    } else {
        consoleSetWindow(con, 0, 29, 40, 1); /* stray output goes to the last line */
    }
}

int menu_debug_row(void)
{
    return DEBUG_ROW + 1; /* 1-based screen row: libctru's cursor positions ignore the window */
}

/* "Bottom screen: Off" turns the bottom backlight off while the game runs (gsp::Lcd) */
static bool g_backlight_off;
static void bottom_backlight(bool on)
{
    if (on != g_backlight_off)
        return;
    if (R_SUCCEEDED(gspLcdInit())) {
        if (on)
            GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
        else
            GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
        gspLcdExit();
    }
    g_backlight_off = !on;
}

/* the game runs with the card and speed hidden (Debug info shows its counters anyway) */
static bool running_hidden(void) { return g_settings.bottom_screen != BOTTOM_MENU && !g_settings.debug_info; }

void menu_screen_suspend(bool suspend)
{
    if (suspend)
        bottom_backlight(true);
    else if (g_running_drawn && running_hidden() && g_settings.bottom_screen == BOTTOM_OFF)
        bottom_backlight(false);
}

static void draw_stats_card(void)
{
    if (running_hidden()) {
        g_stats_dirty = false;
        return;
    }
    const int y = g_settings.debug_info ? RUN_CARD_Y : STATS_Y;
    ui_clip(0, y - 2, UI_W, STATS_H + 5);
    ui_background(y - 2, STATS_H + 5);
    ui_rrect(8, y + 1, UI_W - 16, STATS_H, 8, UI_SHADOW, UI_SHADOW);
    ui_rrect(8, y, UI_W - 16, STATS_H, 8, UI_CARD, UI_CARD_BOT);
    const MenuStats *s = &g_stats;
    const float pct = s->vi_target > 0 ? 100.0f * s->vi_per_s / s->vi_target : 0;
    if (s->vi_per_s <= 0) {
        ui_text(UI_W / 2, y + 14, UI_BODY, UI_TEXT_DIM, UI_CENTER, "Starting...");
    } else {
        const UiCol pc = pct >= 97.0f ? UI_RGB(0x2E, 0x9E, 0x50) : pct >= 85.0f ? UI_WARN : UI_RED;
        ui_text(20, y + 4, UI_SMALL, UI_TEXT_DIM, UI_LEFT, "Speed");
        ui_textf(20, y + 18, UI_TITLE, pc, UI_LEFT, "%.0f%%", pct > 999 ? 999.0f : pct);
        char game[48], skip[24] = "";
        if (s->game_fps_full > 0)
            snprintf(game, sizeof(game), "Game %.0f of %.0f fps", s->game_fps, s->game_fps_full);
        else
            snprintf(game, sizeof(game), "Game %.0f fps", s->game_fps);
        if (g_settings.frameskip == FRAMESKIP_AUTO && s->skipped)
            snprintf(skip, sizeof(skip), "  \xC2\xB7  skipped %u", s->skipped);
        ui_textf(96, y + 6, UI_SMALL, UI_TEXT, UI_LEFT, "%.1f of %.2f VI/s  \xC2\xB7  CPU %.1f ms", s->vi_per_s,
                 s->vi_target, s->cpu_ms);
        /* below its usual frame rate: the game is slowed down even at 100% VI speed */
        const float gpct = s->game_fps_full > 0 ? 100.0f * s->game_fps / s->game_fps_full : 100.0f;
        const UiCol gc = gpct >= 90.0f ? UI_TEXT : gpct >= 75.0f ? UI_WARN : UI_RED;
        ui_textf(96, y + 24, UI_SMALL, gc, UI_LEFT, "%s%s", game, skip);
    }
    ui_noclip();
    ui_present();
    g_stats_dirty = false;
}

static void draw_running(void)
{
    console_window();
    if (running_hidden()) {
        ui_rrect(0, 0, UI_W, UI_H, 0, UI_RGB(0, 0, 0), UI_RGB(0, 0, 0));
        ui_present();
        g_running_drawn = true;
        if (g_settings.bottom_screen == BOTTOM_OFF)
            bottom_backlight(false);
        return;
    }
    ui_background(0, UI_H);
    ui_header(game_title(), "ctr64");
    if (!g_settings.debug_info) {
        const int x = 8, w = UI_W - 16, y = RUN_CARD_Y, h = RUN_CARD_H;
        ui_rrect(x, y + 1, w, h, 10, UI_SHADOW, UI_SHADOW);
        ui_rrect(x, y, w, h, 10, UI_CARD, UI_CARD_BOT);
        ui_text(UI_W / 2, ui_text_y(UI_BODY, y + 17), UI_BODY, UI_TEXT, UI_CENTER, "Touch here or press SELECT");
        ui_text(UI_W / 2, ui_text_y(UI_SMALL, y + 35), UI_SMALL, UI_TEXT_DIM, UI_CENTER,
                "Save states, settings and controls");
        ui_rrect(UI_W / 2 - 60, y + 53, 120, 28, 14, UI_RGB(0x14, 0x9C, 0xDC), UI_RGB(0x14, 0x9C, 0xDC));
        ui_rrect(UI_W / 2 - 60, y + 52, 120, 28, 14, UI_RGB(0x46, 0xC8, 0xF6), UI_RGB(0x1E, 0xA6, 0xE4));
        ui_text(UI_W / 2, ui_text_y(UI_BODY, y + 66), UI_BODY, UI_WHITE, UI_CENTER, "Menu");
        ui_footer_begin();
        ui_footer_button(8, NULL, "SELECT  Menu", false);
        ui_footer_button(UI_W - 8, NULL, "The game is running", true);
    }
    draw_stats_card();
    g_running_drawn = true;
}

void menu_init(const char *settings_path)
{
    snprintf(g_settings_path, sizeof(g_settings_path), "%s", settings_path);
    settings_load(settings_path);
    settings_game_controls_load(core_host_rom_id()); /* the game's own controls, if saved */
    settings_publish();
    menu_apply_settings();
    ui_init();
    consoleClear();
    g_running_drawn = false;
    g_stats_dirty = true;
}

void menu_set_stats(const MenuStats *s)
{
    g_stats = *s;
    g_stats_dirty = true;
}

void menu_draw_running(void)
{
    if (!g_running_drawn)
        draw_running();
    else if (g_stats_dirty)
        draw_stats_card();
}

bool menu_running_touched(u32 down)
{
    if (!(down & KEY_TOUCH) || g_settings.debug_info || running_hidden())
        return (down & KEY_TOUCH) && (g_settings.debug_info || running_hidden()); /* any touch */
    touchPosition tp;
    hidTouchRead(&tp);
    return ui_hit(tp.px, tp.py, 8, RUN_CARD_Y, UI_W - 16, RUN_CARD_H) ||
           ui_hit(tp.px, tp.py, 0, UI_H - UI_FOOTER_H, UI_W / 2, UI_FOOTER_H);
}

/* ---- the menu: pages of widgets ------------------------------------------------------------------ */

typedef enum { PAGE_MAIN, PAGE_AUDIO, PAGE_VIDEO, PAGE_CONTROLS } Page;

enum { M_RESUME, M_SAVE, M_LOAD, M_SLOT, M_AUDIO, M_VIDEO, M_CONTROLS, M_RESET, M_GAME_LIST, M_EXIT, M_COUNT };
enum { A_VOLUME, A_SOUND, A_COUNT };
enum { V_FRAMESKIP, V_LIMIT, V_AA, V_ASPECT, V_BORDER, V_DARK, V_BOTTOM, V_FB_EMU, V_FB_COPY, V_THREADED, V_THREADED_AUDIO, V_PRECOMPILE,
       V_FAST_COMPILE, V_PROFILER, V_DEBUG, V_COUNT };
#define PAD_N ((int)(sizeof(k_n64pad_hot) / sizeof(k_n64pad_hot[0])))
#define PAD_STICK -1
#define PAD_DPAD -2

typedef struct {
    s16 x, y, w, h;
} Rect;

static Page g_page;
static int g_sel;
static int g_scroll;          /* list pages */
static int g_capture = -1;    /* controls: N64 input waiting for a 3DS button */
static bool g_adjust;         /* controls: the Circle Pad range has focus */
static u64 g_capture_until;
static u32 g_capture_keys;    /* controls: 3DS buttons pressed so far while capturing */
static Rect g_w[32];
static int g_nw;
static struct { Rect r; u32 key; } g_foot[8]; /* up to 5 buttons (controls page in a game) */
static int g_nfoot;

#define LIST_Y (UI_HEADER_H + 6)
#define LIST_H (UI_H - UI_FOOTER_H - 4 - LIST_Y)
#define ROW_PITCH 32
#define ROW_H 28
#define PAD_X 9
#define PAD_Y (UI_HEADER_H + 5)

static bool aa_excluded_here(void)
{
    return !g_no_game && ctr64_aa_available() && g_settings.anti_aliasing && g_settings.fb_emulation && ctr64_aa_excluded();
}

static bool fb_emulation_pending(void)
{
    return !g_no_game && (g_settings.fb_emulation != (ctr64_fb_emulation_applied() != 0) ||
           g_settings.aspect != ctr64_aspect_applied() ||
           (ctr64_aa_available() && g_settings.fb_emulation && !aa_excluded_here() &&
            g_settings.anti_aliasing != (ctr64_aa_applied() != 0)));
}

static const char *game_title(void)
{
    const char *t = rom_browser_title();
    return t ? t : core_host_rom_name();
}

static const char *onoff(bool b) { return b ? "On" : "Off"; }

/* one or more 3DS buttons: "L+ZL" */
static const char *map_name(u32 keys)
{
    static char n[64];
    settings_keys_name(keys, n, sizeof(n));
    return n;
}

/* N64 input of a controller hotspot (or PAD_STICK / PAD_DPAD) */
static int pad_input(int h)
{
    static const char *const names[] = {"A", "B", "Z", "L", "R", "START", "C-Up", "C-Down", "C-Left", "C-Right"};
    const char *n = k_n64pad_hot[h].name;
    if (!strcmp(n, "Stick"))
        return PAD_STICK;
    if (!strcmp(n, "D-Pad"))
        return PAD_DPAD;
    for (int i = 0; i < IN_COUNT; i++)
        if (!strcmp(n, names[i]))
            return i;
    return PAD_DPAD;
}

static int list_count(Page p) { return p == PAGE_AUDIO ? A_COUNT : V_COUNT; }

static void layout(void)
{
    g_nw = 0;
    if (g_page == PAGE_MAIN) {
        const int x = 8, gap = 6, w3 = (UI_W - 16 - 2 * gap) / 3, th = 42;
        int y = UI_HEADER_H + 6;
        g_w[M_RESUME] = (Rect){x, (s16)y, UI_W - 16, 34};
        y += 34 + gap;
        static const int rows[3][3] = {{M_SAVE, M_LOAD, M_SLOT}, {M_AUDIO, M_VIDEO, M_CONTROLS}, {M_RESET, M_GAME_LIST, M_EXIT}};
        for (int r = 0; r < 3; r++, y += th + gap)
            for (int c = 0; c < 3; c++)
                g_w[rows[r][c]] = (Rect){(s16)(x + c * (w3 + gap)), (s16)y, (s16)w3, (s16)th};
        g_nw = M_COUNT;
        if (g_no_game) {
            for (int i = 0; i < M_COUNT; i++)
                g_w[i] = (Rect){0, 0, 0, 0};
            y = UI_HEADER_H + 6;
            g_w[M_RESUME] = (Rect){x, (s16)y, UI_W - 16, 34};
            y += 34 + gap;
            const int big = 70;
            static const int ids[3] = {M_AUDIO, M_VIDEO, M_CONTROLS};
            for (int c = 0; c < 3; c++)
                g_w[ids[c]] = (Rect){(s16)(x + c * (w3 + gap)), (s16)y, (s16)w3, (s16)big};
        }
    } else if (g_page == PAGE_CONTROLS) {
        for (int h = 0; h < PAD_N; h++) {
            const int r = k_n64pad_hot[h].r + 4;
            g_w[h] = (Rect){(s16)lroundf(PAD_X + k_n64pad_hot[h].x - r), (s16)lroundf(PAD_Y + k_n64pad_hot[h].y - r), (s16)(2 * r),
                            (s16)(2 * r)};
        }
        g_nw = PAD_N;
    } else {
        const int n = list_count(g_page);
        for (int i = 0; i < n; i++)
            g_w[i] = (Rect){8, (s16)(LIST_Y + i * ROW_PITCH - g_scroll), UI_W - 16 - 8, ROW_H};
        g_nw = n;
    }
}

/* ---- drawing ---- */

static void card(int x, int y, int w, int h, int r, bool sel)
{
    if (sel) {
        ui_rrect(x, y, w, h, r, UI_SEL_FILL, UI_SEL_FILL_BOT);
        ui_rrect_line(x - 1, y - 1, w + 2, h + 2, r + 1, 2.5f, UI_SEL, 32);
    } else {
        ui_rrect(x, y + 1, w, h, r, UI_SHADOW, UI_SHADOW);
        ui_rrect(x, y, w, h, r, UI_CARD, UI_CARD_BOT);
    }
}

static void list_item(int i, const char **label, char *v, size_t vn, bool *dim)
{
    *dim = false;
    v[0] = 0;
    if (g_page == PAGE_AUDIO) {
        if (i == A_VOLUME) {
            *label = "Volume";
            snprintf(v, vn, "%d%%", g_settings.volume);
        } else {
            *label = "Sound";
            snprintf(v, vn, "%s", onoff(g_settings.audio));
        }
        return;
    }
    switch (i) {
    case V_FRAMESKIP: {
        static const char *const fs[] = {"Off", "1", "2", "3", "Auto"};
        *label = "Frameskip";
        snprintf(v, vn, "%s", fs[g_settings.frameskip]);
        break;
    }
    case V_LIMIT:
        *label = "Speed limit (60 VI/s)";
        snprintf(v, vn, "%s", onoff(g_settings.speed_limit));
        break;
    case V_AA:
        *label = "Anti-aliasing";
        if (!ctr64_aa_available()) {
            snprintf(v, vn, "New 3DS only");
            *dim = true;
        } else if (aa_excluded_here()) {
            snprintf(v, vn, "%s -", onoff(g_settings.anti_aliasing));
        } else {
            snprintf(v, vn, "%s%s", onoff(g_settings.anti_aliasing),
                     !g_no_game && g_settings.fb_emulation && g_settings.anti_aliasing != (ctr64_aa_applied() != 0)
                         ? " *"
                         : "");
        }
        break;
    case V_ASPECT: {
        static const char *const as[] = {"4:3", "Stretch", "Widescreen"};
        *label = "Screen";
        snprintf(v, vn, "%s%s", as[g_settings.aspect],
                 !g_no_game && g_settings.aspect != ctr64_aspect_applied() ? " *" : "");
        break;
    }
    case V_BORDER: {
        static const char *const bs[] = {"Off", "Box art", "Console"};
        *label = "Side borders (4:3)";
        snprintf(v, vn, "%s", bs[g_settings.border]);
        *dim = g_settings.aspect != ASPECT_43;
        break;
    }
    case V_DARK:
        *label = "Dark mode";
        snprintf(v, vn, "%s", onoff(g_settings.dark_mode));
        break;
    case V_BOTTOM: {
        static const char *const bs[] = {"Menu", "Blank", "Off"};
        *label = "Bottom screen in game";
        snprintf(v, vn, "%s", bs[g_settings.bottom_screen]);
        break;
    }
    case V_FB_EMU:
        *label = "Frame buffer emulation";
        snprintf(v, vn, "%s%s", onoff(g_settings.fb_emulation),
                 !g_no_game && g_settings.fb_emulation != (ctr64_fb_emulation_applied() != 0) ? " *" : "");
        break;
    case V_FB_COPY: {
        static const char *const modes[] = {"Off", "Sync", "Async", "Async"};
        *label = "FB copy to RDRAM";
        if (g_settings.fb_copy == FB_COPY_GAME && g_no_game)
            snprintf(v, vn, "Game");
        else if (g_settings.fb_copy == FB_COPY_GAME)
            snprintf(v, vn, "Game (%s)", modes[ctr64_game_fb_copy() & 3]);
        else
            snprintf(v, vn, "%s", modes[g_settings.fb_copy]);
        break;
    }
    case V_THREADED:
        *label = "Video on 2nd core";
        snprintf(v, vn, "%s", onoff(g_settings.threaded_video));
        break;
    case V_THREADED_AUDIO:
        *label = "Audio on 3rd core";
        snprintf(v, vn, "%s", onoff(g_settings.threaded_audio));
        break;
    case V_PRECOMPILE:
        *label = "Precompile known code";
        snprintf(v, vn, "%s", onoff(g_settings.remember_blocks));
        break;
    case V_FAST_COMPILE:
        *label = "Fast compile";
        snprintf(v, vn, "%s", onoff(g_settings.fast_compile));
        break;
    case V_PROFILER: {
        static const char *const pr[] = {"Off", "On", "Detailed"};
        extern int ctr64_profiler;
        *label = "Profiler";
        snprintf(v, vn, "%s%s", pr[g_settings.profiler],
                 !g_no_game && g_settings.profiler != ctr64_profiler ? " *" : "");
        break;
    }
    default:
        *label = "Debug info";
        snprintf(v, vn, "%s", onoff(g_settings.debug_info));
        break;
    }
}

static void draw_list_row(int i, bool sel)
{
    const Rect r = g_w[i];
    card(r.x, r.y, r.w, r.h, 7, sel);
    const char *label;
    char v[40];
    bool dim;
    list_item(i, &label, v, sizeof(v), &dim);
    const int ty = ui_text_y(UI_BODY, r.y + r.h / 2);
    ui_text(r.x + 12, ty, UI_BODY, UI_TEXT, UI_LEFT, label);
    const int right = r.x + r.w - 12;
    if (dim) {
        ui_text(right, ty, UI_BODY, UI_TEXT_DIM, UI_RIGHT, v);
        return;
    }
    /* " *": changed, applies when the game restarts (the banner says so) */
    const size_t vl = strlen(v);
    const bool pending = vl > 2 && !strcmp(v + vl - 2, " *");
    if (pending)
        v[vl - 2] = 0;
    const int vw = ui_text_width(UI_BODY, v);
    const UiCol ac = sel ? UI_SEL : UI_RGB(0xB0, 0xB0, 0xB0);
    ui_arrow(right - 4, r.y + r.h / 2, 8, 1, ac);
    ui_text(right - 14, ty, UI_BODY, pending ? UI_WARN : UI_VALUE, UI_RIGHT, v);
    ui_arrow(right - 24 - vw, r.y + r.h / 2, 8, -1, ac);
    if (pending)
        ui_disk((float)(right - 38 - vw), r.y + r.h / 2.0f, 3.0f, UI_WARN);
}

/* the main page tiles: an app-style icon (white symbol on a coloured rounded square) and a label */
static const UiMask *tile_icon(int id, UiCol *top, UiCol *bot)
{
    switch (id) {
    case M_SAVE: *top = UI_RGB(0x5C, 0xC0, 0xF6); *bot = UI_RGB(0x1E, 0x8E, 0xDA); return &k_icon_save;
    case M_LOAD: *top = UI_RGB(0x4E, 0xD0, 0xB6); *bot = UI_RGB(0x16, 0xA0, 0x88); return &k_icon_load;
    case M_AUDIO: *top = UI_RGB(0xFF, 0xB8, 0x4E); *bot = UI_RGB(0xF0, 0x84, 0x14); return &k_icon_audio;
    case M_VIDEO: *top = UI_RGB(0xA8, 0x8C, 0xF4); *bot = UI_RGB(0x76, 0x56, 0xDC); return &k_icon_video;
    case M_CONTROLS: *top = UI_RGB(0x6C, 0xD0, 0x6A); *bot = UI_RGB(0x2E, 0xA2, 0x3A); return &k_icon_controls;
    case M_RESET: *top = UI_RGB(0xA8, 0xA8, 0xB0); *bot = UI_RGB(0x7C, 0x7C, 0x86); return &k_icon_reset;
    case M_GAME_LIST: *top = UI_RGB(0x7C, 0x8C, 0xF0); *bot = UI_RGB(0x48, 0x58, 0xCC); return &k_icon_games;
    default: *top = UI_RGB(0xF4, 0x76, 0x62); *bot = UI_RGB(0xD8, 0x3E, 0x2C); return &k_icon_power;
    }
}

static void slot_arrow(int cx, int cy, int dir, bool sel)
{
    ui_disk((float)cx, (float)cy, 7.5f, sel ? UI_KNOB_SEL : UI_KNOB);
    ui_arrow(cx + dir, cy, 7, dir, sel ? UI_SEL : UI_KNOB_ARROW);
}

static void draw_main_tile(int i, bool sel)
{
    static const char *const names[M_COUNT] = {"Resume",   "Save",  "Load",      "",     "Audio",
                                               "Video",    "Controls", "Reset", "Game list", "Exit"};
    const Rect r = g_w[i];
    if (!r.w)
        return; /* not on this page (no game loaded) */
    const int cy = r.y + r.h / 2;
    card(r.x, r.y, r.w, r.h, 8, sel);
    if (i == M_RESUME && g_no_game) {
        const char *t = "Back to the game list";
        const int tw = ui_text_width(UI_TITLE, t), gw = 22 + 8 + tw, x0 = r.x + (r.w - gw) / 2;
        ui_mask(&k_icon_games, x0 + 11 - k_icon_games.w / 2, cy - k_icon_games.h / 2, UI_SEL);
        ui_text(x0 + 30, ui_text_y(UI_TITLE, cy), UI_TITLE, UI_TEXT, UI_LEFT, t);
        return;
    }
    if (g_no_game) { /* a tall tile: the icon above its name */
        UiCol top, bot;
        const UiMask *m = tile_icon(i, &top, &bot);
        ui_mask(m, r.x + r.w / 2 - m->w / 2, cy - 6 - m->h, UI_SEL);
        ui_text_fit(r.x + r.w / 2, ui_text_y(UI_BODY, cy + 12), UI_BODY, UI_TEXT, UI_CENTER, r.w - 8, names[i]);
        return;
    }
    if (i == M_RESUME) {
        const char *t = "Resume game";
        const int tw = ui_text_width(UI_TITLE, t), gw = 22 + 8 + tw, x0 = r.x + (r.w - gw) / 2;
        ui_disk((float)x0 + 11, (float)cy, 11, UI_SEL);
        ui_mask(&k_icon_play, x0 + 11 - 8 + 1, cy - 8, UI_WHITE);
        ui_text(x0 + 30, ui_text_y(UI_TITLE, cy), UI_TITLE, UI_TEXT, UI_LEFT, t);
        return;
    }
    if (i == M_SLOT) {
        const bool used = core_host_state_exists(g_slot);
        ui_textf(r.x + r.w / 2, ui_text_y(UI_BODY, cy - 7), UI_BODY, UI_TEXT, UI_CENTER, "Slot %d", g_slot);
        ui_text(r.x + r.w / 2, ui_text_y(UI_SMALL, cy + 9), UI_SMALL, used ? UI_VALUE : UI_TEXT_DIM, UI_CENTER,
                used ? "saved" : "empty");
        slot_arrow(r.x + 13, cy, -1, sel);
        slot_arrow(r.x + r.w - 13, cy, 1, sel);
        return;
    }
    UiCol top, bot;
    const UiMask *m = tile_icon(i, &top, &bot);
    const UiCol ic = (i == M_RESET || i == M_EXIT) ? UI_RGB(0x8E, 0x8E, 0x94) : UI_SEL;
    ui_mask(m, r.x + 9, cy - m->h / 2, ic);
    ui_text_fit(r.x + 33, ui_text_y(UI_BODY, cy), UI_BODY, UI_TEXT, UI_LEFT, r.w - 37, names[i]);
}

/* the controller page: the controller on a dark stage at the left, the selected button's mapping
 * in a panel at the right (no labels on the picture itself) */
#define STAGE_X 4
#define STAGE_Y (UI_HEADER_H + 3)
#define STAGE_W (5 + k_n64pad.w + 4)
#define STAGE_H (UI_H - UI_FOOTER_H - 3 - STAGE_Y)
#define PANEL_X (STAGE_X + STAGE_W + 5)
#define PANEL_W (UI_W - 4 - PANEL_X)
#define PANEL_CX (PANEL_X + PANEL_W / 2)
#define CHIP_Y (STAGE_Y + 114)
#define CHIP_H 26

static const char *pad_name(int h) { return k_n64pad_hot[h].name; }

/* the name under the button picture in the panel */
static const char *pad_title(int h)
{
    static const struct { const char *n, *t; } t[] = {{"A", "A button"}, {"B", "B button"}, {"Z", "Z trigger"},
                                                    {"L", "L button"},  {"R", "R button"}, {"Stick", "Control stick"}};
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (!strcmp(pad_name(h), t[i].n))
            return t[i].t;
    return pad_name(h);
}

/* the selected N64 button drawn on its own, centred on (cx, cy) */
static void draw_n64_button(int h, int cx, int cy)
{
    const char *k = k_n64pad_hot[h].kind, *n = pad_name(h);
    const float x = (float)cx, y = (float)cy;
    if (!strcmp(k, "a") || !strcmp(k, "b")) {
        const bool a = !strcmp(k, "a");
        ui_disk(x, y + 1.5f, 15, a ? UI_RGB(0x1B, 0x4B, 0xB0) : UI_RGB(0x23, 0x79, 0x3F));
        ui_disk(x, y, 15, a ? UI_RGB(0x3A, 0x78, 0xE6) : UI_RGB(0x4C, 0xB8, 0x6C));
        ui_text(cx, ui_text_y(UI_TITLE, cy), UI_TITLE, UI_WHITE, UI_CENTER, n);
    } else if (!strcmp(k, "c")) {
        static const struct { const char *n; int dir; } dirs[] = {{"C-Up", -2}, {"C-Down", 2}, {"C-Left", -1}, {"C-Right", 1}};
        int dir = 1;
        for (unsigned i = 0; i < 4; i++)
            if (!strcmp(n, dirs[i].n))
                dir = dirs[i].dir;
        ui_disk(x, y + 1.5f, 14, UI_RGB(0xB8, 0x8F, 0x00));
        ui_disk(x, y, 14, UI_RGB(0xF6, 0xCC, 0x1C));
        ui_arrow(cx, cy, 11, dir, UI_RGB(0xFF, 0xF6, 0xC8));
    } else if (!strcmp(k, "start")) {
        ui_disk(x, y + 1.5f, 13, UI_RGB(0xA8, 0x29, 0x1E));
        ui_disk(x, y, 13, UI_RGB(0xE8, 0x48, 0x3A));
    } else if (!strcmp(k, "shoulder")) {
        ui_rrect(cx - 20, cy - 9, 40, 18, 9, UI_RGB(0xC6, 0xC6, 0xCA), UI_RGB(0x9C, 0x9C, 0xA2));
        ui_text(cx, ui_text_y(UI_BODY, cy), UI_BODY, UI_TEXT, UI_CENTER, n);
    } else if (!strcmp(k, "z")) {
        ui_rrect(cx - 12, cy - 14, 24, 28, 6, UI_RGB(0x60, 0x60, 0x66), UI_RGB(0x48, 0x48, 0x4E));
        ui_text(cx, ui_text_y(UI_TITLE, cy), UI_TITLE, UI_WHITE, UI_CENTER, "Z");
    } else if (!strcmp(k, "stick")) {
        ui_disk(x, y, 16, UI_RGB(0x8E, 0x8E, 0x94));
        ui_disk(x, y + 1, 10, UI_RGB(0x55, 0x55, 0x5B));
        ui_disk(x, y, 10, UI_RGB(0xC4, 0xC4, 0xC8));
    } else { /* D-pad */
        ui_rrect(cx - 6, cy - 17, 12, 34, 3, UI_RGB(0x76, 0x76, 0x7C), UI_RGB(0x5E, 0x5E, 0x64));
        ui_rrect(cx - 17, cy - 6, 34, 12, 3, UI_RGB(0x6E, 0x6E, 0x74), UI_RGB(0x66, 0x66, 0x6C));
    }
}

static void chip(int cx, int cy, int w, const char *s, bool live, bool none)
{
    const int x = cx - w / 2, y = cy - CHIP_H / 2;
    if (live) {
        ui_rrect(x, y, w, CHIP_H, 8, UI_SEL_FILL, UI_SEL_FILL_BOT);
        ui_rrect_line(x - 1, y - 1, w + 2, CHIP_H + 2, 9, 2.0f, UI_SEL, 32);
        ui_text(cx, ui_text_y(UI_TITLE, cy), UI_TITLE, UI_SEL, UI_CENTER, s);
        return;
    }
    ui_rrect(x, y + 1, w, CHIP_H, 8, UI_RGB(0x2C, 0x2C, 0x30), UI_RGB(0x2C, 0x2C, 0x30));
    ui_rrect(x, y, w, CHIP_H, 8, none ? UI_RGB(0xA4, 0xA4, 0xAA) : UI_RGB(0x5E, 0x5E, 0x64),
             none ? UI_RGB(0x8C, 0x8C, 0x92) : UI_RGB(0x40, 0x40, 0x46));
    ui_text_fit(cx, ui_text_y(none ? UI_BODY : UI_TITLE, cy), none ? UI_BODY : UI_TITLE, UI_WHITE, UI_CENTER, w - 8, s);
}

static bool has_cstick(void)
{
    static int known = -1;
    if (known < 0) {
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        known = n3ds;
    }
    return known;
}

static void draw_panel(void)
{
    ui_clip(PANEL_X - 2, STAGE_Y - 2, UI_W - PANEL_X + 2, STAGE_H + 5);
    ui_background(STAGE_Y - 2, STAGE_H + 5);
    card(PANEL_X, STAGE_Y, PANEL_W, STAGE_H, 10, false);
    const int in = pad_input(g_sel);
    const int cx = PANEL_CX;
    ui_text(cx, STAGE_Y + 7, UI_SMALL, UI_TEXT_DIM, UI_CENTER, "N64");
    draw_n64_button(g_sel, cx, STAGE_Y + 38);
    ui_text_fit(cx, ui_text_y(UI_BODY, STAGE_Y + 67), UI_BODY, UI_TEXT, UI_CENTER, PANEL_W - 8, pad_title(g_sel));
    ui_fill(PANEL_X + 12, STAGE_Y + 81, PANEL_W - 24, 1, UI_DIVIDER);
    ui_text_fit(cx, STAGE_Y + 86, UI_SMALL, UI_TEXT_DIM, UI_CENTER, PANEL_W - 6,
                in == PAD_STICK ? (g_settings.swap_dpad ? "3DS D-Pad" : "Circle Pad range") : "3DS");
    const UiCol ac = UI_SEL;
    const int hint_y = STAGE_Y + STAGE_H - 30;
    if (g_capture >= 0) {
        chip(cx, CHIP_Y, PANEL_W - 20, "?", true, false);
        ui_text_fit(cx, hint_y, UI_SMALL, UI_SEL, UI_CENTER, PANEL_W - 6, "Press a button");
        ui_text_fit(cx, hint_y + 12, UI_SMALL, UI_SEL, UI_CENTER, PANEL_W - 6, "(hold 2 = both)");
    } else if (in == PAD_DPAD) {
        /* swap_dpad: the Circle Pad is the N64 D-pad and the 3DS D-pad the stick (racing games) */
        const bool sw = g_settings.swap_dpad;
        chip(cx, CHIP_Y, PANEL_W - 20, sw ? "Circle Pad" : "D-Pad", false, false);
        u32 used = 0;
        for (int i = 0; i < IN_COUNT; i++)
            used |= g_settings.map[i];
        const bool shared = !sw && (used & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)) != 0;
        ui_text(cx, hint_y, UI_SMALL, UI_TEXT_DIM, UI_CENTER,
                sw ? "D-Pad = stick" : shared ? "Not where mapped" : "A: swap with");
        ui_text(cx, hint_y + 12, UI_SMALL, UI_TEXT_DIM, UI_CENTER, sw ? "A: swap back" : shared ? "A: swap" : "Circle Pad");
    } else if (in == PAD_STICK) {
        char v[16];
        snprintf(v, sizeof(v), "%d%%", g_settings.stick_range);
        if (g_adjust)
            chip(cx, CHIP_Y, PANEL_W - 36, v, true, false);
        else
            ui_text(cx, ui_text_y(UI_TITLE, CHIP_Y), UI_TITLE, UI_VALUE, UI_CENTER, v);
        ui_arrow(PANEL_X + 9, CHIP_Y, 9, -1, ac);
        ui_arrow(PANEL_X + PANEL_W - 9, CHIP_Y, 9, 1, ac);
        const UiCol hc = g_adjust ? UI_SEL : UI_TEXT_DIM;
        ui_text(cx, hint_y, UI_SMALL, hc, UI_CENTER, g_adjust ? "Left / Right" : "Press A");
        ui_text(cx, hint_y + 12, UI_SMALL, hc, UI_CENTER, "to change");
    } else {
        const u32 k = g_settings.map[in];
        chip(cx, CHIP_Y, PANEL_W - 36, k ? map_name(k) : "None", false, !k);
        ui_arrow(PANEL_X + 9, CHIP_Y, 9, -1, ac);
        ui_arrow(PANEL_X + PANEL_W - 9, CHIP_Y, 9, 1, ac);
        if (in >= IN_CUP && in <= IN_CRIGHT && has_cstick()) {
            /* core_host reads the C-Stick for the C buttons whatever the mapping */
            ui_text(cx, hint_y, UI_SMALL, UI_TEXT_DIM, UI_CENTER, "The C-Stick");
            ui_text(cx, hint_y + 12, UI_SMALL, UI_TEXT_DIM, UI_CENTER, "works too");
        }
    }
    ui_noclip();
}

static void draw_pad(void)
{
    ui_clip(0, STAGE_Y - 2, PANEL_X - 2, STAGE_H + 5);
    ui_background(STAGE_Y - 2, STAGE_H + 5);
    ui_rrect(STAGE_X, STAGE_Y, STAGE_W, STAGE_H, 10, UI_RGB(0x58, 0x58, 0x5E), UI_RGB(0x3A, 0x3A, 0x40));
    ui_image(&k_n64pad, PAD_X, PAD_Y);
    const PadHot *p = &k_n64pad_hot[g_sel];
    const float r = (float)p->r + (g_capture >= 0 || g_adjust ? 5.0f : 3.5f);
    ui_ring((float)(PAD_X + p->x), (float)(PAD_Y + p->y), r + 1.0f, 4.0f, UI_WHITE, 20);
    ui_ring((float)(PAD_X + p->x), (float)(PAD_Y + p->y), r, 2.5f, UI_SEL, 32);
    ui_noclip();
    draw_panel();
}

static void draw_footer(void)
{
    ui_footer_begin();
    g_nfoot = 0;
    int x = 8;
    /* the controls page has five items while a game runs: closer together, or "START Game" ran
     * off the screen */
    const int squeeze = g_page == PAGE_CONTROLS && !g_no_game ? 5 : 0;
#define FOOT(glyph, label, key)                                                                       \
    do {                                                                                              \
        const int x0 = x;                                                                             \
        x = ui_footer_button(x, glyph, label, false) - squeeze;                                       \
        g_foot[g_nfoot++] = (typeof(g_foot[0])){{(s16)(x0 - 6), UI_H - UI_FOOTER_H, (s16)(x - x0), UI_FOOTER_H}, key}; \
    } while (0)
    if (g_page == PAGE_MAIN && g_no_game) {
        FOOT(UI_GLYPH_A, "OK", KEY_A);
        FOOT(UI_GLYPH_B, "Game list", KEY_B);
    } else if (g_page == PAGE_MAIN) {
        FOOT(UI_GLYPH_A, "OK", KEY_A);
        FOOT(UI_GLYPH_B, "Resume", KEY_B);
        if (!g_status[0])
            FOOT(UI_GLYPH_L UI_GLYPH_R, "Slot", 0);
    } else if (g_page == PAGE_CONTROLS && g_adjust) {
        FOOT(UI_GLYPH_DPAD, "Range", 0);
        FOOT(UI_GLYPH_A, "Done", KEY_A);
    } else if (g_page == PAGE_CONTROLS) {
        const int in = pad_input(g_sel);
        FOOT(UI_GLYPH_A, in == PAD_DPAD ? "Swap" : in == PAD_STICK ? "Change" : "Set", KEY_A);
        FOOT(UI_GLYPH_B, "Back", KEY_B);
        if (!g_status[0]) {
            if (in >= 0)
                FOOT(UI_GLYPH_X, "Clear", KEY_X);
            FOOT(UI_GLYPH_Y, "Reset", KEY_Y);
            /* per-game controls: START saves these for the running game / goes back to the shared */
            if (!g_no_game)
                FOOT(NULL, settings_game_controls_active() ? "START All" : "START Game", KEY_START);
        }
    } else {
        FOOT(UI_GLYPH_B, "Back", KEY_B);
        if (!g_status[0])
            FOOT(UI_GLYPH_DPAD, "Change", 0);
    }
#undef FOOT
    if (g_status[0])
        ui_text_fit(UI_W - 8, ui_text_y(UI_SMALL, UI_H - UI_FOOTER_H / 2), UI_SMALL,
                    UI_RGB(0xFF, 0xE0, 0x80), UI_RIGHT, UI_W - 16 - x + 8, g_status);
}

/* Video settings that apply only when a game starts: a banner pinned above the footer while any
 * differs from what the running game uses, with a button that restarts the game (CIA: the app
 * relaunches into the same game; a .3dsx cannot relaunch itself). Also the anti-aliasing note. */
#define BANNER_H 30
#define BANNER_Y (UI_H - UI_FOOTER_H - BANNER_H - 3)
#define BANNER_BTN_W 74
static bool can_relaunch(void) { return !envIsHomebrew(); }
static bool banner_shown(void) { return g_page == PAGE_VIDEO && (fb_emulation_pending() || aa_excluded_here()); }
static int banner_room(void) { return banner_shown() ? BANNER_H + 6 : 0; }

static void draw_banner(void)
{
    if (!banner_shown())
        return;
    const int x = 8, w = UI_W - 16 - 8, y = BANNER_Y;
    ui_rrect(x, y + 1, w, BANNER_H, 8, UI_SHADOW, UI_SHADOW);
    ui_rrect(x, y, w, BANNER_H, 8, UI_RGB(0xF2, 0x8C, 0x28), UI_RGB(0xDC, 0x6E, 0x10));
    const int cy = y + BANNER_H / 2;
    if (!fb_emulation_pending()) {
        ui_text_fit(x + 10, ui_text_y(UI_SMALL, cy), UI_SMALL, UI_WHITE, UI_LEFT, w - 20,
                    "Anti-aliasing stays off for this game (memory)");
        return;
    }
    ui_disk((float)(x + 12), (float)cy, 3.0f, UI_WHITE);
    if (!can_relaunch()) {
        ui_text_fit(x + 20, ui_text_y(UI_SMALL, cy), UI_SMALL, UI_WHITE, UI_LEFT, w - 28,
                    "Applies when you start ctr64 and the game again");
        return;
    }
    ui_text_fit(x + 20, ui_text_y(UI_SMALL, cy), UI_SMALL, UI_WHITE, UI_LEFT, w - BANNER_BTN_W - 30,
                "Restart the game to apply");
    const int bx = x + w - BANNER_BTN_W - 4;
    ui_rrect(bx, y + 4, BANNER_BTN_W, BANNER_H - 8, 7, UI_WHITE, UI_RGB(0xF4, 0xF4, 0xF4));
    ui_textf(bx + BANNER_BTN_W / 2, ui_text_y(UI_SMALL, cy), UI_SMALL, UI_RGB(0xC0, 0x5A, 0x08), UI_CENTER,
             "%s Restart", UI_GLYPH_X);
}

static bool banner_button_hit(int tx, int ty)
{
    const int x = 8, w = UI_W - 16 - 8;
    return banner_shown() && fb_emulation_pending() && can_relaunch() &&
           ui_hit(tx, ty, x + w - BANNER_BTN_W - 8, BANNER_Y, BANNER_BTN_W + 8, BANNER_H);
}

static void draw_list(void)
{
    ui_clip(0, LIST_Y - 4, UI_W, LIST_H + 8);
    ui_background(LIST_Y - 4, LIST_H + 8);
    ui_clip(0, LIST_Y - 3, UI_W, LIST_H + 6);
    for (int i = 0; i < g_nw; i++)
        if (g_w[i].y + g_w[i].h > LIST_Y - 3 && g_w[i].y < LIST_Y + LIST_H + 3)
            draw_list_row(i, i == g_sel);
    ui_noclip();
    draw_banner();
    const int content = g_nw * ROW_PITCH + banner_room();
    if (content > LIST_H) {
        const int bh = LIST_H * LIST_H / content, by = LIST_Y + (LIST_H - bh) * g_scroll / (content - LIST_H);
        ui_rrect(UI_W - 9, LIST_Y, 4, LIST_H, 2, UI_TRACK, UI_TRACK);
        ui_rrect(UI_W - 9, by, 4, bh, 2, UI_THUMB, UI_THUMB);
    }
}

static int list_max_scroll(void)
{
    const int content = g_nw * ROW_PITCH + banner_room();
    return content > LIST_H ? content - LIST_H : 0;
}

static void draw_page(void)
{
    static const char *const titles[] = {NULL, "Audio", "Video", "Controls"};
    layout();
    if (g_page == PAGE_MAIN) {
        ui_background(UI_HEADER_H, UI_H - UI_HEADER_H - UI_FOOTER_H);
        for (int i = 0; i < M_COUNT; i++)
            draw_main_tile(i, i == g_sel);
        if (g_no_game)
            ui_header("Settings", "ctr64 " CTR64_VERSION);
        else
            ui_header(game_title(), "Paused");
    } else {
        const bool own = g_page == PAGE_CONTROLS && !g_no_game && settings_game_controls_active();
        ui_header_tag(titles[g_page], own ? "(this game)" : NULL, g_no_game ? "Settings" : game_title());
        if (g_page == PAGE_CONTROLS)
            draw_pad();
        else
            draw_list();
    }
    draw_footer();
    ui_present();
}

/* redraw after a selection or value change (cheaper than the whole page) */
static void redraw_widget(int i, bool sel)
{
    if (g_page == PAGE_MAIN) {
        const Rect r = g_w[i];
        ui_clip(r.x - 3, r.y - 3, r.w + 6, r.h + 6);
        ui_background(r.y - 3, r.h + 6);
        ui_noclip();
        draw_main_tile(i, sel);
    }
}

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
    draw_footer();
    ui_present();
}

static void cycle_map(int input, int dir)
{
    int idx = 0;
    for (int i = 0; i < k_map_key_count; i++)
        if (k_map_keys[i] == g_settings.map[input])
            idx = i;
    idx = (idx + dir + k_map_key_count) % k_map_key_count;
    g_settings.map[input] = k_map_keys[idx];
}

/* left/right on an item; true when something changed */
static bool g_repaint; /* a change that needs the whole page drawn again (theme) */
static bool change(int i, int dir)
{
    Settings *s = &g_settings;
    switch (g_page) {
    case PAGE_MAIN:
        if (i == M_SAVE || i == M_LOAD || i == M_SLOT) {
            g_slot = (g_slot + dir + 10) % 10;
            return true;
        }
        return false;
    case PAGE_AUDIO:
        if (i == A_VOLUME) {
            s->volume += dir * 10;
            s->volume = s->volume < 0 ? 0 : s->volume > 100 ? 100 : s->volume;
        } else {
            s->audio = !s->audio;
        }
        menu_apply_settings();
        return true;
    case PAGE_VIDEO:
        if (i == V_FRAMESKIP)
            s->frameskip = (s->frameskip + dir + FRAMESKIP_AUTO + 1) % (FRAMESKIP_AUTO + 1);
        else if (i == V_LIMIT)
            s->speed_limit = !s->speed_limit;
        else if (i == V_AA) {
            if (!ctr64_aa_available())
                return false;
            s->anti_aliasing = !s->anti_aliasing;
            settings_publish();
        } else if (i == V_ASPECT) {
            s->aspect = (s->aspect + (dir ? dir : 1) + ASPECT_COUNT) % ASPECT_COUNT;
            settings_publish();
        } else if (i == V_BORDER) {
            s->border = (s->border + (dir ? dir : 1) + BORDER_COUNT) % BORDER_COUNT;
            if (!g_no_game)
                border_refresh();
        } else if (i == V_DARK) {
            s->dark_mode = !s->dark_mode;
            ui_set_dark(s->dark_mode);
            g_repaint = true; /* every colour changes */
        } else if (i == V_BOTTOM) {
            s->bottom_screen = (s->bottom_screen + (dir ? dir : 1) + BOTTOM_COUNT) % BOTTOM_COUNT;
        } else if (i == V_FB_EMU) {
            s->fb_emulation = !s->fb_emulation;
            settings_publish();
        } else if (i == V_FB_COPY) {
            s->fb_copy += dir;
            if (s->fb_copy > FB_COPY_ASYNC)
                s->fb_copy = FB_COPY_GAME;
            if (s->fb_copy < FB_COPY_GAME)
                s->fb_copy = FB_COPY_ASYNC;
            settings_publish();
        } else if (i == V_THREADED)
            s->threaded_video = !s->threaded_video;
        else if (i == V_THREADED_AUDIO)
            s->threaded_audio = !s->threaded_audio;
        else if (i == V_PRECOMPILE)
            s->remember_blocks = !s->remember_blocks;
        else if (i == V_FAST_COMPILE)
            s->fast_compile = !s->fast_compile;
        else if (i == V_PROFILER)
            s->profiler = (s->profiler + 3 + dir) % 3;
        else if (i == V_DEBUG)
            s->debug_info = !s->debug_info;
        else
            return false;
        return true;
    case PAGE_CONTROLS: {
        const int in = pad_input(i);
        if (in >= 0)
            cycle_map(in, dir);
        else if (in == PAD_STICK) {
            s->stick_range += dir * 5;
            s->stick_range = s->stick_range < 50 ? 50 : s->stick_range > 300 ? 300 : s->stick_range;
        } else
            return false;
        return true;
    }
    }
    return false;
}

/* the nearest widget in a direction (dx, dy one of -1/0/1), or -1 */
static int spatial(int from, int dx, int dy)
{
    const Rect a = g_w[from];
    const int ax = a.x + a.w / 2, ay = a.y + a.h / 2;
    int best = -1, best_score = 1 << 30;
    for (int i = 0; i < g_nw; i++) {
        if (i == from || !g_w[i].w)
            continue;
        const int bx = g_w[i].x + g_w[i].w / 2, by = g_w[i].y + g_w[i].h / 2;
        const int along = dx ? (bx - ax) * dx : (by - ay) * dy;
        const int across = dx ? abs(by - ay) : abs(bx - ax);
        if (along <= 2)
            continue;
        const int score = along + across * 2;
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

static void select_widget(int i)
{
    if (i < 0 || i == g_sel)
        return;
    const int old = g_sel;
    g_sel = i;
    const bool had_status = g_status[0] != 0;
    g_status[0] = 0; /* a message is about the item it came from */
    if (had_status || g_page == PAGE_CONTROLS)
        draw_footer();
    if (g_page == PAGE_MAIN) {
        redraw_widget(old, false);
        redraw_widget(g_sel, true);
    } else if (g_page == PAGE_CONTROLS) {
        draw_pad();
    } else {
        if (g_sel * ROW_PITCH < g_scroll)
            g_scroll = g_sel * ROW_PITCH;
        if (g_sel * ROW_PITCH + ROW_H > g_scroll + LIST_H - banner_room())
            g_scroll = g_sel * ROW_PITCH + ROW_H - LIST_H + banner_room();
        if (g_sel == g_nw - 1 && g_page == PAGE_VIDEO)
            g_scroll = list_max_scroll();
        layout();
        draw_list();
    }
    ui_present();
}

/* A confirmation dialog over the page: true for the action (A, or its button), false for
 * Cancel (B, its button, or a tap outside). The page is drawn again afterwards. */
static bool ask(const char *title, const char *line1, const char *line2, const char *action)
{
    ui_blend_fill(0, 0, UI_W, UI_H, UI_RGB(0, 0, 0), ui_is_dark() ? 16 : 12);
    const int x = 22, w = UI_W - 44, h = 120, y = (UI_H - h) / 2 - 4;
    ui_rrect(x, y + 2, w, h, 12, UI_RGB(0, 0, 0), UI_RGB(0, 0, 0));
    ui_rrect(x, y, w, h, 12, UI_CARD, UI_CARD_BOT);
    ui_text_fit(UI_W / 2, ui_text_y(UI_TITLE, y + 22), UI_TITLE, UI_TEXT, UI_CENTER, w - 24, title);
    if (line1)
        ui_text_fit(UI_W / 2, ui_text_y(UI_SMALL, y + 47), UI_SMALL, UI_TEXT_DIM, UI_CENTER, w - 20, line1);
    if (line2)
        ui_text_fit(UI_W / 2, ui_text_y(UI_SMALL, y + 62), UI_SMALL, UI_TEXT_DIM, UI_CENTER, w - 20, line2);
    const int bw = (w - 16 * 2 - 10) / 2, bh = 28, by = y + h - bh - 12;
    const int cx0 = x + 16, ax0 = cx0 + bw + 10;
    ui_rrect(cx0, by + 1, bw, bh, 14, UI_SHADOW, UI_SHADOW);
    ui_rrect(cx0, by, bw, bh, 14, UI_BG_TOP, UI_BG_BOT);
    ui_textf(cx0 + bw / 2, ui_text_y(UI_BODY, by + bh / 2), UI_BODY, UI_TEXT, UI_CENTER, "%s Cancel", UI_GLYPH_B);
    ui_rrect(ax0, by + 1, bw, bh, 14, UI_RGB(0x14, 0x9C, 0xDC), UI_RGB(0x14, 0x9C, 0xDC));
    ui_rrect(ax0, by, bw, bh, 14, UI_RGB(0x46, 0xC8, 0xF6), UI_RGB(0x1E, 0xA6, 0xE4));
    ui_textf(ax0 + bw / 2, ui_text_y(UI_BODY, by + bh / 2), UI_BODY, UI_WHITE, UI_CENTER, "%s %s", UI_GLYPH_A, action);
    ui_present();
    UiTouch t = {0};
    bool yes = false;
    while (aptMainLoop()) {
        ctr64_heartbeat++;
        gspWaitForVBlank();
        u32 down, held, up;
        ui_scan(&down, &held, &up);
        ui_touch_update(&t, down, held, up);
        if (down & KEY_A) {
            yes = true;
            break;
        }
        if (down & (KEY_B | KEY_SELECT))
            break;
        if (t.tap) {
            if (ui_hit(t.x, t.y, ax0, by - 4, bw, bh + 8)) {
                yes = true;
                break;
            }
            if (ui_hit(t.x, t.y, cx0, by - 4, bw, bh + 8) || !ui_hit(t.x, t.y, x, y, w, h))
                break;
        }
    }
    draw_page();
    return yes;
}

static void after_change(void)
{
    if (g_repaint) {
        g_repaint = false;
        draw_page();
        return;
    }
    if (g_page == PAGE_MAIN) {
        redraw_widget(M_SAVE, g_sel == M_SAVE);
        redraw_widget(M_LOAD, g_sel == M_LOAD);
        redraw_widget(M_SLOT, g_sel == M_SLOT);
    } else if (g_page == PAGE_CONTROLS) {
        if (g_status[0]) {
            g_status[0] = 0;
            draw_footer();
        }
        draw_pad();
    } else {
        draw_list();
    }
    ui_present();
}

static void go_page(Page p, int sel)
{
    g_page = p;
    g_sel = sel;
    g_scroll = 0;
    g_capture = -1;
    g_adjust = false;
    g_status[0] = 0;
    draw_page();
}

static int widget_at(int x, int y)
{
    if ((g_page == PAGE_AUDIO || g_page == PAGE_VIDEO) && (y < LIST_Y || (banner_shown() && y >= BANNER_Y)))
        return -1;
    for (int i = 0; i < g_nw; i++) {
        const Rect r = g_w[i];
        if (g_page != PAGE_MAIN && g_page != PAGE_CONTROLS && (y < LIST_Y || y >= LIST_Y + LIST_H))
            return -1;
        if (g_page == PAGE_CONTROLS) {
            const PadHot *p = &k_n64pad_hot[i];
            const float dx = (float)x + 0.5f - (PAD_X + p->x), dy = (float)y + 0.5f - (PAD_Y + p->y);
            const float rr = (float)(p->r + 6);
            if (dx * dx + dy * dy <= rr * rr)
                return i;
            continue;
        }
        if (r.w && ui_hit(x, y, r.x, r.y, r.w, r.h))
            return i;
    }
    return -1;
}

/* The settings pages from the game list, without a game: Audio, Video, Controls. Saved to
 * `settings_path` when changed; they apply to the next game started. */
void menu_settings(const char *settings_path)
{
    snprintf(g_settings_path, sizeof(g_settings_path), "%s", settings_path);
    g_no_game = true;
    menu_run();
    g_no_game = false;
}

MenuResult menu_run(void)
{
    const Settings before = g_settings;
    MenuResult result = MENU_CONTINUE;
    UiTouch touch = {0};

    audio_ndsp_pause(true);
    bottom_backlight(true);
    core_host_block_input(true);
    hidSetRepeatParameters(18, 5);
    console_window();
    consoleSetWindow(ui_console(), 0, 29, 40, 1);
    go_page(PAGE_MAIN, g_no_game ? M_VIDEO : M_RESUME);

    if (!g_no_game)
        remote_pause_begin(); /* remote runs only (remote.c) */
    bool alive;
    while ((alive = aptMainLoop())) {
        ctr64_heartbeat++;
        gspWaitForVBlank();
        if (!g_no_game)
            remote_pause_poll();
        u32 down, held, up;
        ui_scan(&down, &held, &up);
        u32 rep = hidKeysDownRepeat() | (down & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT));
        ui_touch_update(&touch, down, held, up);

        /* controls: a message in the footer ("N64 A = A", "Controls saved for this game") goes
         * away after a second and the buttons come back (they are hidden while it shows) */
        {
            static char seen[sizeof(g_status)];
            static u64 since;
            if (strcmp(seen, g_status)) {
                memcpy(seen, g_status, sizeof(seen));
                since = osGetTime();
            }
            if (g_page == PAGE_CONTROLS && g_status[0] && g_capture < 0 && osGetTime() - since >= 1000) {
                g_status[0] = seen[0] = 0;
                draw_footer();
                ui_present();
            }
        }

        /* controls: waiting for the 3DS button of an N64 input */
        if (g_capture >= 0) {
            /* every button pressed until all are released again: holding two (L and ZL) maps both */
            const u64 now = svcGetSystemTick();
            for (int i = 1; i < k_map_key_count; i++)
                g_capture_keys |= down & k_map_keys[i];
            const u32 got = g_capture_keys;
            if ((got && !(held & got)) || touch.tap || (down & KEY_SELECT) || now > g_capture_until) {
                if (got) {
                    g_settings.map[g_capture] = got;
                    snprintf(g_status, sizeof(g_status), "N64 %s = %s", k_n64pad_hot[g_sel].name, map_name(got));
                }
                g_capture = -1;
                draw_pad();
                draw_footer();
                ui_present();
            }
            continue;
        }

        if ((down & KEY_SELECT) && !g_no_game)
            break;

        /* controls: the Circle Pad range has focus */
        if (g_adjust) {
            int dir = (rep & (KEY_DLEFT | KEY_CPAD_LEFT | KEY_DDOWN | KEY_CPAD_DOWN)) ? -1
                    : (rep & (KEY_DRIGHT | KEY_CPAD_RIGHT | KEY_DUP | KEY_CPAD_UP)) ? 1 : 0;
            bool leave = (down & (KEY_A | KEY_B)) != 0;
            if (touch.tap) {
                if (ui_hit(touch.x, touch.y, PANEL_X, CHIP_Y - 20, PANEL_W, 46) &&
                    (touch.x < PANEL_X + 20 || touch.x >= PANEL_X + PANEL_W - 20))
                    dir = touch.x < PANEL_CX ? -1 : 1;
                else
                    leave = true;
            }
            if (dir && change(g_sel, dir))
                after_change();
            if (leave) {
                g_adjust = false;
                draw_pad();
                draw_footer();
                ui_present();
            }
            continue;
        }
        /* footer buttons by touch act as their key */
        if (touch.tap && touch.y >= UI_H - UI_FOOTER_H) {
            for (int i = 0; i < g_nfoot; i++)
                if (g_foot[i].key && ui_hit(touch.x, touch.y, g_foot[i].r.x, g_foot[i].r.y, g_foot[i].r.w, g_foot[i].r.h))
                    down |= g_foot[i].key;
            touch.tap = false;
        }

        /* list pages: drag to scroll */
        if ((g_page == PAGE_AUDIO || g_page == PAGE_VIDEO) && touch.dragging && touch.dy) {
            const int m = list_max_scroll();
            g_scroll -= touch.dy;
            g_scroll = g_scroll < 0 ? 0 : g_scroll > m ? m : g_scroll;
            layout();
            draw_list();
            ui_present();
            continue;
        }

        /* touch on a widget */
        if (touch.tap) {
            const int w = widget_at(touch.x, touch.y);
            if (w >= 0) {
                const Rect r = g_w[w];
                const bool was_sel = w == g_sel;
                select_widget(w);
                if (g_page == PAGE_MAIN) {
                    if (w == M_SLOT) {
                        change(w, touch.x < r.x + r.w / 2 ? -1 : 1);
                        after_change();
                    } else {
                        down |= KEY_A;
                    }
                } else if (g_page == PAGE_CONTROLS) {
                    if (was_sel)
                        down |= KEY_A;
                } else {
                    /* the value side: left half of it lowers, right half raises; else next value */
                    const int dir = touch.x >= r.x + r.w - 40 ? 1 : touch.x >= r.x + r.w / 2 ? -1 : (was_sel ? 1 : 0);
                    if (dir && change(w, dir)) {
                        after_change();
                    }
                }
            } else if (g_page == PAGE_CONTROLS && ui_hit(touch.x, touch.y, PANEL_X, CHIP_Y - 20, PANEL_W, 46)) {
                /* the panel: its arrows change the mapping, the chip itself waits for a button */
                if (touch.x < PANEL_X + 20 || touch.x >= PANEL_X + PANEL_W - 20) {
                    if (change(g_sel, touch.x < PANEL_CX ? -1 : 1))
                        after_change();
                } else {
                    down |= KEY_A;
                }
            }
        }

        /* keys */
        int dy = (rep & (KEY_DUP | KEY_CPAD_UP)) ? -1 : (rep & (KEY_DDOWN | KEY_CPAD_DOWN)) ? 1 : 0;
        int dx = (rep & (KEY_DLEFT | KEY_CPAD_LEFT)) ? -1 : (rep & (KEY_DRIGHT | KEY_CPAD_RIGHT)) ? 1 : 0;
        if (g_page == PAGE_MAIN && !g_no_game && (down & (KEY_L | KEY_R))) {
            change(M_SLOT, (down & KEY_L) ? -1 : 1);
            after_change();
        }
        if (dy || dx) {
            if (g_page == PAGE_AUDIO || g_page == PAGE_VIDEO) {
                if (dy)
                    select_widget((g_sel + dy + g_nw) % g_nw);
                else if (change(g_sel, dx))
                    after_change();
            } else if (g_page == PAGE_MAIN && g_sel == M_SLOT && dx) {
                change(M_SLOT, dx);
                after_change();
            } else {
                select_widget(spatial(g_sel, dx, dy));
            }
            continue;
        }
        if ((g_page == PAGE_VIDEO && (down & KEY_X) && banner_shown() && fb_emulation_pending() && can_relaunch()) ||
            (touch.tap && banner_button_hit(touch.x, touch.y))) {
            if (ask("Restart the game?", "The new video settings need a restart.", "Progress since your last save is lost",
                    "Restart")) {
                result = MENU_RESTART;
                goto done;
            }
            continue;
        }
        if (g_page == PAGE_CONTROLS && (down & KEY_X)) {
            const int in = pad_input(g_sel);
            if (in >= 0) {
                g_settings.map[in] = 0;
                after_change();
            }
        }
        if (g_page == PAGE_CONTROLS && (down & KEY_START) && !g_no_game && g_capture < 0 && !g_adjust) {
            const bool on = !settings_game_controls_active();
            if (settings_game_controls_set(on))
                snprintf(g_status, sizeof(g_status), on ? "Controls saved for this game" : "This game uses the shared controls");
            else
                snprintf(g_status, sizeof(g_status), "Could not save the controls");
            draw_page();
            ui_present();
            continue;
        }
        if (g_page == PAGE_CONTROLS && (down & KEY_Y)) {
            Settings d;
            settings_defaults(&d);
            memcpy(g_settings.map, d.map, sizeof(d.map));
            g_settings.stick_range = d.stick_range;
            g_settings.swap_dpad = d.swap_dpad;
            snprintf(g_status, sizeof(g_status), "Controls reset to defaults");
            draw_pad();
            draw_footer();
            ui_present();
        }
        if (down & KEY_B) {
            if (g_page == PAGE_MAIN)
                break;
            go_page(PAGE_MAIN, g_page == PAGE_AUDIO ? M_AUDIO : g_page == PAGE_VIDEO ? M_VIDEO : M_CONTROLS);
            continue;
        }
        if (!(down & KEY_A))
            continue;

        /* A */
        if (g_page == PAGE_CONTROLS) {
            const int in = pad_input(g_sel);
            if (in >= 0) {
                g_capture = in;
                g_capture_keys = 0;
                g_capture_until = svcGetSystemTick() + 5ULL * SYSCLOCK_ARM11;
                g_status[0] = 0;
                draw_pad();
                draw_footer();
                ui_present();
            } else if (in == PAD_DPAD) {
                g_settings.swap_dpad = !g_settings.swap_dpad;
                snprintf(g_status, sizeof(g_status), g_settings.swap_dpad ? "D-Pad is the stick now" : "Circle Pad is the stick again");
                draw_pad();
                draw_footer();
                ui_present();
            } else if (in == PAD_STICK) {
                g_adjust = true;
                g_status[0] = 0;
                draw_pad();
                draw_footer();
                ui_present();
            }
            continue;
        }
        if (g_page != PAGE_MAIN) {
            if (change(g_sel, 1))
                after_change();
            continue;
        }
        switch (g_sel) {
        case M_RESUME:
            goto done;
        case M_AUDIO:
            go_page(PAGE_AUDIO, 0);
            break;
        case M_VIDEO:
            go_page(PAGE_VIDEO, 0);
            break;
        case M_CONTROLS:
            go_page(PAGE_CONTROLS, 0);
            break;
        case M_SLOT:
            change(M_SLOT, 1);
            after_change();
            break;
        case M_SAVE: {
            char t[32];
            snprintf(t, sizeof(t), "Overwrite slot %d?", g_slot);
            if (core_host_state_exists(g_slot) && !ask(t, "The state saved in this slot is replaced", NULL, "Save"))
                break;
            set_status("Saving slot %d...", g_slot);
            set_status(core_host_save_state(g_slot) ? "Saved slot %d" : "Saving slot %d FAILED", g_slot);
            after_change();
            break;
        }
        case M_LOAD:
            if (!core_host_state_exists(g_slot)) {
                set_status("Slot %d is empty", g_slot);
                break;
            }
            {
                char t[32];
                snprintf(t, sizeof(t), "Load slot %d?", g_slot);
                if (!ask(t, "Progress since your last save is lost", NULL, "Load"))
                    break;
            }
            set_status("Loading slot %d...", g_slot);
            if (core_host_load_state(g_slot)) {
                snprintf(g_status, sizeof(g_status), "Loaded slot %d", g_slot);
                goto done;
            }
            set_status("Loading slot %d FAILED", g_slot);
            break;
        case M_RESET:
            if (!ask("Reset the game?", "Progress since your last save is lost", NULL, "Reset"))
                break;
            core_host_reset();
            goto done;
        case M_GAME_LIST:
            if (!ask("Back to the game list?", "Progress since your last save is lost", NULL, "Quit game"))
                break;
            result = MENU_GAME_LIST;
            goto done;
        case M_EXIT:
            if (!ask("Exit ctr64?", "Progress since your last save is lost", NULL, "Exit"))
                break;
            result = MENU_EXIT;
            goto done;
        }
    }
done:
    remote_pause_end();
    if (memcmp(&before, &g_settings, sizeof(before)) != 0)
        settings_save(g_settings_path);
    g_status[0] = 0;
    g_running_drawn = false;
    g_stats_dirty = true;
    if (result == MENU_CONTINUE && !g_no_game)
        draw_running();
    core_host_block_input(false);
    audio_ndsp_pause(false);
    return alive ? result : MENU_EXIT;
}
