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

#elif defined(XIL_CACHE_MOCK_MODEL)

/*
 * Write-back cache model (fabric coherent-source test). One registered
 * region has two images: `cpu` is what a load on the compositor's core
 * returns (the producer's fresh PCM when its lines are dirty, or a stale
 * cached copy), `ddr` is what memory and DMA hold. Like the BSP, an
 * invalidate discards the cached copy of every whole 32-byte line in the
 * range, so those lines read back from `ddr`; the unaligned first and last
 * lines are cleaned and invalidated, so they keep their data. A flush
 * cleans the lines it covers. No region attached means no effect.
 */
struct xil_cache_mock_model {
    uint8_t *cpu;
    uint8_t *ddr;
    unsigned long len;
};

extern struct xil_cache_mock_model g_xil_cache_model;

#define XIL_CACHE_MOCK_LINE 32UL

static inline void xil_cache_model_attach(uint8_t *cpu, uint8_t *ddr,
                                          unsigned long len) {
    g_xil_cache_model.cpu = cpu;
    g_xil_cache_model.ddr = ddr;
    g_xil_cache_model.len = len;
}

static inline void xil_cache_model_detach(void) {
    g_xil_cache_model.cpu = NULL;
    g_xil_cache_model.ddr = NULL;
    g_xil_cache_model.len = 0UL;
}

/* Copies [lo, hi) of the registered region: cpu to ddr (clean) or ddr to
 * cpu (the cached copy dropped). */
static inline void xil_cache_model_copy(uintptr_t lo, uintptr_t hi,
                                        int clean) {
    uintptr_t base = (uintptr_t)g_xil_cache_model.cpu;
    uintptr_t end = base + g_xil_cache_model.len;

    if (lo < base)
        lo = base;
    if (hi > end)
        hi = end;
    if (lo >= hi)
        return;
    if (clean)
        memcpy(g_xil_cache_model.ddr + (lo - base),
               g_xil_cache_model.cpu + (lo - base), hi - lo);
    else
        memcpy(g_xil_cache_model.cpu + (lo - base),
               g_xil_cache_model.ddr + (lo - base), hi - lo);
}

static inline void Xil_DCacheInvalidateRange(INTPTR adr, unsigned long len) {
    uintptr_t lo = (uintptr_t)adr;
    uintptr_t hi = lo + len;

    if (g_xil_cache_model.cpu == NULL || len == 0UL)
        return;
    if ((lo & (XIL_CACHE_MOCK_LINE - 1UL)) != 0UL) {
        uintptr_t line = lo & ~(XIL_CACHE_MOCK_LINE - 1UL);

        xil_cache_model_copy(line, line + XIL_CACHE_MOCK_LINE, 1);
        lo = line + XIL_CACHE_MOCK_LINE;
    }
    if ((hi & (XIL_CACHE_MOCK_LINE - 1UL)) != 0UL) {
        uintptr_t line = hi & ~(XIL_CACHE_MOCK_LINE - 1UL);

        xil_cache_model_copy(line, line + XIL_CACHE_MOCK_LINE, 1);
        hi = line;
    }
    if (lo < hi)
        xil_cache_model_copy(lo, hi, 0);
}

static inline void Xil_DCacheFlushRange(UINTPTR adr, unsigned long len) {
    if (g_xil_cache_model.cpu == NULL || len == 0UL)
        return;
    xil_cache_model_copy(adr & ~(XIL_CACHE_MOCK_LINE - 1UL),
                         (adr + len + XIL_CACHE_MOCK_LINE - 1UL) &
                         ~(XIL_CACHE_MOCK_LINE - 1UL), 1);
}

#else /* !XIL_CACHE_MOCK_RECORD && !XIL_CACHE_MOCK_MODEL */

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

#endif /* XIL_CACHE_MOCK_RECORD / XIL_CACHE_MOCK_MODEL */

#endif /* XIL_CACHE_H */
