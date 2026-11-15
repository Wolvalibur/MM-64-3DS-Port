#ifndef SEGMENTED_ADDRESS_H

#ifdef __3DS__
/* PORT (2026-11-08): z64.h includes this header as "segmented_address.h", and a quoted include looks in the including
 * file's own folder (include/) before the -I paths, so every game file that took it through z64.h got the N64
 * SEGMENTED_TO_K0 (gSegments[n] + K0BASE + offset) instead of the port's (port/include/segmented_address.h), which
 * passes native asset pointers through. Applied to a native pointer the N64 macro keeps only its low 24 bits, so it was
 * right only for data in the binary's first 16 MB. Tingle's eyes lie past that: they were read from an unmapped
 * 0x00065CC8 and the renderer kept whatever texture came before (user report, boot.log "[TEX] skip unmapped tex"). The
 * same macro feeds the eyes and mouths of Romani, the pirates, Guru-Guru, Pamela and the Skulltula man, and Majora's
 * textures. */
#include "../port/include/segmented_address.h"
#else
#define SEGMENTED_ADDRESS_H

#include "ultra64.h"

#include "stdint.h"

extern uintptr_t gSegments[NUM_SEGMENTS];

#define SEGMENTED_TO_K0(addr) (void*)((gSegments[SEGMENT_NUMBER(addr)] + K0BASE) + SEGMENT_OFFSET(addr))

#endif
#endif
