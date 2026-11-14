#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum { ROM_BROWSER_QUIT, ROM_BROWSER_PICKED, ROM_BROWSER_GPU_TEST } RomBrowserResult;

/* Bottom-screen ROM picker. Lists .z64/.n64/.v64 files in `dir`; SELECT runs the GPU test. */
RomBrowserResult rom_browser_pick(const char *dir, char *out, size_t outsz);

/* the bottom screen while a game loads */
void rom_browser_loading(const char *path);
/* progress 0..100 of the first read of a streamed game, on the loading card */
void rom_browser_loading_progress(int pct);
/* the display name of the game shown by rom_browser_loading (NULL before) */
const char *rom_browser_title(void);
/* the game could not be started: an error screen; true = back to the game list, false = quit */
bool rom_browser_error(const char *path);
