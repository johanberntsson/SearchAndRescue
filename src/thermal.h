// The thermal camera: the drone's second sensor, and a palette swap.
//
// documentation/vision.md asks for a FLIR mode and this is it. Nothing about
// the picture changes -- the march samples the same maps, the billboard is
// projected the same way, the framebuffer holds the same bytes. What changes
// is what the palette entries behind those bytes are: the terrain goes to a
// cold monochrome of itself, the sky to nothing at all, and the figure to one
// flat hot white. `vic4_set_entry` at the toggle, and not one cycle a frame.
//
// That is only affordable because the shared ramp gave terrain and figures
// separate indices (see Resources in CLAUDE.md). A map's colours are 16..173
// and every figure's fifteen are above them, so the two can be recoloured
// apart without the renderer ever learning there is a second mode.
#ifndef THERMAL_H
#define THERMAL_H

#include <stdint.h>

#include "voxel.h"   // SKY_BASE

// The one entry a figure is drawn in while the camera is armed. Reserved by
// tools/convmap.py, which keeps it out of the pool the sheets draw from --
// keep the two in step. It sits immediately under the sky gradient, at the
// top of what would otherwise be free.
#define THERMAL_HOT (SKY_BASE - 1)

// Arm it or put it away. Idempotent, so a flight may say what it wants as
// often as it likes. Call it from a flight only: it is the loaded palette of
// map_current that it swaps and restores, and the sky it puts back is
// whatever weather the flight is under.
void thermal_set(uint8_t on);

// Whether it is armed. A flight asks so that it can decide whether a buried
// figure is on the screen to be reported.
uint8_t thermal_on(void);

#endif
