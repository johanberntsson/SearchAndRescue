#include <mega65.h>

#include "dma.h"
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
        a = b = (uint16_t)(FB_BLANK / 64);  // past the bottom of the buffers
      }
      screen[0][row * FB_COLS + col] = a;
      screen[1][row * FB_COLS + col] = b;
    }
  }
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

  dma_fill(FB_BLANK, 0, 64);

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
