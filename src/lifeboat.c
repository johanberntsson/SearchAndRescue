#include "lifeboat.h"

#include "loader.h"
#include "voxel.h"

// Three cells long and two across. **A thing narrower than a map cell is not
// there** -- the march steps a whole cell at twenty cells out, the distance
// a search is flown at, and the pyramid's first terraces vanished into the
// air for exactly this -- so two cells is the least that reads as a raft
// rather than a flicker. Nothing in this world is to scale (see SPR_WORLD_H).
#define BOAT_W 3
#define BOAT_H 2

// How far the raft's floor stands above the water, in height units -- a
// quarter of a cell each. Enough to put a visible edge on it from low down,
// and the figure is twelve.
#define FREEBOARD 2

#define HGT_BYTES (HGT_AXIS * HGT_AXIS)  // per cell, then the colour's
#define CELL_BYTES (HGT_BYTES + COL_AXIS * COL_AXIS)

// What the cells are while the raft is out -- and what they were, while it is
// in: voxel_swap_cell exchanges, so the same bytes go out and come back. The
// bytes themselves are at ATTIC_LIFEBOAT.
static uint8_t at_x, at_y, afloat;

static void swap(void)
{
  uint8_t i, j;
  uint32_t buf = ATTIC_LIFEBOAT;

  for (j = 0; j < BOAT_H; j++)
    for (i = 0; i < BOAT_W; i++)
      buf = voxel_swap_cell((uint8_t)(at_x + i), (uint8_t)(at_y + j), buf);
}

void lifeboat_launch(uint16_t *x, uint16_t *y)
{
  uint8_t deck = (uint8_t)(voxel_ground(*x, *y) + FREEBOARD);
  uint8_t __far *buf = (uint8_t __far *)ATTIC_LIFEBOAT;
  uint8_t n, k;

  // The figure's own cell is the middle of the near row; the two rows are
  // that and the one south of it, and the figure stands on the line between.
  at_x = (uint8_t)((*x >> 8) - 1);
  at_y = (uint8_t)(*y >> 8);
  *y = (uint16_t)(at_y + 1) << 8;

  // In the order voxel_swap_cell walks a cell: heights, then colours.
  for (n = 0; n < BOAT_W * BOAT_H; n++)
    for (k = 0; k < CELL_BYTES; k++)
      *buf++ = k < HGT_BYTES ? deck : LIFEBOAT_COLOUR;
  swap();
  afloat = 1;
}

void lifeboat_sink(void)
{
  if (afloat) {
    swap();
    afloat = 0;
  }
}
