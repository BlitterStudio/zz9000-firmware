/*
 * Host stub for the Xilinx cache API used by zz_config.c.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef XIL_CACHE_H
#define XIL_CACHE_H

#include <stdint.h>

typedef uintptr_t UINTPTR;
typedef uintptr_t INTPTR;

/* Reader-side invalidate used by the playback pump / fabric compositor
 * before pulling producer PCM; a no-op on the host just like the flush. */
static inline void Xil_DCacheInvalidateRange(INTPTR adr, unsigned long len) {
    (void)adr;
    (void)len;
}

static inline void Xil_DCacheFlushRange(UINTPTR adr, unsigned long len) {
    (void)adr;
    (void)len;
}

#endif /* XIL_CACHE_H */
