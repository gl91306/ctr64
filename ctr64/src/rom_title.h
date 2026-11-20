#pragma once

#include <stdbool.h>
#include <stddef.h>

/* The display name of a game (and tags such as "USA, Rev 2") from its ROM header and file name. */
void rom_title_from_header(const unsigned char hdr[64], const char *file, char *title, size_t tn, char *tags, size_t gn);
/* the same, reading the header from the file at path */
void rom_title_read(const char *path, char *title, size_t tn, char *tags, size_t gn);
/* the same, plus the cartridge ID (header 0x3B-0x3E, e.g. "NSME"; "" when not letters/digits) */
void rom_title_read_id(const char *path, char *title, size_t tn, char *tags, size_t gn, char id[5]);
