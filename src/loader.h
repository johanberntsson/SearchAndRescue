// Reading resources off the boot D81 into banked RAM.
#ifndef LOADER_H
#define LOADER_H

#include <stdint.h>

#define HEIGHTMAP 0x40000UL
#define COLOURMAP 0x50000UL

// Attic RAM: 8 MB of HyperRAM at $8000000, off the slow device bus rather
// than in chip RAM. The VIC-IV cannot see it, so it is only good for things
// the CPU or the DMA reach. It is 64K aligned, so a 256x256 map moved here
// keeps the "drop the two high coordinate bytes into the pointer" addressing.
#define ATTIC_BASE 0x8000000UL

#define MAP_SIZE  256
#define MAP_BYTES ((uint32_t)MAP_SIZE * MAP_SIZE)

// Load the heightmap, colourmap and palette. Returns 0 on success. Call this
// before switching the display, so failures can still be printed.
int load_resources(void);

// 768 bytes for vic4_set_palette, valid once load_resources has succeeded.
const uint8_t *loaded_palette(void);

#endif
