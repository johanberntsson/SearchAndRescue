#include "mission.h"

#include "input.h"

// The fixes are the whole of a mission's geography: src/sprite.c stands the
// figure at one and the briefing reads it out loud, because the panel's own
// LAT/LON readout is the only way to navigate and a search area you cannot
// name is not a search area.
//
// **Each mission also names its map**, and the fix is a cell of that map, so
// the two can never be edited apart. The maps are generated from
// maps/island.yaml and maps/plains.yaml (the Makefile's MAP_YAMLS) and both
// are resident in attic RAM at once, which a hand-drawn map pair could never
// have been: two generated maps are about 500 KB crunched against 661 KB for
// one drawn one.
//
//   46.687N 008.106E, map 0, is the top of the step pyramid on the island's
//   northern headland -- the pyramid is an `items:` entry in island.yaml and
//   is built into the terrain itself, so the hiker is standing on generated
//   ground.
//   46.522N 008.081E, map 1, is the shore of the largest lake on the plains,
//   a cell of dry ground with water on three sides of it.
const mission missions[MISSION_COUNT] = {
    {
        "THE LOST HIKER",
        {
            "A HIKER IS OVERDUE FROM THE RIDGE",
            "ABOVE THE VALLEY. THE LAST FIX FROM",
            "THEIR PHONE PUTS THEM NEAR A SUMMIT.",
        },
        "FIND THEM AND FILE A REPORT",
        0,  // nothing in the bay: this one is flown with the camera
        "SURVIVOR LOCATED AND REPORTED",
        0,
        0,
        WEATHER_CLEAR,
        0,      // the island
        46687,
        8106,
    },
    {
        "FIRST AID",
        {
            "A HIKER BY THE LAKE IS IN ALLERGIC",
            "SHOCK AND HIS FRIEND HAS CALLED IT",
            "IN. THE BAY HAS ONE EPIPEN IN IT.",
        },
        "RELEASE THE EPIPEN OVER THEM",
        "EPIPEN",
        "EPIPEN DELIVERED TO THE CASUALTY",
        "THE ONLY EPIPEN IS LOST ON THE HILL",
        1,
        WEATHER_RAIN,
        1,      // the plains
        46522,
        8081,
    },
};

uint16_t mission_action_key(const mission *m)
{
  return m->cargo ? KEY_RETURN : KEY_SPACE;
}

const char *mission_action_name(const mission *m)
{
  return m->cargo ? "RETURN" : "SPACE";
}

const char *mission_action_verb(const mission *m)
{
  return m->cargo ? "RELEASE CARGO" : "FILE REPORT";
}

const char *mission_cargo_name(const mission *m)
{
  return m->cargo ? m->cargo : "EMPTY";
}
