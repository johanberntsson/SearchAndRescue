#include <mega65.h>

#include "dma.h"

#define DMA_ADDR_MSB   (*(volatile uint8_t *)0xD701)
#define DMA_ADDR_BANK  (*(volatile uint8_t *)0xD702)
#define DMA_ADDR_MB    (*(volatile uint8_t *)0xD704)
#define DMA_ADDR_LSB_X (*(volatile uint8_t *)0xD705)  // triggers, extended list

// 12-byte F011B job preceded by option bytes. Layout and the $D705 trigger
// follow the working implementation in mega65/hexwar/src/dma.asm.
struct dma_list {
  uint8_t opt_format;  // $0B: 12-byte F011B job follows the options
  uint8_t opt_src_mb;  // $80
  uint8_t src_mb;      // source address bits 20-27
  uint8_t opt_dst_mb;  // $81
  uint8_t dst_mb;      // destination address bits 20-27
  uint8_t opt_skip;    // $85
  uint8_t dst_skip;    // destination bytes advanced per write
  uint8_t opt_end;     // $00
  uint8_t command;     // 0 = copy, 3 = fill
  uint16_t count;
  uint16_t src;
  uint8_t src_bank;  // source address bits 16-19
  uint16_t dst;
  uint8_t dst_bank;  // destination address bits 16-19
  uint8_t command_msb;
  uint16_t modulo;
};

static struct dma_list list = {
    0x0B, 0x80, 0, 0x81, 0, 0x85, 1, 0x00, 0, 0, 0, 0, 0, 0, 0, 0,
};

static void run(void)
{
  DMA_ADDR_BANK = 0;  // the job list itself lives in bank 0 of megabyte 0
  DMA_ADDR_MB = 0;
  DMA_ADDR_MSB = (uint8_t)((uint16_t)&list >> 8);
  DMA_ADDR_LSB_X = (uint8_t)(uint16_t)&list;
}

void dma_copy(uint32_t src, uint32_t dst, uint16_t count)
{
  list.command = 0;
  list.count = count;
  list.src = (uint16_t)src;
  list.src_bank = (uint8_t)(src >> 16) & 0x0F;
  list.src_mb = (uint8_t)(src >> 20);
  list.dst = (uint16_t)dst;
  list.dst_bank = (uint8_t)(dst >> 16) & 0x0F;
  list.dst_mb = (uint8_t)(dst >> 20);
  list.dst_skip = 1;
  run();
}

void dma_fill(uint32_t dst, uint8_t value, uint16_t count)
{
  list.command = 3;
  list.count = count;
  list.src = value;  // in a fill the source address low byte is the value
  list.src_bank = 0;
  list.src_mb = 0;
  list.dst = (uint16_t)dst;
  list.dst_bank = (uint8_t)(dst >> 16) & 0x0F;
  list.dst_mb = (uint8_t)(dst >> 20);
  list.dst_skip = 1;
  run();
}
