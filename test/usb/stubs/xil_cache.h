#ifndef TEST_XIL_CACHE_H
#define TEST_XIL_CACHE_H

#include "xil_types.h"

static inline void Xil_DCacheFlushRange(u32 address, u32 length)
{
    (void)address;
    (void)length;
}

static inline void Xil_DCacheInvalidateRange(u32 address, u32 length)
{
    (void)address;
    (void)length;
}

#endif
