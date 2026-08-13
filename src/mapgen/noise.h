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
// prints for the same map.
uint32_t noise_run(void);

#endif
