/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Compression/decompression helpers for SDK codec services.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_COMPRESSION_H
#define SDK_COMPRESSION_H

#include <stddef.h>
#include <stdint.h>
#include "sdk_mailbox.h"

struct SDKDecompressResult {
	uint32_t bytes_consumed;
	uint32_t bytes_written;
	uint32_t checksum;
	uint32_t algorithm;
	uint32_t flags;
};

struct SDKDecompressStreamResult {
	uint32_t session;
	uint32_t bytes_consumed;
	uint32_t bytes_written;
	uint32_t checksum;
	uint32_t algorithm;
	uint32_t flags;
};

uint16_t sdk_decompress_buffer(uint32_t algorithm, uint32_t flags,
                               const uint8_t *src, uint32_t src_length,
                               uint8_t *dst, uint32_t dst_capacity,
                               struct SDKDecompressResult *result);
uint16_t sdk_decompress_test_buffer(uint32_t algorithm, uint32_t flags,
                                    const uint8_t *src,
                                    uint32_t src_length,
                                    uint32_t output_limit,
                                    struct SDKDecompressResult *result);
void sdk_decompress_stream_reset_all(void);
uint16_t sdk_decompress_stream_begin(
	uint32_t algorithm, uint32_t flags,
	const uint8_t *src, uint32_t src_length,
	uint32_t output_limit,
	struct SDKDecompressStreamResult *result);
uint16_t sdk_decompress_stream_feed(
	uint32_t session,
	const uint8_t *src, uint32_t src_length,
	uint32_t flags,
	struct SDKDecompressStreamResult *result);
uint16_t sdk_decompress_stream_read(
	uint32_t session,
	uint8_t *dst, uint32_t dst_capacity,
	struct SDKDecompressStreamResult *result);
uint16_t sdk_decompress_stream_close(uint32_t session);

/*
 * Reclaim card-pool-owned decode working sets after a core-1 cold reset.
 * The pool owner can be dropped without touching newlib metadata. Returns
 * nonzero when one or more blocks were reclaimed.
 */
unsigned sdk_compression_reclaim_core1_decode(void);

/* Decode backend allocations use the reset-reclaimable MEDIA card-pool
 * owner. The historical name is retained for codec callback compatibility. */
void *sdk_decode_heap_alloc(size_t size);
void sdk_decode_heap_free(void *ptr);
void *sdk_decode_heap_realloc(void *ptr, size_t size);
void *sdk_decode_heap_calloc(size_t count, size_t size);

#endif /* SDK_COMPRESSION_H */
