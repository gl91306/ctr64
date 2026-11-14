#pragma once

#include "pica.h"

/* Every GPU address is expressed as an offset from this physical base (start of VRAM),
 * which covers VRAM and the FCRAM linear heap. */
#define PICA_BUFFER_BASE_PADDR 0x18000000u

/* Marks all shadowed state stale; called at the start of each frame so a frame never
 * depends on what the previous one (or another process, after a suspend) left behind. */
void pica_state_reset(void);
/* Emits FRAMEBUFFER_FLUSH/INVALIDATE if anything was drawn since the last flush. */
void pica_state_flush_framebuffer(void);
/* Emits any pending uniform/attribute state; called right before a draw trigger. */
void pica_state_prepare_draw(void);
