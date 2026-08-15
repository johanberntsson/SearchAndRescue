// The title music: a three voice SID tune, played from the ROM's own
// interrupt fifty times a second. See audio.h for the interrupt itself.
//
// The player and the tune are written in ACME under `music/` and translated
// into this build's assembler by tools/acme2calypsi.py -- see that file, and
// tools/checkmusic.py, which proves the translation assembles to the same
// bytes ACME does.
//
// Music belongs to the pages and not to the flight: it plays under all of
// them -- the loading screen, the title, the mission list, the briefing and
// the debrief -- and only launching stops it. What a flight has instead is
// engine.h.
#ifndef MUSIC_H
#define MUSIC_H

#include <stdint.h>

// Play, or stop and silence both SIDs. Turning it on again starts the tune
// from the top, intro and all -- it is a title screen, not a radio.
// Idempotent, so a screen that is already musical can say so again.
void music_set(uint8_t on);

#endif
