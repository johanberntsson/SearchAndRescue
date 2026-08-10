// The information panel below the 3D view: six rows of ordinary text
// characters, which cost screen RAM and no pixel writes at all.
#ifndef PANEL_H
#define PANEL_H

#include <stdint.h>

// A text character takes its colour from colour RAM, which in 16-bit
// character mode is a four-bit field -- so these have to be among the first
// sixteen palette entries, and tools/convmap.py moves the terrain colours
// that were there out of the way. The paper is palette 0, which is the screen
// colour and is already black.
#define PANEL_INK   1
#define PANEL_LABEL 2

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

// Refresh the flight readouts. x and y are the camera's 8.8 map position,
// fps10 is frames per second times ten.
void panel_status(int16_t altitude, uint8_t heading, uint16_t x, uint16_t y,
                  uint16_t fps10);

#endif
