/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Functional tests for SDK compression helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include "../ZZ9000_proto.sdk/ZZ9000OS/src/sdk_compression.h"

static int make_deflate_stream(int window_bits, const unsigned char *input,
                               unsigned long input_len,
                               unsigned char *output,
                               unsigned long *output_len)
{
	z_stream stream;
	int rc;

	memset(&stream, 0, sizeof(stream));
	rc = deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED,
	                  window_bits, 8, Z_DEFAULT_STRATEGY);
	if (rc != Z_OK)
		return 0;

	stream.next_in = (Bytef *)input;
	stream.avail_in = (uInt)input_len;
	stream.next_out = output;
	stream.avail_out = (uInt)*output_len;
	rc = deflate(&stream, Z_FINISH);
	if (rc != Z_STREAM_END) {
		deflateEnd(&stream);
		return 0;
	}

	*output_len = stream.total_out;
	deflateEnd(&stream);
	return 1;
}

static int check_algorithm(uint32_t algorithm, int window_bits,
                           const unsigned char *plain,
                           unsigned long plain_len)
{
	unsigned char compressed[256];
	unsigned char decoded[256];
	unsigned long compressed_len = sizeof(compressed);
	struct SDKDecompressResult result;
	uint16_t status;
	uint32_t expected_crc;

	if (!make_deflate_stream(window_bits, plain, plain_len, compressed,
	                         &compressed_len)) {
		printf("failed to create compressed test stream\n");
		return 0;
	}

	memset(decoded, 0xa5, sizeof(decoded));
	memset(&result, 0, sizeof(result));
	status = sdk_decompress_buffer(algorithm,
	                               SDK_DECOMPRESS_FLAG_EXPECT_END,
	                               compressed, (uint32_t)compressed_len,
	                               decoded, (uint32_t)sizeof(decoded),
	                               &result);
	if (status != SDK_STATUS_OK) {
		printf("algorithm %lu returned status %u\n",
		       (unsigned long)algorithm, status);
		return 0;
	}
	if (memcmp(decoded, plain, plain_len) != 0) {
		printf("algorithm %lu decoded bytes mismatch\n",
		       (unsigned long)algorithm);
		return 0;
	}

	expected_crc = (uint32_t)crc32(crc32(0L, Z_NULL, 0),
	                              plain, (uInt)plain_len);
	if (result.bytes_consumed != compressed_len ||
	    result.bytes_written != plain_len ||
	    result.algorithm != algorithm ||
	    result.checksum != expected_crc ||
	    (result.flags & SDK_DECOMPRESS_RESULT_STREAM_END) == 0U ||
	    (result.flags & SDK_DECOMPRESS_RESULT_CHECKSUM_VALID) == 0U) {
		printf("algorithm %lu result fields mismatch\n",
		       (unsigned long)algorithm);
		return 0;
	}

	return 1;
}

static int check_lzma_alone(void)
{
	static const unsigned char plain[] =
		"ZZ9000 SDK codec service: repeatable LZMA-alone inflate path "
		"for 7z payloads";
	static const unsigned char compressed[] = {
		0x5d, 0x00, 0x00, 0x80, 0x00, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x2d, 0x60,
		0xe0, 0x61, 0xa0, 0x20, 0x2b, 0x96, 0xa5, 0x04,
		0xea, 0xea, 0x6c, 0x79, 0xa1, 0x71, 0xc5, 0x2a,
		0x91, 0x5a, 0xa9, 0xd5, 0x57, 0x05, 0xc0, 0x51,
		0xec, 0xf7, 0x03, 0x03, 0x05, 0x1b, 0x33, 0x8c,
		0xc9, 0x84, 0x3b, 0x62, 0x0e, 0x29, 0xf7, 0xd2,
		0xb6, 0x4f, 0x44, 0x3e, 0xe5, 0xc7, 0x20, 0x5a,
		0xd2, 0xcd, 0x75, 0xe0, 0xa4, 0x1f, 0x05, 0xbb,
		0x04, 0xa0, 0xeb, 0x52, 0x7f, 0xd7, 0x6f, 0x88,
		0xed, 0x5e, 0xbf, 0x21, 0x17, 0x61, 0x29, 0x42,
		0x23, 0xff, 0xff, 0x4a, 0x35, 0x40, 0x00
	};
	unsigned char decoded[128];
	struct SDKDecompressResult result;
	uint16_t status;

	memset(decoded, 0xa5, sizeof(decoded));
	memset(&result, 0, sizeof(result));
	status = sdk_decompress_buffer(SDK_COMPRESSION_LZMA_ALONE,
	                               SDK_DECOMPRESS_FLAG_EXPECT_END,
	                               compressed,
	                               (uint32_t)sizeof(compressed),
	                               decoded, (uint32_t)sizeof(decoded),
	                               &result);
	if (status != SDK_STATUS_OK) {
		printf("lzma-alone returned status %u\n", status);
		return 0;
	}
	if (memcmp(decoded, plain, sizeof(plain) - 1U) != 0) {
		printf("lzma-alone decoded bytes mismatch\n");
		return 0;
	}
	if (result.bytes_consumed != sizeof(compressed) ||
	    result.bytes_written != sizeof(plain) - 1U ||
	    result.algorithm != SDK_COMPRESSION_LZMA_ALONE ||
	    result.checksum != 0x08f2a7d0U ||
	    (result.flags & SDK_DECOMPRESS_RESULT_STREAM_END) == 0U ||
	    (result.flags & SDK_DECOMPRESS_RESULT_CHECKSUM_VALID) == 0U) {
		printf("lzma-alone result fields mismatch\n");
		return 0;
	}

	return 1;
}

static int check_lzma_feed_stream(void)
{
	static const unsigned char plain[] =
		"ZZ9000 SDK codec service: repeatable LZMA-alone inflate path "
		"for 7z payloads";
	static const unsigned char compressed[] = {
		0x5d, 0x00, 0x00, 0x80, 0x00, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x2d, 0x60,
		0xe0, 0x61, 0xa0, 0x20, 0x2b, 0x96, 0xa5, 0x04,
		0xea, 0xea, 0x6c, 0x79, 0xa1, 0x71, 0xc5, 0x2a,
		0x91, 0x5a, 0xa9, 0xd5, 0x57, 0x05, 0xc0, 0x51,
		0xec, 0xf7, 0x03, 0x03, 0x05, 0x1b, 0x33, 0x8c,
		0xc9, 0x84, 0x3b, 0x62, 0x0e, 0x29, 0xf7, 0xd2,
		0xb6, 0x4f, 0x44, 0x3e, 0xe5, 0xc7, 0x20, 0x5a,
		0xd2, 0xcd, 0x75, 0xe0, 0xa4, 0x1f, 0x05, 0xbb,
		0x04, 0xa0, 0xeb, 0x52, 0x7f, 0xd7, 0x6f, 0x88,
		0xed, 0x5e, 0xbf, 0x21, 0x17, 0x61, 0x29, 0x42,
		0x23, 0xff, 0xff, 0x4a, 0x35, 0x40, 0x00
	};
	unsigned char decoded[128];
	unsigned char chunk[11];
	struct SDKDecompressStreamResult result;
	uint32_t session;
	uint32_t offset;
	uint32_t total;
	uint16_t status;
	int need_input;

	sdk_decompress_stream_reset_all();
	memset(decoded, 0xa5, sizeof(decoded));
	memset(&result, 0, sizeof(result));
	status = sdk_decompress_stream_begin(
		SDK_COMPRESSION_LZMA_ALONE,
		SDK_DECOMPRESS_FLAG_EXPECT_END |
			SDK_DECOMPRESS_FLAG_FEED_INPUT,
		0, 0U, (uint32_t)sizeof(decoded), &result);
	if (status != SDK_STATUS_OK || result.session == 0U) {
		printf("lzma feed begin failed: %u\n", status);
		return 0;
	}
	session = result.session;

	status = sdk_decompress_stream_feed(session, compressed, 13U, 0U,
	                                    &result);
	if (status != SDK_STATUS_OK ||
	    (result.flags & SDK_DECOMPRESS_RESULT_NEED_INPUT) == 0U ||
	    result.bytes_consumed != 13U) {
		printf("lzma feed header failed: status=%u consumed=%lu flags=%lu\n",
		       status, (unsigned long)result.bytes_consumed,
		       (unsigned long)result.flags);
		sdk_decompress_stream_close(session);
		return 0;
	}
	status = sdk_decompress_stream_read(session, chunk,
	                                    (uint32_t)sizeof(chunk), &result);
	if (status != SDK_STATUS_OK ||
	    (result.flags & SDK_DECOMPRESS_RESULT_NEED_INPUT) == 0U ||
	    result.bytes_written != 0U) {
		printf("lzma feed header read failed: status=%u written=%lu flags=%lu\n",
		       status, (unsigned long)result.bytes_written,
		       (unsigned long)result.flags);
		sdk_decompress_stream_close(session);
		return 0;
	}

	offset = 13U;
	total = 0U;
	need_input = 1;
	while (1) {
		if (need_input) {
			uint32_t feed_len;
			uint32_t flags = 0U;

			if (offset >= sizeof(compressed)) {
				printf("lzma feed exhausted input\n");
				sdk_decompress_stream_close(session);
				return 0;
			}
			feed_len = (uint32_t)sizeof(compressed) - offset;
			if (feed_len > 7U)
				feed_len = 7U;
			if (offset + feed_len == sizeof(compressed))
				flags = SDK_DECOMPRESS_STREAM_FEED_EOF;
			status = sdk_decompress_stream_feed(
				session, compressed + offset, feed_len, flags,
				&result);
			if (status != SDK_STATUS_OK) {
				printf("lzma feed chunk failed: status=%u offset=%lu\n",
				       status, (unsigned long)offset);
				return 0;
			}
			offset += feed_len;
			need_input = 0;
		}

		status = sdk_decompress_stream_read(session, chunk,
		                                    (uint32_t)sizeof(chunk),
		                                    &result);
		if (status != SDK_STATUS_OK) {
			printf("lzma feed read failed: status=%u total=%lu\n",
			       status, (unsigned long)total);
			return 0;
		}
		if (result.bytes_written > sizeof(chunk) ||
		    total + result.bytes_written > sizeof(decoded)) {
			printf("lzma feed output overrun\n");
			sdk_decompress_stream_close(session);
			return 0;
		}
		if (result.bytes_written != 0U) {
			memcpy(decoded + total, chunk, result.bytes_written);
			total += result.bytes_written;
		}
		if ((result.flags & SDK_DECOMPRESS_RESULT_STREAM_END) != 0U)
			break;
		if ((result.flags & SDK_DECOMPRESS_RESULT_NEED_INPUT) != 0U)
			need_input = 1;
		else if (result.bytes_written == 0U) {
			printf("lzma feed made no progress\n");
			sdk_decompress_stream_close(session);
			return 0;
		}
	}

	if (sdk_decompress_stream_close(session) != SDK_STATUS_OK) {
		printf("lzma feed close failed\n");
		return 0;
	}
	if (offset != sizeof(compressed) ||
	    total != sizeof(plain) - 1U ||
	    memcmp(decoded, plain, sizeof(plain) - 1U) != 0 ||
	    result.bytes_consumed != sizeof(compressed) ||
	    result.checksum != 0x08f2a7d0U ||
	    (result.flags & SDK_DECOMPRESS_RESULT_CHECKSUM_VALID) == 0U) {
		printf("lzma feed result mismatch\n");
		return 0;
	}

	return 1;
}

static int check_lzma2(void)
{
	static const unsigned char plain[] =
		"ZZ9000 SDK codec service: repeatable LZMA2 inflate path "
		"for ordinary 7z payloads";
	static const unsigned char compressed[] = {
		0x10, 0x01, 0x00, 0x4f, 0x5a, 0x5a, 0x39, 0x30,
		0x30, 0x30, 0x20, 0x53, 0x44, 0x4b, 0x20, 0x63,
		0x6f, 0x64, 0x65, 0x63, 0x20, 0x73, 0x65, 0x72,
		0x76, 0x69, 0x63, 0x65, 0x3a, 0x20, 0x72, 0x65,
		0x70, 0x65, 0x61, 0x74, 0x61, 0x62, 0x6c, 0x65,
		0x20, 0x4c, 0x5a, 0x4d, 0x41, 0x32, 0x20, 0x69,
		0x6e, 0x66, 0x6c, 0x61, 0x74, 0x65, 0x20, 0x70,
		0x61, 0x74, 0x68, 0x20, 0x66, 0x6f, 0x72, 0x20,
		0x6f, 0x72, 0x64, 0x69, 0x6e, 0x61, 0x72, 0x79,
		0x20, 0x37, 0x7a, 0x20, 0x70, 0x61, 0x79, 0x6c,
		0x6f, 0x61, 0x64, 0x73, 0x00
	};
	unsigned char decoded[128];
	struct SDKDecompressResult result;
	uint16_t status;

	memset(decoded, 0xa5, sizeof(decoded));
	memset(&result, 0, sizeof(result));
	status = sdk_decompress_buffer(SDK_COMPRESSION_LZMA2,
	                               SDK_DECOMPRESS_FLAG_EXPECT_END,
	                               compressed,
	                               (uint32_t)sizeof(compressed),
	                               decoded, (uint32_t)sizeof(decoded),
	                               &result);
	if (status != SDK_STATUS_OK) {
		printf("lzma2 returned status %u\n", status);
		return 0;
	}
	if (memcmp(decoded, plain, sizeof(plain) - 1U) != 0) {
		printf("lzma2 decoded bytes mismatch\n");
		return 0;
	}
	if (result.bytes_consumed != sizeof(compressed) ||
	    result.bytes_written != sizeof(plain) - 1U ||
	    result.algorithm != SDK_COMPRESSION_LZMA2 ||
	    result.checksum != 0x2b0dec5aU ||
	    (result.flags & SDK_DECOMPRESS_RESULT_STREAM_END) == 0U ||
	    (result.flags & SDK_DECOMPRESS_RESULT_CHECKSUM_VALID) == 0U) {
		printf("lzma2 result fields mismatch: consumed=%lu/%lu "
		       "written=%lu/%lu alg=%lu crc=0x%08lx flags=0x%08lx\n",
		       (unsigned long)result.bytes_consumed,
		       (unsigned long)sizeof(compressed),
		       (unsigned long)result.bytes_written,
		       (unsigned long)(sizeof(plain) - 1U),
		       (unsigned long)result.algorithm,
		       (unsigned long)result.checksum,
		       (unsigned long)result.flags);
		return 0;
	}

	return 1;
}

static int check_lzma2_feed_stream(void)
{
	static const unsigned char plain[] =
		"ZZ9000 SDK codec service: repeatable LZMA2 inflate path "
		"for ordinary 7z payloads";
	static const unsigned char compressed[] = {
		0x10, 0x01, 0x00, 0x4f, 0x5a, 0x5a, 0x39, 0x30,
		0x30, 0x30, 0x20, 0x53, 0x44, 0x4b, 0x20, 0x63,
		0x6f, 0x64, 0x65, 0x63, 0x20, 0x73, 0x65, 0x72,
		0x76, 0x69, 0x63, 0x65, 0x3a, 0x20, 0x72, 0x65,
		0x70, 0x65, 0x61, 0x74, 0x61, 0x62, 0x6c, 0x65,
		0x20, 0x4c, 0x5a, 0x4d, 0x41, 0x32, 0x20, 0x69,
		0x6e, 0x66, 0x6c, 0x61, 0x74, 0x65, 0x20, 0x70,
		0x61, 0x74, 0x68, 0x20, 0x66, 0x6f, 0x72, 0x20,
		0x6f, 0x72, 0x64, 0x69, 0x6e, 0x61, 0x72, 0x79,
		0x20, 0x37, 0x7a, 0x20, 0x70, 0x61, 0x79, 0x6c,
		0x6f, 0x61, 0x64, 0x73, 0x00
	};
	unsigned char decoded[128];
	unsigned char chunk[11];
	struct SDKDecompressStreamResult result;
	uint32_t session;
	uint32_t offset;
	uint32_t total;
	uint16_t status;
	int need_input;

	sdk_decompress_stream_reset_all();
	memset(decoded, 0xa5, sizeof(decoded));
	memset(&result, 0, sizeof(result));
	status = sdk_decompress_stream_begin(
		SDK_COMPRESSION_LZMA2,
		SDK_DECOMPRESS_FLAG_EXPECT_END |
			SDK_DECOMPRESS_FLAG_FEED_INPUT,
		0, 0U, (uint32_t)sizeof(decoded), &result);
	if (status != SDK_STATUS_OK || result.session == 0U) {
		printf("lzma2 feed begin failed: %u\n", status);
		return 0;
	}
	session = result.session;

	status = sdk_decompress_stream_feed(session, compressed, 1U, 0U,
	                                    &result);
	if (status != SDK_STATUS_OK ||
	    (result.flags & SDK_DECOMPRESS_RESULT_NEED_INPUT) == 0U ||
	    result.bytes_consumed != 1U) {
		printf("lzma2 feed prop failed: status=%u consumed=%lu flags=%lu\n",
		       status, (unsigned long)result.bytes_consumed,
		       (unsigned long)result.flags);
		sdk_decompress_stream_close(session);
		return 0;
	}
	status = sdk_decompress_stream_read(session, chunk,
	                                    (uint32_t)sizeof(chunk), &result);
	if (status != SDK_STATUS_OK ||
	    (result.flags & SDK_DECOMPRESS_RESULT_NEED_INPUT) == 0U ||
	    result.bytes_written != 0U) {
		printf("lzma2 feed prop read failed: status=%u written=%lu flags=%lu\n",
		       status, (unsigned long)result.bytes_written,
		       (unsigned long)result.flags);
		sdk_decompress_stream_close(session);
		return 0;
	}

	offset = 1U;
	total = 0U;
	need_input = 1;
	while (1) {
		if (need_input) {
			uint32_t feed_len;
			uint32_t flags = 0U;

			if (offset >= sizeof(compressed)) {
				printf("lzma2 feed exhausted input\n");
				sdk_decompress_stream_close(session);
				return 0;
			}
			feed_len = (uint32_t)sizeof(compressed) - offset;
			if (feed_len > 7U)
				feed_len = 7U;
			if (offset + feed_len == sizeof(compressed))
				flags = SDK_DECOMPRESS_STREAM_FEED_EOF;
			status = sdk_decompress_stream_feed(
				session, compressed + offset, feed_len, flags,
				&result);
			if (status != SDK_STATUS_OK) {
				printf("lzma2 feed chunk failed: status=%u offset=%lu\n",
				       status, (unsigned long)offset);
				return 0;
			}
			offset += feed_len;
			need_input = 0;
		}

		status = sdk_decompress_stream_read(session, chunk,
		                                    (uint32_t)sizeof(chunk),
		                                    &result);
		if (status != SDK_STATUS_OK) {
			printf("lzma2 feed read failed: status=%u total=%lu\n",
			       status, (unsigned long)total);
			return 0;
		}
		if (result.bytes_written > sizeof(chunk) ||
		    total + result.bytes_written > sizeof(decoded)) {
			printf("lzma2 feed output overrun\n");
			sdk_decompress_stream_close(session);
			return 0;
		}
		if (result.bytes_written != 0U) {
			memcpy(decoded + total, chunk, result.bytes_written);
			total += result.bytes_written;
		}
		if ((result.flags & SDK_DECOMPRESS_RESULT_STREAM_END) != 0U)
			break;
		if ((result.flags & SDK_DECOMPRESS_RESULT_NEED_INPUT) != 0U)
			need_input = 1;
		else if (result.bytes_written == 0U) {
			printf("lzma2 feed made no progress\n");
			sdk_decompress_stream_close(session);
			return 0;
		}
	}

	if (sdk_decompress_stream_close(session) != SDK_STATUS_OK) {
		printf("lzma2 feed close failed\n");
		return 0;
	}
	if (offset != sizeof(compressed) ||
	    total != sizeof(plain) - 1U ||
	    memcmp(decoded, plain, sizeof(plain) - 1U) != 0 ||
	    result.bytes_consumed != sizeof(compressed) ||
	    result.checksum != 0x2b0dec5aU ||
	    result.algorithm != SDK_COMPRESSION_LZMA2 ||
	    (result.flags & SDK_DECOMPRESS_RESULT_CHECKSUM_VALID) == 0U) {
		printf("lzma2 feed result mismatch\n");
		return 0;
	}

	return 1;
}

static int check_deflate_raw_feed_stream(void)
{
	static const unsigned char plain[] =
		"ZZ9000 SDK codec service: raw deflate feed-mode stream";
	unsigned char compressed[256];
	unsigned char decoded[128];
	unsigned char chunk[13];
	unsigned long compressed_len = sizeof(compressed);
	struct SDKDecompressStreamResult result;
	uint32_t session;
	uint32_t offset;
	uint32_t total;
	uint16_t status;
	uint32_t expected_crc;
	int need_input;

	if (!make_deflate_stream(-MAX_WBITS, plain, sizeof(plain) - 1U,
	                         compressed, &compressed_len)) {
		printf("failed to create raw deflate feed stream\n");
		return 0;
	}

	sdk_decompress_stream_reset_all();
	memset(decoded, 0xa5, sizeof(decoded));
	memset(&result, 0, sizeof(result));
	status = sdk_decompress_stream_begin(
		SDK_COMPRESSION_DEFLATE_RAW,
		SDK_DECOMPRESS_FLAG_EXPECT_END |
			SDK_DECOMPRESS_FLAG_FEED_INPUT,
		0, 0U, (uint32_t)sizeof(decoded), &result);
	if (status != SDK_STATUS_OK || result.session == 0U) {
		printf("deflate feed begin failed: %u\n", status);
		return 0;
	}
	session = result.session;

	offset = 0U;
	total = 0U;
	need_input = 1;
	while (1) {
		if (need_input) {
			uint32_t feed_len;
			uint32_t flags = 0U;

			if (offset >= compressed_len) {
				printf("deflate feed exhausted input\n");
				sdk_decompress_stream_close(session);
				return 0;
			}
			feed_len = (uint32_t)compressed_len - offset;
			if (feed_len > 9U)
				feed_len = 9U;
			if (offset + feed_len == compressed_len)
				flags = SDK_DECOMPRESS_STREAM_FEED_EOF;
			status = sdk_decompress_stream_feed(
				session, compressed + offset, feed_len, flags,
				&result);
			if (status != SDK_STATUS_OK) {
				printf("deflate feed chunk failed: status=%u offset=%lu\n",
				       status, (unsigned long)offset);
				return 0;
			}
			offset += feed_len;
			need_input = 0;
		}

		status = sdk_decompress_stream_read(session, chunk,
		                                    (uint32_t)sizeof(chunk),
		                                    &result);
		if (status != SDK_STATUS_OK) {
			printf("deflate feed read failed: status=%u total=%lu\n",
			       status, (unsigned long)total);
			return 0;
		}
		if (result.bytes_written > sizeof(chunk) ||
		    total + result.bytes_written > sizeof(decoded)) {
			printf("deflate feed output overrun\n");
			sdk_decompress_stream_close(session);
			return 0;
		}
		if (result.bytes_written != 0U) {
			memcpy(decoded + total, chunk, result.bytes_written);
			total += result.bytes_written;
		}
		if ((result.flags & SDK_DECOMPRESS_RESULT_STREAM_END) != 0U)
			break;
		if ((result.flags & SDK_DECOMPRESS_RESULT_NEED_INPUT) != 0U)
			need_input = 1;
		else if (result.bytes_written == 0U) {
			printf("deflate feed made no progress\n");
			sdk_decompress_stream_close(session);
			return 0;
		}
	}

	if (sdk_decompress_stream_close(session) != SDK_STATUS_OK) {
		printf("deflate feed close failed\n");
		return 0;
	}
	expected_crc = (uint32_t)crc32(crc32(0L, Z_NULL, 0),
	                              plain, (uInt)(sizeof(plain) - 1U));
	if (offset != compressed_len ||
	    total != sizeof(plain) - 1U ||
	    memcmp(decoded, plain, sizeof(plain) - 1U) != 0 ||
	    result.bytes_consumed != compressed_len ||
	    result.checksum != expected_crc ||
	    result.algorithm != SDK_COMPRESSION_DEFLATE_RAW ||
	    (result.flags & SDK_DECOMPRESS_RESULT_CHECKSUM_VALID) == 0U) {
		printf("deflate feed result mismatch\n");
		return 0;
	}

	return 1;
}

int main(void)
{
	static const unsigned char plain[] =
		"ZZ9000 SDK codec service: repeatable zlib-backed inflates";
	unsigned char compressed[256];
	unsigned char decoded[8];
	unsigned long compressed_len = sizeof(compressed);
	struct SDKDecompressResult result;
	uint16_t status;

	if (!check_algorithm(SDK_COMPRESSION_DEFLATE_RAW, -MAX_WBITS,
	                     plain, sizeof(plain) - 1U)) {
		return 1;
	}
	if (!check_algorithm(SDK_COMPRESSION_ZLIB, MAX_WBITS,
	                     plain, sizeof(plain) - 1U)) {
		return 2;
	}
	if (!check_algorithm(SDK_COMPRESSION_GZIP, MAX_WBITS + 16,
	                     plain, sizeof(plain) - 1U)) {
		return 3;
	}
	if (!check_lzma_alone()) {
		return 8;
	}
	if (!check_lzma_feed_stream()) {
		return 9;
	}
	if (!check_lzma2()) {
		return 11;
	}
	if (!check_lzma2_feed_stream()) {
		return 12;
	}
	if (!check_deflate_raw_feed_stream()) {
		return 10;
	}

	if (!make_deflate_stream(MAX_WBITS, plain, sizeof(plain) - 1U,
	                         compressed, &compressed_len)) {
		return 4;
	}

	memset(&result, 0, sizeof(result));
	status = sdk_decompress_buffer(SDK_COMPRESSION_ZLIB,
	                               SDK_DECOMPRESS_FLAG_EXPECT_END,
	                               compressed, (uint32_t)compressed_len,
	                               decoded, (uint32_t)sizeof(decoded),
	                               &result);
	if (status != SDK_STATUS_NO_MEMORY)
		return 5;

	status = sdk_decompress_buffer(SDK_COMPRESSION_LZ4_BLOCK, 0U,
	                               compressed, (uint32_t)compressed_len,
	                               decoded, (uint32_t)sizeof(decoded),
	                               &result);
	if (status != SDK_STATUS_UNSUPPORTED)
		return 6;

	status = sdk_decompress_buffer(SDK_COMPRESSION_ZLIB,
	                               SDK_DECOMPRESS_FLAG_EXPECT_END,
	                               plain, sizeof(plain) - 1U,
	                               decoded, (uint32_t)sizeof(decoded),
	                               &result);
	if (status != SDK_STATUS_BAD_REQUEST)
		return 7;

	return 0;
}
