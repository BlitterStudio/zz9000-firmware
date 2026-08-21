/*
 * Host stub for the Xilinx cache API used by zz_config.c.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef XIL_CACHE_H
#define XIL_CACHE_H

#include <stdint.h>

typedef uintptr_t UINTPTR;

static inline void Xil_DCacheFlushRange(UINTPTR adr, unsigned long len) {
    (void)adr;
    (void)len;
}

#endif /* XIL_CACHE_H */
