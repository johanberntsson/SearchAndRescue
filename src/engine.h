// The engine note: what a flight sounds like.
//
// Not a tune -- there is no player, no patterns and no rhythm. Three SID
// voices are gated on for the whole flight and never gated off again; all
// that ever changes is their pitch. The interrupt (audio.h) walks that pitch
// towards wherever the pilot has left the sticks, fifty times a second, so
// opening the throttle spools the motors up over about half a second instead
// of stepping.
//
// The note follows what is being ASKED of the props rather than how fast the
// drone is going, which is why the speed mode moves it on its own: sport
// works the motors harder whatever the sticks are doing. That is the same
// reasoning the battery drain is built on.
#ifndef ENGINE_H
#define ENGINE_H

#include <stdint.h>

// Arm the voices and start the motors from cold, so a launch is heard to
// spool up. Silence and gate them off again at the end of the flight.
void engine_start(void);
void engine_stop(void);

// Once a frame. `mode` is the speed limiter, 0 to 2; `moving` is whether the
// pilot is asking for any forward or back at all; `climb` is 1, 0 or -1.
// Nothing here is a frequency: this sets a target and the interrupt does the
// walking.
void engine_throttle(uint8_t mode, uint8_t moving, int8_t climb);

#endif
