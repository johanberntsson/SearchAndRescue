// Reading resources off the boot D81 into banked RAM.
#ifndef LOADER_H
#define LOADER_H

#include <stdint.h>

// The heightmap stays in chip RAM at 256x256, where a cell address is just
// the two high coordinate bytes and costs nothing. Above that it is too big
// and moves to attic RAM, which the inner loop pays for on every sample: an
// attic read plus a plane lookup. The colourmap is read once per span, so it
// lives in attic whatever its size.
#if HGT_SIZE > 256
#define HEIGHTMAP ATTIC_HEIGHTMAP
#else
#define HEIGHTMAP 0x40000UL
#endif

// Attic RAM: 8 MB of HyperRAM at $8000000, off the slow device bus rather
// than in chip RAM. The VIC-IV cannot see it, so it is only good for things
// the CPU or the DMA reach. It is 64K aligned, so a 256x256 map moved here
// keeps the "drop the two high coordinate bytes into the pointer" addressing.
#define ATTIC_BASE 0x8000000UL

// Attic layout. The maps are crunched on disk, so each is read into staging
// first and decrunched down to its home; staging sits clear of every map's
// largest possible size.
#define ATTIC_COLOURMAP 0x8000000UL  // up to 1 MB, 16 planes at most
#define ATTIC_HEIGHTMAP 0x8100000UL  // up to 1 MB, when it is too big for chip
#define ATTIC_STAGE     0x8200000UL  // the crunched stream being unpacked

#define COLOURMAP ATTIC_COLOURMAP

// A plane is one CELLS x CELLS map, so a whole 64K bank; a map of SIZE ships
// as (SIZE / CELLS)^2 of them, one per sub-cell corner, and the plane number
// is the bank byte. Keep CELLS in sync with tools/convmap.py.
#define CELLS      256
#define MAP_SIZE   CELLS
#define MAP_BYTES  ((uint32_t)CELLS * CELLS)

#define HGT_AXIS   (HGT_SIZE / CELLS)  // planes per axis
#define COL_AXIS   (COL_SIZE / CELLS)

// The panel's overview map: 32x32 pixels of colourmap, shipped already laid
// out as sixteen 8x8 full-colour characters by tools/convmap.py. It has to
// sit on a 64-byte boundary, because a character's NUMBER is its data address
// divided by 64. Bank 1 above the sky template is otherwise unused.
#define OVERVIEW       0x1D000UL
#define OVERVIEW_CHAR  (OVERVIEW / 64)
#define OVERVIEW_CHARS 4  // across and down; keep in sync with convmap.py
#define OVERVIEW_PX    (OVERVIEW_CHARS * 8)
#define OVERVIEW_BYTES (OVERVIEW_CHARS * OVERVIEW_CHARS * 64)

// Load the heightmap, colourmap, palette and overview map. Returns 0 on
// success. Call this before switching the display, so failures can still be
// printed.
int load_resources(void);

// 768 bytes for vic4_set_palette, valid once load_resources has succeeded.
const uint8_t *loaded_palette(void);

#endif
