#include "panel.h"

#include "vic4.h"

// Where each readout's digits start. The labels are drawn once by panel_init
// and never touched again; only the digits are rewritten each frame.
#define ROW_MESSAGE 0
#define ROW_FLIGHT  2
#define ROW_STATUS  3

#define ALT_COL 4
#define HDG_COL 13
#define FPS_COL 4

// Heading in 256ths of a turn, the same units as the camera angle, shown as
// degrees because that is what a pilot expects to read.
#define DEGREES(a) ((uint16_t)(a) * 360 / 256)

// Screen codes are not ASCII: in the uppercase character set the letters run
// from 1 rather than from $41, while digits and most punctuation sit where
// ASCII has them.
static uint8_t screen_code(char c)
{
  uint8_t u = (uint8_t)c;

  if (u >= 0x40 && u < 0x60)
    return u - 0x40;  // A-Z
  if (u >= 0x60 && u < 0x80)
    return u - 0x60;  // a-z, same glyphs
  return u;
}

void panel_puts(uint8_t col, uint8_t row, const char *s, uint8_t colour)
{
  while (*s && col < PANEL_COLS) {
    vic4_panel_char(col, row, screen_code(*s), colour);
    col++;
    s++;
  }
}

// Right-aligned unsigned number, `width` digits, space padded. Returns
// nothing: the panel is fixed-width, so there is nowhere for it to overflow
// to and a too-large value is clamped to all nines.
static void put_number(uint8_t col, uint8_t row, uint16_t value, uint8_t width,
                       uint8_t colour)
{
  uint8_t i;

  for (i = width; i-- > 0;) {
    uint8_t digit = (uint8_t)(value % 10);

    // Leave a leading zero only in the units position, so 0 reads as "0"
    // rather than as a blank.
    if (value == 0 && i != width - 1)
      vic4_panel_char(col + i, row, ' ', colour);
    else
      vic4_panel_char(col + i, row, '0' + digit, colour);
    value /= 10;
  }
}

void panel_init(void)
{
  uint8_t col, row;

  for (row = 0; row < PANEL_ROWS; row++)
    for (col = 0; col < PANEL_COLS; col++)
      vic4_panel_char(col, row, ' ', PANEL_INK);

  panel_puts(0, ROW_FLIGHT, "ALT", PANEL_LABEL);
  panel_puts(9, ROW_FLIGHT, "HDG", PANEL_LABEL);
  panel_puts(0, ROW_STATUS, "FPS", PANEL_LABEL);
}

void panel_message(const char *s)
{
  uint8_t col;

  for (col = 0; col < PANEL_COLS; col++)
    vic4_panel_char(col, ROW_MESSAGE, ' ', PANEL_INK);
  panel_puts(0, ROW_MESSAGE, s, PANEL_INK);
}

void panel_status(int16_t altitude, uint8_t heading, uint16_t fps10)
{
  if (altitude < 0)
    altitude = 0;

  put_number(ALT_COL, ROW_FLIGHT, (uint16_t)altitude, 3, PANEL_INK);
  put_number(HDG_COL, ROW_FLIGHT, DEGREES(heading), 3, PANEL_INK);

  if (fps10 > 999)
    fps10 = 999;
  put_number(FPS_COL, ROW_STATUS, fps10 / 10, 2, PANEL_INK);
  vic4_panel_char(FPS_COL + 2, ROW_STATUS, '.', PANEL_INK);
  put_number(FPS_COL + 3, ROW_STATUS, fps10 % 10, 1, PANEL_INK);
}
