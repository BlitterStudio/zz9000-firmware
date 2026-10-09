/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FLAC_TEST_HOST_RUNTIME_H
#define FLAC_TEST_HOST_RUNTIME_H
#include <stddef.h>
void host_runtime_reset(void);
void host_runtime_fail_on_allocation(unsigned attempt);
unsigned host_runtime_allocation_attempts(void);
size_t host_runtime_allocated_bytes(void);
size_t host_runtime_peak_allocated_bytes(void);
unsigned host_runtime_live_blocks(void);
void *sdk_decode_heap_alloc(size_t size);
void sdk_decode_heap_free(void *ptr);
#endif
