#include <calypsi/intrinsics6502.h>
#include <mega65.h>

#include "input.h"

// Flight needs to know which keys are *held*, which the Kernal's key buffer
// cannot say, so the matrix is scanned directly. Every key used here lives in
// row 1 or row 2, so two probes are enough.
//
//   row 1 ($FD): 3  W  A  4  Z  S  E  LSHIFT   (bit 0 first)
//   row 2 ($FB): 5  R  D  6  C  F  T  X
//
// A pressed key reads as 0.
static uint8_t scan_row(uint8_t row)
{
  uint8_t columns;
  __interrupt_state_t state = __get_interrupt_state();

  // The Kernal scans the keyboard on its own interrupt and would otherwise be
  // free to rewrite the row select between these two accesses.
  __disable_interrupts();
  CIA1.pra = row;
  columns = CIA1.prb;
  __restore_interrupt_state(state);

  return (uint8_t)~columns;
}

uint8_t input_read(void)
{
  uint8_t r1 = scan_row(0xFD);
  uint8_t r2 = scan_row(0xFB);
  uint8_t keys = 0;

  if (r1 & 0x02)
    keys |= KEY_W;
  if (r1 & 0x04)
    keys |= KEY_A;
  if (r1 & 0x20)
    keys |= KEY_S;
  if (r2 & 0x04)
    keys |= KEY_D;
  if (r2 & 0x02)
    keys |= KEY_R;
  if (r2 & 0x20)
    keys |= KEY_F;

  return keys;
}
