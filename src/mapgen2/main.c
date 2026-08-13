// Stage two: the second half of the map generator.
//
// The disk boots AUTOBOOT.C65, which is stage one; stage one leaves the
// terrain in attic RAM and chains here; this paints it and chains to the game.
// Three programs, one map, and nothing shared between them but attic RAM and
// two small headers -- src/mapgen/fields.h for the layout, src/handover.h for
// the block the game checks.
//
// **Why it is a separate program and not another pass.** Stage one's 32 KB is
// full: 27.5 of text alone, and colour would not fit beside it. The costing
// has had the answer in reserve since the first page -- "the generator splits
// again" -- and it costs nothing, because attic RAM survives a program load.

#include <stdint.h>
#include <stdio.h>

#include "../mapgen/fields.h"
#include "../mapgen/fixed.h"
#include "../dma.h"
#include "../profile.h"
#include "colour.h"

#define GAME_NAME "SAR"

#ifndef STAGE1_HOLD
#define STAGE1_HOLD 4
#endif

#define KEY_QUEUE ((volatile uint8_t *)0x02B0)
#define KEY_COUNT (*(volatile uint8_t *)0x00D0)
#define RASTER    (*(volatile uint8_t *)0xD012)

#define SECS(t)       ((t) / (tps / 100) / 100)
#define HUNDREDTHS(t) ((t) / (tps / 100) % 100)

// src/mapgen/kernal.s saves and restores whatever the linker puts in `zzpage`
// -- BASIC keeps its own pointers in the same $02-$7F -- and wants the section
// to exist. Stage two has no other use for zero page, so it declares one byte
// to bring it into being.
__zpage volatile uint8_t zp_anchor;

// Filled by the canary in __low_level_init and counted by cstack_measure.
uint16_t cstack_unused;

void kernal_ioinit(void);
void zp_preserve(void);
void zp_restore(void);
void basic_in(void);

// The same handover stage one uses, for the same reasons -- see the long note
// in src/mapgen/mapgen.c. NEW first, and on its own line.
static void chain(const char *name)
{
  uint8_t n = 0;
  const char *p;

  KEY_QUEUE[n++] = 'N';
  KEY_QUEUE[n++] = 'E';
  KEY_QUEUE[n++] = 'W';
  KEY_QUEUE[n++] = 13;
  KEY_QUEUE[n++] = 'R';
  KEY_QUEUE[n++] = 'U';
  KEY_QUEUE[n++] = 'N';
  KEY_QUEUE[n++] = '"';
  for (p = name; *p; p++)
    KEY_QUEUE[n++] = (uint8_t)*p;
  KEY_QUEUE[n++] = '"';
  KEY_QUEUE[n++] = 13;
  KEY_COUNT = n;
}

static void hold(uint8_t seconds, uint32_t tps)
{
  uint32_t start = profile_now32();
  uint32_t want = tps * seconds;

  while ((uint32_t)(start - profile_now32()) < want)
    ;
}

int main(void)
{
  uint32_t start, ticks, tps, sum;

  zp_anchor = 0;        // ... and referenced, or the section is dropped again
  zp_preserve();

  putchar(147);
  printf("\n\n     SEARCH AND RESCUE\n");
  printf("     STAGE TWO: COLOUR\n\n\n");

  profile_init();
  profile_calibrate();
  tps = profile_ticks_per_second();

  // **Carry on the stream rather than starting one.** The sequence of draws is
  // the whole of what reproduces a map, so the dithers below have to follow
  // stage one's last draw exactly.
  {
    static uint32_t seed;

    dma_copy(HANDOVER_RND, (uint32_t)(uint16_t)&seed, 4);
    rnd_seed(seed);
  }

  start = profile_now32();
  sum = colour_build();
  ticks = start - profile_now32();
  printf("     COLOUR %lu.%02lu S  %04X%04X\n",
         SECS(ticks), HUNDREDTHS(ticks),
         (uint16_t)(sum >> 16), (uint16_t)sum);

  start = profile_now32();
  planes_write();
  ticks = start - profile_now32();
  printf("     PLANES %lu.%02lu SECONDS\n\n", SECS(ticks), HUNDREDTHS(ticks));

  hold(STAGE1_HOLD, tps);

  kernal_ioinit();
  zp_restore();
  basic_in();
  chain(GAME_NAME);
  return 0;
}
