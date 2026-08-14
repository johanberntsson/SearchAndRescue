#include "music.h"

// Both SIDs' volume registers. The MEGA65 has one per stereo channel and the
// player writes both (see music/player.asm), so silence takes two writes.
#define SID_VOLUME  (*(volatile uint8_t *)0xD418)
#define SID2_VOLUME (*(volatile uint8_t *)0xD438)

// From the tune, via tools/acme2calypsi.py.
void music_init(void);
void music_play(void);

// From src/music_irq.s: the two halves that have to be assembly, because one
// is an interrupt handler and the other rewrites a vector under SEI.
void music_hook(void);

// Read by the interrupt every frame. Not static: the handler is assembly and
// imports it by name.
uint8_t music_enabled;

// The handler music_hook displaced, jumped to at the end of ours.
uint16_t music_chain;

static uint8_t started;

void music_begin(void)
{
  if (started)
    return;
  started = 1;
  music_enabled = 0;
  music_hook();
}

void music_set(uint8_t on)
{
  if (!started || (on != 0) == (music_enabled != 0))
    return;

  if (on) {
    // Rewind rather than resume. Everything the player owns is reset by its
    // own init, so there is no state left over from the last time the menus
    // were up, and the two bar intro is heard the way it was written.
    music_init();
    music_enabled = 1;
    return;
  }

  // Stop the interrupt writing the SID before taking the volume away, or the
  // frame in between would leave a note ringing under a silent mixer.
  music_enabled = 0;
  SID_VOLUME = 0;
  SID2_VOLUME = 0;
}
