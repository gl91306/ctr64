#include <glsm/glsmsym.h>

retro_hw_context_reset_t ctr64_core_context_reset;
retro_hw_context_reset_t ctr64_core_context_destroy;

bool glsm_ctl(enum glsm_state_ctl state, void *data)
{
    if (state == GLSM_CTL_STATE_CONTEXT_INIT && data) {
        const glsm_ctx_params_t *p = (const glsm_ctx_params_t *)data;
        ctr64_core_context_reset = p->context_reset;
        ctr64_core_context_destroy = p->context_destroy;
    }
    return true;
}
