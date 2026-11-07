#ifndef XIL_CACHE_H
#define XIL_CACHE_H
#include <stdint.h>
void Xil_DCacheFlushRange(uint32_t address, uint32_t length);
void Xil_DCacheInvalidateRange(uint32_t address, uint32_t length);
void Xil_ICacheInvalidateRange(uint32_t address, uint32_t length);
#endif
