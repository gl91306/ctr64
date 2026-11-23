/* Frameskip, at the render level. GLideN64 processes every display list as usual, so the game
 * sees every side effect it waits for (RDP interrupts, fills and copies to RDRAM, frame buffer
 * bookkeeping); in a skipped frame the PICA backend only drops the draws and clears into N64 frame buffers
 * (pica_ContextImpl.cpp). The N64 displays a buffer drawn in an earlier frame, so a present is
 * dropped (the previous image stays on screen) when the buffer it shows had draws dropped, not
 * when its own frame is skipped; a fixed pattern presented only stale buffers (OoT froze, SSB
 * flashed old frames). This saves the backend's draw
 * work and the GPU time, not GLideN64's own display-list processing. A frame ends at a present.
 * Frames after one that copied a frame buffer to or from RDRAM are never skipped: such effects
 * (OoT's pause menu, for one) reuse rendered images, which a skipped frame would leave empty.
 * Nor are frames skipped unless the last drawn one cleared or drew over (nearly) all of its frame
 * buffer and sampled no frame buffer: screens drawn a little at a time onto the persistent N64 image (Paper Mario's name
 * entry) or blended with the previous frame (MM's cutscene blur) would lose what a skipped frame
 * drew, and froze on hardware. */
#include <stdbool.h>

#include "settings.h"

volatile bool ctr64_speed_behind; /* set by the speed limiter (main.c) */
unsigned ctr64_frames_skipped;   /* presents dropped per second, for the stats */
unsigned ctr64_frames_drawskip;  /* frames whose draws were dropped, per second */
unsigned ctr64_frames_healed;    /* skipped buffers given the last picture shown (backend), per second */
bool ctr64_render_skip;          /* the current frame is being skipped */

static unsigned g_frame;

/* frame buffer copies to or from RDRAM since the last present: read-backs and texture updates
 * in the PICA backend (GLideN64 uploads textures with init, updates are its copies) */
unsigned ctr64_fb_copies;

static bool frame_buffer_effects(void)
{
    const bool any = ctr64_fb_copies != 0;
    ctr64_fb_copies = 0;
    return any;
}

bool ctr64_frame_cleared; /* the frame cleared or drew over ~all of a frame buffer (backend) */
bool ctr64_frame_fbtex;   /* the frame sampled a frame buffer as a texture (backend) */
bool ctr64_frame_drew;    /* the frame drew (or dropped draws) into an N64 frame buffer (backend) */

/* whether the frame just ended redraws its whole image: only then can the next one be skipped
 * (a skipped frame draws nothing, so it keeps the last drawn frame's verdict) */
static bool redraws_all(bool skipped)
{
    static bool all;
    if (!skipped)
        all = ctr64_frame_cleared && !ctr64_frame_fbtex;
    ctr64_frame_cleared = ctr64_frame_fbtex = false;
    return all;
}

/* Called at each present, stale when the image to present had draws dropped: returns whether
 * to present, and decides about the next frame. */
bool ctr64_frameskip_present(bool stale)
{
    static unsigned stale_run;
    const bool skipped = ctr64_render_skip;
    if (stale)
        ctr64_frames_skipped++;
    stale_run = stale ? stale_run + 1 : 0;
    /* a frame drawing nothing (SM64 presents every VI, draws every other) counts for nothing:
     * the decision carries over to the next frame that draws */
    if (!ctr64_frame_drew && stale_run < 2)
        return !stale;
    ctr64_frame_drew = false;
    if (skipped)
        ctr64_frames_drawskip++;
    g_frame++;
    const int n = g_settings.frameskip;
    const bool fx = frame_buffer_effects();
    if (!redraws_all(skipped) || fx || stale_run >= 2) /* never three stale presents in a row */
        ctr64_render_skip = false;
    else if (n == FRAMESKIP_AUTO)
        ctr64_render_skip = ctr64_speed_behind && !skipped; /* at most every other frame */
    else if (n > 0)
        ctr64_render_skip = g_frame % (unsigned)(n + 1) != 0;
    else
        ctr64_render_skip = false;
    return !stale;
}
