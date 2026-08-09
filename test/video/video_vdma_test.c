/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Host-side checks for video VDMA sizing helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "video_vdma.h"
#include <stdint.h>
#include <stdio.h>

static int expect_u32(const char *label, uint32_t actual, uint32_t expected)
{
	if (actual != expected) {
		printf("%s: got %u expected %u\n", label, actual, expected);
		return 0;
	}

	return 1;
}

static int test_line_bytes_do_not_grow_with_stream_width(void)
{
	if (!expect_u32("1080p32 line bytes",
	                video_vdma_line_bytes(1920U, 1U), 7680U)) {
		return 1;
	}
	if (!expect_u32("1080p16 line bytes",
	                video_vdma_line_bytes(1920U, 2U), 3840U)) {
		return 2;
	}
	if (!expect_u32("1080p8 line bytes",
	                video_vdma_line_bytes(1920U, 4U), 1920U)) {
		return 3;
	}
	if (!expect_u32("2560x1440x32 line bytes",
	                video_vdma_line_bytes(2560U, 1U), 10240U)) {
		return 4;
	}
	if (!expect_u32("1280x1024x32 line bytes",
	                video_vdma_line_bytes(1280U, 1U), 5120U)) {
		return 5;
	}

	return 0;
}

static int test_pan_stride_uses_framebuffer_width(void)
{
	if (!expect_u32("unscaled 32-bit pan stride",
	                video_vdma_stride_bytes(1920U, 1U, 2048U, 1U),
	                8192U)) {
		return 1;
	}
	if (!expect_u32("scaled 32-bit pan stride",
	                video_vdma_stride_bytes(1280U, 2U, 800U, 2U),
	                3200U)) {
		return 2;
	}
	if (!expect_u32("16-bit pan stride",
	                video_vdma_stride_bytes(1920U, 2U, 2048U, 1U),
	                4096U)) {
		return 3;
	}
	if (!expect_u32("1280x1024x32 pan stride",
	                video_vdma_stride_bytes(1280U, 1U, 0U, 1U),
	                5120U)) {
		return 4;
	}

	return 0;
}

static int test_default_stride_matches_line_bytes(void)
{
	if (!expect_u32("default 32-bit stride",
	                video_vdma_stride_bytes(1920U, 1U, 0U, 1U), 7680U)) {
		return 1;
	}
	if (!expect_u32("matching pan width falls back to line bytes",
	                video_vdma_stride_bytes(1920U, 1U, 1920U, 1U), 7680U)) {
		return 2;
	}
	if (!expect_u32("matching scaled pan width falls back to line bytes",
	                video_vdma_stride_bytes(1280U, 2U, 640U, 2U), 2560U)) {
		return 3;
	}

	return 0;
}

static int test_native_pan_moves_start_before_capture_buffer(void)
{
	if (!expect_u32("1280 native 64-pixel right pan",
	                video_vdma_pan_right_start(0x00e00000U,
	                                           VIDEO_VDMA_FULL_WIDTH_PAN_PIXELS),
	                0x00dfff00U)) {
		return 1;
	}
	if (!expect_u32("right pan clamps before offset zero",
	                video_vdma_pan_right_start(0x00000100U, 128U),
	                0U)) {
		return 2;
	}

	return 0;
}

int main(void)
{
	int result;

	result = test_line_bytes_do_not_grow_with_stream_width();
	if (result)
		return 10 + result;

	result = test_pan_stride_uses_framebuffer_width();
	if (result)
		return 30 + result;

	result = test_default_stride_matches_line_bytes();
	if (result)
		return 50 + result;

	result = test_native_pan_moves_start_before_capture_buffer();
	if (result)
		return 70 + result;

	return 0;
}
