// What the pilot is being sent to do. The missions are deliberately the same
// flight with different words on it: fly to a figure standing at a fix and
// press a key. What varies is the cargo bay, and that one field decides the
// rest -- see `cargo` below.
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
  const char *objective;  // the order itself, one line of the briefing
  // What is in the cargo bay, or null when it is empty -- and with it the
  // whole difference between the two kinds of mission there are. An empty bay
  // means the job is to look: SPACE files a report, and getting it wrong
  // costs nothing but the time to go round again. A full one means the job is
  // to deliver: RETURN releases the cargo, and there is only one of whatever
  // it is, so releasing it in the wrong place ends the flight.
  const char *cargo;
  const char *done;   // the debrief's line when it goes right
  const char *lost;   // ... and when the cargo goes down in the wrong place
  uint8_t figure;     // which billboard stands at the fix; see sprite.h
  uint16_t lat;       // millidegrees north of the target's last known fix
  uint16_t lon;       // millidegrees east
} mission;

#define MISSION_COUNT 2

extern const mission missions[MISSION_COUNT];

// The key that carries out the mission, and its name for the briefing and the
// panel. Derived from the cargo bay rather than stored, so that a mission
// cannot say one thing and do another.
uint16_t mission_action_key(const mission *m);
const char *mission_action_name(const mission *m);
const char *mission_action_verb(const mission *m);

// What the panel's cargo line reads for this mission.
const char *mission_cargo_name(const mission *m);

#endif
