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
#define ATTIC_PALETTE   0x8300000UL  // 768 bytes, clear of the largest stream

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

// Where every billboard waits, above the overview map's pristine copy (which
// ends at $1DC00) and well clear of the colour RAM alias at $1F800. The 32K
// the program shares has room for one figure and not for two, and only the
// current mission's is ever drawn, so they are parked here at load time and
// src/sprite.c pulls the one it needs down into its near buffer when a flight
// starts. A DMA of a kilobyte, once, against a per-pixel far read forever.
#define SPRITE_STORE 0x1DC00UL

// Where the palette waits for vic4_set_palette. It used to be a C array, and
// the 768 bytes of it went to the survivor sprite when the 32K the program,
// its data and its stack share ran out. Attic RAM is fine for it: it is read
// once, at startup, and 768 slow reads are nothing.
#define PALETTE_BUF   ATTIC_PALETTE
#define PALETTE_BYTES 768

// Called as loading proceeds, with how much of it is done as a percentage.
// It is called once per chunk read, so most calls repeat the last figure.
typedef void (*load_progress)(uint8_t percent);

// Load the heightmap, colourmap, palette, overview map and survivor sprite.
// Returns 0 on success; `report` may be null.
//
// This runs with the display already in the game's own mode, so it must not
// print: the Kernal's screen editor writes colour RAM, which is live. Only
// the failure paths still do, and whatever they corrupt is redrawn by the
// error screen.
int load_resources(load_progress report);

// Why the last load_resources failed, and which file it was on. Null until
// something does fail.
const char *loader_error(void);
const char *loader_error_file(void);

// Read a small resource into a near buffer, returning how many bytes arrived
// -- which may be fewer than asked for, since the only way to read a whole
// file is to ask for more than it holds. See the note in the implementation.
int load_small(const char *name, void *dest, uint16_t length);

// 768 bytes for vic4_set_palette, valid once load_resources has succeeded.
const uint8_t __far *loaded_palette(void);

#endif
