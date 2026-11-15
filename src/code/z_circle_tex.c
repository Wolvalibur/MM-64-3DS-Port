#include "ultra64.h"

#ifdef __3DS__
/* PORT (2026-11-15): writable section: the ROM-free package takes it out (see fault_drawer.c sFaultDrawerFont) */
__attribute__((section(".data.port_rf_circle")))
#endif
const u64 gCircleTex[] = {
#include "assets/code/circle_tex/circle.i4.inc.c"
};
