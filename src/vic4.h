// VIC-IV split display: a 160x152 8-bit framebuffer for the 3D view,
// hardware-stretched to fill the 320-pixel-wide screen and double buffered in
// bank 1, over a six-row text panel for flight information.
#ifndef VIC4_H
#define VIC4_H

#include <stdint.h>

// The framebuffer is always 320 pixels wide, and CHRXSCL always 1:1, so the
// panel below it is 40 real characters rather than 20 stretched ones.
//
// A raster interrupt that switched the bottom rows to 40 columns was tried
// first and CANNOT work: the VIC-IV latches SCRNPTR, LINESTEP, CHRCOUNT *and
// CHRXSCL* once a frame, so changing the geometry part way down the screen
// does nothing until the next frame starts and whichever half was written
// last simply wins the whole of it. Only per-pixel registers like the border
// colour answer mid-frame. See documentation and irq notes in git history.
#define FB_WIDTH  320

// How many of those pixels get a ray of their own. WIDE=1 marches all 320,
// which doubles the march -- two thirds of the frame -- and roughly halves
// the frame rate. The default marches 160 and has each ray fill the two
// neighbouring pixels, which costs one extra byte write per span pixel and
// leaves the picture indistinguishable from the old stretched one.
#if WIDE
#define VX_COLS   320
#else
#define VX_COLS   160
#endif
#define VX_STEP   (FB_WIDTH / VX_COLS)  // pixels written per ray

#define FB_HEIGHT 152
#define FB_COLS   (FB_WIDTH / 8)   // character strips across
#define FB_ROWS   (FB_HEIGHT / 8)  // 19 characters down each strip
#define FB_STRIDE (FB_ROWS * 64)   // 1216 bytes per strip

// The display is 25 character rows tall; whatever the framebuffer does not
// cover is the panel.
#define SCREEN_ROWS 25
#define PANEL_ROWS  (SCREEN_ROWS - FB_ROWS)
#define PANEL_COLS  FB_COLS

// Where the overview map sits in the panel: a square of full-colour tiles
// tucked against the right edge, clear of the readouts on the left.
// PANEL_MAP_SIZE must match OVERVIEW_CHARS in loader.h.
#define PANEL_MAP_SIZE 4
#define PANEL_MAP_COL  (PANEL_COLS - PANEL_MAP_SIZE)
#define PANEL_MAP_ROW  1

// Colour RAM is aliased into chip RAM at $1F800, so bank 1 only offers
// $10000-$1F7FF. Writing past that does not fault, it silently fills colour
// RAM with pixel data and the VIC-IV reads it back as character attributes,
// blanking cells across the display.
//
// 48640 bytes each at 320 wide. Bank 1 holds one and the sky template; the
// other needs a whole bank of its own, which is why both maps live in attic
// RAM. Each buffer must stay inside one 64K bank: the fill loop's pointer
// step never carries into byte 2.
#define FB_A 0x10000UL  // to $1BE00
#define FB_B 0x50000UL  // to $5BE00

// In column-strip layout every strip's sky is the same FB_STRIDE bytes, so
// one strip is prepared at startup and DMAd across the buffer each frame
// instead of being drawn a pixel at a time.
#define FB_SKY 0x1C000UL

// Full-colour mode has no linear bitmap: the screen is a grid of 8x8
// characters whose 64 bytes of pixel data live at (character number * 64).
// Laying the characters out in column strips instead of rows makes a vertical
// span a single pointer stepping by 8, with no tile-boundary special case:
//
//     address(x, y) = base + (x >> 3) * FB_STRIDE + (x & 7) + y * 8
//
#define FB_COLUMN(base, x) ((base) + (uint32_t)((x) >> 3) * FB_STRIDE + ((x) & 7))

void vic4_init(void);

// Put one character into the panel. Character numbers below $100 are ordinary
// 8x8 text -- FCLRHI is set and FCLRLO is not, so only the framebuffer's own
// character numbers are full colour. Both screen tables get it: the panel is
// the same in either buffer, so it is not double buffered.
void vic4_panel_char(uint8_t col, uint8_t row, uint8_t ch, uint8_t colour);

// Put a full-colour character into a panel cell -- a character number above
// $FF, whose 64 bytes of palette indices live at number * 64. This is what
// mixes the overview map's tiles in among the panel's text: the mode is
// chosen per character number, so the two cost the same screen RAM.
void vic4_panel_tile(uint8_t col, uint8_t row, uint16_t charnum);

// Take the copy of the overview map that vic4_crosshair restores from. Call
// it once the map has been loaded and before the first crosshair.
void vic4_overview_ready(void);

// Mark a pixel of the overview map, 0..OVERVIEW_PX-1 on each axis, restoring
// whatever the last call covered. Drawn into the map rather than carried by a
// hardware sprite -- see the comment on the implementation for why.
void vic4_crosshair(uint8_t px, uint8_t py);

// planes: 768 bytes, 256 red then 256 green then 256 blue, each already
// nybble-swapped for the palette registers (see tools/convmap.py). Far,
// because it is read straight out of the banked buffer it was loaded into --
// the 32K the program lives in has no room to spare for something used once.
void vic4_set_palette(const uint8_t __far *planes);

// Point the display at framebuffer 0 (FB_A) or 1 (FB_B).
void vic4_show(uint8_t buffer);

uint32_t vic4_base(uint8_t buffer);

#endif
