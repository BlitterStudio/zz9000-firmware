/*
 * Host stub for the Xilinx L1 cache API used by overlay.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef XIL_CACHE_L_H
#define XIL_CACHE_L_H

#include "xil_cache.h"

static inline void Xil_L1DCacheFlushRange(INTPTR address,
                                          unsigned long length)
{
	(void)address;
	(void)length;
}

#endif /* XIL_CACHE_L_H */
