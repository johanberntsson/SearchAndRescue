#include "hud.h"

#include "vic4.h"

#define GLYPH_W  4
#define GLYPH_H  6
#define ADVANCE  (GLYPH_W + 1)
#define HUD_X    2
#define HUD_Y    2
#define DIGITS   4  // "NN.N"
#define PANEL_W  (DIGITS * ADVANCE + 1)
#define PANEL_H  (GLYPH_H + 2)

#define DOT 10

// 4x6, one nybble per row, high bit leftmost.
static const uint8_t font[11][GLYPH_H] = {
    {0x6, 0x9, 0x9, 0x9, 0x9, 0x6}, // 0
    {0x2, 0x6, 0x2, 0x2, 0x2, 0x7}, // 1
    {0x6, 0x9, 0x1, 0x2, 0x4, 0xF}, // 2
    {0xE, 0x1, 0x6, 0x1, 0x1, 0xE}, // 3
    {0x2, 0x6, 0xA, 0xF, 0x2, 0x2}, // 4
    {0xF, 0x8, 0xE, 0x1, 0x1, 0xE}, // 5
    {0x6, 0x8, 0xE, 0x9, 0x9, 0x6}, // 6
    {0xF, 0x1, 0x2, 0x2, 0x4, 0x4}, // 7
    {0x6, 0x9, 0x6, 0x9, 0x9, 0x6}, // 8
    {0x6, 0x9, 0x9, 0x7, 0x1, 0x6}, // 9
    {0x0, 0x0, 0x0, 0x0, 0x0, 0x4}, // .
};

// Generic per-pixel addressing: glyphs are only a few hundred pixels a frame,
// and this way they can straddle a character strip without any special case.
static void plot(uint32_t base, uint8_t x, uint8_t y, uint8_t colour)
{
  uint8_t __far *p = (uint8_t __far *)FB_COLUMN(base, x);

  p[(int16_t)y * 8] = colour;
}

static void glyph(uint32_t base, uint8_t x, uint8_t y, uint8_t c)
{
  uint8_t row, col;

  for (row = 0; row < GLYPH_H; row++) {
    uint8_t bits = font[c][row];
    for (col = 0; col < GLYPH_W; col++)
      if (bits & (0x8 >> col))
        plot(base, x + col, y + row, HUD_INK);
  }
}

void hud_fps(uint32_t base, uint16_t fps10)
{
  uint8_t digits[DIGITS];
  uint8_t x, y, i;

  if (fps10 > 999)
    fps10 = 999;

  digits[0] = (uint8_t)(fps10 / 100);
  digits[1] = (uint8_t)(fps10 / 10 % 10);
  digits[2] = DOT;
  digits[3] = (uint8_t)(fps10 % 10);

  // Terrain colours run under the text, so give it something to sit on.
  for (y = 0; y < PANEL_H; y++)
    for (x = 0; x < PANEL_W; x++)
      plot(base, HUD_X - 1 + x, HUD_Y - 1 + y, HUD_PAPER);

  for (i = 0; i < DIGITS; i++)
    glyph(base, HUD_X + i * ADVANCE, HUD_Y, digits[i]);
}
