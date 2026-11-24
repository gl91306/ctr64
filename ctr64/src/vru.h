#pragma once

/* The N64 Voice Recognition Unit on the 3DS microphone (vru.c), for the games that use one:
 * Hey You, Pikachu! / Pikachu Genki Dechu (NPG) and Densha de GO! 64 (ND6). The core speaks the
 * VRU's protocol (mupen64plus-core device/controllers/vru_controller.c) and asks the frontend for
 * the word list, the microphone and the results. Talking: hold the button mapped to the N64 Z
 * trigger; the words are matched when it is let go (vru_rec.c). */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* from the ROM header (0x40 bytes, big-endian): puts a VRU on port 4 for the voice games */
void vru_detect(const unsigned char *header);
bool vru_active(void);
/* every input poll of port 4: the talk button's state in, the Z bit to report out (held while
 * talking and until the words have been matched) */
bool vru_talk(bool held);
/* the game ended: microphone off, memory freed */
void vru_shutdown(void);

#ifdef __cplusplus
}
#endif
