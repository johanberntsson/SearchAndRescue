// What the pilot is being sent to do. One entry so far; the table is here so
// that the second one is data rather than a rewrite.
#ifndef MISSION_H
#define MISSION_H

#include <stdint.h>

#include "panel.h"

// A GPS fix in millidegrees is a map cell and nothing else -- the world is
// 256 cells square and a cell is one millidegree -- and latitude counts down
// the map, so cell row 0 is the northern edge. The low byte puts the target
// in the middle of its cell rather than on the corner.
#define FIX_TO_X(lon) ((uint16_t)((lon) - MAP_LON_WEST) << 8 | 0x80)
#define FIX_TO_Y(lat) ((uint16_t)(255 - ((lat) - MAP_LAT_SOUTH)) << 8 | 0x80)

#define BRIEF_LINES 3

typedef struct {
  const char *name;
  const char *brief[BRIEF_LINES];
  const char *found;  // what the debrief says was achieved
  uint16_t lat;       // millidegrees north of the target's last known fix
  uint16_t lon;       // millidegrees east
} mission;

#define MISSION_COUNT 1

extern const mission missions[MISSION_COUNT];

#endif
