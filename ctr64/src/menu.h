#pragma once

#include <stdbool.h>

/* Bottom-screen UI while a game runs: a status block with the speed, and a menu (SELECT)
 * that pauses the game for save states and settings. Drawn with ui/ui.h (touch and buttons). */

typedef struct {
    float vi_per_s, vi_target;     /* emulated VIs per second, and the N64's rate */
    float game_fps, game_fps_full; /* new images shown per second, and the rate at full speed */
    unsigned skipped;              /* frames skipped by frameskip in the last second */
    float cpu_ms;                  /* emulation time per VI */
} MenuStats;

/* MENU_GAME_LIST: unload the game and go back to the ROM browser */
/* MENU_RESTART: as MENU_GAME_LIST, then start the same game again (settings that apply at game start) */
typedef enum { MENU_CONTINUE, MENU_EXIT, MENU_GAME_LIST, MENU_RESTART } MenuResult;

void menu_init(const char *settings_path);
void menu_set_stats(const MenuStats *s);
/* while the game runs: draws the status block when it changed */
void menu_draw_running(void);
/* the menu, with the game paused; returns when the player resumes or exits */
MenuResult menu_run(void);
/* while the game runs: a touch that opens the menu (down: this frame's keys) */
bool menu_running_touched(unsigned long down);
/* first console row (inside the debug window) for the developer counters (settings: debug info) */
int menu_debug_row(void);
void menu_apply_settings(void);
