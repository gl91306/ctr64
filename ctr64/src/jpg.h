#pragma once

/* JPEG decoder for box art the user puts on the SD card (boxart.c): baseline and progressive
 * Huffman images, gray or color. Large images come out at 1/2, 1/4 or 1/8 size (the art is shrunk
 * to 256 pixels anyway), which keeps time and memory small. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* RGBA8 (malloc), *w x *h; NULL with a reason in *err. *quit stops it. */
uint8_t *jpg_decode(const uint8_t *data, size_t n, int *w, int *h, const volatile bool *quit, const char **err);
