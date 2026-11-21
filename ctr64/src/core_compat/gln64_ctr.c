/* Symbols libretro.c expects from GLideN64's threaded OpenGL wrapper, which the PICA build
 * does not compile (the renderer always runs on the emulation thread). */

#include <stdbool.h>

bool threaded_gl_safe_shutdown = true;

void gln64_thr_gl_invoke_command_loop(void) {}

/* per-second backend counters shown by main.c: triangle draws, rect draws, clears, presents,
 * draws dropped (no valid target / culled entirely), frame buffer binds, failed GPU allocations */
unsigned int ctr64_gfx_stats[8];
unsigned int ctr64_gfx_fcram[2];
unsigned int ctr64_gfx_fb_reads[2]; /* reads of render targets: VRAM, FCRAM */
unsigned int ctr64_gfx_far_draws; /* draws clipped at the far w limit, pica_ContextImpl.cpp GB_WFAR (stats.log) */
unsigned int ctr64_gfx_vram_moves[2]; /* KB copied into / out of VRAM by keepInVram */ /* draws into FCRAM color / depth targets */
unsigned int ctr64_game_starts; /* games started (core_host.c) */
char ctr64_rt_mode[64]; /* frame buffer storage of the running game (pica_ContextImpl init), for stats.log */
unsigned int ctr64_gfx_shadow[6]; /* shadow copies for sampling non-power-of-two storage (pica_ContextImpl shadowOf): count, KB */
unsigned int ctr64_gfx_reused; /* render target storage taken over from a released texture (pica_ContextImpl allocStorage) */

/* ticks: [0] PICA backend draw calls, [1] GLideN64 display list processing, [2] present
 * (including the wait for VBlank) */
unsigned long long ctr64_gfx_ticks[3];

/* frames presented by the PICA backend so far (input scripts count in these) */
unsigned int ctr64_video_frames;
/* GLideN64 profiling scopes (GLideN64/src/Ctr64Prof.h), written to stats.log by main.c */
unsigned long long ctr64_prof_ticks[48];
unsigned int ctr64_prof_calls[48];
unsigned long long ctr64_prof_total[48];
/* dev switch nocrcmemo.txt (read in pica_ContextImpl.cpp): GLideN64 texture hashes never reused */
_Bool ctr64_crc_nomemo;
/* dev switch fbcheckfull.txt: FrameBuffer::isValid always compares every word (no sampled pass) */
_Bool ctr64_fbcheck_full;
/* scope names, Ctr64Prof.h ids (stats.log "prof" line, profiler "s" rows) */
const char *const ctr64_prof_names[48] = {
    "vtx", "procvtx", "tris", "texrect", "rect", "texupd", "texload", "tmem", "fbsave", "fbfromrdram",
    "fbrender", "gdptexrect", "states", "combiner", "fbtordram", "upload", "fbtexchk", "tmemcopy",
    "swrender", "rb_read", "rb_gx", "rb_conv", "rb_prep", "texcrc", "texconv", "texinit", "texalloc",
    "texswz", "texdel", "gpudrain", "texfetch", "texflush", "trstate", "trvtx", "subdiv", "trdraw",
    "upfetch", "upstage", "upwait", "upwrite", "swclip", "stash", "", "prep", "", "", "", ""};

/* backend storage accounting (pica_ContextImpl.cpp) */
unsigned int ctr64_gfx_mem[4];

/* last failed GPU allocation (pica_ContextImpl.cpp), printed to stats.log */
const char *ctr64_allocfail_where;
unsigned ctr64_allocfail_bytes;
unsigned ctr64_gfx_evicted; /* allocations that dropped cached textures (pica_ContextImpl.cpp gpuAlloc) */
