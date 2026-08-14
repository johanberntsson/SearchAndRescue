#include "audio.h"

#include <stdint.h>

// From src/audio_irq.s.
void audio_hook(void);

// The handler audio_hook displaced, jumped to at the end of ours. Not static:
// the handler is assembly and imports it by name.
uint16_t audio_chain;

static uint8_t started;

void audio_begin(void)
{
  if (started)
    return;
  started = 1;
  audio_hook();
}
