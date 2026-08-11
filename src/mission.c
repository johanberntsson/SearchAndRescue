#include "mission.h"

#include "input.h"

// The fixes are the whole of a mission's geography: src/sprite.c stands the
// figure at one and the briefing reads it out loud, because the panel's own
// LAT/LON readout is the only way to navigate and a search area you cannot
// name is not a search area.
//
//   46.713N 008.110E is the top of the pyramid in the north west of the map.
//   46.597N 008.227E is the shore of the lake in the south east, five height
//   units above the water.
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
        46713,
        8110,
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
        46597,
        8227,
    },
};

uint16_t mission_action_key(const mission *m)
{
  return m->cargo ? KEY_RETURN : KEY_SPACE;
}

const char *mission_action_name(const mission *m)
{
  return m->cargo ? "RETURN" : "SPACE ";
}

const char *mission_action_verb(const mission *m)
{
  return m->cargo ? "RELEASE CARGO" : "FILE REPORT";
}

const char *mission_cargo_name(const mission *m)
{
  return m->cargo ? m->cargo : "EMPTY";
}
