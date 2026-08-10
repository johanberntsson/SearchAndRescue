// The full-screen text pages either side of a flight: the title and its
// loading bar, the mission list, the briefing, and the debrief.
//
// They cost no pixels. The display picks text or full colour per character
// NUMBER, so a page is a rewrite of screen RAM and nothing else; see
// vic4_text_mode.
#ifndef SCREENS_H
#define SCREENS_H

#include <stdint.h>

// Loading, on the ROM's own text screen, because the disk cannot be read once
// vic4_init has run -- a Kernal open fails outright afterwards. So these
// three print rather than draw, and they are the only screens that do.
//
// The bar only ever grows, one character at a time, so it needs no cursor
// control: printing is enough.
void screens_boot(void);
void screens_loading(uint8_t percent);
void screens_load_failed(const char *why, const char *file);

// The title, on the game's own display once there is a palette for it.
void screens_title(void);

// The mission list, with `selected` highlighted. One mission exists so far.
void screens_missions(uint8_t selected);

// What the pilot is being sent to do, and the controls to do it with.
void screens_briefing(uint8_t mission);

// The debrief, after a report is filed. `seconds` is how long the flight took.
void screens_accomplished(uint8_t mission, uint16_t seconds);

#endif
