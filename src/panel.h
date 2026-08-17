// The information panel below the 3D view: six rows of ordinary text
// characters, which cost screen RAM and no pixel writes at all.
#ifndef PANEL_H
#define PANEL_H

#include <stdint.h>

// A text character takes its colour from colour RAM, which in 16-bit
// character mode is a four-bit field -- so these have to be among the first
// sixteen palette entries, and tools/convmap.py moves the terrain colours
// that were there out of the way.
#define PANEL_INK   1
#define PANEL_LABEL 2

// The panel's own ink, and the only colour on it: a hardware sprite carries
// one colour for the whole sprite, so the plane that holds the readouts is
// green from end to end. The mockups in screenshots/ wanted exactly that, and
// a second colour would mean a second plane. Reserved by tools/convmap.py
// with the two above.
#define PANEL_TEXT  3

// Except the battery, which has a sprite of its own so that it can have a
// colour of its own: green down to PANEL_WARN_AT percent, then yellow, then
// red. Reserved by tools/convmap.py with the rest.
#define PANEL_WARN   4
#define PANEL_ALARM  5

// Where those two begin. A quarter of the pack is about a minute of normal
// flying and a tenth is twenty-odd seconds -- time to turn for the fix, and
// then time to regret not having.
#define PANEL_WARN_AT  25
#define PANEL_ALARM_AT 10

// What panel_battery drew: 0 green, 1 yellow, 2 red. The caller compares it
// with the last one to decide whether anything should be heard about it --
// the panel says how it looks and main decides how it sounds.
#define BATTERY_OK    0
#define BATTERY_WARN  1
#define BATTERY_ALARM 2

// The paper, and the one thing about the panel that is not per character: a
// text character's background is the screen colour, full stop. It used to be
// palette 0 and black, which was fine over nothing and punches a hole in the
// background artwork.
//
// tools/convmap.py puts the artwork's commonest colour -- the flat green its
// readout boxes are painted in -- in the first of the artwork's own entries
// for exactly this, so a readout drawn inside a box disappears into it. The
// screen colour is switched to it while the view is up and back to 0 for the
// full-screen text pages, which want a black page.
#define PANEL_PAPER 242  // PANEL_ART_BASE in tools/convmap.py

// The world is 256 cells square and each cell is exactly one millidegree, so
// the map spans 0.256 degrees -- roughly 28 km, a plausible search area. The
// origin is arbitrary; these two constants are the whole of it, and moving
// them moves the readout. Latitude counts down the map, so cell row 0 is the
// northern edge and the overview map is north up.
#define MAP_LAT_SOUTH 46500  // millidegrees north at the bottom edge
#define MAP_LON_WEST   8000  // millidegrees east at the left edge

void panel_init(void);

// Write a string anywhere in the panel at all, in pixels rather than cells:
// the text rides on a plane of hardware sprites over the artwork, which is
// what took it off the 8-pixel character grid. See src/overlay.h.
void panel_puts(uint16_t x, uint8_t y, const char *s);

// Anything the game wants to tell the pilot. It takes the two top boxes, and
// so covers the fix and the battery for as long as it is up; `panel_message(0)`
// is how an alert ends and gives them back.
void panel_message(const char *s);

// The speed limiter: 0 cinematic, 1 normal, 2 sport.
void panel_speed(uint8_t mode);

// What is in the cargo bay -- "EMPTY" when there is nothing, which is what a
// mission flown with the camera alone shows for the whole flight.
void panel_cargo(const char *what);

// What is left in the pack, 0 to 100. Called only when the figure changes,
// which at the fastest drain is every ninth frame or so. Returns the level it
// drew -- BATTERY_OK, _WARN or _ALARM -- so that the caller can beep when it
// falls.
uint8_t panel_battery(uint8_t percent);

// Show or hide the frame rate, which is off when a flight starts. There is
// nothing on any screen about it: it is P, and it is not a drone's
// instrument.
void panel_fps(uint8_t on);

// The wind. `from` is the direction it blows FROM in 256ths of a turn, the
// same units as the heading, shown in degrees beside it; `mps` is its speed.
// Only called when the wind actually shifts, not every frame.
void panel_wind(uint8_t from, uint8_t mps);

// Refresh the flight readouts. x and y are the camera's 8.8 map position,
// fps10 is frames per second times ten.
void panel_status(int16_t altitude, uint8_t heading, uint16_t x, uint16_t y,
                  uint16_t fps10);

#endif
