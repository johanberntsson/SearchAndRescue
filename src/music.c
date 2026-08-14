#include "music.h"

#include "audio.h"

// Everything goes to both SIDs, one per stereo channel; the player does the
// same (music/player.asm).
static void sid_put(uint8_t reg, uint8_t value)
{
  ((volatile uint8_t *)SID_BASE)[reg] = value;
  ((volatile uint8_t *)SID2_BASE)[reg] = value;
}

// From the tune, via tools/acme2calypsi.py.
void music_init(void);
void music_play(void);

// Read by the interrupt every frame. Not static: the handler is assembly and
// imports it by name.
uint8_t music_enabled;

void music_set(uint8_t on)
{
  if ((on != 0) == (music_enabled != 0))
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

  // Gates down, not just the volume. A silent SID with its gates still up is
  // a trap for whatever plays next: an envelope only triggers on a 0 -> 1
  // edge, so the next thing to write a waveform with the gate bit set gets no
  // note at all. That is exactly how the engine came out silent. engine.c
  // does not rely on this -- it pulls the gates down itself -- but leaving
  // them up was the wrong thing for `stop` to mean.
  {
    uint8_t v;

    for (v = 0; v < 3; v++)
      sid_put((uint8_t)(v * 7 + 4), 0);
  }
  sid_put(0x18, 0);
}
