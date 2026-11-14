#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Core audio (interleaved stereo int16) through the 3DS DSP. Needs sdmc:/3ds/dspfirm.cdc on
 * hardware; if NDSP cannot start, audio is silently dropped. */
bool audio_ndsp_init(unsigned sample_rate);
void audio_ndsp_exit(void);
size_t audio_ndsp_write(const int16_t *frames, size_t count);
unsigned audio_ndsp_queued_ms(void);
bool audio_ndsp_active(void);
unsigned audio_ndsp_rate(void);
void audio_ndsp_set_volume(float v); /* 0..1 */
void audio_ndsp_pause(bool paused);
