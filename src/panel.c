#include "panel.h"

#include "loader.h"
#include "vic4.h"

// Where each readout's digits start. The labels are drawn once by panel_init
// and never touched again; only the digits are rewritten each frame.
#define ROW_MESSAGE 0
#define ROW_FLIGHT  2
#define ROW_POS     3
#define ROW_STATUS  4

#define ALT_COL 4
#define HDG_COL 13
#define LAT_COL 4
#define LON_COL 17
#define FPS_COL 4

// Heading in 256ths of a turn, the same units as the camera angle, shown as
// degrees because that is what a pilot expects to read. 45/32 rather than the
// equal 360/256: a heading of 183 or more overflows the 16-bit product of the
// latter, and 192 -- due west -- came out as 14 degrees.
#define DEGREES(a) ((uint16_t)(a) * 45 / 32)

void panel_puts(uint8_t col, uint8_t row, const char *s, uint8_t colour)
{
  vic4_puts(col, (uint8_t)(FB_ROWS + row), s, colour);
}

// Zero padded rather than space padded, for the fractional half of a
// coordinate: .005 of a degree has to read as 005, not as 5.
static void put_frac(uint8_t col, uint8_t row, uint16_t value, uint8_t width,
                     uint8_t colour)
{
  uint8_t i;

  for (i = width; i-- > 0;) {
    vic4_panel_char(col + i, row, '0' + (uint8_t)(value % 10), colour);
    value /= 10;
  }
}

// Degrees and three decimals, with the hemisphere letter after them, which is
// how a drone controller shows a GPS fix.
static void put_degrees(uint8_t col, uint8_t row, uint16_t mdeg, uint8_t width,
                        char hemisphere)
{
  put_frac(col, row, mdeg / 1000, width, PANEL_INK);
  vic4_panel_char(col + width, row, '.', PANEL_INK);
  put_frac(col + width + 1, row, mdeg % 1000, 3, PANEL_INK);
  vic4_panel_char(col + width + 4, row, vic4_screen_code(hemisphere), PANEL_INK);
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
  panel_puts(0, ROW_POS, "LAT", PANEL_LABEL);
  panel_puts(13, ROW_POS, "LON", PANEL_LABEL);
  panel_puts(0, ROW_STATUS, "FPS", PANEL_LABEL);

  panel_puts(10, ROW_STATUS, "SPD", PANEL_LABEL);

  // The overview map: full-colour tiles dropped straight into panel cells.
  // Their pixels came off the disk already in character order, so there is
  // nothing to do here but name them.
  //
  // vic4_overview_ready is NOT called here. This runs again every time a
  // flight starts, and by then the map carries a crosshair -- taking the
  // pristine copy from it now would bake that crosshair in permanently. main
  // takes the copy once, when the map is first loaded.
  for (row = 0; row < OVERVIEW_CHARS; row++)
    for (col = 0; col < OVERVIEW_CHARS; col++)
      vic4_panel_tile(PANEL_MAP_COL + col, PANEL_MAP_ROW + row,
                      (uint16_t)OVERVIEW_CHAR + row * OVERVIEW_CHARS + col);
}

// Cinematic / normal / sport, the three a real drone offers.
void panel_speed(uint8_t mode)
{
  static const char *const names[] = {"SLO", "NRM", "SPT"};

  panel_puts(14, ROW_STATUS, names[mode > 2 ? 2 : mode], PANEL_INK);
}

void panel_message(const char *s)
{
  uint8_t col;

  for (col = 0; col < PANEL_COLS; col++)
    vic4_panel_char(col, ROW_MESSAGE, ' ', PANEL_INK);
  panel_puts(0, ROW_MESSAGE, s, PANEL_INK);
}

void panel_status(int16_t altitude, uint8_t heading, uint16_t x, uint16_t y,
                  uint16_t fps10)
{
  uint8_t cx = (uint8_t)(x >> 8);  // the map cell, and one millidegree
  uint8_t cy = (uint8_t)(y >> 8);

  if (altitude < 0)
    altitude = 0;

  put_number(ALT_COL, ROW_FLIGHT, (uint16_t)altitude, 3, PANEL_INK);
  put_number(HDG_COL, ROW_FLIGHT, DEGREES(heading), 3, PANEL_INK);

  put_degrees(LAT_COL, ROW_POS, MAP_LAT_SOUTH + (255 - cy), 2, 'N');
  put_degrees(LON_COL, ROW_POS, MAP_LON_WEST + cx, 3, 'E');

  // The overview is one pixel per eight cells, so the cell index shifts
  // straight down into it.
  vic4_crosshair((uint8_t)(cx * OVERVIEW_PX / 256),
                 (uint8_t)(cy * OVERVIEW_PX / 256));

  if (fps10 > 999)
    fps10 = 999;
  put_number(FPS_COL, ROW_STATUS, fps10 / 10, 2, PANEL_INK);
  vic4_panel_char(FPS_COL + 2, ROW_STATUS, '.', PANEL_INK);
  put_number(FPS_COL + 3, ROW_STATUS, fps10 % 10, 1, PANEL_INK);
}
