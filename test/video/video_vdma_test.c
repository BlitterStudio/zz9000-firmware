/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Host-side checks for video VDMA sizing helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "video_vdma.h"
#include "video_scale.h"
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

static int test_native_scanout_starts_at_capture_row(void)
{
	if (!expect_u32("1280 native row-aligned start",
	                video_vdma_native_row_start(0x00e00000U),
	                0x00e00000U)) {
		return 1;
	}

	return 0;
}

static int test_centered_output_keeps_native_content_geometry(void)
{
	struct video_videocap_geometry full =
		video_videocap_output_geometry(ZZ_VIDEOCAP_OUTPUT_FULL_60);
	struct video_videocap_geometry centered =
		video_videocap_output_geometry(
			ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60);

	if (!expect_u32("full canvas width", full.canvas_width, 1280U) ||
	    !expect_u32("full canvas height", full.canvas_height, 1024U) ||
	    !expect_u32("full content width", full.content_width, 1280U) ||
	    !expect_u32("full content height", full.content_height, 1024U) ||
	    !expect_u32("centered canvas width", centered.canvas_width, 1920U) ||
	    !expect_u32("centered canvas height", centered.canvas_height, 1080U) ||
	    !expect_u32("centered content width", centered.content_width, 1280U) ||
	    !expect_u32("centered content height", centered.content_height, 1024U) ||
	    !expect_u32("centered viewport x", centered.viewport_x, 320U) ||
	    !expect_u32("centered viewport y", centered.viewport_y, 28U)) {
		return 1;
	}

	if (!expect_u32("centered VDMA line bytes",
	                video_vdma_line_bytes(centered.content_width, 1U),
	                5120U) ||
	    !expect_u32("centered VDMA pitch",
	                video_vdma_stride_bytes(centered.content_width, 1U,
	                                        0U, 1U), 5120U) ||
	    !expect_u32("centered progressive capture rows",
	                centered.content_height /
	                video_vertical_scale_factor(
	                    video_videocap_scalemode(1U, 0U)), 256U) ||
	    !expect_u32("centered interlaced capture rows",
	                centered.content_height /
	                video_vertical_scale_factor(
	                    video_videocap_scalemode(1U, 1U)), 512U)) {
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

	result = test_native_scanout_starts_at_capture_row();
	if (result)
		return 70 + result;

	result = test_centered_output_keeps_native_content_geometry();
	if (result)
		return 90 + result;

	return 0;
}
