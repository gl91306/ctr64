#pragma once

/* Breadcrumbs for the stall watchdog (main.c): code paths that can block or loop record where
 * they are; when the main loop stops advancing, the watchdog prints the last crumb. */

#ifdef __cplusplus
extern "C" {
#endif

extern const char *volatile ctr64_crumb;

#ifdef __cplusplus
}
#endif

#define CRUMB(s) (ctr64_crumb = (s))
