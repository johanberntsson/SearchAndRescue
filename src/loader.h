// Reading resources off the boot D81 into banked RAM.
#ifndef LOADER_H
#define LOADER_H

#include <stdint.h>

#define HEIGHTMAP 0x40000UL

// Attic RAM: 8 MB of HyperRAM at $8000000, off the slow device bus rather
// than in chip RAM. The VIC-IV cannot see it, so it is only good for things
// the CPU or the DMA reach. It is 64K aligned, so a 256x256 map moved here
// keeps the "drop the two high coordinate bytes into the pointer" addressing.
#define ATTIC_BASE 0x8000000UL

// The colourmap lives in attic RAM. It has to: at twice the heightmap's
// resolution it is 256K, four 64K planes at $8000000, $8010000, $8020000 and
// $8030000, one per half-cell corner. It can afford to be out there because
// the inner loop reads it once per span drawn rather than once per sample,
// and an attic read costs a flat +16 cycles (measured on hardware, see
// CLAUDE.md).
//
// The plane number is the bank byte, so this must stay 64K aligned with a
// zero bank byte.
#define COLOURMAP ATTIC_BASE

#define MAP_SIZE  256
#define MAP_BYTES ((uint32_t)MAP_SIZE * MAP_SIZE)

// Half-cell subdivision of the colourmap: COL_SUB x COL_SUB planes. Keep in
// sync with COL_SUB in tools/convmap.py.
#define COL_SUB     2
#define COL_PLANES  (COL_SUB * COL_SUB)
#define COL_BYTES   (MAP_BYTES * COL_PLANES)

// Load the heightmap, colourmap and palette. Returns 0 on success. Call this
// before switching the display, so failures can still be printed.
int load_resources(void);

// 768 bytes for vic4_set_palette, valid once load_resources has succeeded.
const uint8_t *loaded_palette(void);

#endif
