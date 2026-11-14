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
    int stick_range; /* percent: 100 = full N64 deflection at the Circle Pad's edge */
    uint32_t map[IN_COUNT]; /* 3DS KEY_* bit, 0 = none */
} Settings;

extern Settings g_settings;

extern const char *const k_input_names[IN_COUNT];
/* 3DS buttons offered for mapping, in menu order (SELECT is reserved for the menu) */
extern const uint32_t k_map_keys[];
extern const char *const k_map_key_names[];
extern const int k_map_key_count;

void settings_defaults(Settings *s);
/* Hands the frame buffer settings to GLideN64 (ctr64_setting_fb_*; the copy mode also to the
 * running game). Call after loading or changing settings. */
void settings_publish(void);
void settings_load(const char *path);
bool settings_save(const char *path);

#define SETTINGS_PATH "sdmc:/3ds/ctr64/settings.txt"
