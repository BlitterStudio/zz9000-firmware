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
	                video_videocap_scalemode(0U, 0U, 0U), 2U))
		return 6;
	if (!expect_u32("filtered interlaced capture uses x1",
	                video_videocap_scalemode(0U, 1U, 0U), 0U))
		return 7;
	if (!expect_u32("full progressive capture uses x4",
	                video_videocap_scalemode(1U, 0U, 0U), 4U))
		return 8;
	if (!expect_u32("full interlaced capture uses x2",
	                video_videocap_scalemode(1U, 1U, 0U), 2U))
		return 9;
	if (!expect_u32("PAL progressive fullscan reads 256 rows",
	                video_videocap_source_rows(1024U, 1U, 0U, 0U, 0U, 0U), 256U))
		return 10;
	if (!expect_u32("NTSC progressive fullscan reads 200 rows",
	                video_videocap_source_rows(1024U, 1U, 1U, 0U, 0U, 0U), 200U))
		return 11;
	if (!expect_u32("PAL interlaced fullscan reads 512 rows",
	                video_videocap_source_rows(1024U, 1U, 0U, 1U, 0U, 0U), 512U))
		return 12;
	if (!expect_u32("NTSC interlaced fullscan reads 400 rows",
	                video_videocap_source_rows(1024U, 1U, 1U, 1U, 0U, 0U), 400U))
		return 13;
	if (!expect_u32("filtered NTSC keeps legacy output-row division",
	                video_videocap_source_rows(480U, 0U, 1U, 0U, 0U, 0U), 240U))
		return 14;
	if (!expect_u32("PAL fullscan keeps power-of-two scale control",
	                video_videocap_scale_control(1U, 0U, 0U, 0U), 4U))
		return 15;
	if (!expect_u32("PAL fullscan interlaced keeps legacy x2 control",
	                video_videocap_scale_control(1U, 0U, 1U, 0U), 10U))
		return 16;
	if (!expect_u32("NTSC progressive fullscan keeps legacy x4 control",
	                video_videocap_scale_control(1U, 1U, 0U, 0U), 4U))
		return 17;
	if (!expect_u32("NTSC interlaced fullscan keeps legacy x2 control",
	                video_videocap_scale_control(1U, 1U, 1U, 0U), 10U))
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
	{
		struct video_videocap_scanout_rect r =
			video_videocap_fullscan_rect(
				ZZ_VIDEOCAP_OUTPUT_FULL_60, 1U, 0U, 0U, 0U, 0U, 0U,
				256U, 0U, 0U);
		if (r.x != 0U || r.y != 0U || r.width != 1280U ||
		    r.height != 1024U)
			return 22;
		r = video_videocap_fullscan_rect(
			ZZ_VIDEOCAP_OUTPUT_FULL_60, 1U, 1U, 0U, 0U, 0U, 0U, 200U,
			0U, 0U);
		if (r.x != 0U || r.y != 112U || r.width != 1280U ||
		    r.height != 800U)
			return 23;
		r = video_videocap_fullscan_rect(
			ZZ_VIDEOCAP_OUTPUT_FULL_60, 1U, 1U, 1U, 0U, 0U, 0U, 400U,
			0U, 0U);
		if (r.x != 0U || r.y != 112U || r.width != 1280U ||
		    r.height != 800U)
			return 24;
		r = video_videocap_fullscan_rect(
			ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60, 1U, 1U, 0U, 0U, 0U,
			0U, 200U, 0U, 0U);
		if (r.x != 320U || r.y != 140U || r.width != 1280U ||
		    r.height != 800U)
			return 25;
		r = video_videocap_fullscan_rect(
			ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_50, 1U, 0U, 1U, 0U, 0U,
			0U, 512U, 0U, 0U);
		if (r.x != 320U || r.y != 28U || r.width != 1280U ||
		    r.height != 1024U)
			return 26;
	}
	/* Doubled-scan and 24 kHz sources (status [12:10]): rows shown one
	 * to one on filtered output; full-width pixel-doubled (bit 0) with
	 * x2 rows while the woven frame fits 512, x1 when it is taller; the
	 * NTSC 200/400-row window is a 15 kHz property and stays off. */
	if (!expect_u32("doubled filtered capture uses x1",
	                video_videocap_scalemode(0U, 0U,
	                    VIDEO_VIDEOCAP_SOURCE_SHORT | VIDEO_VIDEOCAP_SOURCE_DOUBLED), 0U))
		return 40;
	if (!expect_u32("doubled full capture selects horizontal x2 (Euro72 427)",
	                video_videocap_scalemode(1U, 0U,
	                    VIDEO_VIDEOCAP_SOURCE_SHORT | VIDEO_VIDEOCAP_SOURCE_DOUBLED), 3U))
		return 41;
	if (!expect_u32("tall doubled source uses horizontal x2 and vertical x1",
	                video_videocap_scalemode(1U, 1U,
	                    VIDEO_VIDEOCAP_SOURCE_SHORT | VIDEO_VIDEOCAP_SOURCE_DOUBLED |
	                    VIDEO_VIDEOCAP_SOURCE_TALL |
	                    VIDEO_VIDEOCAP_ROWS_CLASS_3), 1U))
		return 42;
	if (!expect_u32("DblPAL no-lace combines horizontal x2 with vertical x4",
	                video_videocap_scalemode(1U, 0U,
	                    VIDEO_VIDEOCAP_SOURCE_SHORT | VIDEO_VIDEOCAP_SOURCE_DOUBLED |
	                    VIDEO_VIDEOCAP_ROWS_CLASS_1), 5U))
		return 54;
	if (!expect_u32("DblPAL laced combines horizontal and vertical x2",
	                video_videocap_scalemode(1U, 1U,
	                    VIDEO_VIDEOCAP_SOURCE_SHORT | VIDEO_VIDEOCAP_SOURCE_DOUBLED |
	                    VIDEO_VIDEOCAP_ROWS_CLASS_2), 3U))
		return 55;
	if (!expect_u32("unknown doubled row class falls back to horizontal plus vertical x2",
	                video_videocap_scalemode(1U, 0U,
	                    VIDEO_VIDEOCAP_SOURCE_SHORT | VIDEO_VIDEOCAP_SOURCE_DOUBLED), 3U))
		return 56;
	if (!expect_u32("Super72 full capture (658 woven) shows rows x1",
	                video_videocap_scalemode(1U, 1U,
	                    VIDEO_VIDEOCAP_SOURCE_SHORT | VIDEO_VIDEOCAP_SOURCE_TALL |
	                    VIDEO_VIDEOCAP_ROWS_CLASS_3), 0U))
		return 43;
	if (!expect_u32("doubled full capture enables integer horizontal repeat",
	                video_videocap_scale_control(1U, 1U, 0U,
	                    VIDEO_VIDEOCAP_SOURCE_SHORT | VIDEO_VIDEOCAP_SOURCE_DOUBLED),
	                3U | 8U))
		return 44;
	if (!expect_u32("doubled full capture reads 512 source rows at x2",
	                video_videocap_source_rows(1024U, 1U, 1U, 0U,
	                    VIDEO_VIDEOCAP_SOURCE_SHORT | VIDEO_VIDEOCAP_SOURCE_DOUBLED,
	                    0U), 512U))
		return 45;
	/* Full-width doubled sources use SCALEX to fill the 1280-word canvas. */
	{
		struct video_videocap_scanout_rect r =
			video_videocap_fullscan_rect(
				ZZ_VIDEOCAP_OUTPUT_FULL_60, 1U, 0U, 0U,
				VIDEO_VIDEOCAP_SOURCE_SHORT |
				VIDEO_VIDEOCAP_SOURCE_DOUBLED |
				VIDEO_VIDEOCAP_SOURCE_TALL |
				VIDEO_VIDEOCAP_ROWS_CLASS_3,
				0U, 0U, 1024U, 0U, 0U);
		if (r.x != 0U || r.y != 0U || r.width != 1280U ||
		    r.height != 512U)
			return 52;
		/* DblPAL no-lace class-1 x4 fills both canvas dimensions. */
		r = video_videocap_fullscan_rect(
			ZZ_VIDEOCAP_OUTPUT_FULL_60, 1U, 0U, 0U,
			VIDEO_VIDEOCAP_SOURCE_SHORT |
			VIDEO_VIDEOCAP_SOURCE_DOUBLED |
			VIDEO_VIDEOCAP_ROWS_CLASS_1,
			0U, 0U, 256U, 0U, 0U);
		if (r.x != 0U || r.y != 0U || r.width != 1280U ||
		    r.height != 1024U)
			return 53;
		/* DblPAL class-2 fills 1024 rows and centers in 1080p. */
		r = video_videocap_fullscan_rect(
			ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_50, 1U, 0U, 0U,
			VIDEO_VIDEOCAP_SOURCE_SHORT |
			VIDEO_VIDEOCAP_SOURCE_DOUBLED |
			VIDEO_VIDEOCAP_ROWS_CLASS_2,
			0U, 0U, 512U, 0U, 0U);
		if (r.x != 320U || r.y != 28U || r.width != 1280U ||
		    r.height != 1024U)
			return 57;
		/* Width override stays in captured words, then repeats 2x. */
		r = video_videocap_fullscan_rect(
			ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_50, 1U, 0U, 0U,
			VIDEO_VIDEOCAP_SOURCE_SHORT |
			VIDEO_VIDEOCAP_SOURCE_DOUBLED |
			VIDEO_VIDEOCAP_ROWS_CLASS_2,
			320U, 0U, 512U, 0U, 0U);
		if (r.x != 640U || r.y != 28U || r.width != 640U ||
		    r.height != 1024U)
			return 58;
		/* Euro72 / DblNTSC class 2 must not take the 15 kHz 800-line
		 * letterbox: vertical x2 would then consume only 400 rows. */
		r = video_videocap_fullscan_rect(
			ZZ_VIDEOCAP_OUTPUT_FULL_60, 1U, 1U, 0U,
			VIDEO_VIDEOCAP_SOURCE_SHORT |
			VIDEO_VIDEOCAP_SOURCE_DOUBLED |
			VIDEO_VIDEOCAP_ROWS_CLASS_2,
			0U, 0U, 512U, 0U, 0U);
		if (r.x != 0U || r.y != 0U || r.width != 1280U ||
		    r.height != 1024U)
			return 59;
		r = video_videocap_fullscan_rect(
			ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60, 1U, 1U, 0U,
			VIDEO_VIDEOCAP_SOURCE_SHORT |
			VIDEO_VIDEOCAP_SOURCE_DOUBLED |
			VIDEO_VIDEOCAP_ROWS_CLASS_2,
			0U, 0U, 512U, 0U, 0U);
		if (r.x != 320U || r.y != 28U || r.width != 1280U ||
		    r.height != 1024U)
			return 60;
	}
	/* Manual capture-window overrides: height bounds the source rows and
	 * can only shrink the automatic window; width/height letterbox the
	 * fullscan rectangle around the shrunken content. */
	if (!expect_u32("height override shrinks PAL progressive rows",
	                video_videocap_source_rows(1024U, 1U, 0U, 0U, 0U, 240U),
	                240U))
		return 46;
	if (!expect_u32("height override cannot grow NTSC rows",
	                video_videocap_source_rows(1024U, 1U, 1U, 0U, 0U, 400U),
	                200U))
		return 47;
	{
		struct video_videocap_scanout_rect r =
			video_videocap_fullscan_rect(
				ZZ_VIDEOCAP_OUTPUT_FULL_60, 1U, 0U, 0U, 0U, 640U,
				240U, 240U, 0U, 0U);
		if (r.x != 320U || r.y != 32U || r.width != 640U ||
		    r.height != 960U)
			return 48;
		r = video_videocap_fullscan_rect(
			ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60, 1U, 0U, 0U, 0U,
			640U, 240U, 240U, 0U, 0U);
		if (r.x != 320U + 320U || r.y != 28U + 32U ||
		    r.width != 640U || r.height != 960U)
			return 49;
		/* Filtered override centers in the active mode, not 1280x1024.
		 * Progressive PAL 640x240 at vertical x2 is 640x480 inside
		 * 800x600. */
		r = video_videocap_fullscan_rect(
			ZZ_VIDEOCAP_OUTPUT_FULL_60, 0U, 0U, 0U, 0U, 640U,
			240U, 240U, 800U, 600U);
		if (r.x != 80U || r.y != 60U || r.width != 640U ||
		    r.height != 480U)
			return 61;
		r = video_videocap_fullscan_rect(
			ZZ_VIDEOCAP_OUTPUT_FULL_60, 0U, 1U, 0U, 0U, 640U,
			200U, 200U, 720U, 480U);
		if (r.x != 40U || r.y != 40U || r.width != 640U ||
		    r.height != 400U)
			return 62;
	}
	if (!expect_u32("width bounds reject unaligned or out-of-range values",
	                video_videocap_width_valid(640U) +
	                video_videocap_width_valid(648U) +
	                video_videocap_width_valid(1296U) +
	                video_videocap_width_valid(240U), 1U))
		return 50;
	if (!expect_u32("height bounds reject out-of-range values",
	                video_videocap_height_valid(240U) +
	                video_videocap_height_valid(99U) +
	                video_videocap_height_valid(1025U), 1U))
		return 51;

	return 0;
}
