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

// At 320 wide the second framebuffer needs a whole 64K bank, and bank 5 is
// the only one left -- so the colourmap moves out to attic RAM. It can
// afford to: the inner loop reads it once per span drawn rather than once
// per sample, and an attic read costs a flat +16 cycles (measured, see
// CLAUDE.md), which is under 2% of a frame.
#if WIDE
#define COLOURMAP ATTIC_BASE
#else
#define COLOURMAP 0x50000UL
#endif

#define MAP_SIZE  256
#define MAP_BYTES ((uint32_t)MAP_SIZE * MAP_SIZE)

// Load the heightmap, colourmap and palette. Returns 0 on success. Call this
// before switching the display, so failures can still be printed.
int load_resources(void);

// 768 bytes for vic4_set_palette, valid once load_resources has succeeded.
const uint8_t *loaded_palette(void);

#endif
