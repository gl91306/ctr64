/* Accessors for libctru structures whose layout depends on enum size.
 * libctru is built with the toolchain default (1-byte enums); ctr64 code that includes core
 * headers is built with -fno-short-enums (see core.cmake). This file alone is compiled with
 * -fshort-enums (CMakeLists.txt), so it sees libctru's real layouts. */

#include <3ds.h>

#include "ctru_abi.h"

DVLE_s *ctru_dvle(DVLB_s *dvlb, int index)
{
    return dvlb && index >= 0 && (u32)index < dvlb->numDVLE ? &dvlb->DVLE[index] : NULL;
}
