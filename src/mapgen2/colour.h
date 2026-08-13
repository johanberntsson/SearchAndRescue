// Stage two's two passes. See colour.c.
#ifndef MAPGEN2_COLOUR_H
#define MAPGEN2_COLOUR_H

#include <stdint.h>

// Turn the finished terrain into palette indices -- what the renderer eats.
// `--stage colour` on the PC side.
uint32_t colour_build(void);

// Write the two finished maps into attic RAM in the layout src/voxel_asm.s
// addresses: 256x256 planes, one per sub-cell corner, each on a 64K boundary.
void planes_write(void);

#endif
