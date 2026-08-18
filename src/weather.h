// The weather over a flight: an overcast sky, and rain or snow drawn on top of
// the finished picture. Which one a mission gets is a field in the mission
// table, the same way its cargo and its figure are.
//
// **Snow is the rain loop with different constants**, not a second system:
// same forty-eight drops in four layers, same state in LOW_FREE, same drawing
// loop. What it adds is a sideways drift, which needs the wind and the
// camera's heading and so is handed in by the flight.
#ifndef WEATHER_H
#define WEATHER_H

#include <stdint.h>

// Keep in sync with WEATHER in tools/campaign.py. weather_draw indexes its
// tables by this less one, so the falling kinds have to stay contiguous
// above WEATHER_CLEAR.
#define WEATHER_CLEAR 0
#define WEATHER_RAIN  1
#define WEATHER_SNOW  2

// Set the weather for a flight, once, from flight(). It rewrites the sky's
// sixteen palette entries, so it has to come after vic4_set_palette has put
// the loaded palette up -- and after weather_seed, because arming the rain
// scatters the first drops.
void weather_set(uint8_t weather);

// Put this flight's sky back, without touching the rain. The thermal camera
// takes the sky over while it is armed and this is what it hands back -- which
// has to be the *weather's* sky, since a rainy flight is overcast and the
// palette on disk is not.
void weather_sky(void);

// How far a snowflake is carried sideways for every row it falls, in 8.8
// screen columns and signed. The flight works it out every frame, because it
// is the wind's component *across the view* and so moves when the camera turns
// as well as when the wind veers. Rain ignores it: a raindrop falls too fast
// to be blown anywhere, and its fixed lean is what it has instead.
void weather_drift(int16_t sideways);

// Tell the weather whether the thermal camera is armed. Snow left in its own
// near-white would be the brightest thing on a screen whose whole point is
// that a body is; this is what makes cold weather read cold. See src/thermal.h.
void weather_thermal(uint8_t on);

// Draw this frame's rain or snow over the finished picture. voxel_render calls
// it after the terrain and the billboard, so weather is in front of
// everything; it returns immediately on a clear flight.
//
// Nothing has to erase it: the sky DMA at the top of the next frame repaints
// every pixel of the buffer before anything reads it.
void weather_draw(uint32_t base);

// One pseudo-random stream for all of the weather -- the wind's gusts and the
// rain's drops come out of the same generator rather than carrying two. Seed
// it once a flight, from something that differs between them.
void weather_seed(uint16_t seed);
uint16_t weather_rnd(void);

#endif
