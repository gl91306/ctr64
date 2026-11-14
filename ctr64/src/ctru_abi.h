#pragma once

/* See ctru_abi.c: libctru structures that must not be indexed from -fno-short-enums code. */

#include <3ds.h>

#ifdef __cplusplus
extern "C" {
#endif

/* &dvlb->DVLE[index] with libctru's element size, or NULL if out of range. */
DVLE_s *ctru_dvle(DVLB_s *dvlb, int index);

#ifdef __cplusplus
}
#endif
