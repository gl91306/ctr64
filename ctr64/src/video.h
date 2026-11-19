#pragma once

/* Presentation helpers shared by the emulator and the GPU test: one vertex format, one shader,
 * and the 240x400 portrait screen target that is transferred to the top LCD. */

#include <3ds.h>
#include <stdbool.h>

#include "pica/pica.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x, y, z;
    u8 r, g, b, a;
    float u, v;
} VideoVertex;

typedef struct {
    VideoVertex *v;
    int n, cap;
} VideoBatch;

bool video_init(void);
void video_exit(void);

shaderProgram_s *video_program(void);
/* The parsed shader library: DVLE[0] = basic.v.pica, DVLE[1] = gfx.v.pica (GLideN64 backend). */
DVLB_s *video_shader_library(void);
int video_projection_uniform(void);
const PicaTarget *video_screen(void);

/* Binds the standard program and vertex layout with the given projection. */
void video_use_program(const float proj[4][4]);

bool video_batch_begin(VideoBatch *b, int cap);
void video_batch_vertex(VideoBatch *b, float x, float y, u32 rgb, float u, float v);
/* Quad as two triangles; colors are top-left, top-right, bottom-left, bottom-right. */
void video_batch_quad4(VideoBatch *b, float x0, float y0, float x1, float y1, u32 tl, u32 tr, u32 bl,
                       u32 br, float u0, float v0, float u1, float v1);
void video_batch_quad(VideoBatch *b, float x0, float y0, float x1, float y1, u32 rgb);
void video_batch_draw(VideoBatch *b);

/* Clears the screen target and draws `tex` (w x h used of a tw x th tiled texture) 4:3 centered.
 * `top_is_t0`: true when image row 0 is stored at texture row 0 (CPU uploads), false for
 * render targets (row 0 is the bottom). */
void video_draw_to_screen(const void *tex, u16 tw, u16 th, GPU_TEXCOLOR fmt, u16 w, u16 h,
                          bool top_is_t0, bool linear);

/* Like video_draw_to_screen, but stretched over the whole 400x240 screen (the caller already
 * laid out the image, e.g. GLideN64's window buffer). */
void video_clear_draw(const PicaTarget *t, u32 rgba); /* a color target cleared by a quad (FCRAM) */
void video_draw_fullscreen(const void *tex, u16 tw, u16 th, GPU_TEXCOLOR fmt, u16 w, u16 h,
                           bool top_is_t0, bool linear);

void video_present(void);

/* start-up check: can the GPU sample non-power-of-two widths (frame buffer textures) */
void video_npot_selftest(void);
extern bool video_npot_ok;
extern char video_npot_report[96];

#ifdef __cplusplus
}
#endif
