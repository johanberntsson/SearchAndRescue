// The terrain noise on the machine. See noise.c.
#ifndef MAPGEN_NOISE_H
#define MAPGEN_NOISE_H

#include <stdint.h>

// Where the field goes: the front of the spare map slot. 512x512 Q0.16 values
// is 512 KB of the slot's 2 MB. The handover block sits well past it -- see
// HANDOVER_BASE in src/handover.h, which is offset for exactly this reason.
#include "../loader.h"
#define NOISE_FIELD MAP_SLOT(2)

// Draw the lattices and the offsets, in genmap.py's order. Call once.
void noise_init(void);

// Build the field into attic RAM. Returns Fletcher's checksum of it, the two
// wrapping 16-bit sums packed as b<<16 | a, which is what tools/fbmcheck.py
// prints for the same map at `--stage octaves`.
uint32_t noise_run(void);

// Rescale it between its own 0.5th and 99.5th percentiles and put it on the
// type's elevation range, in place. `--stage shape` on the PC side.
uint32_t noise_stretch(void);

// Draw the mask's lattice, and its salt -- which must be the next thing the
// stream is asked for after noise_init's draws. Then multiply the radial
// falloff into the terrain: `--stage terrain`.
void mask_init(void);
uint32_t mask_apply(void);

// Drop the hills on the dry land: `--stage hills`.
void hills_apply(void);

// Read the whole field back and checksum it. Verification only -- it costs
// more than the passes it checks, because it is a quarter of a million reads
// out of attic RAM and nothing else.
uint32_t field_checksum(void);

// Where a lake could go: the dry local minima below the median of their own
// heights. `--stage minima` on the PC side, checksummed as positions rather
// than as a field.
uint32_t minima_find(void);

// Flood the basins at those candidates. The checksum is of the water level
// field, not the terrain -- `--stage lakes`.
uint32_t lakes_fill(void);

// The blurred height field the rivers run downhill on: `--stage flow`.
uint32_t flow_build(void);

// Carve the rivers. Returns the terrain's checksum and fills `level_sum` with
// the water level's, since carving moves both: `--stage rivers`.
uint32_t rivers_carve(uint32_t *level_sum);

#endif
