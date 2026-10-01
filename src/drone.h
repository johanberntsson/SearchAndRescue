// The drone on the boot and title screens: two 16-colour hardware sprites side
// by side, hovering in the middle while the disk loads and then flying about
// the band between the logo and the prompt for as long as the title is up.
//
// Sprites 6 and 7, because a 16-colour sprite takes its colours from palette
// entries sprite * 16 onwards and those two land on 96..127 -- terrain ramp,
// which no page shows. The panel's text plane is sprites 0-5 and is only ever
// up in a flight, so the two never meet.
#ifndef DRONE_H
#define DRONE_H

#include <stdint.h>

// Read it off the disk; before the boot screen, beside the logo. Non-zero
// if it is not there, in which case nothing else here draws anything.
int drone_load(void);

// Put it up, wherever it last was -- the middle of the band, the first time.
// After vic4_text_mode, which takes every sprite away; and again after a
// flight, whose map_use() puts the palette entries it borrows back.
void drone_show(void);

// One frame: turn the rotors, and fly on if `fly` says so. The loading screen
// spins them in place; the title moves it as well.
void drone_tick(uint8_t fly);

#endif
