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

// A flight begins: the motors from cold, so a launch is heard to spool up.
// `heard` is the pilot's mute setting, which the caller keeps between
// missions -- a muted launch never touches the SID at all, rather than making
// a click and then falling silent.
void engine_start(uint8_t heard);

// The mute key, and the end of a flight. Coming back on picks the note up
// where the throttle left it rather than spooling from cold again.
void engine_set(uint8_t on);

// Once a frame. `mode` is the speed limiter, 0 to 2; `moving` is whether the
// pilot is asking for any forward or back at all; `climb` is 1, 0 or -1.
// Nothing here is a frequency: this sets a target and the interrupt does the
// walking.
void engine_throttle(uint8_t mode, uint8_t moving, int8_t climb);

// The battery warning, and the one thing in a flight that is not the motors.
// `level` is BATTERY_WARN or BATTERY_ALARM (panel.h): a higher, longer note
// for the second. It is heard only if the engine is -- M means a quiet
// flight, and this is the flight's own channel.
//
// **It borrows the engine's third voice**, the triangle an octave down, which
// is the quietest of the three and the least missed for the third of a second
// this takes. The other two carry the note straight through it.
void engine_beep(uint8_t level);

// Once a frame while a flight is running: this is what ends the beep and
// gives the voice back. Doing it here rather than in the interrupt keeps the
// SID writes on one side of the fence.
void engine_beep_step(void);

#endif
