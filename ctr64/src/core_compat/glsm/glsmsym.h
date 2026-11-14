#pragma once

/* ctr64 replacement for libretro-common's glsm (OpenGL state manager).
 *
 * The Mupen64Plus-Next glue drives its GLideN64 path through glsm_ctl(). The 3DS has no
 * OpenGL: the PICA backend owns the GPU directly, so these calls only need to report success
 * and hand the core's context_reset callback to the ctr64 frontend. This header shadows
 * libretro-common/include/glsm/glsmsym.h via include order. */

#include <stdbool.h>
#include <libretro.h>

enum glsm_state_ctl {
    GLSM_CTL_NONE = 0,
    GLSM_CTL_STATE_SETUP,
    GLSM_CTL_STATE_BIND,
    GLSM_CTL_STATE_UNBIND,
    GLSM_CTL_STATE_CONTEXT_RESET,
    GLSM_CTL_STATE_CONTEXT_DESTROY,
    GLSM_CTL_STATE_CONTEXT_INIT,
    GLSM_CTL_IS_IMM_VBO,
    GLSM_CTL_SET_IMM_VBO,
    GLSM_CTL_UNSET_IMM_VBO,
    GLSM_CTL_IMM_VBO_DISABLE,
    GLSM_CTL_IMM_VBO_DRAW,
    GLSM_CTL_PROC_ADDRESS_GET
};

typedef bool (*glsm_imm_vbo_draw)(void *);
typedef bool (*glsm_imm_vbo_disable)(void *);
typedef bool (*glsm_framebuffer_lock)(void *);

typedef struct glsm_ctx_params {
    glsm_framebuffer_lock framebuffer_lock;
    glsm_imm_vbo_draw imm_vbo_draw;
    glsm_imm_vbo_disable imm_vbo_disable;
    retro_hw_context_reset_t context_reset;
    retro_hw_context_reset_t context_destroy;
    retro_environment_t environ_cb;
    bool stencil;
    unsigned major;
    unsigned minor;
} glsm_ctx_params_t;

bool glsm_ctl(enum glsm_state_ctl state, void *data);

/* ctr64: the core's context_reset/destroy, captured at GLSM_CTL_STATE_CONTEXT_INIT. */
extern retro_hw_context_reset_t ctr64_core_context_reset;
extern retro_hw_context_reset_t ctr64_core_context_destroy;
