#include "music.h"

#include "audio.h"

// Both SIDs' volume registers; the player writes both (music/player.asm), so
// silence takes two writes.
#define SID_VOLUME  (*(volatile uint8_t *)(SID_BASE + 0x18))
#define SID2_VOLUME (*(volatile uint8_t *)(SID2_BASE + 0x18))

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
  SID_VOLUME = 0;
  SID2_VOLUME = 0;
}
