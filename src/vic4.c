#include <mega65.h>

#include "vic4.h"

#define COLOUR_RAM 0xFF80000UL

// Screen RAM: one 16-bit character number per cell, read row by row.
static uint16_t screen[2][SCREEN_ROWS * FB_COLS];

static void build_screen_tables(void)
{
  uint8_t row, col;

  for (row = 0; row < SCREEN_ROWS; row++) {
    for (col = 0; col < FB_COLS; col++) {
      uint16_t a, b;

      if (row < FB_ROWS) {
        uint16_t cell = col * FB_ROWS + row;  // column-major, see vic4.h
        a = (uint16_t)(FB_A / 64) + cell;
        b = (uint16_t)(FB_B / 64) + cell;
      } else {
        a = b = ' ';  // panel: a text character number, below $100
      }
      screen[0][row * FB_COLS + col] = a;
      screen[1][row * FB_COLS + col] = b;
    }
  }
}

void vic4_panel_char(uint8_t col, uint8_t row, uint8_t ch, uint8_t colour)
{
  uint16_t cell = (uint16_t)(FB_ROWS + row) * FB_COLS + col;
  uint8_t __far *cram = (uint8_t __far *)COLOUR_RAM;

  screen[0][cell] = ch;
  screen[1][cell] = ch;

  // Two colour RAM bytes per cell in 16-bit character mode: attributes then
  // the foreground colour, which is a full 8-bit palette index.
  cram[(int16_t)(cell * 2)] = 0;
  cram[(int16_t)(cell * 2 + 1)] = colour;
}

void vic4_init(void)
{
  uint16_t i;
  uint8_t __far *cram = (uint8_t __far *)COLOUR_RAM;

  CPU_PORTDDR = 0x41;  // 40 MHz

  VICIV.key = 0x47;  // unlock the VIC-IV registers
  VICIV.key = 0x53;

  VICIV.ctrlb &= (uint8_t)~0x80;  // H320, not H640

  // Turn off the hot registers, or a stray write to a legacy VIC-II register
  // makes the VIC-IV recompute the layout and undo everything below.
  VICIV.sdbdrwd_msb &= (uint8_t)~VIC4_HOTREG_MASK;

  VICIV.ctrlc |= VIC4_CHR16_MASK    // 16-bit character numbers
                 | VIC4_FCLRHI_MASK // full colour for characters above $FF
                 | VIC4_VFAST_MASK;

  // Character data for the panel's text rows. Nothing had set this before the
  // panel existed, and whatever the ROM left behind draws a horizontal line
  // for a space. The C65 ROM's 8x8 set sits at $2D000 and the VIC-IV can read
  // it where it is, so the panel costs no RAM for a font. Written a byte at a
  // time: the 32-bit field runs over $D06B, which is not part of the pointer.
  VICIV.charptr_lsb = 0x00;
  VICIV.charptr_msb = 0xD0;
  VICIV.charptr_bnk = 0x02;

  VICIV.linestep = FB_COLS * 2;  // bytes of screen RAM per row
  VICIV.chrcount = FB_COLS;
  // Source pixels per output pixel, in 120ths. 120 is 1:1, so 60 makes every
  // pixel two screen pixels wide and stretches the 160 columns across 320.
  VICIV.chrxscl = 60;

  VICIV.bordercol = 0;
  VICIV.screencol = 0;

  // Character attributes are not used, and whatever the ROM left behind would
  // be interpreted as one.
  for (i = 0; i < SCREEN_ROWS * FB_COLS * 2; i++)
    cram[i] = 0;

  build_screen_tables();
  vic4_show(0);
}

void vic4_set_palette(const uint8_t *planes)
{
  uint16_t i;

  for (i = 0; i < 256; i++) {
    PALETTE.red[i] = planes[i];
    PALETTE.green[i] = planes[256 + i];
    PALETTE.blue[i] = planes[512 + i];
  }
}

void vic4_show(uint8_t buffer)
{
  // Writing all four bytes also clears the CHRCOUNT high bits and the screen
  // pointer megabyte in $D063, which is what we want.
  VICIV.scrnptr = (uint32_t)(uint16_t)&screen[buffer & 1][0];
}

uint32_t vic4_base(uint8_t buffer)
{
  return (buffer & 1) ? FB_B : FB_A;
}
