// Stage one: the program that prepares attic RAM and then hands the machine
// to the game.
//
// **Why there are two programs.** The game already fills the 32 KB at $2001 --
// the link map says 97.6% of it, about 790 bytes free -- and a map generator is
// several times the code a *loader* is. The two are never live at the same
// moment, and attic RAM survives a program load, so stage one can fill the
// attic and vanish. It pays twice: stage one owns the whole 32 KB and banks 1,
// 4 and 5, since no framebuffer or screen table exists yet, and the game
// eventually loses the loader and the exomizer decruncher.
// documentation/on-device-maps.md has the costing.
//
// What it does *today* is write a proof block and chain. The generator goes in
// here one pass at a time; the boot arrangement is what this first version is
// for, because it is the part that either works or does not.

#include <stdint.h>
#include <stdio.h>

#include "../handover.h"

// Who to hand over to: the game's own file on the boot disk. Typed into the
// keyboard queue below exactly as a pilot would type it.
#define GAME_NAME "SAR"

// The C65's keyboard queue, which is not the C64's at $0277/$C6 -- this runs
// in C65 mode, under BASIC 65. Sixteen bytes at $02B0 with the count in zero
// page at $D0.
#define KEY_QUEUE ((volatile uint8_t *)0x02B0)
#define KEY_COUNT (*(volatile uint8_t *)0x00D0)
#define KEY_QUEUE_MAX 16

// The raster, for a seed. Any of the low bits of it is a different number
// every boot, which is the whole requirement -- see handover.h.
#define RASTER (*(volatile uint8_t *)0xD012)

// Hand the machine to the game by typing for the pilot.
//
// **This is ozmoo's restart trick** (`z_ins_restart` in its asm/disk.asm), and
// it is the one mechanism here that does not depend on the toolchain: the
// command goes in the keyboard queue, main returns to BASIC, BASIC prints
// READY and the screen editor reads the queue as though somebody at the
// keyboard had typed the line. No second BASIC line to squeeze past Calypsi's
// `SYS 8206` stub, and no loader of our own to keep out of the way of the
// program being loaded -- the ROM does all of it after we are gone.
//
// ozmoo prints its command to the screen and queues only the RETURN, because
// its line is longer than the sixteen bytes the queue holds and needs cursor
// movement in it. `RUN"SAR"` is nine bytes, so the whole line fits and the
// editor echoes it for us.
static void chain(const char *name)
{
  uint8_t n = 0;
  const char *p;

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

// Write the proof block: a seed, and a stream drawn from it. Returns the
// seed, for the report.
static uint16_t proof_write(void)
{
  volatile uint8_t __far *p = (uint8_t __far *)HANDOVER_PROOF;
  volatile uint8_t __far *h = (uint8_t __far *)HANDOVER_BASE;
  uint16_t state, seed;
  int16_t i;

  // Two reads of the raster, a whole boot's worth of loading apart in the
  // instruction stream and not the same number. Zero is the one value
  // xorshift cannot be seeded with.
  seed = (uint16_t)RASTER | (uint16_t)((uint16_t)RASTER << 8);
  if (!seed)
    seed = 1;

  state = seed;
  for (i = 0; i < HANDOVER_PROOF_BYTES; i++) {
    state = handover_step(state);
    p[i] = (uint8_t)state;
  }

  // The magic goes down last, so a handover interrupted part way through
  // reads as absent rather than as corrupt.
  h[HANDOVER_OFF_SEED + 0] = (uint8_t)seed;
  h[HANDOVER_OFF_SEED + 1] = (uint8_t)(seed >> 8);
  h[3] = (uint8_t)(HANDOVER_MAGIC >> 24);
  h[2] = (uint8_t)(HANDOVER_MAGIC >> 16);
  h[1] = (uint8_t)(HANDOVER_MAGIC >> 8);
  h[0] = (uint8_t)HANDOVER_MAGIC;

  return seed;
}

int main(void)
{
  uint16_t seed;

  putchar(147);  // clear
  printf("\n\n     SEARCH AND RESCUE\n");
  printf("     STAGE ONE\n\n\n");
  printf("     PREPARING ATTIC RAM\n");

  seed = proof_write();
  printf("     HANDOVER SEED %u\n\n", seed);

  // ozmoo clears the screen before it queues its keys; this does not, and can
  // afford not to. The editor executes the *logical line the cursor is on*,
  // and BASIC's READY leaves it at the start of a fresh one -- so the report
  // above scrolls up out of the way rather than being read back. Keeping it
  // means a failed handover leaves something on screen to look at, which is
  // worth more here than a tidy screen for the second or so before the game
  // clears it anyway.
  chain(GAME_NAME);
  return 0;
}
