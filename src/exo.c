#include "exo.h"

// State for the decruncher in src/exo_asm.s. Declared here rather than in the
// assembly so the zero page allocation goes through the compiler, the same way
// the renderer's vx_* block does.
//
// The three pointers are 32-bit and in zero page for the 45GS02's [zp],z
// addressing; everything else is ordinary storage.
__zpage uint8_t exo_cr[4];   // crunched stream, read forwards
__zpage uint8_t exo_src[4];  // back-reference into the plaintext
__zpage uint8_t exo_out[4];  // output pointer

uint8_t exo_bitbuf, exo_bits_lo, exo_bits_hi, exo_count;
uint8_t exo_len_lo, exo_len_hi;
uint8_t exo_a_lo, exo_a_hi, exo_b, exo_t_lo, exo_t_hi;
uint8_t exo_c_lo, exo_c_hi, exo_idx;

// The 52-entry decode table exodec.c builds from the stream header.
uint8_t exo_tabl_lo[52], exo_tabl_hi[52], exo_tabl_bi[52];

// Static bit counts and offsets for sequence lengths 1, 2 and 3+.
const uint8_t exo_tabl_bit[3] = {2, 4, 4};
const uint8_t exo_tabl_off[3] = {48, 32, 16};

void exo_decrunch_asm(void);

static void set_pointer(uint8_t *p, uint32_t address)
{
  p[0] = (uint8_t)address;
  p[1] = (uint8_t)(address >> 8);
  p[2] = (uint8_t)(address >> 16);
  p[3] = (uint8_t)(address >> 24);
}

void exo_decrunch(uint32_t src, uint32_t dst)
{
  set_pointer(exo_cr, src);
  set_pointer(exo_out, dst);
  exo_decrunch_asm();
}
