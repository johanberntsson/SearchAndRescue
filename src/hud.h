// On-screen overlay drawn into the back buffer after the terrain.
#ifndef HUD_H
#define HUD_H

#include <stdint.h>

// Palette entries reserved for the overlay, set by tools/convmap.py.
#define HUD_INK   240
#define HUD_PAPER 241

// Draw "NN.N" in the top left. fps10 is frames per second times ten.
void hud_fps(uint32_t base, uint16_t fps10);

#endif
