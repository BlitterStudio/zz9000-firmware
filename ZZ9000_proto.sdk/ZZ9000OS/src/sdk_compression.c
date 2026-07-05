/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * zlib-backed SDK codec service helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>
#include "sdk_compression.h"
#include "sdk_smp_lock.h"
#include "sdk_decode_reclaim.h"
#include "Lzma2Dec.h"
#include "LzmaDec.h"
#include "zlib.h"

#define SDK_DECOMPRESS_TEST_CHUNK 32768U
#define SDK_DECOMPRESS_STREAM_SESSIONS 4U
#define SDK_LZMA2_PROPS_SIZE 1U

static uint8_t decompress_test_scratch[SDK_DECOMPRESS_TEST_CHUNK];

struct SDKDecompressStreamSession {
	uint32_t id;
	const uint8_t *src;
	uint32_t src_length;
	uint32_t compressed_size;
	uint32_t total_in;
	uint32_t total_out;
	uint32_t target_size;
	uint32_t output_limit;
	uint32_t algorithm;
	uint32_t flags;
	uLong crc;
	int has_known_size;
	int finished;
	int allocated;
	int feed_mode;
	int initialized;
	const uint8_t *pending_src;
	uint32_t pending_length;
	uint32_t pending_offset;
	uint32_t header_bytes;
	uint8_t header[LZMA_PROPS_SIZE + 8U];
	int feed_eof;
	CLzmaDec lzma;
	z_stream zlib;
	int zlib_mode;
	int zlib_initialized;
	int lzma2_mode;
	CLzma2Dec lzma2;
};

static struct SDKDecompressStreamSession decompress_streams[
	SDK_DECOMPRESS_STREAM_SESSIONS
];
static uint32_t next_decompress_stream_id = 1U;

/*
 * Heap wrappers for the decode backends. On core 1 the block is recorded (and
 * the table cleaned to DRAM via sdk_decode_flush_table) so core 0 can reclaim
 * it if the worker is cold-reset mid-decode; on core 0 (inline fallback, and
 * the stream/test paths, which never run on core 1) they are plain malloc/free
 * because core 0 is never force-reset. Ordering is deliberate -- track AFTER a
 * successful malloc, untrack-then-clean BEFORE free -- so a worker halted at any
 * point leaks at most one block and never exposes a freed pointer to core 0's
 * reclaim. See sdk_decode_reclaim.h.
 */
static void *decode_malloc(size_t size)
{
	void *p = malloc(size);

	if (p && smp_cpu_id() == 1) {
		sdk_decode_track(p);
		sdk_decode_flush_table();
	}
	return p;
}

static void decode_free(void *ptr)
{
	if (ptr && smp_cpu_id() == 1) {
		sdk_decode_untrack(ptr);
		sdk_decode_flush_table();   /* DRAM shows slot cleared before free */
	}
	free(ptr);
}

static void *lzma_alloc(ISzAllocPtr alloc, size_t size)
{
	(void)alloc;
	return decode_malloc(size);
}

static void lzma_free(ISzAllocPtr alloc, void *address)
{
	(void)alloc;
	decode_free(address);
}

static const ISzAlloc lzma_allocator = { lzma_alloc, lzma_free };

/* zlib allocator callbacks routed through the same core-1-tracking wrappers. */
static voidpf zlib_decode_alloc(voidpf opaque, uInt items, uInt size)
{
	(void)opaque;
	return (voidpf)decode_malloc((size_t)items * (size_t)size);
}

static void zlib_decode_free(voidpf opaque, voidpf address)
{
	(void)opaque;
	decode_free((void *)address);
}

/*
 * Reclaim the blocks a killed core-1 decode still held. This closes the leak
 * and, together with the tracking table's clean/invalidate discipline, the
 * table-driven double-free.
 *
 * Residual (known, low-probability): the blocks live in the shared newlib heap,
 * and free() trusts that heap's chunk/arena metadata. That metadata was written
 * by core 1 under g_malloc_lock (ordering barriers, not write-back); if a line
 * of it were still dirty-only in core 1's L1 at the force-reset it would be
 * dropped, and freeing against the stale DRAM view could corrupt the free list.
 * Decode working sets are large, so that metadata is almost always evicted to
 * DRAM long before any reset -- but the only absolute guarantee is a dedicated
 * non-shared decode arena (tracked follow-up). This is a property of core-1
 * heap use under force-reset, not of the reclaim itself.
 */
unsigned sdk_compression_reclaim_core1_decode(void)
{
	unsigned reclaimed;

	/*
	 * Discard any stale copy of the table in core 0's cache so the read sees
	 * core 1's last cleaned-to-DRAM state (invalidate, not flush: core 0 holds
	 * no dirty table lines here -- the previous reclaim ended with a flush --
	 * so nothing of core 1's fresh state is overwritten).
	 */
	sdk_decode_invalidate_table();
	reclaimed = sdk_decode_reclaim(free);
	/*
	 * Push the cleared slots to DRAM before core 1 is released: the restarted
	 * worker boots with an empty cache and must read the emptied table.
	 */
	sdk_decode_flush_table();
	return reclaimed;
}

static void free_decompress_stream(
	struct SDKDecompressStreamSession *stream)
{
	if (!stream)
		return;
	if (stream->zlib_initialized)
		inflateEnd(&stream->zlib);
	if (stream->allocated) {
		if (stream->lzma2_mode)
			Lzma2Dec_Free(&stream->lzma2, &lzma_allocator);
		else
			LzmaDec_Free(&stream->lzma, &lzma_allocator);
	}
	memset(stream, 0, sizeof(*stream));
}

static struct SDKDecompressStreamSession *find_decompress_stream(
	uint32_t session)
{
	uint32_t i;

	if (session == 0U)
		return 0;
	for (i = 0; i < SDK_DECOMPRESS_STREAM_SESSIONS; i++) {
		if (decompress_streams[i].id == session)
			return &decompress_streams[i];
	}
	return 0;
}

static struct SDKDecompressStreamSession *alloc_decompress_stream(void)
{
	uint32_t i;

	for (i = 0; i < SDK_DECOMPRESS_STREAM_SESSIONS; i++) {
		if (decompress_streams[i].id == 0U) {
			uint32_t id = next_decompress_stream_id++;

			if (next_decompress_stream_id == 0U)
				next_decompress_stream_id = 1U;
			memset(&decompress_streams[i], 0,
			       sizeof(decompress_streams[i]));
			decompress_streams[i].id = id;
			return &decompress_streams[i];
		}
	}
	return 0;
}

static int compression_window_bits(uint32_t algorithm)
{
	switch (algorithm) {
	case SDK_COMPRESSION_DEFLATE_RAW:
		return -MAX_WBITS;
	case SDK_COMPRESSION_ZLIB:
		return MAX_WBITS;
	case SDK_COMPRESSION_GZIP:
		return MAX_WBITS + 16;
	default:
		return 0;
	}
}

static int compression_uses_zlib(uint32_t algorithm)
{
	return compression_window_bits(algorithm) != 0;
}

static int compression_uses_lzma(uint32_t algorithm)
{
	return algorithm == SDK_COMPRESSION_LZMA_ALONE ||
	       algorithm == SDK_COMPRESSION_LZMA2;
}

static uint32_t lzma_stream_header_size(
	const struct SDKDecompressStreamSession *stream)
{
	return stream->lzma2_mode ? SDK_LZMA2_PROPS_SIZE :
	       (LZMA_PROPS_SIZE + 8U);
}

static int lzma_alone_size_unknown(const uint8_t *src)
{
	uint32_t i;

	for (i = 0; i < 8U; i++) {
		if (src[5U + i] != 0xffU)
			return 0;
	}
	return 1;
}

static uint32_t lzma_alone_size32(const uint8_t *src, int *fits32)
{
	uint32_t i;
	uint32_t size = 0;

	*fits32 = 1;
	for (i = 0; i < 8U; i++) {
		uint8_t byte = src[5U + i];

		if (i < 4U)
			size |= ((uint32_t)byte) << (i * 8U);
		else if (byte != 0U)
			*fits32 = 0;
	}
	return size;
}

static uint16_t map_lzma_status(SRes rc)
{
	if (rc == SZ_ERROR_MEM)
		return SDK_STATUS_NO_MEMORY;
	if (rc == SZ_ERROR_UNSUPPORTED)
		return SDK_STATUS_UNSUPPORTED;
	if (rc == SZ_ERROR_INPUT_EOF)
		return SDK_STATUS_BAD_REQUEST;
	return SDK_STATUS_BAD_REQUEST;
}

static uint16_t init_lzma_stream_from_header(
	struct SDKDecompressStreamSession *stream,
	const uint8_t *header)
{
	int has_known_size;
	int fits32;
	uint32_t known_size;
	SRes rc;

	has_known_size = !lzma_alone_size_unknown(header);
	known_size = lzma_alone_size32(header, &fits32);
	if (has_known_size && (!fits32 || known_size > stream->output_limit))
		return SDK_STATUS_NO_MEMORY;

	LzmaDec_Construct(&stream->lzma);
	rc = LzmaDec_Allocate(&stream->lzma, header, LZMA_PROPS_SIZE,
	                      &lzma_allocator);
	if (rc != SZ_OK)
		return map_lzma_status(rc);
	LzmaDec_Init(&stream->lzma);
	stream->allocated = 1;
	stream->target_size = has_known_size ? known_size : stream->output_limit;
	stream->has_known_size = has_known_size;
	stream->initialized = 1;
	return SDK_STATUS_OK;
}

static uint16_t init_lzma2_stream_from_prop(
	struct SDKDecompressStreamSession *stream,
	uint8_t prop)
{
	SRes rc;

	Lzma2Dec_Construct(&stream->lzma2);
	rc = Lzma2Dec_Allocate(&stream->lzma2, prop, &lzma_allocator);
	if (rc != SZ_OK)
		return map_lzma_status(rc);
	Lzma2Dec_Init(&stream->lzma2);
	stream->allocated = 1;
	stream->lzma2_mode = 1;
	stream->target_size = stream->output_limit;
	stream->initialized = 1;
	return SDK_STATUS_OK;
}

static uint16_t sdk_decompress_lzma_alone(const uint8_t *src,
                                          uint32_t src_length,
                                          uint8_t *dst,
                                          uint32_t dst_capacity,
                                          struct SDKDecompressResult *result)
{
	SizeT dest_len;
	SizeT source_len;
	ELzmaStatus lzma_status;
	SRes rc;
	int has_known_size;
	int fits32;
	uint32_t known_size;

	if (src_length <= (LZMA_PROPS_SIZE + 8U))
		return SDK_STATUS_BAD_REQUEST;

	has_known_size = !lzma_alone_size_unknown(src);
	known_size = lzma_alone_size32(src, &fits32);
	if (has_known_size && (!fits32 || known_size > dst_capacity))
		return SDK_STATUS_NO_MEMORY;

	dest_len = has_known_size ? (SizeT)known_size : (SizeT)dst_capacity;
	source_len = (SizeT)(src_length - (LZMA_PROPS_SIZE + 8U));
	lzma_status = LZMA_STATUS_NOT_SPECIFIED;
	rc = LzmaDecode(dst, &dest_len,
	                src + LZMA_PROPS_SIZE + 8U, &source_len,
	                src, LZMA_PROPS_SIZE,
	                LZMA_FINISH_END, &lzma_status, &lzma_allocator);

	result->bytes_consumed = (uint32_t)source_len + LZMA_PROPS_SIZE + 8U;
	result->bytes_written = (uint32_t)dest_len;
	result->checksum = (uint32_t)crc32(crc32(0L, Z_NULL, 0),
	                                  dst, (uInt)result->bytes_written);
	result->flags = SDK_DECOMPRESS_RESULT_CHECKSUM_VALID;
	if (rc == SZ_OK &&
	    (lzma_status == LZMA_STATUS_FINISHED_WITH_MARK ||
	     lzma_status == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK)) {
		result->flags |= SDK_DECOMPRESS_RESULT_STREAM_END;
	}

	if (rc == SZ_OK && (result->flags & SDK_DECOMPRESS_RESULT_STREAM_END) != 0U)
		return SDK_STATUS_OK;
	if (rc == SZ_ERROR_MEM)
		return SDK_STATUS_NO_MEMORY;
	if (rc == SZ_ERROR_UNSUPPORTED)
		return SDK_STATUS_UNSUPPORTED;
	if (rc == SZ_ERROR_INPUT_EOF)
		return SDK_STATUS_BAD_REQUEST;
	if (dest_len == dst_capacity && !has_known_size)
		return SDK_STATUS_NO_MEMORY;
	return SDK_STATUS_BAD_REQUEST;
}

static uint16_t sdk_decompress_lzma2(const uint8_t *src,
                                     uint32_t src_length,
                                     uint8_t *dst,
                                     uint32_t dst_capacity,
                                     struct SDKDecompressResult *result)
{
	SizeT dest_len;
	SizeT source_len;
	ELzmaStatus lzma_status;
	SRes rc;

	if (src_length <= SDK_LZMA2_PROPS_SIZE)
		return SDK_STATUS_BAD_REQUEST;

	dest_len = (SizeT)dst_capacity;
	source_len = (SizeT)(src_length - SDK_LZMA2_PROPS_SIZE);
	lzma_status = LZMA_STATUS_NOT_SPECIFIED;
	rc = Lzma2Decode(dst, &dest_len,
	                 src + SDK_LZMA2_PROPS_SIZE, &source_len,
	                 src[0], LZMA_FINISH_END, &lzma_status,
	                 &lzma_allocator);

	result->bytes_consumed = (uint32_t)source_len + SDK_LZMA2_PROPS_SIZE;
	result->bytes_written = (uint32_t)dest_len;
	result->checksum = (uint32_t)crc32(crc32(0L, Z_NULL, 0),
	                                  dst, (uInt)result->bytes_written);
	result->flags = SDK_DECOMPRESS_RESULT_CHECKSUM_VALID;
	if (rc == SZ_OK && lzma_status == LZMA_STATUS_FINISHED_WITH_MARK)
		result->flags |= SDK_DECOMPRESS_RESULT_STREAM_END;

	if (rc == SZ_OK && (result->flags & SDK_DECOMPRESS_RESULT_STREAM_END) != 0U)
		return SDK_STATUS_OK;
	if (rc == SZ_ERROR_MEM)
		return SDK_STATUS_NO_MEMORY;
	if (rc == SZ_ERROR_UNSUPPORTED)
		return SDK_STATUS_UNSUPPORTED;
	if (rc == SZ_ERROR_INPUT_EOF)
		return SDK_STATUS_BAD_REQUEST;
	if (dest_len == dst_capacity)
		return SDK_STATUS_NO_MEMORY;
	return SDK_STATUS_BAD_REQUEST;
}

static uint16_t sdk_decompress_lzma_alone_test(
	const uint8_t *src, uint32_t src_length, uint32_t output_limit,
	struct SDKDecompressResult *result)
{
	CLzmaDec state;
	ELzmaStatus lzma_status;
	SRes rc;
	uLong crc;
	uint32_t total_in;
	uint32_t total_out;
	uint32_t compressed_size;
	uint32_t target_size;
	int has_known_size;
	int fits32;
	uint32_t known_size;
	uint16_t status;

	if (src_length <= (LZMA_PROPS_SIZE + 8U) || output_limit == 0U)
		return SDK_STATUS_BAD_REQUEST;

	has_known_size = !lzma_alone_size_unknown(src);
	known_size = lzma_alone_size32(src, &fits32);
	if (has_known_size && (!fits32 || known_size > output_limit))
		return SDK_STATUS_NO_MEMORY;

	compressed_size = src_length - (LZMA_PROPS_SIZE + 8U);
	target_size = has_known_size ? known_size : output_limit;
	total_in = 0U;
	total_out = 0U;
	crc = crc32(0L, Z_NULL, 0);
	status = SDK_STATUS_BAD_REQUEST;

	LzmaDec_Construct(&state);
	rc = LzmaDec_Allocate(&state, src, LZMA_PROPS_SIZE,
	                      &lzma_allocator);
	if (rc != SZ_OK)
		return map_lzma_status(rc);
	LzmaDec_Init(&state);

	while (1) {
		SizeT dest_len;
		SizeT source_len;
		uint32_t remaining_out;
		ELzmaFinishMode finish_mode;

		if (total_out >= target_size) {
			status = SDK_STATUS_NO_MEMORY;
			break;
		}

		remaining_out = target_size - total_out;
		dest_len = remaining_out < SDK_DECOMPRESS_TEST_CHUNK ?
			(SizeT)remaining_out : (SizeT)SDK_DECOMPRESS_TEST_CHUNK;
		source_len = (SizeT)(compressed_size - total_in);
		finish_mode = (has_known_size && dest_len == remaining_out) ?
			LZMA_FINISH_END : LZMA_FINISH_ANY;
		lzma_status = LZMA_STATUS_NOT_SPECIFIED;

		rc = LzmaDec_DecodeToBuf(
			&state, decompress_test_scratch, &dest_len,
			src + LZMA_PROPS_SIZE + 8U + total_in, &source_len,
			finish_mode, &lzma_status);
		total_in += (uint32_t)source_len;
		total_out += (uint32_t)dest_len;
		if (dest_len != 0U) {
			crc = crc32(crc, decompress_test_scratch, (uInt)dest_len);
		}

		if (rc != SZ_OK) {
			status = map_lzma_status(rc);
			break;
		}
		if (lzma_status == LZMA_STATUS_FINISHED_WITH_MARK ||
		    (has_known_size && total_out == known_size &&
		     lzma_status == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK)) {
			status = SDK_STATUS_OK;
			break;
		}
		if (has_known_size && total_out == known_size) {
			status = SDK_STATUS_BAD_REQUEST;
			break;
		}
		if (!has_known_size && total_out == output_limit) {
			status = SDK_STATUS_NO_MEMORY;
			break;
		}
		if (lzma_status == LZMA_STATUS_NEEDS_MORE_INPUT &&
		    total_in >= compressed_size) {
			status = SDK_STATUS_BAD_REQUEST;
			break;
		}
		if (source_len == 0U && dest_len == 0U) {
			status = SDK_STATUS_BAD_REQUEST;
			break;
		}
	}

	LzmaDec_Free(&state, &lzma_allocator);
	if (status != SDK_STATUS_OK)
		return status;

	result->bytes_consumed = total_in + LZMA_PROPS_SIZE + 8U;
	result->bytes_written = total_out;
	result->checksum = (uint32_t)crc;
	result->algorithm = SDK_COMPRESSION_LZMA_ALONE;
	result->flags = SDK_DECOMPRESS_RESULT_CHECKSUM_VALID |
	                SDK_DECOMPRESS_RESULT_STREAM_END;
	return SDK_STATUS_OK;
}

static uint16_t sdk_decompress_lzma2_test(
	const uint8_t *src, uint32_t src_length, uint32_t output_limit,
	struct SDKDecompressResult *result)
{
	CLzma2Dec state;
	ELzmaStatus lzma_status;
	SRes rc;
	uLong crc;
	uint32_t total_in;
	uint32_t total_out;
	uint32_t compressed_size;
	uint16_t status;

	if (src_length <= SDK_LZMA2_PROPS_SIZE || output_limit == 0U)
		return SDK_STATUS_BAD_REQUEST;

	compressed_size = src_length - SDK_LZMA2_PROPS_SIZE;
	total_in = 0U;
	total_out = 0U;
	crc = crc32(0L, Z_NULL, 0);
	status = SDK_STATUS_BAD_REQUEST;

	Lzma2Dec_Construct(&state);
	rc = Lzma2Dec_Allocate(&state, src[0], &lzma_allocator);
	if (rc != SZ_OK)
		return map_lzma_status(rc);
	Lzma2Dec_Init(&state);

	while (1) {
		SizeT dest_len;
		SizeT source_len;
		uint32_t remaining_out;

		if (total_out >= output_limit) {
			status = SDK_STATUS_NO_MEMORY;
			break;
		}

		remaining_out = output_limit - total_out;
		dest_len = remaining_out < SDK_DECOMPRESS_TEST_CHUNK ?
			(SizeT)remaining_out : (SizeT)SDK_DECOMPRESS_TEST_CHUNK;
		source_len = (SizeT)(compressed_size - total_in);
		lzma_status = LZMA_STATUS_NOT_SPECIFIED;

		rc = Lzma2Dec_DecodeToBuf(
			&state, decompress_test_scratch, &dest_len,
			src + SDK_LZMA2_PROPS_SIZE + total_in, &source_len,
			LZMA_FINISH_ANY, &lzma_status);
		total_in += (uint32_t)source_len;
		total_out += (uint32_t)dest_len;
		if (dest_len != 0U)
			crc = crc32(crc, decompress_test_scratch,
			            (uInt)dest_len);

		if (rc != SZ_OK) {
			status = map_lzma_status(rc);
			break;
		}
		if (lzma_status == LZMA_STATUS_FINISHED_WITH_MARK) {
			status = SDK_STATUS_OK;
			break;
		}
		if (total_out == output_limit) {
			status = SDK_STATUS_NO_MEMORY;
			break;
		}
		if (lzma_status == LZMA_STATUS_NEEDS_MORE_INPUT &&
		    total_in >= compressed_size) {
			status = SDK_STATUS_BAD_REQUEST;
			break;
		}
		if (source_len == 0U && dest_len == 0U) {
			status = SDK_STATUS_BAD_REQUEST;
			break;
		}
	}

	Lzma2Dec_Free(&state, &lzma_allocator);
	if (status != SDK_STATUS_OK)
		return status;

	result->bytes_consumed = total_in + SDK_LZMA2_PROPS_SIZE;
	result->bytes_written = total_out;
	result->checksum = (uint32_t)crc;
	result->algorithm = SDK_COMPRESSION_LZMA2;
	result->flags = SDK_DECOMPRESS_RESULT_CHECKSUM_VALID |
	                SDK_DECOMPRESS_RESULT_STREAM_END;
	return SDK_STATUS_OK;
}

void sdk_decompress_stream_reset_all(void)
{
	uint32_t i;

	for (i = 0; i < SDK_DECOMPRESS_STREAM_SESSIONS; i++)
		free_decompress_stream(&decompress_streams[i]);
	next_decompress_stream_id = 1U;
}

uint16_t sdk_decompress_stream_begin(
	uint32_t algorithm, uint32_t flags,
	const uint8_t *src, uint32_t src_length,
	uint32_t output_limit,
	struct SDKDecompressStreamResult *result)
{
	struct SDKDecompressStreamSession *stream;
	uint32_t allowed_flags;
	int feed_mode;
	uint16_t status;

	allowed_flags = SDK_DECOMPRESS_FLAG_EXPECT_END |
	                SDK_DECOMPRESS_FLAG_FEED_INPUT;
	feed_mode = (flags & SDK_DECOMPRESS_FLAG_FEED_INPUT) != 0U;
	if (!result || output_limit == 0U) {
		return SDK_STATUS_BAD_REQUEST;
	}
	if ((flags & ~allowed_flags) != 0U)
		return SDK_STATUS_UNSUPPORTED;
	if (!compression_uses_lzma(algorithm) && !compression_uses_zlib(algorithm))
		return SDK_STATUS_UNSUPPORTED;
	if (feed_mode) {
		if (src || src_length != 0U)
			return SDK_STATUS_BAD_REQUEST;
	} else if (!src || src_length == 0U) {
		return SDK_STATUS_BAD_REQUEST;
	}
	if (algorithm == SDK_COMPRESSION_LZMA_ALONE && !feed_mode &&
	    src_length <= (LZMA_PROPS_SIZE + 8U))
		return SDK_STATUS_BAD_REQUEST;
	if (algorithm == SDK_COMPRESSION_LZMA2 && !feed_mode &&
	    src_length <= SDK_LZMA2_PROPS_SIZE)
		return SDK_STATUS_BAD_REQUEST;

	stream = alloc_decompress_stream();
	if (!stream)
		return SDK_STATUS_BUSY;

	stream->src_length = src_length;
	stream->output_limit = output_limit;
	stream->algorithm = algorithm;
	stream->flags = flags;
	stream->crc = crc32(0L, Z_NULL, 0);
	stream->feed_mode = feed_mode;
	if (algorithm == SDK_COMPRESSION_LZMA_ALONE && feed_mode) {
		stream->target_size = output_limit;
	} else if (algorithm == SDK_COMPRESSION_LZMA2 && feed_mode) {
		stream->target_size = output_limit;
	} else if (algorithm == SDK_COMPRESSION_LZMA_ALONE) {
		status = init_lzma_stream_from_header(stream, src);
		if (status != SDK_STATUS_OK) {
			free_decompress_stream(stream);
			return status;
		}
		stream->src = src + LZMA_PROPS_SIZE + 8U;
		stream->compressed_size = src_length - (LZMA_PROPS_SIZE + 8U);
	} else if (algorithm == SDK_COMPRESSION_LZMA2) {
		status = init_lzma2_stream_from_prop(stream, src[0]);
		if (status != SDK_STATUS_OK) {
			free_decompress_stream(stream);
			return status;
		}
		stream->src = src + SDK_LZMA2_PROPS_SIZE;
		stream->compressed_size = src_length - SDK_LZMA2_PROPS_SIZE;
	} else {
		int rc;
		int window_bits = compression_window_bits(algorithm);

		stream->zlib_mode = 1;
		stream->target_size = output_limit;
		if (!feed_mode) {
			stream->src = src;
			stream->compressed_size = src_length;
		}
		memset(&stream->zlib, 0, sizeof(stream->zlib));
		rc = inflateInit2(&stream->zlib, window_bits);
		if (rc != Z_OK) {
			free_decompress_stream(stream);
			return SDK_STATUS_INTERNAL_ERROR;
		}
		stream->zlib_initialized = 1;
		stream->initialized = 1;
	}

	memset(result, 0, sizeof(*result));
	result->session = stream->id;
	result->algorithm = algorithm;
	return SDK_STATUS_OK;
}

uint16_t sdk_decompress_stream_feed(
	uint32_t session,
	const uint8_t *src, uint32_t src_length,
	uint32_t flags,
	struct SDKDecompressStreamResult *result)
{
	struct SDKDecompressStreamSession *stream;
	uint32_t pos;
	uint16_t status;

	if (!src || !result || src_length == 0U ||
	    (flags & ~SDK_DECOMPRESS_STREAM_FEED_EOF) != 0U) {
		return SDK_STATUS_BAD_REQUEST;
	}

	stream = find_decompress_stream(session);
	if (!stream)
		return SDK_STATUS_BAD_HANDLE;
	if (!stream->feed_mode || stream->finished ||
	    stream->pending_src || stream->pending_offset != 0U) {
		return SDK_STATUS_BAD_REQUEST;
	}

	if (stream->zlib_mode) {
		stream->pending_src = src;
		stream->pending_length = src_length;
		stream->pending_offset = 0U;
		if ((flags & SDK_DECOMPRESS_STREAM_FEED_EOF) != 0U)
			stream->feed_eof = 1;

		memset(result, 0, sizeof(*result));
		result->session = stream->id;
		result->bytes_consumed = stream->total_in;
		result->checksum = (uint32_t)stream->crc;
		result->algorithm = stream->algorithm;
		result->flags = SDK_DECOMPRESS_RESULT_NEED_INPUT;
		return SDK_STATUS_OK;
	}

	pos = 0U;
	if (!stream->initialized) {
		uint32_t header_size = stream->algorithm == SDK_COMPRESSION_LZMA2 ?
			SDK_LZMA2_PROPS_SIZE : (LZMA_PROPS_SIZE + 8U);

		while (stream->header_bytes < header_size && pos < src_length) {
			stream->header[stream->header_bytes++] = src[pos++];
		}
		if (stream->header_bytes == header_size) {
			if (stream->algorithm == SDK_COMPRESSION_LZMA2)
				status = init_lzma2_stream_from_prop(
					stream, stream->header[0]);
			else
				status = init_lzma_stream_from_header(
					stream, stream->header);
			if (status != SDK_STATUS_OK) {
				free_decompress_stream(stream);
				return status;
			}
		} else if ((flags & SDK_DECOMPRESS_STREAM_FEED_EOF) != 0U) {
			free_decompress_stream(stream);
			return SDK_STATUS_BAD_REQUEST;
		}
	}

	if (pos < src_length) {
		stream->pending_src = src + pos;
		stream->pending_length = src_length - pos;
		stream->pending_offset = 0U;
	}
	if ((flags & SDK_DECOMPRESS_STREAM_FEED_EOF) != 0U)
		stream->feed_eof = 1;

	memset(result, 0, sizeof(*result));
	result->session = stream->id;
	result->bytes_consumed = stream->header_bytes + stream->total_in;
	result->checksum = (uint32_t)stream->crc;
	result->algorithm = stream->algorithm;
	result->flags = SDK_DECOMPRESS_RESULT_NEED_INPUT;
	return SDK_STATUS_OK;
}

static uint16_t sdk_decompress_zlib_stream_read(
	struct SDKDecompressStreamSession *stream,
	uint8_t *dst, uint32_t dst_capacity,
	struct SDKDecompressStreamResult *result)
{
	uint32_t remaining_out;
	uInt source_before;
	uInt output_before;
	uInt consumed;
	uInt produced;
	int rc;
	uint16_t status;

	if (stream->total_out >= stream->target_size)
		return SDK_STATUS_NO_MEMORY;
	if (stream->feed_mode && !stream->pending_src) {
		memset(result, 0, sizeof(*result));
		result->session = stream->id;
		result->bytes_consumed = stream->total_in;
		result->checksum = (uint32_t)stream->crc;
		result->algorithm = stream->algorithm;
		if (!stream->feed_eof) {
			result->flags = SDK_DECOMPRESS_RESULT_NEED_INPUT;
			return SDK_STATUS_OK;
		}
		free_decompress_stream(stream);
		return SDK_STATUS_BAD_REQUEST;
	}

	remaining_out = stream->target_size - stream->total_out;
	if (dst_capacity > remaining_out)
		dst_capacity = remaining_out;

	if (stream->feed_mode) {
		stream->zlib.next_in =
			(Bytef *)(stream->pending_src + stream->pending_offset);
		stream->zlib.avail_in =
			(uInt)(stream->pending_length -
			       stream->pending_offset);
	} else {
		stream->zlib.next_in = (Bytef *)(stream->src + stream->total_in);
		stream->zlib.avail_in =
			(uInt)(stream->compressed_size - stream->total_in);
	}
	stream->zlib.next_out = dst;
	stream->zlib.avail_out = (uInt)dst_capacity;
	source_before = stream->zlib.avail_in;
	output_before = stream->zlib.avail_out;

	rc = inflate(&stream->zlib, Z_NO_FLUSH);
	consumed = source_before - stream->zlib.avail_in;
	produced = output_before - stream->zlib.avail_out;

	if (stream->feed_mode) {
		stream->pending_offset += (uint32_t)consumed;
		if (stream->pending_offset >= stream->pending_length) {
			stream->pending_src = 0;
			stream->pending_length = 0U;
			stream->pending_offset = 0U;
		}
	}
	stream->total_in = (uint32_t)stream->zlib.total_in;
	stream->total_out = (uint32_t)stream->zlib.total_out;
	if (produced != 0U)
		stream->crc = crc32(stream->crc, dst, produced);

	memset(result, 0, sizeof(*result));
	result->session = stream->id;
	result->bytes_consumed = stream->total_in;
	result->bytes_written = (uint32_t)produced;
	result->checksum = (uint32_t)stream->crc;
	result->algorithm = stream->algorithm;

	status = SDK_STATUS_OK;
	if (rc == Z_STREAM_END) {
		stream->finished = 1;
		result->flags = SDK_DECOMPRESS_RESULT_CHECKSUM_VALID |
		                SDK_DECOMPRESS_RESULT_STREAM_END;
		return SDK_STATUS_OK;
	}
	if (rc == Z_MEM_ERROR) {
		status = SDK_STATUS_NO_MEMORY;
	} else if (rc == Z_BUF_ERROR) {
		if (stream->feed_mode && !stream->pending_src &&
		    !stream->feed_eof) {
			result->flags = SDK_DECOMPRESS_RESULT_NEED_INPUT;
			return SDK_STATUS_OK;
		}
		status = SDK_STATUS_BAD_REQUEST;
	} else if (rc != Z_OK) {
		status = SDK_STATUS_BAD_REQUEST;
	} else if (stream->total_out >= stream->target_size) {
		status = SDK_STATUS_NO_MEMORY;
	} else if (stream->feed_mode && !stream->pending_src &&
	           !stream->feed_eof) {
		result->flags = SDK_DECOMPRESS_RESULT_NEED_INPUT;
	} else if (!stream->feed_mode && stream->total_in >=
	           stream->compressed_size && produced == 0U) {
		status = SDK_STATUS_BAD_REQUEST;
	} else if (consumed == 0U && produced == 0U) {
		if (stream->feed_mode && !stream->feed_eof)
			result->flags = SDK_DECOMPRESS_RESULT_NEED_INPUT;
		else
			status = SDK_STATUS_BAD_REQUEST;
	}

	if (status != SDK_STATUS_OK)
		free_decompress_stream(stream);
	return status;
}

uint16_t sdk_decompress_stream_read(
	uint32_t session,
	uint8_t *dst, uint32_t dst_capacity,
	struct SDKDecompressStreamResult *result)
{
	struct SDKDecompressStreamSession *stream;
	ELzmaStatus lzma_status;
	ELzmaFinishMode finish_mode;
	SizeT dest_len;
	SizeT source_len;
	uint32_t remaining_out;
	uint32_t header_size;
	SRes rc;
	uint16_t status;

	if (!dst || !result || dst_capacity == 0U)
		return SDK_STATUS_BAD_REQUEST;

	stream = find_decompress_stream(session);
	if (!stream)
		return SDK_STATUS_BAD_HANDLE;
	if (stream->finished)
		return SDK_STATUS_BAD_REQUEST;
	if (stream->zlib_mode)
		return sdk_decompress_zlib_stream_read(
			stream, dst, dst_capacity, result);
	header_size = lzma_stream_header_size(stream);
	if (!stream->initialized) {
		memset(result, 0, sizeof(*result));
		result->session = stream->id;
		result->algorithm = stream->algorithm;
		result->flags = SDK_DECOMPRESS_RESULT_NEED_INPUT;
		return SDK_STATUS_OK;
	}
	if (stream->total_out >= stream->target_size)
		return SDK_STATUS_NO_MEMORY;
	if (stream->feed_mode && !stream->pending_src) {
		memset(result, 0, sizeof(*result));
		result->session = stream->id;
		result->bytes_consumed = stream->header_bytes + stream->total_in;
		result->checksum = (uint32_t)stream->crc;
		result->algorithm = stream->algorithm;
		if (!stream->feed_eof) {
			result->flags = SDK_DECOMPRESS_RESULT_NEED_INPUT;
			return SDK_STATUS_OK;
		}
		free_decompress_stream(stream);
		return SDK_STATUS_BAD_REQUEST;
	}

	remaining_out = stream->target_size - stream->total_out;
	dest_len = remaining_out < dst_capacity ?
		(SizeT)remaining_out : (SizeT)dst_capacity;
	if (stream->feed_mode) {
		source_len = (SizeT)(stream->pending_length -
		                     stream->pending_offset);
	} else {
		source_len = (SizeT)(stream->compressed_size -
		                     stream->total_in);
	}
	finish_mode = (!stream->lzma2_mode && stream->has_known_size &&
	               dest_len == remaining_out) ?
		LZMA_FINISH_END : LZMA_FINISH_ANY;
	lzma_status = LZMA_STATUS_NOT_SPECIFIED;

	if (stream->lzma2_mode) {
		rc = Lzma2Dec_DecodeToBuf(
			&stream->lzma2, dst, &dest_len,
			stream->feed_mode ?
				stream->pending_src + stream->pending_offset :
				stream->src + stream->total_in,
			&source_len, finish_mode, &lzma_status);
	} else {
		rc = LzmaDec_DecodeToBuf(
			&stream->lzma, dst, &dest_len,
			stream->feed_mode ?
				stream->pending_src + stream->pending_offset :
				stream->src + stream->total_in,
			&source_len, finish_mode, &lzma_status);
	}
	if (stream->feed_mode) {
		stream->pending_offset += (uint32_t)source_len;
		if (stream->pending_offset >= stream->pending_length) {
			stream->pending_src = 0;
			stream->pending_length = 0U;
			stream->pending_offset = 0U;
		}
	}
	stream->total_in += (uint32_t)source_len;
	stream->total_out += (uint32_t)dest_len;
	if (dest_len != 0U)
		stream->crc = crc32(stream->crc, dst, (uInt)dest_len);

	memset(result, 0, sizeof(*result));
	result->session = stream->id;
	result->bytes_consumed = stream->total_in + header_size;
	result->bytes_written = (uint32_t)dest_len;
	result->checksum = (uint32_t)stream->crc;
	result->algorithm = stream->algorithm;

	status = SDK_STATUS_OK;
	if (rc != SZ_OK) {
		status = map_lzma_status(rc);
	} else if (lzma_status == LZMA_STATUS_FINISHED_WITH_MARK ||
	           (stream->has_known_size &&
	            stream->total_out == stream->target_size &&
	            lzma_status ==
	            LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK)) {
		stream->finished = 1;
		result->flags = SDK_DECOMPRESS_RESULT_CHECKSUM_VALID |
		                SDK_DECOMPRESS_RESULT_STREAM_END;
	} else if (stream->has_known_size &&
	           stream->total_out == stream->target_size) {
		status = SDK_STATUS_BAD_REQUEST;
	} else if (!stream->has_known_size &&
	           stream->total_out == stream->target_size) {
		status = SDK_STATUS_NO_MEMORY;
	} else if (lzma_status == LZMA_STATUS_NEEDS_MORE_INPUT &&
	           (!stream->feed_mode ||
	            (!stream->pending_src && stream->feed_eof))) {
		status = SDK_STATUS_BAD_REQUEST;
	} else if (stream->feed_mode && !stream->pending_src &&
	           !stream->feed_eof) {
		result->flags = SDK_DECOMPRESS_RESULT_NEED_INPUT;
	} else if (source_len == 0U && dest_len == 0U) {
		if (stream->feed_mode && !stream->feed_eof)
			result->flags = SDK_DECOMPRESS_RESULT_NEED_INPUT;
		else
			status = SDK_STATUS_BAD_REQUEST;
	}

	if (status != SDK_STATUS_OK)
		free_decompress_stream(stream);
	return status;
}

uint16_t sdk_decompress_stream_close(uint32_t session)
{
	struct SDKDecompressStreamSession *stream;

	stream = find_decompress_stream(session);
	if (!stream)
		return SDK_STATUS_BAD_HANDLE;
	free_decompress_stream(stream);
	return SDK_STATUS_OK;
}

uint16_t sdk_decompress_buffer(uint32_t algorithm, uint32_t flags,
                               const uint8_t *src, uint32_t src_length,
                               uint8_t *dst, uint32_t dst_capacity,
                               struct SDKDecompressResult *result)
{
	z_stream stream;
	int window_bits;
	int rc;

	if (!src || !dst || !result || src_length == 0U ||
	    dst_capacity == 0U) {
		return SDK_STATUS_BAD_REQUEST;
	}
	if ((flags & ~SDK_DECOMPRESS_FLAG_EXPECT_END) != 0U)
		return SDK_STATUS_UNSUPPORTED;

	memset(result, 0, sizeof(*result));
	result->algorithm = algorithm;
	if (algorithm == SDK_COMPRESSION_LZMA_ALONE)
		return sdk_decompress_lzma_alone(src, src_length, dst,
		                                 dst_capacity, result);
	if (algorithm == SDK_COMPRESSION_LZMA2)
		return sdk_decompress_lzma2(src, src_length, dst,
		                            dst_capacity, result);

	window_bits = compression_window_bits(algorithm);
	if (window_bits == 0)
		return SDK_STATUS_UNSUPPORTED;

	memset(&stream, 0, sizeof(stream));
	/* Track this decode's heap use so a core-1 cold-reset mid-inflate does not
	 * leak the inflate state/window (see sdk_decode_reclaim.h). */
	stream.zalloc = zlib_decode_alloc;
	stream.zfree = zlib_decode_free;
	stream.opaque = Z_NULL;
	stream.next_in = (Bytef *)src;
	stream.avail_in = (uInt)src_length;
	stream.next_out = dst;
	stream.avail_out = (uInt)dst_capacity;

	rc = inflateInit2(&stream, window_bits);
	if (rc != Z_OK)
		return SDK_STATUS_INTERNAL_ERROR;

	rc = inflate(&stream, Z_FINISH);
	result->bytes_consumed = (uint32_t)stream.total_in;
	result->bytes_written = (uint32_t)stream.total_out;
	result->checksum = (uint32_t)crc32(crc32(0L, Z_NULL, 0),
	                                  dst,
	                                  (uInt)result->bytes_written);
	result->flags = SDK_DECOMPRESS_RESULT_CHECKSUM_VALID;
	if (rc == Z_STREAM_END)
		result->flags |= SDK_DECOMPRESS_RESULT_STREAM_END;

	inflateEnd(&stream);

	if (rc == Z_STREAM_END)
		return SDK_STATUS_OK;
	if (stream.avail_out == 0U)
		return SDK_STATUS_NO_MEMORY;
	if ((flags & SDK_DECOMPRESS_FLAG_EXPECT_END) != 0U)
		return SDK_STATUS_BAD_REQUEST;
	if (rc == Z_OK)
		return SDK_STATUS_OK;
	if (rc == Z_BUF_ERROR)
		return SDK_STATUS_BAD_REQUEST;
	return SDK_STATUS_BAD_REQUEST;
}

uint16_t sdk_decompress_test_buffer(uint32_t algorithm, uint32_t flags,
                                    const uint8_t *src,
                                    uint32_t src_length,
                                    uint32_t output_limit,
                                    struct SDKDecompressResult *result)
{
	z_stream stream;
	uLong crc;
	int window_bits;
	int rc;

	if (!src || !result || src_length == 0U || output_limit == 0U) {
		return SDK_STATUS_BAD_REQUEST;
	}
	if ((flags & ~SDK_DECOMPRESS_FLAG_EXPECT_END) != 0U)
		return SDK_STATUS_UNSUPPORTED;

	memset(result, 0, sizeof(*result));
	result->algorithm = algorithm;
	if (algorithm == SDK_COMPRESSION_LZMA_ALONE)
		return sdk_decompress_lzma_alone_test(src, src_length,
		                                      output_limit, result);
	if (algorithm == SDK_COMPRESSION_LZMA2)
		return sdk_decompress_lzma2_test(src, src_length,
		                                 output_limit, result);

	window_bits = compression_window_bits(algorithm);
	if (window_bits == 0)
		return SDK_STATUS_UNSUPPORTED;

	memset(&stream, 0, sizeof(stream));
	stream.next_in = (Bytef *)src;
	stream.avail_in = (uInt)src_length;

	rc = inflateInit2(&stream, window_bits);
	if (rc != Z_OK)
		return SDK_STATUS_INTERNAL_ERROR;

	crc = crc32(0L, Z_NULL, 0);
	while (1) {
		uInt chunk;
		uInt produced;

		if (stream.total_out >= output_limit) {
			inflateEnd(&stream);
			return SDK_STATUS_NO_MEMORY;
		}
		chunk = (uInt)(output_limit - stream.total_out);
		if (chunk > SDK_DECOMPRESS_TEST_CHUNK)
			chunk = SDK_DECOMPRESS_TEST_CHUNK;

		stream.next_out = decompress_test_scratch;
		stream.avail_out = chunk;
		rc = inflate(&stream, Z_NO_FLUSH);
		produced = chunk - stream.avail_out;
		if (produced != 0U)
			crc = crc32(crc, decompress_test_scratch, produced);

		if (rc == Z_STREAM_END)
			break;
		if (rc != Z_OK) {
			inflateEnd(&stream);
			return rc == Z_MEM_ERROR ? SDK_STATUS_NO_MEMORY :
				SDK_STATUS_BAD_REQUEST;
		}
		if (stream.avail_in == 0U && produced == 0U) {
			inflateEnd(&stream);
			return SDK_STATUS_BAD_REQUEST;
		}
	}

	result->bytes_consumed = (uint32_t)stream.total_in;
	result->bytes_written = (uint32_t)stream.total_out;
	result->checksum = (uint32_t)crc;
	result->flags = SDK_DECOMPRESS_RESULT_CHECKSUM_VALID |
	                SDK_DECOMPRESS_RESULT_STREAM_END;
	inflateEnd(&stream);
	return SDK_STATUS_OK;
}
