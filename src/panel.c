#include "panel.h"

#include "loader.h"
#include "vic4.h"

// Where each readout goes -- and it is the background artwork that decides,
// not taste. A text character's paper is the screen colour and there is only
// one of those, so everything the panel says has to sit inside one of the
// picture's sunken boxes, where that colour is right. The boxes, measured off
// resources/panel.png in character cells:
//
//   rows 0-1  cols 8-23 and 24-33
//   rows 2-3  cols 8-23 and 25-33
//   row  5    cols 20-33   the cargo bay, its CARGO label painted in
//   cols 0-6  the compass; cols 35-39 the map. No text in either.
//
// So a row holds one readout and the columns are counted to the character:
// there is nowhere for anything to overflow to, and a readout that grows has
// to be traded against its neighbour rather than pushed along the row.
#define ROW_MESSAGE 0
#define ROW_POS     1
#define ROW_FLIGHT  2
#define ROW_WIND    3
#define ROW_CARGO   5

// Both top boxes; the divider between them is two pixels of the character at
// column 23, which a long message crosses rather than stops at.
#define MSG_COL    8
#define MSG_WIDTH 26

// "46.632N 008.171E" -- the box to the character, and the way a fix reads on
// a real controller, with the hemisphere letters doing the labelling.
#define LAT_COL   8
#define LON_COL  16
// 25 and not 24: the box starts halfway through the character at column 23,
// so a readout at 24 reads as though it were part of the fix beside it.
// "BATT 100%" is nine characters and the box is nine wide from here.
#define BATT_COL 25

// "ALT 126  HDG 090" and "SPD NRM" beside it.
#define ALT_COL  12
#define HDG_COL  21
#define SPD_COL  25

// "WIND 248DEG 4M/S", which is 16 characters exactly, and "FPS 11.6".
#define WIND_COL  8
#define FPS_COL  29

// The bay itself. The word CARGO is part of the picture, painted on the frame
// to the left of the box, so the whole box is the load's name.
#define CARGO_COL   20
#define CARGO_WIDTH 14

// An angle in 256ths of a turn -- the camera's units, and the wind's -- as the
// compass bearing a pilot expects to read.
//
// The quarter turn is the whole point of it. Angle 0 moves the camera along
// +x, which is east, while the overview map is north up, so the raw angle
// printed as degrees called east 000 and north 270. Adding 64 first turns it
// into a real bearing; the cast back to uint8_t is what wraps 192 (north)
// round to 000 rather than letting it read 360.
//
// 45/32 rather than the equal 360/256: a heading of 183 or more overflows the
// 16-bit product of the latter, and 192 came out as 14 degrees.
//
// **The wrap is a 16-bit mask and must not be a cast through uint8_t.**
// Written the obvious way, `(uint16_t)(uint8_t)((a) + 64)`, Calypsi 5.18
// SIGN-extends the byte -- `ora #127 / bmi / lda #0` in the listing -- so
// every bearing from 128 up came out negative and the wind read 896 degrees.
// Widening first and masking in 16 bits never puts the value in a byte at
// all. This is the same family as the int8_t miscompile under Performance.
#define DEGREES(a) (((((uint16_t)(a) + 64) & 0xFF) * 45) / 32)

#if PANEL_ART_COLS != PANEL_COLS || PANEL_ART_ROWS != PANEL_ROWS
#error "the panel artwork is not the shape of the panel"
#endif

void panel_puts(uint8_t col, uint8_t row, const char *s, uint8_t colour)
{
  vic4_puts(col, (uint8_t)(FB_ROWS + row), s, colour);
}

// The panel's paper is a picture now, so clearing a cell means putting the
// artwork character back rather than writing a space. Every readout that
// blanks a field before writing it goes through here: a space would punch a
// hole in the background and never fill it in again.
static void panel_blank(uint8_t col, uint8_t row, uint8_t width)
{
  while (width--) {
    vic4_panel_tile(col, row,
                    (uint16_t)(PANEL_ART_CHAR + (uint16_t)row * PANEL_ART_COLS
                               + col));
    col++;
  }
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
      panel_blank((uint8_t)(col + i), row, 1);
    else
      vic4_panel_char(col + i, row, '0' + digit, colour);
    value /= 10;
  }
}

void panel_init(void)
{
  uint8_t col, row;

  // The background picture first, and everything else over it. It is 240
  // full-colour characters that came off the disk already in this order, so
  // this is the whole of drawing it -- no pixels move, and it costs the same
  // screen RAM the spaces it replaced did.
  for (row = 0; row < PANEL_ROWS; row++)
    panel_blank(0, row, PANEL_COLS);

  // The position needs no labels: the hemisphere letters are the labels.
  panel_puts(BATT_COL, ROW_POS, "BATT", PANEL_LABEL);
  vic4_panel_char(BATT_COL + 8, ROW_POS, vic4_screen_code('%'), PANEL_LABEL);

  panel_puts(ALT_COL - 4, ROW_FLIGHT, "ALT", PANEL_LABEL);
  panel_puts(HDG_COL - 4, ROW_FLIGHT, "HDG", PANEL_LABEL);
  panel_puts(SPD_COL, ROW_FLIGHT, "SPD", PANEL_LABEL);

  // Only the two numbers are rewritten when the wind shifts; the units are
  // part of the furniture, like every other label here.
  panel_puts(WIND_COL, ROW_WIND, "WIND", PANEL_LABEL);
  panel_puts(WIND_COL + 8, ROW_WIND, "DEG", PANEL_LABEL);
  panel_puts(WIND_COL + 13, ROW_WIND, "M/S", PANEL_LABEL);
  panel_puts(FPS_COL - 4, ROW_WIND, "FPS", PANEL_LABEL);

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

  panel_puts(SPD_COL + 4, ROW_FLIGHT, names[mode > 2 ? 2 : mode], PANEL_INK);
}

void panel_cargo(const char *what)
{
  panel_blank(CARGO_COL, ROW_CARGO, CARGO_WIDTH);
  panel_puts(CARGO_COL, ROW_CARGO, what, PANEL_INK);
}

void panel_battery(uint8_t percent)
{
  put_number(BATT_COL + 5, ROW_POS, percent, 3, PANEL_INK);
}

void panel_wind(uint8_t from, uint8_t mps)
{
  // A bearing is read with its leading zeros -- 045, not 45 -- so this gets
  // the zero-padded field the coordinates use rather than the space-padded
  // one the altitude does. Same units as the heading above it.
  put_frac(WIND_COL + 5, ROW_WIND, DEGREES(from), 3, PANEL_INK);
  // One digit, which is all WIND_MPS can produce: the box is sixteen
  // characters wide and every one of them is spoken for.
  put_number(WIND_COL + 12, ROW_WIND, mps > 9 ? 9 : mps, 1, PANEL_INK);
}

void panel_message(const char *s)
{
  uint8_t col;

  // Clipped to the box rather than to the display: past column 33 there is
  // the frame of the picture and then the map, and a message that ran on
  // would write over both.
  panel_blank(MSG_COL, ROW_MESSAGE, MSG_WIDTH);
  for (col = 0; col < MSG_WIDTH && s[col]; col++)
    vic4_panel_char((uint8_t)(MSG_COL + col), ROW_MESSAGE,
                    vic4_screen_code(s[col]), PANEL_INK);
}

void panel_status(int16_t altitude, uint8_t heading, uint16_t x, uint16_t y,
                  uint16_t fps10)
{
  uint8_t cx = (uint8_t)(x >> 8);  // the map cell, and one millidegree
  uint8_t cy = (uint8_t)(y >> 8);

  if (altitude < 0)
    altitude = 0;

  put_number(ALT_COL, ROW_FLIGHT, (uint16_t)altitude, 3, PANEL_INK);
  put_frac(HDG_COL, ROW_FLIGHT, DEGREES(heading), 3, PANEL_INK);

  put_degrees(LAT_COL, ROW_POS, MAP_LAT_SOUTH + (255 - cy), 2, 'N');
  put_degrees(LON_COL, ROW_POS, MAP_LON_WEST + cx, 3, 'E');

  // The overview is one pixel per eight cells, so the cell index shifts
  // straight down into it.
  vic4_crosshair((uint8_t)(cx * OVERVIEW_PX / 256),
                 (uint8_t)(cy * OVERVIEW_PX / 256));

  if (fps10 > 999)
    fps10 = 999;
  put_number(FPS_COL, ROW_WIND, fps10 / 10, 2, PANEL_INK);
  vic4_panel_char(FPS_COL + 2, ROW_WIND, '.', PANEL_INK);
  put_number(FPS_COL + 3, ROW_WIND, fps10 % 10, 1, PANEL_INK);
}
