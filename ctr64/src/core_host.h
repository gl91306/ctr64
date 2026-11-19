#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Hosts the statically linked Mupen64Plus-Next core through its libretro API. */

bool core_host_init(const char *data_dir); /* e.g. "sdmc:/3ds/ctr64" */
bool core_host_load(const char *rom_path);
void core_host_run_frame(void);            /* emulates until the next VI */
void core_host_unload(void);
void core_host_shutdown(void);

double core_host_fps(void);
unsigned core_host_sample_rate(void);

/* save state slots: <data dir>/states/<ROM name>.state<slot> (same format as the Windows build) */
bool core_host_save_state(int slot);
void core_host_state_path(int slot, char *out, size_t n); /* states/<rom>.state<slot> */
void core_host_rb_save(void);
/* writes out core.log (it is buffered: menu, suspend, exit) */
void core_host_flush_log(void);
void core_host_rb_tick(void);
bool core_host_load_state(int slot);
bool core_host_state_exists(int slot);
void core_host_reset(void);
const char *core_host_rom_name(void);
void core_host_block_input(bool blocked); /* the game sees no buttons while the menu is open */

/* the loaded ROM's id (header CRCs + country), "" before a load: names per-game files */
const char *core_host_rom_id(void);
