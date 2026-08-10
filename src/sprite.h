// A world-anchored billboard drawn into the framebuffer after the terrain:
// one scaled 2D figure, clipped against the heightfield it stands in.
//
// This is the software sprite documentation/vision.md asks for. It is not a
// VIC-IV hardware sprite: those are for the HUD, and the one experiment with
// them (the overview crosshair, see vic4.c) ran into xemu ignoring SPRPTRADR.
#ifndef SPRITE_H
#define SPRITE_H

#include <stdint.h>

#include "voxel.h"

// Read the figure off the disk. Call it from load_resources, before the
// display is switched: it prints its own failures.
int sprite_load(void);

// Stand the survivor at an 8.8 map position, feet on the terrain there.
void sprite_place(uint16_t x, uint16_t y);

// Project it for this frame and return the march step its depth falls on, so
// the terrain march can sample its y buffer there, or VOXEL_NO_STEP when
// there is nothing to draw. Called by voxel_render before the columns.
uint8_t sprite_prepare(const camera *cam, int16_t cs, int16_t sn);

// Draw it over the terrain just marched, clipped against voxel_yclip.
void sprite_draw(uint32_t base);

#endif
