#include "overlay.h"

#include <mega65.h>

#include "dma.h"
#include "loader.h"
#include "panel.h"
#include "vic4.h"

// The MEGA65's own sprite registers. mega65.h names the legacy ones; these
// are spelled out so the code says what it is doing, and every one of them
// was measured by src/sprtest.c before anything here was written.
#define SPRHGTEN   (*(volatile uint8_t *)0xD055)  // extended height, per sprite
#define SPRHGHT    (*(volatile uint8_t *)0xD056)  // how tall, in pixels
#define SPRX64EN   (*(volatile uint8_t *)0xD057)  // 64 pixels wide, per sprite
#define SPRPTRADRL (*(volatile uint8_t *)0xD06C)
#define SPRPTRADRM (*(volatile uint8_t *)0xD06D)
#define SPRPTRBNK  (*(volatile uint8_t *)0xD06E)  // bits 0-6 address, bit 7:
#define SPR_PTR16  0x80  // sprite data on any 64-byte boundary in chip RAM

#define SPRITE_ENABLE (*(volatile uint8_t *)0xD015)
#define SPRITE_X(n)   (*(volatile uint8_t *)(0xD000 + (n) * 2))
#define SPRITE_Y(n)   (*(volatile uint8_t *)(0xD001 + (n) * 2))
#define SPRITE_XMSB   (*(volatile uint8_t *)0xD010)
#define SPRITE_COL(n) (*(volatile uint8_t *)(0xD027 + (n)))

// Sprite coordinates are the VIC-II's: the 40-column display starts at X 24
// and its first character row at Y 50, so the panel -- the six rows under the
// framebuffer's nineteen -- starts at Y 202. Confirmed on the machine, not
// worked out from a manual.
#define SPR_ORIGIN_X 24
#define SPR_ORIGIN_Y (50 + FB_ROWS * 8)

// The panel's own character set, read off the disk into bank 4 -- see
// PANEL_FONT in loader.h for why the panel gets one and the game's pages do
// not. It is read here by the CPU, a row at a time, rather than fetched by
// the VIC-IV: nothing about this is a character any more.
#if OVERLAY_PLANE + OVERLAY_SPRITES * OVERLAY_SLOT > PANEL_FONT
#error "the sprite plane runs into the panel font in bank 4"
#endif

// One byte column of the plane. The five sprites are separate 384-byte
// blocks, so the plane is in column strips exactly as the framebuffer is:
// eight bytes of a strip are one sprite's row, and stepping down a row is +8
// whether or not the column crosses into the next sprite.
static volatile uint8_t __far *plane_col(uint8_t col)
{
  return (volatile uint8_t __far *)(OVERLAY_PLANE
                                    + (uint32_t)(col >> 3) * OVERLAY_SLOT
                                    + (col & 7));
}

static void overlay_wipe(void)
{
  dma_fill(OVERLAY_PLANE, 0, OVERLAY_SPRITES * OVERLAY_SLOT);
}

void overlay_clear(uint16_t x, uint8_t y, uint16_t w, uint8_t h)
{
  uint16_t end = x + w;  // exclusive
  uint8_t first = (uint8_t)(x >> 3);
  uint8_t last = (uint8_t)((end - 1) >> 3);
  uint8_t col, r;

  for (col = first; col <= last && col < OVERLAY_COLS; col++) {
    // Stepped by a row rather than indexed: recomputing (y + r) * 8 into a
    // far pointer every row is most of what this loop would cost.
    volatile uint8_t __far *p = plane_col(col) + (int16_t)(y * OVERLAY_STRIDE);
    uint8_t gone = 0xFF;  // the bits this column loses

    // **Masked at both ends, not rounded out to whole columns.** Rounding is
    // what the first version did and it ate its neighbours: the wind's
    // bearing ends inside the same byte column its DEG begins in, so
    // redrawing the bearing turned DEG into )EG.
    if (col == first)
      gone &= (uint8_t)(0xFF >> (x & 7));
    if (col == last && (end & 7))
      gone &= (uint8_t)(0xFF << (8 - (end & 7)));
    gone = (uint8_t)~gone;

    for (r = 0; r < h; r++, p += OVERLAY_STRIDE)
      *p &= gone;
  }
}

// One glyph ORed in, so that two glyphs sharing a byte column -- which any
// string not on an 8-pixel boundary does -- do not erase each other.
static void overlay_glyph(uint16_t x, uint8_t y, uint8_t code)
{
  const uint8_t __far *rom;
  uint8_t col = (uint8_t)(x >> 3);
  uint8_t shift = (uint8_t)(x & 7);
  volatile uint8_t __far *lo;
  volatile uint8_t __far *hi;
  uint8_t spills;  // does the glyph reach into the next byte column?
  uint8_t r;

  if (col >= OVERLAY_COLS)
    return;
  // Only the glyphs that ship, which is everything the panel can be asked
  // for; anything else draws nothing rather than eight bytes of whatever
  // follows the font in bank 4.
  if (code < PANEL_FONT_FIRST
      || code >= PANEL_FONT_FIRST + PANEL_FONT_GLYPHS)
    return;
  rom = (const uint8_t __far *)(PANEL_FONT
                                + (uint32_t)(code - PANEL_FONT_FIRST) * 8);
  spills = shift && (uint8_t)(col + 1) < OVERLAY_COLS;
  // Both pointers land on the glyph's first row and step by one row after
  // that, for the reason in overlay_clear.
  lo = plane_col(col) + (int16_t)(y * OVERLAY_STRIDE);
  hi = plane_col((uint8_t)(spills ? col + 1 : col))
       + (int16_t)(y * OVERLAY_STRIDE);

  for (r = 0; r < 8; r++, lo += OVERLAY_STRIDE, hi += OVERLAY_STRIDE) {
    // Widened to 16 bits before it is shifted, and never allowed to sit in a
    // byte in the middle of the expression: a value that reaches the top of
    // an int here would come back sign extended -- the trap CLAUDE.md records
    // under Performance -- and every glyph with a pixel in its first column
    // would smear across the panel.
    uint16_t bits = (uint16_t)rom[r];

    bits <<= 8;
    bits >>= shift;
    *lo |= (uint8_t)(bits >> 8);
    if (spills)
      *hi |= (uint8_t)bits;
  }
}

void overlay_text(uint16_t x, uint8_t y, const char *s)
{
  uint16_t at = x;
  const char *p = s;

  while (*p) {
    at += 8;
    p++;
  }
  overlay_clear(x, y, (uint16_t)(at - x), 8);

  // **The character itself, not a screen code.** The panel's font is in the
  // C64 screen-code layout, in which space through Z sit exactly where ASCII
  // puts them -- so the conversion the game's text pages need is one this
  // does not, and doing it anyway drew every letter as its lowercase.
  for (at = x; *s; s++, at += 8)
    overlay_glyph(at, y, (uint8_t)*s);
}

void overlay_on(void)
{
  volatile uint8_t __far *ptrs = (volatile uint8_t __far *)OVERLAY_PTRS;
  uint8_t n;

  overlay_wipe();

  // The pointer list, and the reason this works at all: with SPR_PTR16 the
  // VIC-IV reads its eight pointers from SPRPTRADR as 16-bit values, each the
  // data address divided by 64. Without it they come from the legacy
  // screen+$3F8, which in this layout is a framebuffer tile in the middle of
  // the 3D view and cannot be given a useful value.
  for (n = 0; n < 8; n++) {
    uint16_t p = (uint16_t)((OVERLAY_PLANE
                             + (uint32_t)(n < OVERLAY_SPRITES ? n : 0)
                                   * OVERLAY_SLOT) / 64);

    ptrs[(int16_t)(n * 2)] = (uint8_t)p;
    ptrs[(int16_t)(n * 2 + 1)] = (uint8_t)(p >> 8);
  }

  SPRPTRADRL = (uint8_t)(OVERLAY_PTRS & 0xFF);
  SPRPTRADRM = (uint8_t)((OVERLAY_PTRS >> 8) & 0xFF);
  SPRPTRBNK = (uint8_t)(((OVERLAY_PTRS >> 16) & 0x7F) | SPR_PTR16);

  SPRX64EN = (1 << OVERLAY_SPRITES) - 1;  // 64 pixels wide, all of them
  SPRHGTEN = (1 << OVERLAY_SPRITES) - 1;
  SPRHGHT = OVERLAY_ROWS;

  SPRITE_XMSB = 0;
  for (n = 0; n < OVERLAY_SPRITES; n++) {
    // The plane's five are laid end to end across the panel; the battery's
    // one is put over the artwork's box for it. All of them start green -- the
    // battery's changes colour as the pack runs down, and nothing else does.
    uint16_t at = n < OVERLAY_PLANE_SPRITES ? (uint16_t)(n * 64)
                                            : OVERLAY_ALERT_AT;

    at += SPR_ORIGIN_X;
    SPRITE_COL(n) = PANEL_TEXT;
    SPRITE_X(n) = (uint8_t)at;
    SPRITE_Y(n) = SPR_ORIGIN_Y;
    // A sprite past X 255 -- the rightmost of the plane, at 280 -- keeps the
    // ninth bit of its position over here.
    if (at > 255)
      SPRITE_XMSB |= (uint8_t)(1 << n);
  }

  SPRITE_ENABLE = (uint8_t)((1 << OVERLAY_SPRITES) - 1);
}

void overlay_ink(uint8_t sprite, uint8_t entry)
{
  if (sprite < OVERLAY_SPRITES)
    SPRITE_COL(sprite) = entry;
}

void overlay_off(void)
{
  SPRITE_ENABLE = 0;
}
