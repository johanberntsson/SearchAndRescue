// A life raft on the sea: a few map cells raised out of the water for as
// long as one flight lasts, with the figure standing in it.
//
// **It is not in the map.** The island is shared with The Lost Hiker, and a
// raft drifting off its coast would be there too if genmap.py built it in. So
// the flight that wants one exchanges a handful of cells of the resident map
// with a buffer at launch and exchanges them back when it lands -- see
// voxel_swap_cell. The renderer never learns anything unusual is there, the
// figure stands on it because voxel_ground reads it, and only the thermal
// camera shows it, because its colour is the sea's until that is armed.
#ifndef LIFEBOAT_H
#define LIFEBOAT_H

#include <stdint.h>

// Keep in step with the `lifeboat` band in maps/palette.yaml: one unshaded
// entry, the first above the road, taken from the figures' pool. It is the
// deepest water's colour on the optical camera, so the raft cannot be seen;
// src/thermal.c warms it while the thermal camera is armed.
#define LIFEBOAT_COLOUR 174

// Put the raft on the water around a figure at an 8.8 map position, and
// return where the figure should stand in it. Call it after map_use and
// before sprite_place, which reads the ground the raft has just raised.
void lifeboat_launch(uint16_t *x, uint16_t *y);

// Take it out again if there is one. Idempotent, so the flight's caller can
// say it whatever the mission was. Before the next map_use, since it puts
// the cells back into whichever map is current.
void lifeboat_sink(void);

#endif
