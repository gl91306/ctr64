#pragma once

/* Word matching for the VRU (vru.c): the recorded speech against the words the game listens for.
 * The game gives each word as its sounds (Hey You, Pikachu!: sound unit numbers; Densha de GO!
 * 64: kana), so no dictionary is needed, only what each sound is. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VRU_MAX_UNITS 48 /* sound units (or kana) per word */

typedef struct {
    uint16_t unit[VRU_MAX_UNITS];
    uint8_t n;
    uint8_t japanese; /* kana (Densha de GO! 64, Pikachu Genki Dechu) instead of sound units */
} VruWord;

/* what the VRU reports (pastebin.com/6UiErk5h): error flags (0x0400 voice too low, 0x0800 too
 * high, 0x4000 no word close enough, 0x8000 too much noise), up to 5 hits of word index +
 * deviance (smaller: closer; the games take below ~0x640), levels and length */
typedef struct {
    uint16_t error_flags;
    uint16_t num_results;
    uint16_t mic_level;
    uint16_t voice_level;
    uint16_t voice_length;
    uint16_t match[10]; /* index, deviance, ...; index 0x7FFF: none */
} VruResult;

/* pcm: 16-bit mono at rate Hz */
void vru_recognize(const int16_t *pcm, int samples, int rate, const VruWord *words, int nwords, VruResult *out);

#ifdef __cplusplus
}
#endif
