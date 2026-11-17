/*
 * Per-core thread-local storage for firmware code.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CPU_TLS_H
#define CPU_TLS_H

#include <stdint.h>

/* ARM EABI TLS variant 1: TPIDRURO points at an 8-byte thread control
 * block, and the .tdata/.tbss image follows it. */
#define CPU_TLS_TCB_BYTES  8u
#define CPU_TLS_MAX_BYTES  64u
#define CPU_TLS_CPUS       2u

/* Lay out `block` for a TLS image of `tdata_bytes` initialised bytes at
 * `tdata` followed by `tbss_bytes` zero bytes, `tbss_offset` bytes after
 * the image start. Returns the thread pointer, or 0 if it does not fit. */
uintptr_t cpu_tls_layout(uint8_t *block, uint32_t block_bytes,
			 const uint8_t *tdata, uint32_t tdata_bytes,
			 uint32_t tbss_offset, uint32_t tbss_bytes);

/* Give the calling CPU its own TLS block and point TPIDRURO at it.
 * Returns 0 on success. Must run before any code that uses
 * thread-local variables (libjpeg-turbo's SIMD probe does). */
int cpu_tls_init(uint32_t cpu);

#endif
