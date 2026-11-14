#pragma once

#include <stdbool.h>
#include <stdio.h>

/* The profiler (menu: Video settings > Profiler). Each game session gets its own file,
 * sdmc:/3ds/ctr64/profile/<game> <date> <time>.prof, never overwritten:
 *   - a header (ctr64 build, settings, core.log),
 *   - the per-second statistics that otherwise go to stats.log,
 *   - one "v" line per VI with where its time went (see prof.c and tools/prof/prof_report.py).
 * A low-priority thread writes it to the SD card, so the card's write hiccups do not land on
 * the emulation threads. */

/* 0 off, 1 on, 2 detailed (also GLideN64's per-function timers: a few % slower) */
extern int ctr64_profiler;

/* starts a session for the loaded ROM; false if the file could not be made */
bool prof_begin(const char *rom_path);
void prof_end(void);
bool prof_active(void);
/* a FILE that appends to the profile (the per-second statistics) */
FILE *prof_stats_stream(void);
/* without the profiler: stats.log written the same way (by the background thread) */
FILE *prof_stats_begin(const char *path);
/* around each VI of the main loop */
void prof_vi_begin(void);
void prof_vi_end(unsigned vi, bool behind);
void prof_vi_run(unsigned long long ticks); /* the CPU thread's emulation time of this VI */
void prof_vi_resync(void);                  /* after a pause (the menu) */
/* an event at the next VI ("m <vi> <text>" in the file): state loads, saves, resets */
void prof_mark(const char *fmt, ...);
/* the speed limiter's sleep and precompile time (main.c limit_speed) */
extern unsigned long long ctr64_prof_sleep_ticks, ctr64_prof_rbidle_ticks;
/* the CPU thread waiting for the audio thread (audio_async.c) */
extern unsigned long long ctr64_prof_audio_wait_ticks;
