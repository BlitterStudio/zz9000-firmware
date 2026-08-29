/*
 * Host stub for the Xilinx cache API used by zz_config.c.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef XIL_CACHE_H
#define XIL_CACHE_H

#include <stdint.h>
#include <string.h>

typedef uintptr_t UINTPTR;
typedef uintptr_t INTPTR;

#ifdef XIL_CACHE_MOCK_RECORD
/*
 * Recording mode (fabric cache-fidelity test): every invalidate/flush is
 * logged as (kind, address, length) so a test can assert the exact ranges
 * the compositor reads from and writes to. Off by default -- only the
 * lease-plane test binary compiles with -DXIL_CACHE_MOCK_RECORD -- and
 * recording itself stays disabled until xil_cache_mock_set_enabled()
 * turns it on, so suites that never touch it run without behavior or
 * overhead changes. The state object is defined in fabric_test_common.c.
 */
enum {
    XIL_CACHE_MOP_INVALIDATE = 0,
    XIL_CACHE_MOP_FLUSH = 1,
};

#define XIL_CACHE_MOCK_CAPACITY 512U

struct xil_cache_mock_op {
    uintptr_t adr;
    unsigned long len;
    int kind; /* XIL_CACHE_MOP_* */
};

struct xil_cache_mock_state {
    int enabled;
    unsigned count;
    unsigned overflow; /* ops dropped because the log filled up */
    struct xil_cache_mock_op ops[XIL_CACHE_MOCK_CAPACITY];
};

extern struct xil_cache_mock_state g_xil_cache_mock;

static inline void xil_cache_mock_record(int kind, uintptr_t adr,
                                         unsigned long len) {
    struct xil_cache_mock_op *op;

    if (!g_xil_cache_mock.enabled)
        return;
    if (g_xil_cache_mock.count >= XIL_CACHE_MOCK_CAPACITY) {
        g_xil_cache_mock.overflow++;
        return;
    }
    op = &g_xil_cache_mock.ops[g_xil_cache_mock.count];
    op->adr = adr;
    op->len = len;
    op->kind = kind;
    g_xil_cache_mock.count++;
}

static inline void Xil_DCacheInvalidateRange(INTPTR adr, unsigned long len) {
    xil_cache_mock_record(XIL_CACHE_MOP_INVALIDATE, (uintptr_t)adr, len);
}

static inline void Xil_DCacheFlushRange(UINTPTR adr, unsigned long len) {
    xil_cache_mock_record(XIL_CACHE_MOP_FLUSH, adr, len);
}

/* Test-only helpers (not part of the Xilinx API). */
static inline void xil_cache_mock_reset(void) {
    memset(&g_xil_cache_mock, 0, sizeof(g_xil_cache_mock));
}

static inline void xil_cache_mock_set_enabled(int on) {
    g_xil_cache_mock.enabled = on ? 1 : 0;
}

/* One logged op by index; returns 0 past the end of the log. */
static inline int xil_cache_mock_op(unsigned index, int *kind,
                                    uintptr_t *adr, unsigned long *len) {
    if (index >= g_xil_cache_mock.count)
        return 0;
    *kind = g_xil_cache_mock.ops[index].kind;
    *adr = g_xil_cache_mock.ops[index].adr;
    *len = g_xil_cache_mock.ops[index].len;
    return 1;
}

#else /* !XIL_CACHE_MOCK_RECORD */

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

#endif /* XIL_CACHE_MOCK_RECORD */

#endif /* XIL_CACHE_H */
