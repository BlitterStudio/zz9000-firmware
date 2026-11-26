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
/* One-shot decompress whose codec scratch is allocated under scratch_owner,
 * so an unwind out of the call (app force-stop) is reclaimed with that
 * owner instead of the shared decode pool. */
uint16_t sdk_decompress_buffer_owned(uint32_t algorithm, uint32_t flags,
                                     const uint8_t *src, uint32_t src_length,
                                     uint8_t *dst, uint32_t dst_capacity,
                                     struct SDKDecompressResult *result,
                                     uint32_t scratch_owner);
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
uint16_t sdk_decompress_stream_begin_owned(
	uint32_t algorithm, uint32_t flags,
	const uint8_t *src, uint32_t src_length, uint32_t output_limit,
	uint16_t owner_id, struct SDKDecompressStreamResult *result);
int sdk_decompress_stream_release_client(uint16_t owner_id);
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
 * MEDIA instances are partitioned: 1 through 0xff are SDK video-session
 * owners; 0x100 through 0x7fffff are app decoder owners; 0x800000 through
 * 0xffffff are ZZM resident-module owners (module ids are small numbers). The
 * shared core-1 decode heap uses 0xfffffe and is reclaimed only on a core-1
 * reset.
 * 0xfffffd is the core-0 audio ring (WHDLoad) decoders: their FIFOs and every
 * backend allocation. It is never released by a core-1 reset, only by the
 * lease that owns it and by an Amiga reset. 0xfffffc is reserved.
 * MEDIA_INPUT has its own card-pool class, so mailbox media buffers are
 * outside this map.
 */
#define SDK_MEDIA_VIDEO_INSTANCE_LIMIT 0x00000100U
#define SDK_MEDIA_APP_INSTANCE_BASE SDK_MEDIA_VIDEO_INSTANCE_LIMIT
#define SDK_MEDIA_APP_INSTANCE_LIMIT 0x00800000U
#define SDK_MEDIA_SHARED_INSTANCE   0x00FFFFFCU

/*
 * The MEDIA owner of an app's decoders, from the 24-bit admission serial of
 * its process record (never 0, never reused between resets). Consecutive
 * serials give distinct owners, all inside the app range above, so it is
 * never a video-session, module or shared-heap owner. Two serials share an
 * owner only if they differ by a multiple of the 0x7fff00 app instances, which
 * needs a record that lived through that many admissions.
 */
static inline uint32_t sdk_compression_app_owner_bits(uint32_t serial)
{
	uint32_t span = SDK_MEDIA_APP_INSTANCE_LIMIT -
			SDK_MEDIA_APP_INSTANCE_BASE;

	return ((uint32_t)2u << 24) |
	       (SDK_MEDIA_APP_INSTANCE_BASE + serial % span);
}

unsigned sdk_compression_reclaim_core1_decode(void);
unsigned sdk_compression_reclaim_app_decode(uint32_t owner);
/* The owner of record `pid`'s decoders, 0 for a pid with no live record. */
uint32_t sdk_compression_app_decode_owner(uint32_t pid);

/* Shared core-1 decode heap. Class 2 is CARD_POOL_CLASS_MEDIA;
 * sdk_compression.c asserts the match. */
#define SDK_DECODE_POOL_OWNER (((uint32_t)2u << 24) | 0x00fffffeu)

/* Decode backend allocations use the reset-reclaimable MEDIA card-pool
 * owner. The historical name is retained for codec callback compatibility. */
void *sdk_decode_heap_alloc(size_t size);
void *sdk_decode_heap_alloc_owned(size_t size, uint32_t owner);
void sdk_decode_heap_free(void *ptr);
void *sdk_decode_heap_realloc(void *ptr, size_t size);
void *sdk_decode_heap_calloc(size_t count, size_t size);

/* Owner of every allocation made for a core-0 audio ring (codec lease)
 * decoder. Distinct from SDK_DECODE_POOL_OWNER so the core-1 reset reclaim
 * does not free a lease that survives it. */
#define SDK_AUDIO_LEASE_DECODE_OWNER (((uint32_t)2u << 24) | 0x00fffffdu)
typedef char sdk_audio_lease_owner_is_reserved[
	(SDK_AUDIO_LEASE_DECODE_OWNER != SDK_DECODE_POOL_OWNER &&
	 (SDK_AUDIO_LEASE_DECODE_OWNER >> 24) == 2u &&
	 (SDK_AUDIO_LEASE_DECODE_OWNER & 0x00ffffffu) >= 0x00800000u &&
	 (SDK_AUDIO_LEASE_DECODE_OWNER & 0x00ffffffu) !=
		SDK_MEDIA_SHARED_INSTANCE) ? 1 : -1];

#endif /* SDK_COMPRESSION_H */
