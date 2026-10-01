#include <calypsi/intrinsics6502.h>
#include <mega65.h>

#include "input.h"

// Flight needs to know which keys are *held*, which the Kernal's key buffer
// cannot say, so the matrix is scanned directly. Every key used here lives in
// one of six rows, so six probes are enough.
//
//   row 0 ($FE): DEL  RETURN  CRSR-R  F7  F1  F3  F5  CRSR-D
//   row 1 ($FD): 3  W  A  4  Z  S  E  LSHIFT   (bit 0 first)
//   row 2 ($FB): 5  R  D  6  C  F  T  X
//   row 4 ($EF): 9  I  J  0  M  K  O  N
//   row 5 ($DF): +  P  L  -  .  :  @  ,
//   row 7 ($7F): 1  <-  CTRL  2  SPACE  C=  Q  STOP
//
// A pressed key reads as 0.
//
// HELP is not in that matrix at all. It is one of the C65's extra keys --
// NO SCROLL, TAB, ALT, HELP, F9, F11, F13, ESC -- which sit on a ninth row
// selected by bit 1 of $D607 (port E of the C65's UART, with its direction
// register at $D608) rather than by $DC00. See scan_extra().
//
// **The joysticks are on the same two registers**, which is the whole of what
// makes reading them awkward. Port 2 pulls the *row select* lines low, $DC00,
// and port 1 pulls the *column* lines low, $DC01 -- so a stick in port 1
// reads as keys in every row of the matrix: pushed up, it is `3` in row 1 and
// `1` in row 7, which is sport mode and cinematic mode at once. read_sticks()
// finds out which column lines port 1 is holding and scan_row() leaves them
// out. Port 2 cannot invent a key on its own -- a row line pulled low only
// matters if a key on that row is down -- so it needs nothing.
static uint8_t port1_lines;

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

  return (uint8_t)(~columns & ~port1_lines);
}

// The C65's extra row, read with every ordinary row deselected so that
// nothing but it can pull a column low. HELP is bit 3. Both port E registers
// are put back as they were found: the ROM's own keyboard scan drives them too.
#define D607 (*(volatile uint8_t *)0xD607)
#define D608 (*(volatile uint8_t *)0xD608)

static uint8_t scan_extra(void)
{
  uint8_t columns, data, ddr, save;
  __interrupt_state_t state = __get_interrupt_state();

  __disable_interrupts();
  save = CIA1.pra;
  data = D607;
  ddr = D608;
  CIA1.pra = 0xFF;
  D607 = (uint8_t)(data & ~0x02);
  D608 = (uint8_t)(ddr | 0x02);
  columns = CIA1.prb;
  D608 = ddr;
  D607 = data;
  CIA1.pra = save;
  __restore_interrupt_state(state);

  return (uint8_t)(~columns & ~port1_lines);
}

// Both ports, active high: bit 0 up, 1 down, 2 left, 3 right, 4 fire.
//
// With every row deselected no key can pull a column low, so whatever reads
// low on $DC01 is port 1's stick; and $DC00 reads back its pins rather than
// what was written to it, so whatever reads low there is port 2's. The row
// select is put back afterwards, with interrupts off across all of it,
// because the Kernal's keyboard scan writes it too.
#define JOY_UP    0x01
#define JOY_DOWN  0x02
#define JOY_LEFT  0x04
#define JOY_RIGHT 0x08
#define JOY_FIRE  0x10
#define JOY_LINES 0x1F

static uint8_t read_sticks(void)
{
  uint8_t p1, p2, save;
  __interrupt_state_t state = __get_interrupt_state();

  __disable_interrupts();
  save = CIA1.pra;
  CIA1.pra = 0xFF;
  p1 = (uint8_t)(~CIA1.prb & JOY_LINES);
  p2 = (uint8_t)(~CIA1.pra & JOY_LINES);
  CIA1.pra = save;
  __restore_interrupt_state(state);

  port1_lines = p1;
  return (uint8_t)(p1 | p2);
}

// **A stick is debounced and a key is not.** The keyboard has never been
// seen to double a press here, but a joystick is a set of microswitches read
// raw, and one push of the button chatters for a few milliseconds -- release, press,
// release -- which the edge detector below would take as two presses, and a
// page that a press dismisses would dismiss the next page with it. So a line
// counts as let go only once it has read released on JOY_SETTLE scans in a
// row. The pages scan once a frame (input_frame), which makes that 60 ms; the
// flight scans once a frame too, at about twelve a second, where the same
// three scans are a quarter of a second before the button can be taken
// again. Neither is anything a pilot would notice.
#define JOY_SETTLE 3

static uint8_t joy_held;
static uint8_t joy_idle[5];

static uint8_t sticks(void)
{
  uint8_t now = read_sticks();
  uint8_t i, bit;

  for (i = 0, bit = 1; i < 5; i++, bit <<= 1) {
    if (now & bit) {
      joy_held |= bit;
      joy_idle[i] = 0;
    } else if (joy_held & bit && ++joy_idle[i] >= JOY_SETTLE) {
      joy_held &= (uint8_t)~bit;
    }
  }
  return joy_held;
}

// What was held at the previous scan, for the edge detector.
static keymask was_held;

static keymask scan(void)
{
  // First, because it is what tells scan_row which columns are a stick.
  uint8_t joy = sticks();
  uint8_t r0 = scan_row(0xFE);
  uint8_t r1 = scan_row(0xFD);
  uint8_t r2 = scan_row(0xFB);
  uint8_t r4 = scan_row(0xEF);
  uint8_t r5 = scan_row(0xDF);
  uint8_t r7 = scan_row(0x7F);
  uint8_t extra = scan_extra();
  keymask keys = 0;

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
  if (r5 & 0x02)
    keys |= KEY_P;
  if (r2 & 0x40)
    keys |= KEY_T;
  if ((r0 & 0x10) || (extra & 0x08))  // F1, or HELP
    keys |= KEY_HELP;

  // The stick is WASD: forward, back and yaw in the air, up and down a list.
  if (joy & JOY_UP)
    keys |= KEY_W;
  if (joy & JOY_DOWN)
    keys |= KEY_S;
  if (joy & JOY_LEFT)
    keys |= KEY_A;
  if (joy & JOY_RIGHT)
    keys |= KEY_D;
  if (joy & JOY_FIRE)
    keys |= KEY_FIRE;

  return keys;
}

void input_scan(keymask *held, keymask *pressed)
{
  keymask keys = scan();

  if (held)
    *held = keys;
  if (pressed)
    *pressed = keys & ~was_held;
  was_held = keys;
}

void input_flush(void)
{
  // Anything still down now counts as held rather than as a fresh press, so
  // the key that dismissed one screen cannot dismiss the next one too.
  was_held = scan();
}

void input_frame(void)
{
  // $D012 is the low eight bits of the raster line, and a PAL frame is 312
  // lines, so line 240 comes round exactly once a frame.
  while (VICII.rasterline != 240)
    ;
  while (VICII.rasterline == 240)
    ;
}
