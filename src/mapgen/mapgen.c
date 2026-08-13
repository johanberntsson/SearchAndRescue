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
// What it does today is generate the terrain noise of one map -- the dominant
// term of the whole generator -- time itself, print a checksum the PC can
// match, and hand over. The rest of the pipeline goes in behind it, one pass
// at a time.

#include <stdint.h>
#include <stdio.h>

#include "../handover.h"
#include "../profile.h"
#include "noise.h"

// Who to hand over to: the game's own file on the boot disk. Typed into the
// keyboard queue below exactly as a pilot would type it, so it has to match
// the Makefile's $(PRG) basename -- diskutil.rb names a file on disk after
// its host file.
#define GAME_NAME "SAR"

// How long the report stays up before the handover, so that a headless run
// can screenshot it and a real machine can be read. `make REPORT=n` sets it,
// the same knob the game's benchmark report uses.
#ifndef REPORT_SECONDS
#define REPORT_SECONDS 20
#endif

// The C65's keyboard queue, which is not the C64's at $0277/$C6 -- this runs
// in C65 mode, under BASIC 65. Sixteen bytes at $02B0 with the count in zero
// page at $D0.
#define KEY_QUEUE ((volatile uint8_t *)0x02B0)
#define KEY_COUNT (*(volatile uint8_t *)0x00D0)

// The raster, for a seed. Any of the low bits of it is a different number
// every boot on hardware -- see handover.h.
#define RASTER (*(volatile uint8_t *)0xD012)

void kernal_ioinit(void);

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
// movement in it. `NEW` and `RUN"SAR"` are thirteen bytes with their two
// RETURNs, so both whole lines fit and the editor echoes them for us.
//
// **The NEW is not optional, and it is not there for the program text.** A C
// program of any size leaves BASIC's zero page as Calypsi's pseudo registers
// found it -- they own $02-$7F, which is where BASIC keeps its own pointers --
// and the first version of stage one got away with it only because it used so
// little of them. The moment this file grew 32-bit arithmetic, `RUN"SAR"` came
// back `?FORMULA TOO COMPLEX ERROR`: BASIC's temporary string stack pointer
// was left past its end, so the filename constant had nowhere to go. NEW runs
// a CLR, which puts every one of those pointers back.
//
// **And it has to be its own line.** `NEW:RUN"SAR"` runs the NEW and silently
// drops the rest: NEW resets the interpreter's text pointer, so BASIC finds
// end-of-line where the colon was. Two queued RETURNs is the answer, which is
// what ozmoo does as well.
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

// Hold the report up, so a headless screenshot can catch it and somebody at a
// real machine can read it. Any key cuts it short. The raster is the clock
// because the profiler's has just been handed back to the Kernal.
static void hold(uint8_t seconds)
{
  uint16_t frames = (uint16_t)seconds * 50;
  uint8_t last = RASTER;

  while (frames) {
    uint8_t r = RASTER;

    if (r < last)  // the raster wrapped: one frame
      frames--;
    last = r;
    if (*(volatile uint8_t *)0x00D0)  // something in the keyboard queue
      break;
  }
}

int main(void)
{
  uint16_t seed;
  uint32_t start, ticks, tps;
  uint32_t sum;

  putchar(147);  // clear
  printf("\n\n     SEARCH AND RESCUE\n");
  printf("     STAGE ONE: MAP GENERATOR\n\n\n");

  seed = proof_write();

  // The clock. profile_init takes CIA2's timers, which is why kernal_ioinit
  // has to put them back before the handover reads the disk.
  profile_init();
  profile_calibrate();
  tps = profile_ticks_per_second();

  printf("     TERRAIN NOISE 512X512\n");
  noise_init();

  start = profile_now32();
  sum = noise_run();
  ticks = start - profile_now32();

  // Seconds and hundredths, from a tick count that would overflow a
  // multiply by 100. tps/100 is the ticks in a hundredth of a second.
  {
    uint32_t hundredths = ticks / (tps / 100);

    printf("     %lu.%02lu SECONDS\n", hundredths / 100, hundredths % 100);
  }
  printf("     CHECKSUM %04X%04X\n\n", (uint16_t)(sum >> 16), (uint16_t)sum);
  printf("     HANDOVER SEED %u\n", seed);

  hold(REPORT_SECONDS);

  // Give the Kernal its timers back before anything asks it to read a disk --
  // and the handover is exactly that, since BASIC has to LOAD the game.
  kernal_ioinit();

  // ozmoo clears the screen before it queues its keys; this does not, and can
  // afford not to. The editor executes the *logical line the cursor is on*,
  // and BASIC's READY leaves it at the start of a fresh one -- so the report
  // above scrolls up out of the way rather than being read back. Keeping it
  // means a failed handover leaves something on screen to look at.
  chain(GAME_NAME);
  return 0;
}
