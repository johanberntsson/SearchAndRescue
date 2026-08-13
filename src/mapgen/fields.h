// Where the generator's fields live in attic RAM, and the few constants both
// generator programs have to agree on.
//
// **This header is the contract between stage one and stage two**, the way
// src/handover.h is the one between stage one and the game. Stage one fills
// these and vanishes; stage two paints them and vanishes; the game flies what
// is left. Attic RAM survives a program load, which is what makes all three
// possible -- see documentation/on-device-maps.md.
//
// The map is 512 square. Slots 0 and 1 of attic RAM are scratch while the
// generator runs, because the game does not load its own maps until both
// generator programs are gone.
#ifndef MAPGEN_FIELDS_H
#define MAPGEN_FIELDS_H

#include <stdint.h>

#include "../loader.h"

#define SIZE      512
#define SIZE_MASK (SIZE - 1)

// TYPES["island"]["sea"], 0.26, and the level field's "no water here".
#define SEA 17039UL
#define DRY 0xFFFF

// The stretch's histogram, and every percentile after it: 1024 buckets, six
// bits of a value to a bucket. Keep in step with BUCKETSHIFT in genmap.py.
#define BUCKETSHIFT 6
#define BUCKETS     (int)(65536L >> BUCKETSHIFT)

// Slot 2: the terrain, then the water level, then the handover block.
#define NOISE_FIELD  MAP_SLOT(2)
#define LEVEL_FIELD  (MAP_SLOT(2) + 0x80000UL)

// Slot 0: the rivers' scratch while stage one runs, and then **the finished
// planes**, which is why the colour field is not here -- MAP_HEIGHTMAP(0) is
// at +0x100000 and would land on top of it while it was still being read.
#define FLOW_FIELD   MAP_SLOT(0)
#define BLUR_TMP     (MAP_SLOT(0) + 0x80000UL)

// Slot 1: the terrain as the rivers found it, the bed under the water, and
// what has been built on top.
#define SURFACE_FIELD MAP_SLOT(1)
#define BED_FIELD     (MAP_SLOT(1) + 0x80000UL)
#define BUILT_FIELD   (MAP_SLOT(1) + 0x100000UL)
#define COLOUR_FIELD  (MAP_SLOT(1) + 0x180000UL)  // one byte a pixel

// **Where stage one leaves the random stream.** The whole of what reproduces a
// map is the sequence of draws, so stage two cannot start a stream of its own:
// it has to carry on from where the terrain finished, or its dithers are a
// different field. Past the proof block, in the handover slot.
#define HANDOVER_RND (MAP_SLOT(2) + 0x100000UL + 0x8000UL)

#endif
