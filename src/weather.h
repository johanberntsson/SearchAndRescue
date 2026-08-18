// The weather over a flight: an overcast sky, and rain drawn on top of the
// finished picture. Which one a mission gets is a field in the mission table,
// the same way its cargo and its figure are.
#ifndef WEATHER_H
#define WEATHER_H

#include <stdint.h>

#define WEATHER_CLEAR 0
#define WEATHER_RAIN  1

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

// Draw this frame's rain over the finished picture. voxel_render calls it
// after the terrain and the billboard, so rain is in front of everything; it
// returns immediately when it is not raining.
//
// Nothing has to erase it: the sky DMA at the top of the next frame repaints
// every pixel of the buffer before anything reads it.
void weather_rain_draw(uint32_t base);

// One pseudo-random stream for all of the weather -- the wind's gusts and the
// rain's drops come out of the same generator rather than carrying two. Seed
// it once a flight, from something that differs between them.
void weather_seed(uint16_t seed);
uint16_t weather_rnd(void);

#endif
