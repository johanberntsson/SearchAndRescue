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

// Attic layout, and **the whole reason more than one map can be resident**.
// A map slot is 2 MB: a colourmap of up to 1 MB on a megabyte boundary, then
// its heightmap. Three of them fit below the staging area, and the maps are
// crunched on disk so each is read into staging first and decrunched down to
// its home.
//
// Nothing about a slot is in the renderer: `voxel_set_map` rebuilds the plane
// tables from a slot's bank byte, so switching maps between missions is 512
// bytes of table and no pixels at all. That is what makes a disk with several
// maps on it worth building -- see documentation/on-device-maps.md for the
// measurement that says a generated map is a quarter the size of a drawn one.
#define MAP_SLOTS       3
#define MAP_SLOT_BYTES  0x200000UL
#define MAP_SLOT(n)     (ATTIC_BASE + (uint32_t)(n) * MAP_SLOT_BYTES)
#define MAP_COLOURMAP(n) MAP_SLOT(n)                    // up to 1 MB, 16 planes
#define MAP_HEIGHTMAP(n) (MAP_SLOT(n) + 0x100000UL)     // up to 1 MB

#define ATTIC_COLOURMAP MAP_COLOURMAP(0)
#define ATTIC_HEIGHTMAP MAP_HEIGHTMAP(0)
#define ATTIC_STAGE     0x8600000UL  // the crunched stream being unpacked
#define ATTIC_PALETTE   0x8700000UL  // one 768-byte palette per map slot

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

// The information panel's background artwork: PANEL_ART_COLS x PANEL_ART_ROWS
// full-colour characters, 15360 bytes, laid out in reading order by
// tools/convmap.py so the loader reads it straight into character memory and
// src/panel.c does nothing but name the numbers.
//
// **Bank 4 is the only VIC-visible RAM with room for it.** Bank 1 is full to
// the byte, and bank 5 above the screen tables has 10 KB against the 15 this
// wants -- deduplicating the characters was measured and does not close that
// gap either, 209 of the 240 being unique. Bank 4 is free whenever the
// heightmap is larger than 256x256, which is every shipping build.
//
// Its palette entries are fixed ones above the sky (242..255 in convmap.py)
// that the terrain ramp cannot reach, so the artwork costs the figures none
// of their pool and one file serves every map.
#define PANEL_ART       0x40000UL
#define PANEL_ART_CHAR  (PANEL_ART / 64)
#define PANEL_ART_COLS  40  // keep in sync with PANEL_COLS in vic4.h
#define PANEL_ART_ROWS  6   // and PANEL_ROWS; src/panel.c checks both
#define PANEL_ART_BYTES (PANEL_ART_COLS * PANEL_ART_ROWS * 64UL)

// And the sprite plane that carries the panel's text over that artwork: eight
// sprite pointers, then five 384-byte bitmaps. Bank 4 again, above the
// picture, and on a 64-byte boundary because a 16-bit sprite pointer is the
// data address divided by 64. See src/overlay.h for what it is for.
#define OVERLAY_PTRS  (PANEL_ART + PANEL_ART_BYTES)  // $43C00
#define OVERLAY_PLANE (OVERLAY_PTRS + 64)            // $43C40, / 64 = $10F1

// And the panel's own character set: font/ClairsysOzmoo-Regular-US.fnt, 256
// glyphs of eight bytes, read off the disk into bank 4 above the plane.
//
// **The panel has a font of its own and the game's pages do not.** The pages
// are text characters and take whatever CHARPTR points at, which is the C65
// ROM's set at $2D000 -- no RAM, no loading, and it is the font every C65
// screen is in. The panel's text is not characters at all any more: it is
// drawn a glyph at a time into the sprite plane, so what it is drawn *from*
// is only a table this reads, and swapping that table costs 2 KB and nothing
// else. src/overlay.c checks at compile time that it clears the plane.
//
// It is in the C64 screen-code layout, which for everything the panel writes
// -- space through Z -- is the same as ASCII, so a glyph is indexed by the
// character itself. Lowercase is not: those codes are graphics in that
// layout, and the panel has always been uppercase.
//
// **Only 96 of the 256 glyphs ship**, from space up: that is every code the
// panel can be asked for, it is 768 bytes rather than 2048, and -- the reason
// it is 96 and not 256 -- it fits in the staging buffer, so the font is read
// the way the palette is and not through load_far. **load_far hangs on it**,
// exactly as it hangs on the palette: the loader stopped dead partway through
// a map, which is not even where the font is read. See the note on the
// palette in src/loader.c; that mystery is still unexplained and this is the
// second file to walk into it.
#define PANEL_FONT        0x44800UL
#define PANEL_FONT_FIRST  32   // the first glyph shipped: space
#define PANEL_FONT_GLYPHS 96   // keep in step with the slice in the Makefile
#define PANEL_FONT_BYTES  (PANEL_FONT_GLYPHS * 8)

#if HGT_SIZE <= 256
#error "the panel artwork and a 256x256 heightmap both want bank 4. Raise \
HGT_SIZE, or find the artwork 15 KB somewhere else the VIC-IV can read."
#endif

// Where every billboard waits, above the overview map's pristine copy (which
// ends at $1DC00) and well clear of the colour RAM alias at $1F800. The 32K
// the program shares has room for one figure and not for two, and only the
// current mission's is ever drawn, so they are parked here at load time and
// src/sprite.c pulls the one it needs down into its near buffer when a flight
// starts. A DMA of a kilobyte, once, against a per-pixel far read forever.
#define SPRITE_STORE 0x1DC00UL

// $1600-$1EFF is 2304 bytes of ordinary chip RAM that the linker rules for
// this target hand to a section called `zpsave`, which nothing in this
// program puts anything in. It is near-addressable, so moving a table there
// costs nothing per access, and the 32K the program shares is the scarcest
// thing in the build -- without this there is no room for a feature at all.
//
// **Only tables the renderer builds and owns.** It sits below $2001, where
// the C65 ROM keeps its own buffers, so nothing that has to survive a Kernal
// disk call may live here. Everything currently in it is first written after
// the last resource has been read.
//
// Budget, of 2304: 800 bytes of per-ray tables, 304 for the sprite's ramps,
// 96 for the rain, and 1024 for the renderer's plane lookups. The link fails
// outright if it is overrun.
//
// **That last kilobyte is why WIDE=1 is retired.** At 320 rays the per-ray
// tables are 1600 bytes rather than 800 and the plane tables no longer fit,
// and they are the ones that cannot move: the inner loop reads them 10240
// times a frame, so they have to be near. Marching 320 rays was already a
// settled dead end -- half the frame rate for a picture that barely changes,
// see todo.md -- so the space is better spent. The renderer still has the
// code for it; reviving it means finding another kilobyte first.
#define LOW_FREE __attribute__((section("zpsave")))

#if WIDE
#error "WIDE=1 is retired: the plane tables now live in the low free RAM at \
$1600, which only has room for them when the per-ray tables are half size. \
The march still supports 320 rays -- see LOW_FREE in loader.h for what would \
have to move to bring it back."
#endif

// The one near buffer everything is staged through. It has two users, and
// they are never live at the same time:
//
//   - the loader bounces every Kernal read through it -- CHUNK-sized pieces
//     for the maps and the overview, and the palette whole at 768 bytes;
//   - src/sprite.c keeps the flight's billboard here and draws out of it.
//
// Loading and flying cannot overlap: the Kernal cannot open a file once
// vic4_init has run, so nothing loads during a flight, and nothing draws
// during loading. **Do not call a loader function while a flight is running**
// -- it would repaint the figure being drawn.
//
// Sized by the sprite, which is the larger user at a 32x32 figure plus its
// four byte header; src/sprite.c static-asserts against it. That the
// palette's 768 bytes fit inside is the point rather than a happy accident:
// the palette used to be read into a separate 512-byte bounce buffer and
// overran it by 256 bytes every boot.
#define LOAD_STAGING (4 + 32 * 32)

extern uint8_t load_staging[LOAD_STAGING];

// Where the palette waits for vic4_set_palette. It used to be a C array, and
// the 768 bytes of it went to the survivor sprite when the 32K the program,
// its data and its stack share ran out. Attic RAM is fine for it: it is read
// once, at startup, and 768 slow reads are nothing.
// One per map slot, because a climate is a palette: the shared ramp means two
// generated maps hold the same *indices*, and a temperate island and a hot
// plain put different colours behind them. `vic4_set_palette` is given the
// flight's own at launch.
#define PALETTE_BYTES 768
#define PALETTE_SLOT(n) (ATTIC_PALETTE + (uint32_t)(n) * 1024UL)
#define PALETTE_BUF   PALETTE_SLOT(0)

// The panel's overview map is per map too, and small enough to keep all of
// them in bank 1: the flight's own is DMAd down to OVERVIEW at launch.
//
// **$1EC00 rather than $1E800 since the campaign owns the figures**, because
// SPRITE_STORE below it now has to hold SPRITE_MAX of them rather than a
// hardcoded two. Three 1028-byte slots reach $1E80C and three overviews reach
// exactly $1F800, where the colour RAM alias starts -- so bank 1 is full to
// the byte and both ends are checked at compile time in src/sprite.c. Another
// figure or another map slot means finding the room somewhere else.
#define OVERVIEW_STORE 0x1EC00UL
#define OVERVIEW_SLOT(n) (OVERVIEW_STORE + (uint32_t)(n) * OVERVIEW_BYTES)

// How many maps the disk actually carries is campaign_maps(), read off the
// disk with the campaign -- it used to be a constant here that had to be kept
// in step with the Makefile and with src/mission.c by hand. MAP_SLOTS above
// is still the ceiling: that is attic RAM and not a choice.

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
const uint8_t __far *loaded_palette(uint8_t slot);

// Which map slot the game is flying. Set by map_use() when a flight starts;
// read by anything that needs the *current* map's resources rather than a
// particular one -- the weather's clear sky, which restores sixteen entries
// from the palette that shipped with this map.
extern uint8_t map_current;

// Point the renderer, the palette and the panel's overview at one map slot.
// Everything a map is, in one call: 512 bytes of plane table, a palette
// upload and a 1 KB DMA. No pixels move and nothing reloads.
void map_use(uint8_t slot);

#endif
