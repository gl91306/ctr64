#include "settings.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Settings g_settings;

const char *const k_input_names[IN_COUNT] = {"A", "B", "Z", "L", "R", "Start", "C-Up", "C-Down", "C-Left", "C-Right"};

const uint32_t k_map_keys[] = {0,     KEY_A,  KEY_B,     KEY_X,     KEY_Y,     KEY_L,      KEY_R,
                               KEY_ZL, KEY_ZR, KEY_START, KEY_DUP, KEY_DDOWN, KEY_DLEFT, KEY_DRIGHT};
const char *const k_map_key_names[] = {"none", "A",  "B",     "X",       "Y",         "L",         "R",
                                       "ZL",   "ZR", "START", "D-Up",    "D-Down",    "D-Left",    "D-Right"};
const int k_map_key_count = sizeof(k_map_keys) / sizeof(k_map_keys[0]);

void settings_defaults(Settings *s)
{
    memset(s, 0, sizeof(*s));
    s->volume = 100;
    s->audio = true;
    s->frameskip = FRAMESKIP_AUTO; /* only skips drawing while emulation is behind */
    s->speed_limit = true;
    s->threaded_video = true;
    s->threaded_audio = true; /* New 3DS only (audio_async.c) */
    s->debug_info = false;
    s->profiler = 0;
    s->remember_blocks = true;
    /* shorter stalls for ~2-7% more CPU work in gameplay (and an extra load per N64 memory access
     * in the code it makes): off since the background precompile on core 1 (2026-11-08), CPU
     * emulation being what holds most games below full speed on a New 3DS */
    s->fast_compile = false;
    s->fb_copy = FB_COPY_GAME;
    s->fb_emulation = true;
    s->anti_aliasing = false;
    s->aspect = ASPECT_43;
    s->stick_range = 100;
    s->map[IN_A] = KEY_A;
    s->map[IN_B] = KEY_B;
    s->map[IN_Z] = KEY_L;
    s->map[IN_L] = KEY_ZL;
    s->map[IN_R] = KEY_R;
    s->map[IN_START] = KEY_START;
    s->map[IN_CUP] = KEY_X;
    s->map[IN_CLEFT] = KEY_Y;
    s->map[IN_CDOWN] = 0; /* the C-stick gives all four on a New 3DS */
    s->map[IN_CRIGHT] = 0;
    bool new3ds = false;
    APT_CheckNew3DS(&new3ds);
    if (!new3ds) {
        /* no C-stick, ZL or ZR: the C buttons on the D-pad (which then is not the N64 D-pad,
         * rarely used by games), N64 L on X */
        s->map[IN_L] = KEY_X;
        s->map[IN_CUP] = KEY_DUP;
        s->map[IN_CDOWN] = KEY_DDOWN;
        s->map[IN_CLEFT] = KEY_DLEFT;
        s->map[IN_CRIGHT] = KEY_DRIGHT;
    }
}

/* read by GLideN64's Config_LoadConfig (custom/GLideN64/mupenplus/Config_mupenplus.cpp) */
int ctr64_setting_fb_copy = FB_COPY_GAME;
int ctr64_setting_fb_emulation = 1;
int ctr64_setting_aa = 0, ctr64_setting_aspect = ASPECT_43;
void ctr64_apply_fb_copy(int mode); /* GLideN64: the running game's copy mode */

void settings_publish(void)
{
    ctr64_setting_fb_copy = g_settings.fb_copy;
    ctr64_setting_fb_emulation = g_settings.fb_emulation;
    ctr64_setting_aa = g_settings.anti_aliasing;
    ctr64_setting_aspect = g_settings.aspect;
    ctr64_apply_fb_copy(g_settings.fb_copy);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint32_t key_by_name(const char *n)
{
    for (int i = 0; i < k_map_key_count; i++)
        if (!strcmp(n, k_map_key_names[i]))
            return k_map_keys[i];
    return 0;
}

static const char *key_name(uint32_t k)
{
    for (int i = 0; i < k_map_key_count; i++)
        if (k_map_keys[i] == k)
            return k_map_key_names[i];
    return "none";
}

void settings_load(const char *path)
{
    settings_defaults(&g_settings);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        Settings *s = &g_settings;
        if (!strcmp(k, "volume"))
            s->volume = clampi(atoi(v), 0, 100);
        else if (!strcmp(k, "audio"))
            s->audio = atoi(v) != 0;
        else if (!strcmp(k, "frameskip"))
            s->frameskip = clampi(atoi(v), 0, FRAMESKIP_AUTO);
        else if (!strcmp(k, "speed_limit"))
            s->speed_limit = atoi(v) != 0;
        else if (!strcmp(k, "threaded_video"))
            s->threaded_video = atoi(v) != 0;
        else if (!strcmp(k, "threaded_audio"))
            s->threaded_audio = atoi(v) != 0;
        else if (!strcmp(k, "remember_blocks"))
            s->remember_blocks = atoi(v) != 0;
        else if (!strcmp(k, "fast_compile"))
            s->fast_compile = atoi(v) != 0;
        else if (!strcmp(k, "debug_info"))
            s->debug_info = atoi(v) != 0;
        else if (!strcmp(k, "profiler"))
            s->profiler = atoi(v) < 0 ? 0 : atoi(v) > 2 ? 2 : atoi(v);
        else if (!strcmp(k, "fb_copy"))
            s->fb_copy = clampi(atoi(v), FB_COPY_GAME, FB_COPY_ASYNC);
        else if (!strcmp(k, "fb_emulation"))
            s->fb_emulation = atoi(v) != 0;
        else if (!strcmp(k, "anti_aliasing"))
            s->anti_aliasing = atoi(v) != 0;
        else if (!strcmp(k, "aspect"))
            s->aspect = clampi(atoi(v), 0, ASPECT_COUNT - 1);
        else if (!strcmp(k, "dark_mode"))
            s->dark_mode = atoi(v) != 0;
        else if (!strcmp(k, "stick_range"))
            s->stick_range = clampi(atoi(v), 50, 150);
        else if (!strncmp(k, "map_", 4))
            for (int i = 0; i < IN_COUNT; i++)
                if (!strcmp(k + 4, k_input_names[i]))
                    s->map[i] = key_by_name(v);
    }
    fclose(f);
}

bool settings_save(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    const Settings *s = &g_settings;
    fprintf(f, "volume=%d\naudio=%d\nframeskip=%d\nspeed_limit=%d\nthreaded_video=%d\nthreaded_audio=%d\n"
            "debug_info=%d\nfb_copy=%d\nfb_emulation=%d\nstick_range=%d\nremember_blocks=%d\nfast_compile=%d\n"
            "anti_aliasing=%d\naspect=%d\nprofiler=%d\ndark_mode=%d\n", s->volume,
            s->audio, s->frameskip, s->speed_limit, s->threaded_video, s->threaded_audio, s->debug_info, s->fb_copy,
            s->fb_emulation, s->stick_range, s->remember_blocks, s->fast_compile, s->anti_aliasing, s->aspect,
            s->profiler, s->dark_mode);
    for (int i = 0; i < IN_COUNT; i++)
        fprintf(f, "map_%s=%s\n", k_input_names[i], key_name(s->map[i]));
    return fclose(f) == 0;
}
