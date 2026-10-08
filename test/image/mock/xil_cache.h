/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZ9K_TEST_XIL_CACHE_H
#define ZZ9K_TEST_XIL_CACHE_H
#include <stdint.h>
typedef intptr_t INTPTR;
static inline void Xil_DCacheFlushRange(INTPTR address, unsigned long length)
{
  (void)address;
  (void)length;
}
static inline void Xil_DCacheInvalidateRange(INTPTR address, unsigned long length)
{
  (void)address;
  (void)length;
}
#endif
