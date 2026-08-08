// DMAgic (F011B) block copy and fill across the full 28-bit address space.
#ifndef DMA_H
#define DMA_H

#include <stdint.h>

void dma_copy(uint32_t src, uint32_t dst, uint16_t count);
void dma_fill(uint32_t dst, uint8_t value, uint16_t count);

#endif
