/*
 * Host stub for the Xilinx cache API used by sdk_video_stream.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef XIL_CACHE_H
#define XIL_CACHE_H

#include <stdint.h>

typedef intptr_t INTPTR;

static inline void Xil_DCacheFlushRange(INTPTR address, unsigned long length)
{
	(void)address;
	(void)length;
}

#endif /* XIL_CACHE_H */
