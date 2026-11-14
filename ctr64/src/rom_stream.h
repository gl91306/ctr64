/* ctr64: cartridge ROM streamed from the SD card (games that do not fit in memory, Old 3DS).
 * The core reads the ROM through ctr64_rs_page() (device/memory/memory.h) while this is active. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* the ROM should be streamed rather than loaded whole (size in bytes) */
bool rom_stream_wanted(long size, const char *data_dir);

/* Opens the stream on f (ownership passes; it stays open while the game runs), with the MD5 the
 * core needs (from <data_dir>/jit/<id>.md5, or computed once by reading the whole file;
 * progress(0..100) is called meanwhile). Returns the first page in the core's byte order (it
 * stays resident), or NULL. */
const uint8_t *rom_stream_open(FILE *f, long size, const char *data_dir, void (*progress)(int pct));

/* gives memory back to the heap when it runs low (call now and then while the game runs) */
void rom_stream_trim(void);

/* one line of counters for core.log / debug info, "" when not streaming */
const char *rom_stream_report(void);

void rom_stream_close(void);
