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

// Write a string at a panel cell. Text is stopped at the right edge rather
// than wrapped, so a long message cannot walk over the row below.
void panel_puts(uint8_t col, uint8_t row, const char *s, uint8_t colour);

// The top row, for anything the game wants to tell the pilot.
void panel_message(const char *s);

// The speed limiter: 0 cinematic, 1 normal, 2 sport.
void panel_speed(uint8_t mode);

// What is in the cargo bay -- "EMPTY" when there is nothing, which is what a
// mission flown with the camera alone shows for the whole flight.
void panel_cargo(const char *what);

// What is left in the pack, 0 to 100. Called only when the figure changes,
// which at the fastest drain is every ninth frame or so.
void panel_battery(uint8_t percent);

// The wind. `from` is the direction it blows FROM in 256ths of a turn, the
// same units as the heading, shown in degrees beside it; `mps` is its speed.
// Only called when the wind actually shifts, not every frame.
void panel_wind(uint8_t from, uint8_t mps);

// Refresh the flight readouts. x and y are the camera's 8.8 map position,
// fps10 is frames per second times ten.
void panel_status(int16_t altitude, uint8_t heading, uint16_t x, uint16_t y,
                  uint16_t fps10);

#endif
