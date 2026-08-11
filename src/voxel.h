// Front-to-back heightfield ray marching.
#ifndef VOXEL_H
#define VOXEL_H

#include <stdint.h>

#include "vic4.h"

// Sky occupies a run of palette entries the colourmap never uses.
// Keep in sync with SKY_BASE/SKY_SHADES in tools/convmap.py.
#define SKY_BASE   224
#define SKY_SHADES 16

// tan(30 degrees) * 256: half of a 60 degree horizontal field of view. The
// ray for screen column i leaves the view axis by this much times
// (2i / FB_WIDTH - 1), so anything else that projects into the same picture
// -- a billboard, a HUD marker -- has to use the same number.
#define TAN_HALF_FOV 148

// Screen pixels one unit of sideways tangent moves a point: the inverse of
// the line above, so a lateral offset over a depth lands on a column.
// 256L because a 16-bit int does not hold the half-width in 8.8 on the way.
#define PX_PER_TAN \
  ((int16_t)((FB_WIDTH / 2 * 256L + TAN_HALF_FOV / 2) / TAN_HALF_FOV))

// "No step": further away than the march ever samples. Also the value that
// switches the y buffer snapshot off, because it is higher than the march's
// step index can reach.
#define VOXEL_NO_STEP 255

typedef struct {
  uint16_t x, y;   // 8.8 fixed point map position; the high byte is the cell,
                   // so movement wraps around the 256x256 map for free
  uint8_t angle;   // 256 units to the full turn
  int16_t height;  // heightmap units, of which there are 4 to the map cell
  int16_t horizon; // screen row the horizon projects to
} camera;

void voxel_init(void);
void voxel_render(uint32_t base, const camera *cam);

// Terrain height under a map position, for keeping the camera above ground
// and for standing world objects on it.
uint8_t voxel_ground(uint16_t x, uint16_t y);

// sin(angle) * 256, for moving the camera along its heading.
int16_t voxel_sin(uint8_t angle);

// (a * b) >> 8 on the 45GS02's hardware multiplier, which costs 85 cycles
// against the compiler's 2203. The inner loop has this inlined; this is the
// same thing for code that needs it a handful of times a frame.
int16_t voxel_mul_shift8(int16_t a, int16_t b);

// Screen rows one height unit spans at depth z (8.8 map cells), in 8.8. This
// is the perspective scale the march's own table holds per step, worked out
// for an arbitrary depth so a billboard can be projected exactly rather than
// snapped to a step.
uint16_t voxel_inv_z(uint16_t z);

// The first march step at or beyond depth z, or VOXEL_NO_STEP if the march
// never gets that far. Everything the march draws before this step is nearer
// than z, which is what makes a depth clip out of the y buffer.
uint8_t voxel_depth_step(uint16_t z);

// Sampled during the march: for each ray column, the topmost screen row that
// terrain *nearer* than the frame's billboard reached. A sprite pixel at or
// below this row is behind that terrain and must not be drawn. FB_HEIGHT --
// the row below the bottom of the view -- means nothing is in the way.
extern uint8_t voxel_yclip[VX_COLS];

// Byte offset into a framebuffer of pixel row 0 of ray column `raycol`. The
// march already precomputes these, so anything else drawing a vertical run
// can have one for free rather than paying FB_COLUMN's 16-bit multiply --
// 657 cycles a go from C.
uint16_t voxel_column_offset(uint16_t raycol);

#endif
