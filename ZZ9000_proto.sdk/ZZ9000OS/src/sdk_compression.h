/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Compression/decompression helpers for SDK codec services.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_COMPRESSION_H
#define SDK_COMPRESSION_H

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

#endif /* SDK_COMPRESSION_H */
