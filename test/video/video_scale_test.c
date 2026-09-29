/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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

int main(void)
{
	if (!expect_u32("vertical scale x1",
	                video_vertical_scale_factor(0U), 1U))
		return 1;
	if (!expect_u32("vertical scale x2",
	                video_vertical_scale_factor(2U), 2U))
		return 2;
	if (!expect_u32("vertical scale x4",
	                video_vertical_scale_factor(4U), 4U))
		return 3;
	if (!expect_u32("x2 keeps legacy sprite doubling",
	                video_formatter_scale_control(2U), 10U))
		return 4;
	if (!expect_u32("x4 leaves sprite doubling independent",
	                video_formatter_scale_control(4U), 4U))
		return 5;
	if (!expect_u32("filtered progressive capture uses x2",
	                video_videocap_scalemode(0U, 0U), 2U))
		return 6;
	if (!expect_u32("filtered interlaced capture uses x1",
	                video_videocap_scalemode(0U, 1U), 0U))
		return 7;
	if (!expect_u32("full progressive capture uses x4",
	                video_videocap_scalemode(1U, 0U), 4U))
		return 8;
	if (!expect_u32("full interlaced capture uses x2",
	                video_videocap_scalemode(1U, 1U), 2U))
		return 9;
	if (!expect_u32("PAL progressive fullscan reads 256 rows",
	                video_videocap_source_rows(1024U, 1U, 0U, 0U), 256U))
		return 10;
	if (!expect_u32("NTSC progressive fullscan reads 200 rows",
	                video_videocap_source_rows(1024U, 1U, 1U, 0U), 200U))
		return 11;
	if (!expect_u32("PAL interlaced fullscan reads 512 rows",
	                video_videocap_source_rows(1024U, 1U, 0U, 1U), 512U))
		return 12;
	if (!expect_u32("NTSC interlaced fullscan reads 400 rows",
	                video_videocap_source_rows(1024U, 1U, 1U, 1U), 400U))
		return 13;
	if (!expect_u32("filtered NTSC keeps legacy output-row division",
	                video_videocap_source_rows(480U, 0U, 1U, 0U), 240U))
		return 14;
	if (!expect_u32("PAL fullscan keeps power-of-two scale control",
	                video_videocap_scale_control(1U, 0U, 0U), 4U))
		return 15;
	if (!expect_u32("PAL fullscan interlaced keeps legacy x2 control",
	                video_videocap_scale_control(1U, 0U, 1U), 10U))
		return 16;
	if (!expect_u32("NTSC progressive fullscan carries 200 source rows",
	                video_videocap_scale_control(1U, 1U, 0U),
	                (200U << 16) | 4U))
		return 17;
	if (!expect_u32("NTSC interlaced fullscan keeps legacy x2 control",
	                video_videocap_scale_control(1U, 1U, 1U), 10U))
		return 18;
	if (!expect_u32("filtered request stays filtered",
	                video_videocap_full_width(0U, 1U), 0U))
		return 19;
	if (!expect_u32("full request needs full-rate hardware",
	                video_videocap_full_width(1U, 0U), 0U))
		return 20;
	if (!expect_u32("full request uses full-rate hardware",
	                video_videocap_full_width(1U, 1U), 1U))
		return 21;
	if (!expect_u32("PAL progressive fills the raster",
	                video_videocap_fullscan_content_height(256U), 1024U))
		return 22;
	if (!expect_u32("PAL interlaced fills the raster",
	                video_videocap_fullscan_content_height(512U), 1024U))
		return 23;
	if (!expect_u32("NTSC progressive letterboxes at x5",
	                video_videocap_fullscan_content_height(200U), 1000U))
		return 24;
	if (!expect_u32("NTSC interlaced letterboxes at x2",
	                video_videocap_fullscan_content_height(400U), 800U))
		return 25;
	{
		struct video_videocap_scanout_rect r =
			video_videocap_fullscan_rect(
				ZZ_VIDEOCAP_OUTPUT_FULL_60, 0U, 0U);
		if (r.x != 0U || r.y != 0U || r.width != 1280U ||
		    r.height != 1024U)
			return 26;
		r = video_videocap_fullscan_rect(
			ZZ_VIDEOCAP_OUTPUT_FULL_60, 1U, 0U);
		if (r.x != 0U || r.y != 12U || r.width != 1280U ||
		    r.height != 1000U)
			return 27;
		r = video_videocap_fullscan_rect(
			ZZ_VIDEOCAP_OUTPUT_FULL_60, 1U, 1U);
		if (r.x != 0U || r.y != 112U || r.width != 1280U ||
		    r.height != 800U)
			return 28;
		r = video_videocap_fullscan_rect(
			ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60, 1U, 0U);
		if (r.x != 320U || r.y != 40U || r.width != 1280U ||
		    r.height != 1000U)
			return 29;
		r = video_videocap_fullscan_rect(
			ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_50, 0U, 1U);
		if (r.x != 320U || r.y != 28U || r.width != 1280U ||
		    r.height != 1024U)
			return 30;
	}

	return 0;
}
