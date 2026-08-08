// Held-key state for flight controls, read straight off the CIA1 matrix.
#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

#define KEY_W 0x01
#define KEY_A 0x02
#define KEY_S 0x04
#define KEY_D 0x08
#define KEY_R 0x10
#define KEY_F 0x20

// Bitmask of the keys currently held down.
uint8_t input_read(void);

#endif
