// VIC-IV full-colour display: a 160x200 8-bit framebuffer, hardware-stretched
// to fill the 320-pixel-wide screen, double buffered in bank 1.
#ifndef VIC4_H
#define VIC4_H

#include <stdint.h>

#define FB_WIDTH  160
#define FB_HEIGHT 192
#define FB_COLS   (FB_WIDTH / 8)   // 20 character strips across
#define FB_ROWS   (FB_HEIGHT / 8)  // 24 characters down each strip
#define FB_STRIDE (FB_ROWS * 64)   // 1536 bytes per strip

// Colour RAM is aliased into chip RAM at $1F800, so bank 1 only offers
// $10000-$1F7FF: 63488 bytes, which is 512 short of two full-height 160x200
// buffers. Hence 192 rows rather than 200 -- 30720 bytes each, and room left
// over for the blank character below. Writing past $1F800 does not fault, it
// silently fills colour RAM with pixel data and the VIC-IV reads it back as
// character attributes, blanking cells across the display.
#define FB_A 0x10000UL
#define FB_B 0x17800UL

// 64 zero bytes, for the screen row past the bottom of the framebuffer.
#define FB_BLANK 0x1F000UL

// The display is 25 character rows tall whatever the framebuffer holds.
#define SCREEN_ROWS 25

// Full-colour mode has no linear bitmap: the screen is a grid of 8x8
// characters whose 64 bytes of pixel data live at (character number * 64).
// Laying the characters out in column strips instead of rows makes a vertical
// span a single pointer stepping by 8, with no tile-boundary special case:
//
//     address(x, y) = base + (x >> 3) * FB_STRIDE + (x & 7) + y * 8
//
#define FB_COLUMN(base, x) ((base) + (uint32_t)((x) >> 3) * FB_STRIDE + ((x) & 7))

void vic4_init(void);

// planes: 768 bytes, 256 red then 256 green then 256 blue, each already
// nybble-swapped for the palette registers (see tools/convmap.py).
void vic4_set_palette(const uint8_t *planes);

// Point the display at framebuffer 0 (FB_A) or 1 (FB_B).
void vic4_show(uint8_t buffer);

uint32_t vic4_base(uint8_t buffer);

#endif
