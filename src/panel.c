#include "panel.h"

#include "loader.h"
#include "overlay.h"
#include "vic4.h"

// **Every readout is a pixel position now, not a character cell.** The panel
// is a picture, the picture is not drawn on an 8-pixel grid, and the text
// rides over it on a plane of hardware sprites -- see src/overlay.h for why
// that and not a shifted font or the Raster Rewrite Buffer.
//
// So these are pixel coordinates within the panel's 320x48, measured off
// resources/panel.png, and each one puts its text inside one of the picture's
// boxes:
//
//   x 0..55    the compass, which carries the altitude and the heading
//   x 57..188  y 2..15   the fix -- or a message, which takes both top boxes
//   x 191..270 y 2..15   the battery
//   x 57..188  y 18..32  the wind
//   x 198..270 y 18..32  the speed limiter
//   x 156..265 y 36..44  the cargo bay, its CARGO label painted into the art
//   x 276..314           the overview map, which is still full-colour tiles
//
// The frame rate is the one readout with no box of its own: it sits on the
// frame to the left of the painted CARGO, because the artwork has five text
// areas and the game has six things to say.
#define ALT_X   16
#define ALT_Y   12
#define HDG_X   16
#define HDG_Y   28

#define FIX_X   59
#define FIX_Y    5
#define FIX_W  128  // "46.681N 008.083E" to the pixel

// The battery is drawn on its own sprite, so its x is that sprite's own
// coordinates rather than the panel's; OVERLAY_ALERT_AT is where it lands.
#define BATT_X OVERLAY_ALERT
#define BATT_Y   5

#define WIND_X  59
#define WIND_Y  21

#define SPD_X  202
#define SPD_Y   21

#define CARGO_X  160
#define CARGO_Y   36
#define CARGO_W  104  // what the box holds, and what a shrinking name clears

#define FPS_X    60
#define FPS_Y    36

// A message takes both of the top boxes, crossing the two-pixel divider
// between them, and covers the fix and the battery for as long as it is up.
// That is deliberate: an alert on an instrument takes the field it needs.
#define MSG_X   59
#define MSG_Y    5
#define MSG_W  208  // 26 characters

// An angle in 256ths of a turn -- the camera's units, and the wind's -- as the
// compass bearing a pilot expects to read.
//
// The quarter turn is the whole point of it. Angle 0 moves the camera along
// +x, which is east, while the overview map is north up, so the raw angle
// printed as degrees called east 000 and north 270. Adding 64 first turns it
// into a real bearing; the mask is what wraps 192 (north) round to 000 rather
// than letting it read 360.
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

// Fields are built here and drawn in one go, because the plane is written a
// byte at a time and adjacent glyphs share byte columns: a string is cleared
// and redrawn as a unit, a character at a time would erase its neighbour.
static char field[28];

// **A field that has not changed is not redrawn**, and that is not a
// micro-optimisation: drawing one is about 170 cycles a byte in C, and the
// four readouts refreshed every frame came to 4.1 ms of an 88 ms one. Most of
// them do not actually move every frame -- the fix is in millidegrees, which
// is one map cell, so it changes only when the camera crosses a cell, and the
// heading only when the pilot yaws. Caching took the whole panel from 4.1 ms
// to well under one.
static char last_fix[18];
static char last_alt[5];
static char last_hdg[5];
static char last_fps[6];

// True if `field` differs from what was drawn last, updating the record if
// it does. Compared rather than remembering the numbers behind it, because
// two different altitudes can print the same three characters.
static uint8_t moved(char *last, uint8_t size)
{
  uint8_t i;

  for (i = 0; i < size; i++) {
    if (last[i] != field[i]) {
      for (; i < size; i++) {
        last[i] = field[i];
        if (!field[i])
          break;
      }
      return 1;
    }
    if (!field[i])
      return 0;
  }
  return 0;
}

// What is on the panel that something else can cover up. A message takes the
// fix's box and the battery's, so both have to be able to come back: the fix
// is redrawn every frame anyway, and the battery only when it changes, so the
// last figure is kept.
static uint8_t msg_up;
static uint8_t batt_last;

// The frame rate is not a drone's instrument and the panel is a drone's
// panel, so it is off unless somebody asks for it with P. Undocumented on
// purpose -- there is no room on the briefing for a line about it, and the
// figure appearing is all the acknowledgement the key needs.
static uint8_t fps_shown;

// Right aligned in `width`. `pad` is what leads: a space for a measurement
// that reads better without leading zeros, '0' for a bearing or a coordinate,
// which are always read with them.
static char *put_u(char *d, uint16_t value, uint8_t width, char pad)
{
  uint8_t i;

  for (i = width; i-- > 0;) {
    uint8_t digit = (uint8_t)(value % 10);

    d[i] = (value || i == width - 1 || pad == '0') ? (char)('0' + digit) : pad;
    value /= 10;
  }
  return d + width;
}

// Degrees and three decimals with the hemisphere letter after them, which is
// how a drone controller shows a GPS fix -- and, with the letters doing the
// labelling, why the fix needs no LAT and LON in front of it.
static char *put_s(char *d, const char *s)
{
  while (*s)
    *d++ = *s++;
  return d;
}

static char *put_degrees(char *d, uint16_t mdeg, uint8_t width, char hemisphere)
{
  d = put_u(d, mdeg / 1000, width, '0');
  *d++ = '.';
  d = put_u(d, mdeg % 1000, 3, '0');
  *d++ = hemisphere;
  return d;
}

void panel_puts(uint16_t x, uint8_t y, const char *s)
{
  overlay_text(x, y, s);
}

void panel_init(void)
{
  uint8_t col, row;

  // The background picture: 240 full-colour characters that came off the disk
  // already in this order, so this is the whole of drawing it. No pixels
  // move, and it costs the same screen RAM the spaces it replaced did.
  for (row = 0; row < PANEL_ROWS; row++)
    for (col = 0; col < PANEL_COLS; col++)
      vic4_panel_tile(col, row,
                      (uint16_t)(PANEL_ART_CHAR + (uint16_t)row * PANEL_ART_COLS
                                 + col));

  // The overview map: full-colour tiles dropped straight into panel cells,
  // over the box the artwork leaves for them.
  //
  // vic4_overview_ready is NOT called here. This runs again every time a
  // flight starts, and by then the map carries a crosshair -- taking the
  // pristine copy from it now would bake that crosshair in permanently. main
  // takes the copy once, when the map is first loaded.
  for (row = 0; row < OVERVIEW_CHARS; row++)
    for (col = 0; col < OVERVIEW_CHARS; col++)
      vic4_panel_tile(PANEL_MAP_COL + col, PANEL_MAP_ROW + row,
                      (uint16_t)OVERVIEW_CHAR + row * OVERVIEW_CHARS + col);

  // And the text plane over the lot, wiped and empty. **Nothing is drawn
  // here**: every readout writes its own label with its own value, as one
  // string, so a label cannot be left behind by whatever covers its box --
  // which is exactly what happened when the launch message wiped BATT and
  // there was nothing left to put it back.
  overlay_on();
  msg_up = 0;
  batt_last = 100;

  // Nothing on the plane, so nothing that was drawn last time still holds.
  last_fix[0] = last_alt[0] = last_hdg[0] = last_fps[0] = 0;
  fps_shown = 0;
}

// P, and nothing on the screen says so. The label is drawn once here rather
// than with every new figure -- FPS is the readout that changes most often --
// which is safe because a clear is masked to the pixel and cannot eat what
// shares its byte column.
void panel_fps(uint8_t on)
{
  fps_shown = on;
  if (on) {
    overlay_text(FPS_X, FPS_Y, "FPS");
    last_fps[0] = 0;  // whatever it read before, put the figure back
  } else {
    overlay_clear(FPS_X, FPS_Y, 8 * 8, 8);
  }
}

// Cinematic / normal / sport, the three a real drone offers.
void panel_speed(uint8_t mode)
{
  static const char *const names[] = {"SLO", "NRM", "SPT"};
  char *d = put_s(field, "SPD ");

  put_s(d, names[mode > 2 ? 2 : mode])[0] = 0;
  overlay_text(SPD_X, SPD_Y, field);
}

void panel_cargo(const char *what)
{
  // The bay can hold a shorter name than it did a moment ago, so the whole
  // box is cleared rather than the string's own span.
  overlay_clear(CARGO_X, CARGO_Y, CARGO_W, 8);
  overlay_text(CARGO_X, CARGO_Y, what);
}

uint8_t panel_battery(uint8_t percent)
{
  uint8_t level = percent < PANEL_ALARM_AT   ? BATTERY_ALARM
                  : percent < PANEL_WARN_AT  ? BATTERY_WARN
                                             : BATTERY_OK;
  char *d;

  batt_last = percent;

  // The colour goes on even while a message covers the figure, so that the
  // pack is already red when the box is handed back.
  overlay_ink(OVERLAY_PLANE_SPRITES,
              level == BATTERY_ALARM  ? PANEL_ALARM
              : level == BATTERY_WARN ? PANEL_WARN
                                      : PANEL_TEXT);
  if (msg_up)
    return level;  // panel_message(0) puts the figure back

  // "BAT 100%" is eight characters, which is this sprite's whole 64 pixels.
  // The fourth character of BATTERY is what it costs to have the readout on
  // a sprite of its own, and a sprite of its own is what it costs to have a
  // colour of its own.
  d = put_u(put_s(field, "BAT "), percent, 3, ' ');
  *d++ = '%';
  *d = 0;
  overlay_text(BATT_X, BATT_Y, field);
  return level;
}

void panel_wind(uint8_t from, uint8_t mps)
{
  // "WIND 201DEG 1M/S" -- sixteen characters, which is the box to the pixel.
  // One digit of speed is all WIND_MPS can produce and all there is room for.
  char *d = put_u(put_s(field, "WIND "), DEGREES(from), 3, '0');

  d = put_u(put_s(d, "DEG "), mps > 9 ? 9 : mps, 1, ' ');
  put_s(d, "M/S")[0] = 0;
  overlay_text(WIND_X, WIND_Y, field);
}

void panel_message(const char *s)
{
  // Both boxes: the message's own span on the plane, and the battery, which
  // is on a sprite of its own and would otherwise show through the middle of
  // the message.
  overlay_clear(MSG_X, MSG_Y, MSG_W, 8);
  overlay_clear(OVERLAY_ALERT, MSG_Y, 64, 8);

  if (!s) {
    // The alert is over: give the two boxes back. The fix redraws itself on
    // the next frame; the battery only changes every ninth one, so it has to
    // be put back from what it last read.
    msg_up = 0;
    panel_battery(batt_last);
    return;
  }
  msg_up = 1;
  overlay_text(MSG_X, MSG_Y, s);
}

void panel_status(int16_t altitude, uint8_t heading, uint16_t x, uint16_t y,
                  uint16_t fps10)
{
  uint8_t cx = (uint8_t)(x >> 8);  // the map cell, and one millidegree
  uint8_t cy = (uint8_t)(y >> 8);
  char *d;

  if (altitude < 0)
    altitude = 0;

  put_u(field, (uint16_t)altitude, 3, ' ')[0] = 0;
  if (moved(last_alt, sizeof last_alt))
    overlay_text(ALT_X, ALT_Y, field);
  put_u(field, DEGREES(heading), 3, '0')[0] = 0;
  if (moved(last_hdg, sizeof last_hdg))
    overlay_text(HDG_X, HDG_Y, field);

  if (!msg_up) {
    d = put_degrees(field, MAP_LAT_SOUTH + (255 - cy), 2, 'N');
    *d++ = ' ';
    d = put_degrees(d, MAP_LON_WEST + cx, 3, 'E');
    *d = 0;
    if (moved(last_fix, sizeof last_fix))
      overlay_text(FIX_X, FIX_Y, field);
  }

  // The overview is one pixel per eight cells, so the cell index shifts
  // straight down into it.
  vic4_crosshair((uint8_t)(cx * OVERVIEW_PX / 256),
                 (uint8_t)(cy * OVERVIEW_PX / 256));

  if (!fps_shown)
    return;
  if (fps10 > 999)
    fps10 = 999;
  d = put_u(field, fps10 / 10, 2, ' ');
  *d++ = '.';
  d = put_u(d, fps10 % 10, 1, '0');
  *d = 0;
  if (moved(last_fps, sizeof last_fps))
    overlay_text(FPS_X + 4 * 8, FPS_Y, field);
}
