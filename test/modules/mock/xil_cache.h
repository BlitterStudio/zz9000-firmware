#ifndef XIL_CACHE_H
#define XIL_CACHE_H
#include <stdint.h>
static inline void Xil_DCacheFlushRange(uint32_t address, uint32_t length) { (void)address; (void)length; }
static inline void Xil_ICacheInvalidateRange(uint32_t address, uint32_t length) { (void)address; (void)length; }
#endif
