#ifndef XIL_CACHE_L_H
#define XIL_CACHE_L_H
#include <stdint.h>
typedef uintptr_t INTPTR;
typedef uint32_t u32;
void Xil_L1DCacheFlushRange(INTPTR address, u32 length);
void Xil_L1DCacheInvalidateRange(INTPTR address, u32 length);
#endif
