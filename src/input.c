#include <calypsi/intrinsics6502.h>
#include <mega65.h>

#include "input.h"

// Flight needs to know which keys are *held*, which the Kernal's key buffer
// cannot say, so the matrix is scanned directly. Every key used here lives in
// one of five rows, so five probes are enough.
//
//   row 0 ($FE): DEL  RETURN  CRSR-R  F7  F1  F3  F5  CRSR-D
//   row 1 ($FD): 3  W  A  4  Z  S  E  LSHIFT   (bit 0 first)
//   row 2 ($FB): 5  R  D  6  C  F  T  X
//   row 4 ($EF): 9  I  J  0  M  K  O  N
//   row 7 ($7F): 1  <-  CTRL  2  SPACE  C=  Q  STOP
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

// What was held at the previous scan, for the edge detector.
static uint16_t was_held;

static uint16_t scan(void)
{
  uint8_t r0 = scan_row(0xFE);
  uint8_t r1 = scan_row(0xFD);
  uint8_t r2 = scan_row(0xFB);
  uint8_t r4 = scan_row(0xEF);
  uint8_t r7 = scan_row(0x7F);
  uint16_t keys = 0;

  if (r1 & 0x02)
    keys |= KEY_W;
  if (r1 & 0x04)
    keys |= KEY_A;
  if (r1 & 0x20)
    keys |= KEY_S;
  if (r1 & 0x40)
    keys |= KEY_E;
  if (r1 & 0x01)
    keys |= KEY_3;
  if (r2 & 0x04)
    keys |= KEY_D;
  if (r2 & 0x02)
    keys |= KEY_R;
  if (r2 & 0x20)
    keys |= KEY_F;
  if (r7 & 0x40)
    keys |= KEY_Q;
  if (r7 & 0x01)
    keys |= KEY_1;
  if (r7 & 0x08)
    keys |= KEY_2;
  if (r7 & 0x10)
    keys |= KEY_SPACE;
  if (r7 & 0x80)
    keys |= KEY_STOP;
  if (r0 & 0x02)
    keys |= KEY_RETURN;
  if (r4 & 0x10)
    keys |= KEY_M;

  return keys;
}

void input_scan(uint16_t *held, uint16_t *pressed)
{
  uint16_t keys = scan();

  if (held)
    *held = keys;
  if (pressed)
    *pressed = keys & (uint16_t)~was_held;
  was_held = keys;
}

void input_flush(void)
{
  // Anything still down now counts as held rather than as a fresh press, so
  // the key that dismissed one screen cannot dismiss the next one too.
  was_held = scan();
}
