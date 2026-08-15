// What the pilot is being sent to do. The missions are deliberately the same
// flight with different words on it: fly to a figure standing at a fix and
// press a key. What varies is the cargo bay, and that one field decides the
// rest -- see `cargo` below.
//
// **None of it is compiled in.** `campaign.yaml` and the files in `missions/`
// are the campaign; tools/campaign.py turns them into `campaign.bin`, which
// the loader reads before anything else because it says how many maps and
// figures there are to read after it. campaign_load() then points the array
// below into that blob, once, and nothing else in the game learns that any of
// it came off a disk.
#ifndef MISSION_H
#define MISSION_H

#include <stdint.h>

#include "panel.h"
#include "weather.h"

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
  uint8_t weather;    // WEATHER_CLEAR or WEATHER_RAIN; see weather.h
  // Which of the maps resident in attic RAM this mission is flown over. The
  // fix below is a cell of *that* map, so the two travel together: change one
  // and the target is in the sea. See MAP_SLOT in loader.h.
  uint8_t map;
  uint16_t lat;       // millidegrees north of the target's last known fix
  uint16_t lon;       // millidegrees east
} mission;

// How many the game can hold, which is what tools/campaign.py checks a
// campaign against. The mission list draws them two rows apart from row 6, so
// eight is what the page has room for; the buffer is what the whole campaign
// -- records and every string in it -- has to fit inside, and 455 bytes is
// what the two shipping missions come to. Both are spelled once more in
// tools/campaign.py and a disagreement is caught there.
#define MISSION_MAX    8
#define CAMPAIGN_BYTES 1024

extern mission missions[MISSION_MAX];

// Read campaign.bin and point the array at it. Returns null, or what went
// wrong -- there is no display worth the name at this point, so the string
// goes to the boot screen's error line. Call it before anything else is
// loaded: the two counts below are only good afterwards.
const char *campaign_load(void);

uint8_t mission_count(void);
uint8_t campaign_maps(void);     // map slots to fill; see MAP_SLOTS
uint8_t campaign_figures(void);  // billboards to read; see SPRITE_MAX

// The key that carries out the mission, and its name for the briefing and the
// panel. Derived from the cargo bay rather than stored, so that a mission
// cannot say one thing and do another.
uint16_t mission_action_key(const mission *m);
const char *mission_action_name(const mission *m);
const char *mission_action_verb(const mission *m);

// What the panel's cargo line reads for this mission.
const char *mission_cargo_name(const mission *m);

#endif
