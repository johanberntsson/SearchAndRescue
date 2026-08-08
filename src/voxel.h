// Front-to-back heightfield ray marching.
#ifndef VOXEL_H
#define VOXEL_H

#include <stdint.h>

// Sky occupies a run of palette entries the colourmap never uses.
// Keep in sync with SKY_BASE/SKY_SHADES in tools/convmap.py.
#define SKY_BASE   224
#define SKY_SHADES 16

typedef struct {
  uint16_t x, y;   // 8.8 fixed point map position; the high byte is the cell,
                   // so movement wraps around the 256x256 map for free
  uint8_t angle;   // 256 units to the full turn
  int16_t height;  // heightmap units, of which there are 4 to the map cell
  int16_t horizon; // screen row the horizon projects to
} camera;

void voxel_init(void);
void voxel_render(uint32_t base, const camera *cam);

// Terrain height under a map position, for keeping the camera above ground.
uint8_t voxel_ground(uint16_t x, uint16_t y);

// sin(angle) * 256, for moving the camera along its heading.
int16_t voxel_sin(uint8_t angle);

#endif
