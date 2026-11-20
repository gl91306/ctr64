#pragma once

/* Game list: box art on the top screen, a carousel that follows the selected game.
 *
 * Art comes from libretro-thumbnails (Nintendo 64, Named_Boxarts) over Wi-Fi and is kept on the
 * SD card (sdmc:/3ds/ctr64/boxart) already shrunk to at most 256x256 and laid out as a GPU texture,
 * so showing it is one file read. Games are matched by cartridge ID (ui/boxart_names.h), so the
 * ROM's file name does not matter. Without Wi-Fi, or for games without art, a title card is shown.
 * Downloads and file reads run on their own threads; the carousel is drawn with the GPU, which is
 * idle while the game list is open. Everything is freed by boxart_end, before a game starts. */

#include <stdbool.h>

void boxart_begin(void);
/* one game of the list, in list order; id = cartridge ID ("" if unknown), hack = a ROM hack
 * (shown under its own title: matched by file name only) */
void boxart_add(const char *path, const char *title, const char *id, bool hack);
/* after the last boxart_add: starts the loader and download threads */
void boxart_start(void);
void boxart_select(int index);
/* the theme changed: draw again */
void boxart_redraw(void);
/* draws the top screen when something changed (call once per frame) */
void boxart_frame(void);
/* stops the threads and frees the textures and the network (before a game starts) */
void boxart_end(void);
