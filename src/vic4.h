// VIC-IV split display: a 160x152 8-bit framebuffer for the 3D view,
// hardware-stretched to fill the 320-pixel-wide screen and double buffered in
// bank 1, over a six-row text panel for flight information.
#ifndef VIC4_H
#define VIC4_H

#include <stdint.h>

#define FB_WIDTH  160
#define FB_HEIGHT 152
#define FB_COLS   (FB_WIDTH / 8)   // 20 character strips across
#define FB_ROWS   (FB_HEIGHT / 8)  // 19 characters down each strip
#define FB_STRIDE (FB_ROWS * 64)   // 1216 bytes per strip

// The display is 25 character rows tall; whatever the framebuffer does not
// cover is the panel.
#define SCREEN_ROWS 25
#define PANEL_ROWS  (SCREEN_ROWS - FB_ROWS)
#define PANEL_COLS  FB_COLS

// Colour RAM is aliased into chip RAM at $1F800, so bank 1 only offers
// $10000-$1F7FF. Writing past that does not fault, it silently fills colour
// RAM with pixel data and the VIC-IV reads it back as character attributes,
// blanking cells across the display. Two 24320-byte buffers and the sky
// template leave plenty of room now; at 320 wide they will not, and one of
// the maps moves to attic RAM.
#define FB_A 0x10000UL  // 24320 bytes, to $15F00
#define FB_B 0x16000UL  // 24320 bytes, to $1BF00

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

// planes: 768 bytes, 256 red then 256 green then 256 blue, each already
// nybble-swapped for the palette registers (see tools/convmap.py).
void vic4_set_palette(const uint8_t *planes);

// Point the display at framebuffer 0 (FB_A) or 1 (FB_B).
void vic4_show(uint8_t buffer);

uint32_t vic4_base(uint8_t buffer);

#endif
