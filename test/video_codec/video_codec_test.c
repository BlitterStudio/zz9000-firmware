/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "sdk_video_backend.h"
#include "sdk_video_yuy2.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpeg1_ps_fixture.inc"

#define EXPECTED_FRAMES 5U
#define EXPECTED_FNV64 UINT64_C(0x12cf0e63c2e2c840)

static uint64_t fnv1a64(uint64_t hash, const uint8_t *bytes, uint32_t length)
{
	uint32_t i;

	for (i = 0U; i < length; i++) {
		hash ^= bytes[i];
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static int test_yuy2_conversion(void)
{
	static const uint8_t y[8] = {1, 2, 3, 0, 4, 5, 6, 0};
	static const uint8_t cb[2] = {10, 11};
	static const uint8_t cr[2] = {20, 21};
	static const uint8_t expected[20] = {
		1, 10, 2, 20, 3, 11, 3, 21, 0xaa, 0xaa,
		4, 10, 5, 20, 6, 11, 6, 21, 0xaa, 0xaa,
	};
	uint8_t dst[20];
	uint32_t bytes_written = 0U;

	memset(dst, 0xaa, sizeof(dst));
	if (sdk_video_yuy2_row_bytes(3U) != 8U)
		return 1;
	if (!sdk_video_yuv420_to_yuy2(dst, 10U, 3U, 2U, y, 4U,
	                              cb, cr, 2U, &bytes_written))
		return 2;
	if (bytes_written != 16U || memcmp(dst, expected, sizeof(dst)) != 0)
		return 3;
	return 0;
}

static int test_streaming_decode(void)
{
	const struct SDKVideoDecoderOps *ops;
	struct SDKVideoDecoderInfo info;
	struct SDKVideoDecodedFrame frame;
	uint8_t *yuy2;
	uint32_t row_bytes;
	uint32_t offset = 0U;
	uint32_t frames = 0U;
	uint64_t hash = UINT64_C(14695981039346656037);
	void *decoder;
	int result;

	ops = sdk_video_backend_find(1U, 1U);
	if (!ops || sdk_video_backend_find(2U, 1U) != 0)
		return 1;
	decoder = ops->create();
	if (!decoder)
		return 2;

	/* Deliberately awkward chunks exercise header and packet boundaries. */
	while (offset < zz9k_mpeg1_ps_fixture_len) {
		uint32_t chunk = 137U;
		uint32_t accepted = 0U;

		if (chunk > zz9k_mpeg1_ps_fixture_len - offset)
			chunk = zz9k_mpeg1_ps_fixture_len - offset;
		if (!ops->write(decoder, zz9k_mpeg1_ps_fixture + offset,
		                chunk, 0, &accepted) || accepted != chunk) {
			ops->destroy(decoder);
			return 3;
		}
		offset += chunk;

		for (;;) {
			uint32_t bytes_written;
			uint32_t row;

			result = ops->decode(decoder, &frame);
			if (result != SDK_VIDEO_BACKEND_FRAME)
				break;
			if (frame.width != 32U || frame.height != 16U) {
				ops->destroy(decoder);
				return 4;
			}
			row_bytes = sdk_video_yuy2_row_bytes(frame.width);
			yuy2 = (uint8_t *)malloc(row_bytes * frame.height);
			if (!yuy2) {
				ops->destroy(decoder);
				return 5;
			}
			if (!sdk_video_yuv420_to_yuy2(
			        yuy2, row_bytes, frame.width, frame.height,
			        frame.y, frame.y_pitch, frame.cb, frame.cr,
			        frame.chroma_pitch, &bytes_written) ||
			    bytes_written != row_bytes * frame.height) {
				free(yuy2);
				ops->destroy(decoder);
				return 6;
			}
			for (row = 0U; row < frame.height; row++)
				hash = fnv1a64(hash, yuy2 + row * row_bytes, row_bytes);
			free(yuy2);
			frames++;
		}
		if (result == SDK_VIDEO_BACKEND_ERROR) {
			ops->destroy(decoder);
			return 7;
		}
	}

	/* The mailbox API permits clients to release/reuse their shared input
	 * buffer after the final data write, then signal EOF with no source. */
	{
		uint32_t accepted = 1U;

		if (!ops->write(decoder, NULL, 0U, 1, &accepted) || accepted != 0U) {
			ops->destroy(decoder);
			return 8;
		}
	}

	while (result != SDK_VIDEO_BACKEND_DONE) {
		result = ops->decode(decoder, &frame);
		if (result == SDK_VIDEO_BACKEND_NEED_INPUT ||
		    result == SDK_VIDEO_BACKEND_ERROR) {
			ops->destroy(decoder);
			return 9;
		}
		if (result == SDK_VIDEO_BACKEND_FRAME) {
			uint32_t bytes_written;
			uint32_t row;

			row_bytes = sdk_video_yuy2_row_bytes(frame.width);
			yuy2 = (uint8_t *)malloc(row_bytes * frame.height);
			if (!yuy2 || !sdk_video_yuv420_to_yuy2(
			        yuy2, row_bytes, frame.width, frame.height,
			        frame.y, frame.y_pitch, frame.cb, frame.cr,
			        frame.chroma_pitch, &bytes_written)) {
				free(yuy2);
				ops->destroy(decoder);
				return 10;
			}
			for (row = 0U; row < frame.height; row++)
				hash = fnv1a64(hash, yuy2 + row * row_bytes, row_bytes);
			free(yuy2);
			frames++;
		}
	}

	if (!ops->get_info(decoder, &info) || info.width != 32U ||
	    info.height != 16U || info.frame_rate_milli != 25000U) {
		ops->destroy(decoder);
		return 11;
	}
	ops->destroy(decoder);

	printf("video fixture: frames=%lu rate_milli=%lu fnv64=%016llx\n",
	       (unsigned long)frames, (unsigned long)info.frame_rate_milli,
	       (unsigned long long)hash);
	if (EXPECTED_FRAMES != 0U &&
	    (frames != EXPECTED_FRAMES || hash != EXPECTED_FNV64))
		return 12;
	return 0;
}

int main(void)
{
	int result = test_yuy2_conversion();

	if (result != 0)
		return 10 + result;
	result = test_streaming_decode();
	if (result != 0)
		return 30 + result;
	return 0;
}
