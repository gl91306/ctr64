#pragma once

#include <stdbool.h>
#include <stdint.h>

/* User settings, edited in the bottom-screen menu (menu.c) and kept in
 * sdmc:/3ds/ctr64/settings.txt as key=value lines. */

/* N64 inputs that can be assigned a 3DS button (the analog stick, C-stick and D-pad are fixed:
 * Circle Pad = stick, C-stick = C buttons, D-pad = D-pad) */
enum {
    IN_A, IN_B, IN_Z, IN_L, IN_R, IN_START, IN_CUP, IN_CDOWN, IN_CLEFT, IN_CRIGHT,
    IN_COUNT
};

/* frameskip: 0 off, 1-3 skip that many of every n+1 frames, FRAMESKIP_AUTO only when behind */
#define FRAMESKIP_AUTO 4

/* fb_copy: the game's own setting (GLideN64's per-game ini), else GLideN64's copyToRDRAM mode */
#define FB_COPY_GAME (-1)
#define FB_COPY_ASYNC 2
enum { ASPECT_43, ASPECT_STRETCH, ASPECT_WIDE, ASPECT_COUNT };
/* bottom_screen while a game runs: the menu card and speed, black, or the backlight off */
enum { BOTTOM_MENU, BOTTOM_BLANK, BOTTOM_OFF, BOTTOM_COUNT };

typedef struct {
    int volume;      /* percent, 0-100 */
    bool audio;      /* off mutes */
    int frameskip;
    bool speed_limit;
    bool threaded_video; /* GLideN64 on core 2 (gfx_async.c), New 3DS only */
    bool threaded_audio; /* audio tasks on core 2 too (audio_async.c) */
    bool debug_info; /* the developer counters on the bottom screen */
    int profiler;    /* 0 off, 1 on, 2 detailed: a profile file per game session (prof.h) */
    bool remember_blocks; /* compile code the game ran before ahead of need (new_dynarec.c ctr64_rb_*) */
    bool fast_compile; /* skip optional optimiser passes when a game needs new code (ctr64_fast_compile) */
    int fb_copy;      /* frame buffer copy to RDRAM: FB_COPY_GAME or GLideN64's 0 off, 1 sync, 2 async */
    bool fb_emulation; /* GLideN64 frame buffer emulation (applies when a game starts) */
    bool anti_aliasing; /* render at 2x and scale down (New 3DS; applies when a game starts) */
    int aspect;        /* ASPECT_*: 4:3, stretched to 5:3, widescreen hack (applies when a game starts) */
    bool dark_mode;    /* the bottom-screen UI in dark colours (ui_set_dark) */
    int bottom_screen; /* BOTTOM_*: the bottom screen while a game runs (any touch opens the menu) */
    int stick_range; /* percent: 100 = full N64 deflection at the Circle Pad's edge */
    bool swap_dpad;  /* the 3DS D-pad is the N64 stick (full deflection), the Circle Pad the N64 D-pad */
    uint32_t map[IN_COUNT]; /* 3DS KEY_* bits (one or more buttons), 0 = none */
} Settings;

extern Settings g_settings;

extern const char *const k_input_names[IN_COUNT];
/* 3DS buttons offered for mapping, in menu order (SELECT is reserved for the menu) */
extern const uint32_t k_map_keys[];
extern const char *const k_map_key_names[];
extern const int k_map_key_count;

void settings_defaults(Settings *s);
/* the 3DS buttons in a map[] entry as text: "L+ZL", "none" */
void settings_keys_name(uint32_t keys, char *out, int size);
/* Hands the frame buffer settings to GLideN64 (ctr64_setting_fb_*; the copy mode also to the
 * running game). Call after loading or changing settings. */
void settings_publish(void);
void settings_load(const char *path);
bool settings_save(const char *path);

#define SETTINGS_PATH "sdmc:/3ds/ctr64/settings.txt"

/* Per-game controls: sdmc:/3ds/ctr64/controls/<ROM id>.txt holds a game's own mapping (map_*,
 * stick_range, swap_dpad). Loaded over the shared one when the game starts (after settings_load);
 * while it is in use, settings_save writes the controls there and keeps the shared ones in
 * settings.txt. */
void settings_game_controls_load(const char *rom_id);
bool settings_game_controls_active(void);
/* on: the current controls become the game's own (file written); off: the file is removed and
 * the shared controls are back */
bool settings_game_controls_set(bool on);
