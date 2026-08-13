// What stage one leaves in attic RAM for the game to find.
//
// **This header is the contract between the two programs**, and besides the
// disk it is the only thing they share. Stage one writes the block below and
// then vanishes; the game reads it and never learns that a generator existed.
// Both sides include this file, so the layout cannot drift between them the
// way two copies of a struct would.
//
// Today the block carries nothing but a proof that the handover works at all
// -- see documentation/on-device-maps.md for what it is a scaffold for.
#ifndef HANDOVER_H
#define HANDOVER_H

#include <stdint.h>

#include "loader.h"

// The block sits in the *spare* map slot, above the two the disk still
// carries, so nothing stage one does can disturb a map the game loads for
// itself. When the generator is real, what it writes is the slots themselves
// and this moves to a corner of one of them.
#define HANDOVER_BASE MAP_SLOT(2)

// 'S','A','R','1'.
#define HANDOVER_MAGIC 0x31524153UL

// The proof: bytes of a known stream, written by stage one and compared byte
// for byte by the game. 16 KB is not the whole of a map and is not meant to
// be -- what supersedes this block is a *map* up here, which proves the
// handover by being flyable. The size is kept under 64 K deliberately: a
// Calypsi far pointer indexes with an int16_t (see MAP_BIAS in src/voxel.c),
// so a longer run would need the pointer walked rather than indexed, which is
// code this scaffolding does not deserve.
#define HANDOVER_PROOF_BYTES 0x4000
#define HANDOVER_PROOF (HANDOVER_BASE + 8)

// Offsets within the block. Spelled out rather than given as a struct because
// the two sides reach them through far pointers, one field at a time, and a
// struct read through a far pointer costs more than it explains.
//
// There is no checksum field, and that is not an omission: the game generates
// the stream for itself and compares every byte, which is strictly stronger
// than agreeing on a sum -- and cheaper, since a sum would have to be built
// on both sides as well.
#define HANDOVER_OFF_MAGIC 0
#define HANDOVER_OFF_SEED  4  // what the proof stream was drawn from

// The proof stream: xorshift16, the same one src/weather.c uses for the rain.
// Both programs step it, so it lives here rather than in either of them.
static inline uint16_t handover_step(uint16_t s)
{
  s ^= (uint16_t)(s << 7);
  s ^= s >> 9;
  s ^= (uint16_t)(s << 8);
  return s;
}

// **The seed is drawn from the raster, not fixed**, and that is what makes the
// check mean anything. Attic RAM is not cleared by a reset, so a block left up
// there by an earlier boot would pass a fixed-pattern test forever -- including
// on the boot where stage one was never loaded at all. A fresh seed each boot,
// plus the game clearing the magic once it has read the block, makes it a
// one-shot proof: run the game twice without stage one in between and the
// second run reports the handover missing.

// What the game found. Anything but HANDOVER_OK means the two-stage boot is
// not working, and which one says where to look.
typedef enum {
  HANDOVER_OK,       // stage one ran, and every byte of it came through
  HANDOVER_ABSENT,   // no magic: the game was started on its own
  HANDOVER_CORRUPT,  // magic, but the bytes behind it are not the stream
} handover_result;

// Read stage one's block, check it, and invalidate it. Fills `seed` with what
// stage one drew, for the report -- it is the number that distinguishes this
// boot's block from an older one still lying in attic RAM.
handover_result handover_check(uint16_t *seed);

#endif
