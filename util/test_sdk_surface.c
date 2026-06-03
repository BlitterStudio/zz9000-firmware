/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Host-side checks for SDK firmware surface helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_surface.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int expect_bytes(const char *label, const uint8_t *actual,
                        const uint8_t *expected, uint32_t length)
{
	uint32_t i;

	for (i = 0; i < length; i++) {
		if (actual[i] != expected[i]) {
			printf("%s: mismatch at byte %u got %02x expected %02x\n",
			       label, i, actual[i], expected[i]);
			return 0;
		}
	}

	return 1;
}

static int test_fill_rect_writes_surface_format_bytes(void)
{
	uint8_t argb[4U * 4U * 4U];
	uint8_t bgra[4U * 1U];
	uint8_t rgb888[3U * 1U];
	uint8_t rgb565[2U * 2U * 2U];
	static const uint8_t expected_argb[] = {
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0xaa, 0x11, 0x22, 0x33,
		0xaa, 0x11, 0x22, 0x33,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0xaa, 0x11, 0x22, 0x33,
		0xaa, 0x11, 0x22, 0x33,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00
	};
	static const uint8_t expected_rgb565[] = {
		0xab, 0xcd, 0xab, 0xcd,
		0xab, 0xcd, 0xab, 0xcd
	};
	static const uint8_t expected_bgra[] = {
		0x33, 0x22, 0x11, 0xaa
	};
	static const uint8_t expected_rgb888[] = {
		0x11, 0x22, 0x33
	};

	memset(argb, 0, sizeof(argb));
	if (!sdk_surface_fill_rect(argb, 4U, 4U, 16U,
	                           SDK_SURFACE_FORMAT_ARGB8888,
	                           1U, 1U, 2U, 2U, 0xaa112233UL)) {
		return 1;
	}
	if (!expect_bytes("argb-fill", argb, expected_argb, sizeof(argb))) {
		return 2;
	}

	memset(bgra, 0, sizeof(bgra));
	if (!sdk_surface_fill_rect(bgra, 1U, 1U, 4U,
	                           SDK_SURFACE_FORMAT_BGRA8888,
	                           0U, 0U, 1U, 1U, 0xaa112233UL)) {
		return 6;
	}
	if (!expect_bytes("bgra-fill", bgra, expected_bgra, sizeof(bgra))) {
		return 7;
	}

	memset(rgb888, 0, sizeof(rgb888));
	if (!sdk_surface_fill_rect(rgb888, 1U, 1U, 3U,
	                           SDK_SURFACE_FORMAT_RGB888,
	                           0U, 0U, 1U, 1U, 0xaa112233UL)) {
		return 8;
	}
	if (!expect_bytes("rgb888-fill", rgb888, expected_rgb888,
	                  sizeof(rgb888))) {
		return 9;
	}

	memset(rgb565, 0, sizeof(rgb565));
	if (!sdk_surface_fill_rect(rgb565, 2U, 2U, 4U,
	                           SDK_SURFACE_FORMAT_RGB565,
	                           0U, 0U, 2U, 2U, 0x0000abcdUL)) {
		return 3;
	}
	if (!expect_bytes("rgb565-fill", rgb565, expected_rgb565,
	                  sizeof(rgb565))) {
		return 4;
	}

	if (sdk_surface_fill_rect(argb, 4U, 4U, 16U,
	                          SDK_SURFACE_FORMAT_ARGB8888,
	                          3U, 3U, 2U, 1U, 0U)) {
		return 5;
	}

	return 0;
}

static int test_copy_rect_handles_overlap(void)
{
	uint8_t surface[4U * 4U];
	static const uint8_t expected[] = {
		0x00, 0x01, 0x02, 0x03,
		0x04, 0x00, 0x01, 0x07,
		0x08, 0x04, 0x05, 0x0b,
		0x0c, 0x0d, 0x0e, 0x0f
	};
	uint32_t i;

	for (i = 0; i < sizeof(surface); i++)
		surface[i] = (uint8_t)i;

	if (!sdk_surface_copy_rect(surface, 4U, 4U, 4U,
	                           surface, 4U, 4U, 4U,
	                           SDK_SURFACE_FORMAT_INDEX8,
	                           0U, 0U, 1U, 1U, 2U, 2U)) {
		return 1;
	}
	if (!expect_bytes("overlap-copy", surface, expected, sizeof(surface))) {
		return 2;
	}

	if (sdk_surface_copy_rect(surface, 4U, 4U, 4U,
	                          surface, 4U, 4U, 4U,
	                          SDK_SURFACE_FORMAT_INDEX8,
	                          3U, 0U, 0U, 0U, 2U, 2U)) {
		return 3;
	}

	return 0;
}

static int test_scale_rect_bilinear_bgra8888_interpolates(void)
{
	uint8_t src[2U * 2U * 4U] = {
		0x00, 0x00, 0x00, 0xff,
		0x64, 0x00, 0x00, 0xff,
		0x00, 0x64, 0x00, 0xff,
		0x64, 0x64, 0x00, 0xff
	};
	uint8_t dst[3U * 3U * 4U];
	static const uint8_t expected[] = {
		0x00, 0x00, 0x00, 0xff,
		0x32, 0x00, 0x00, 0xff,
		0x64, 0x00, 0x00, 0xff,
		0x00, 0x32, 0x00, 0xff,
		0x32, 0x32, 0x00, 0xff,
		0x64, 0x32, 0x00, 0xff,
		0x00, 0x64, 0x00, 0xff,
		0x32, 0x64, 0x00, 0xff,
		0x64, 0x64, 0x00, 0xff
	};

	memset(dst, 0, sizeof(dst));
	if (!sdk_surface_scale_rect(dst, 3U, 3U, 12U,
	                            src, 2U, 2U, 8U,
	                            SDK_SURFACE_FORMAT_BGRA8888,
	                            0U, 0U, 2U, 2U,
	                            0U, 0U, 3U, 3U,
	                            SDK_SCALE_BILINEAR)) {
		return 1;
	}
	if (!expect_bytes("bilinear-scale", dst, expected, sizeof(dst))) {
		return 2;
	}

	return 0;
}

static int test_scale_rect_clipped_preserves_full_mapping(void)
{
	uint8_t src[2U * 2U * 4U] = {
		0x00, 0x00, 0x00, 0xff,
		0x64, 0x00, 0x00, 0xff,
		0x00, 0x64, 0x00, 0xff,
		0x64, 0x64, 0x00, 0xff
	};
	uint8_t full[4U * 4U * 4U];
	uint8_t clipped[4U * 4U * 4U];
	uint32_t x;
	uint32_t y;

	memset(full, 0, sizeof(full));
	memset(clipped, 0xee, sizeof(clipped));
	if (!sdk_surface_scale_rect(full, 4U, 4U, 16U,
	                            src, 2U, 2U, 8U,
	                            SDK_SURFACE_FORMAT_BGRA8888,
	                            0U, 0U, 2U, 2U,
	                            0U, 0U, 4U, 4U,
	                            SDK_SCALE_BILINEAR)) {
		return 1;
	}
	if (!sdk_surface_scale_rect_clipped(clipped, 4U, 4U, 16U,
	                                    src, 2U, 2U, 8U,
	                                    SDK_SURFACE_FORMAT_BGRA8888,
	                                    0U, 0U, 2U, 2U,
	                                    0U, 0U, 4U, 4U,
	                                    1U, 1U, 2U, 2U,
	                                    SDK_SCALE_BILINEAR)) {
		return 2;
	}

	for (y = 0; y < 4U; y++) {
		for (x = 0; x < 4U; x++) {
			uint32_t offset = ((y * 4U) + x) * 4U;

			if (x >= 1U && x < 3U && y >= 1U && y < 3U) {
				if (memcmp(&clipped[offset], &full[offset], 4U) != 0) {
					printf("clipped-scale: visible pixel mismatch at %u,%u\n",
					       x, y);
					return 3;
				}
			} else if (clipped[offset] != 0xeeU ||
			           clipped[offset + 1U] != 0xeeU ||
			           clipped[offset + 2U] != 0xeeU ||
			           clipped[offset + 3U] != 0xeeU) {
				printf("clipped-scale: modified hidden pixel at %u,%u\n",
				       x, y);
				return 4;
			}
		}
	}

	memset(clipped, 0xcc, sizeof(clipped));
	if (!sdk_surface_scale_rect_clipped(clipped, 4U, 4U, 16U,
	                                    src, 2U, 2U, 8U,
	                                    SDK_SURFACE_FORMAT_BGRA8888,
	                                    0U, 0U, 2U, 2U,
	                                    0U, 0U, 4U, 4U,
	                                    6U, 6U, 2U, 2U,
	                                    SDK_SCALE_BILINEAR)) {
		return 5;
	}
	for (x = 0; x < sizeof(clipped); x++) {
		if (clipped[x] != 0xccU) {
			printf("clipped-scale: no-intersection changed byte %u\n", x);
			return 6;
		}
	}

	return 0;
}

int main(void)
{
	int result;

	result = test_fill_rect_writes_surface_format_bytes();
	if (result)
		return 10 + result;

	result = test_copy_rect_handles_overlap();
	if (result)
		return 30 + result;

	result = test_scale_rect_bilinear_bgra8888_interpolates();
	if (result)
		return 50 + result;

	result = test_scale_rect_clipped_preserves_full_mapping();
	if (result)
		return 70 + result;

	return 0;
}
