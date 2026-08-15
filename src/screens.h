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
// vic4_init has run -- a Kernal open fails outright afterwards.
//
// They are made to look like screens_title all the same: the display is put
// into forty columns and the same two lines are written white on black, with
// LOADING and a bar where PRESS SPACE will be. So the boot is one picture
// throughout, and finishing the load takes the bar away rather than scrolling
// a new screen up under it.
//
// None of them print. The ROM's screen editor cannot address a row, cannot
// erase one, and lays a line out eighty bytes wide whatever the display is
// showing -- so these write screen RAM directly.
void screens_boot(void);
void screens_loading(uint8_t percent);

// Loading is done: take the LOADING line and its bar away, leaving the title.
void screens_loaded(void);

void screens_load_failed(const char *why, const char *file);

// Give the ROM back the eighty-column display it thinks it has. Only the
// startup benchmark report needs this -- it is the one thing left that prints,
// and only when REPORT_SECONDS asks for it.
void screens_boot_restore(void);

// The title, on the game's own display once there is a palette for it.
void screens_title(void);

// The mute setting, on the line under every page's prompt. Every page draws
// it as it stands; call this when it changes. `M` mutes whatever the place
// you are in sounds like, so this line is about the music and the flight says
// its own on the panel.
void screens_music(uint8_t on);

// The mission list, with `selected` highlighted.
void screens_missions(uint8_t selected);

// What the pilot is being sent to do, and the controls to do it with. Both
// come out of the mission rather than being spelled out here, so the two
// kinds of flight read differently without being written twice.
void screens_briefing(uint8_t mission);

// How a flight ended. The three are one screen with different words on it,
// which is all the difference there is between them.
typedef enum {
  FLIGHT_DONE,     // the job was done
  FLIGHT_LOST,     // the cargo went down in the wrong place
  FLIGHT_CRASHED,  // flown into the hillside, which only sport mode allows
  FLIGHT_FLAT,     // the battery ran out before the job was done
  FLIGHT_ABORTED,  // the pilot gave up and flew home
} flight_outcome;

// The debrief. `seconds` is how long the flight took, however it ended.
void screens_debrief(uint8_t mission, flight_outcome how, uint16_t seconds);

#endif
