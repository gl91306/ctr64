#pragma once

/* Side borders: what fills the two 40-pixel strips left and right of a 4:3 game picture on the
 * 400x240 top screen (Screen 4:3), instead of black. Video menu "Side borders":
 *   Off      black
 *   Box art  the game's box art (from the game list's cache), enlarged, blurred and darkened
 *   Console  dark grey plastic with an embossed D-pad on the left and A/B buttons on the right
 * The user's own picture wins over both (not over Off): sdmc:/3ds/ctr64/borders/<ROM file name
 * without its ending>.png or .jpg, else borders/default.png or .jpg. A 400x240 picture is used as it
 * is (only its sides show); other sizes are scaled to cover the screen. */

#include <stdbool.h>

#include <3ds.h>

#ifdef __cplusplus
extern "C" {
#endif

/* after the settings are loaded for a game: builds the borders of that ROM */
void border_prepare(const char *rom_path);
/* the setting changed (menu): builds them again */
void border_refresh(void);
/* the game ended: frees everything */
void border_free(void);

/* the texture to draw (RGB565, BORDER_TW x BORDER_TH, image row 0 in memory row 0); NULL: none.
 * *changed: new contents since the last call (flush the GPU's texture cache). */
#define BORDER_TW 128
#define BORDER_TH 256
#define BORDER_SIDE 40      /* strip width: (400 - 320) / 2 */
#define BORDER_RIGHT_U 64   /* texel column of the right strip */
const void *border_texture(bool *changed);

#ifdef __cplusplus
}
#endif
