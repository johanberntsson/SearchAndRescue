// The game's half of the handover: read what stage one left in attic RAM,
// check every byte of it, and say so. See src/handover.h for the contract and
// src/mapgen/mapgen.c for the program that writes it.
//
// This is scaffolding. It exists to prove the two-stage boot before there is a
// generator to hang on it, and it goes when the block carries a real map --
// at which point what proves the handover is that there is a world to fly
// over. The 32 KB is too tight to carry it a day longer than that.

#include "handover.h"

handover_result handover_check(uint16_t *seed)
{
  volatile uint8_t __far *h = (uint8_t __far *)HANDOVER_BASE;
  const uint8_t __far *p = (const uint8_t __far *)HANDOVER_PROOF;
  uint16_t state;
  int16_t i;

  // Byte at a time rather than assembling a uint32_t: the comparison only
  // needs to fail, and 32-bit arithmetic here is a library call.
  if (h[0] != (uint8_t)HANDOVER_MAGIC
      || h[1] != (uint8_t)(HANDOVER_MAGIC >> 8)
      || h[2] != (uint8_t)(HANDOVER_MAGIC >> 16)
      || h[3] != (uint8_t)(HANDOVER_MAGIC >> 24))
    return HANDOVER_ABSENT;

  state = (uint16_t)h[HANDOVER_OFF_SEED]
        | ((uint16_t)h[HANDOVER_OFF_SEED + 1] << 8);
  *seed = state;

  // Once, and only from this boot: clearing the magic is what stops a block
  // left over from an earlier run passing on a boot where stage one never
  // ran. See the note in handover.h.
  h[0] = 0;

  for (i = 0; i < HANDOVER_PROOF_BYTES; i++) {
    state = handover_step(state);
    if (p[i] != (uint8_t)state)
      return HANDOVER_CORRUPT;
  }

  return HANDOVER_OK;
}
