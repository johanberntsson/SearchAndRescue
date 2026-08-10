#include "mission.h"

// 46.713N 008.110E is the top of the pyramid in the north west of the map,
// where src/sprite.c stands the survivor. The briefing gives the fix out
// loud, because the panel's own LAT/LON readout is the only way to navigate
// and a search area you cannot name is not a search area.
const mission missions[MISSION_COUNT] = {
    {
        "THE LOST HIKER",
        {
            "A HIKER IS OVERDUE FROM THE RIDGE",
            "ABOVE THE VALLEY. THE LAST FIX FROM",
            "THEIR PHONE PUTS THEM NEAR A SUMMIT.",
        },
        "SURVIVOR LOCATED AND REPORTED",
        46713,
        8110,
    },
};
