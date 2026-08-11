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

// How many figures there are, and the number a mission names to pick one.
// They all live in bank 1 and only the flight's own is held in the 32K the
// program shares -- see SPRITE_STORE in loader.h. Adding one means a sheet in
// the Makefile's SPRITES, an entry in spr_files, and fifteen more palette
// slots, of which tools/convmap.py reports how many are left.
#define SPRITE_FIGURES 2

// Read every figure off the disk and park it in bank 1. Call it from
// load_resources, before the display is switched. Returns null, or the name of
// the file it could not make sense of.
const char *sprite_load(void);

// Bring one figure down into the drawing buffer, for the flight about to
// start. Nothing is drawn until this has been called.
void sprite_select(uint8_t figure);

// Stand the figure at an 8.8 map position, feet on the terrain there.
void sprite_place(uint16_t x, uint16_t y);

// Project it for this frame and return the march step its depth falls on, so
// the terrain march can sample its y buffer there, or VOXEL_NO_STEP when
// there is nothing to draw. Called by voxel_render before the columns.
uint8_t sprite_prepare(const camera *cam, int16_t cs, int16_t sn);

// Draw it over the terrain just marched, clipped against voxel_yclip.
void sprite_draw(uint32_t base);

// Whether the figure was on screen in the frame just drawn AND near enough to
// file a report about: what the report button asks. Being on screen is half
// the test on purpose -- a report means you looked at them, not that you flew
// past with the camera pointed elsewhere.
uint8_t sprite_reportable(void);

// Whether the drone is over the figure, wherever the camera happens to point:
// what the cargo release asks. A parcel is delivered by flying to the spot,
// and the range is shorter than the camera's to say so.
uint8_t sprite_in_range(void);

#endif
