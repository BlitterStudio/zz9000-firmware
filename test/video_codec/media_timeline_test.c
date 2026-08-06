/*
 * Integer 90 kHz Program Stream timestamp mapping.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_media_timeline.h"

#include <stdint.h>

static int test_missing_pts_derivation(void)
{
	struct SDKMediaTimeline timeline;
	struct SDKMediaTimelineResult result;

	sdk_media_timeline_init(&timeline);
	if (!sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_VIDEO,
	                            90000U, 1U, 25U, &result) ||
	    result.pts != 90000U || result.origin != 90000U ||
	    result.flags != 0U)
		return 1;
	if (!sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_VIDEO,
	                            SDK_MEDIA_TIMELINE_NO_PTS,
	                            1U, 25U, &result) ||
	    result.pts != 93600U ||
	    (result.flags & SDK_MEDIA_TIMELINE_DERIVED) == 0U)
		return 2;
	return 0;
}

static int test_demux_order_seeds_missing_track_pts(void)
{
	struct SDKMediaTimeline timeline;
	struct SDKMediaTimelineResult result;

	sdk_media_timeline_init(&timeline);
	sdk_media_timeline_seed_origin(&timeline, 48600U);
	sdk_media_timeline_seed_origin(&timeline, 90000U);
	if (!sdk_media_timeline_map(
		    &timeline, SDK_MEDIA_TIMELINE_AUDIO,
		    SDK_MEDIA_TIMELINE_NO_PTS, 1152U, 44100U, &result) ||
	    result.origin != 48600U || result.pts != 48600U ||
	    (result.flags & SDK_MEDIA_TIMELINE_DERIVED) == 0U)
		return 1;
	return 0;
}

static int test_wrap_and_av_offset(void)
{
	struct SDKMediaTimeline timeline;
	struct SDKMediaTimelineResult result;
	const uint64_t modulus = UINT64_C(1) << 33;

	sdk_media_timeline_init(&timeline);
	if (!sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_VIDEO,
	                            modulus - 1800U, 1U, 25U, &result))
		return 1;
	if (!sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_VIDEO,
	                            900U, 1U, 25U, &result) ||
	    result.pts != modulus + 900U)
		return 2;

	sdk_media_timeline_init(&timeline);
	if (!sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_VIDEO,
	                            90000U, 1U, 25U, &result) ||
	    !sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_AUDIO,
	                            94500U, 1152U, 44100U, &result) ||
	    result.origin != 90000U || result.pts != 94500U)
		return 3;
	return 0;
}

static int test_discontinuity_is_one_shot(void)
{
	struct SDKMediaTimeline timeline;
	struct SDKMediaTimelineResult result;

	sdk_media_timeline_init(&timeline);
	if (!sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_AUDIO,
	                            10000U, 1152U, 48000U, &result))
		return 1;
	if (!sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_AUDIO,
	                            500000U, 1152U, 48000U, &result) ||
	    (result.flags & (SDK_MEDIA_TIMELINE_DISCONTINUITY |
	                     SDK_MEDIA_TIMELINE_REBASED)) !=
	        (SDK_MEDIA_TIMELINE_DISCONTINUITY |
	         SDK_MEDIA_TIMELINE_REBASED))
		return 2;
	if (!sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_AUDIO,
	                            SDK_MEDIA_TIMELINE_NO_PTS,
	                            1152U, 48000U, &result) ||
	    (result.flags & (SDK_MEDIA_TIMELINE_DISCONTINUITY |
	                     SDK_MEDIA_TIMELINE_REBASED)) != 0U ||
	    (result.flags & SDK_MEDIA_TIMELINE_DERIVED) == 0U)
		return 3;
	return 0;
}

static int test_rational_accumulation(void)
{
	struct SDKMediaTimeline timeline;
	struct SDKMediaTimelineResult result;
	uint32_t i;

	sdk_media_timeline_init(&timeline);
	if (!sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_VIDEO,
	                            0U, 1001U, 30000U, &result) ||
	    !sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_VIDEO,
	                            SDK_MEDIA_TIMELINE_NO_PTS,
	                            1001U, 30000U, &result) ||
	    result.pts != 3003U)
		return 1;

	sdk_media_timeline_init(&timeline);
	if (!sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_AUDIO,
	                            0U, 1152U, 44100U, &result))
		return 2;
	for (i = 0U; i < 1000U; i++) {
		if (!sdk_media_timeline_map(
		        &timeline, SDK_MEDIA_TIMELINE_AUDIO,
		        SDK_MEDIA_TIMELINE_NO_PTS, 1152U, 44100U, &result))
			return 3;
	}
	if (result.pts !=
	        (UINT64_C(1000) * 1152U * 90000U) / 44100U ||
	    timeline.track[SDK_MEDIA_TIMELINE_AUDIO].remainder !=
	        (UINT64_C(1000) * 1152U * 90000U) % 44100U)
		return 4;
	return 0;
}

static int test_first_raw_rebases_derived_track(void)
{
	struct SDKMediaTimeline timeline;
	struct SDKMediaTimelineResult result;

	sdk_media_timeline_init(&timeline);
	if (!sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_VIDEO,
	                            SDK_MEDIA_TIMELINE_NO_PTS,
	                            1U, 25U, &result) ||
	    result.origin != SDK_MEDIA_TIMELINE_NO_PTS)
		return 1;
	if (!sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_VIDEO,
	                            SDK_MEDIA_TIMELINE_NO_PTS,
	                            1U, 25U, &result))
		return 2;
	if (!sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_VIDEO,
	                            90000U, 1U, 25U, &result) ||
	    result.origin != 90000U || result.pts != 90000U ||
	    (result.flags & (SDK_MEDIA_TIMELINE_DISCONTINUITY |
	                     SDK_MEDIA_TIMELINE_REBASED)) !=
	        (SDK_MEDIA_TIMELINE_DISCONTINUITY |
	         SDK_MEDIA_TIMELINE_REBASED))
		return 3;
	if (!sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_VIDEO,
	                            SDK_MEDIA_TIMELINE_NO_PTS,
	                            1U, 25U, &result) ||
	    (result.flags & (SDK_MEDIA_TIMELINE_DISCONTINUITY |
	                     SDK_MEDIA_TIMELINE_REBASED)) != 0U)
		return 4;
	return 0;
}

static int test_ordered_output_ignores_reorder_jitter(void)
{
	struct SDKMediaTimeline timeline;
	struct SDKMediaTimelineResult result;

	sdk_media_timeline_init(&timeline);
	if (!sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_VIDEO,
	                            10000U, 1U, 25U, &result) ||
	    !sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_VIDEO,
	                            SDK_MEDIA_TIMELINE_NO_PTS,
	                            1U, 25U, &result) ||
	    result.pts != 13600U)
		return 1;
	/* A demux PTS consumed while pl_mpeg is decoding a future I/P
	 * reference must not retime the earlier B-frame returned now. */
	if (!sdk_media_timeline_observe_ordered(
	        &timeline, SDK_MEDIA_TIMELINE_VIDEO, 17200U, &result) ||
	    result.pts != 13600U || result.flags != 0U)
		return 2;
	if (!sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_VIDEO,
	                            SDK_MEDIA_TIMELINE_NO_PTS,
	                            1U, 25U, &result) ||
	    result.pts != 17200U)
		return 3;
	if (!sdk_media_timeline_observe_ordered(
	        &timeline, SDK_MEDIA_TIMELINE_VIDEO, 500000U, &result) ||
	    result.pts != 500000U ||
	    (result.flags & (SDK_MEDIA_TIMELINE_DISCONTINUITY |
	                     SDK_MEDIA_TIMELINE_REBASED)) !=
	        (SDK_MEDIA_TIMELINE_DISCONTINUITY |
	         SDK_MEDIA_TIMELINE_REBASED))
		return 4;
	if (!sdk_media_timeline_map(&timeline, SDK_MEDIA_TIMELINE_VIDEO,
	                            SDK_MEDIA_TIMELINE_NO_PTS,
	                            1U, 25U, &result) ||
	    (result.flags & (SDK_MEDIA_TIMELINE_DISCONTINUITY |
	                     SDK_MEDIA_TIMELINE_REBASED)) != 0U)
		return 5;
	return 0;
}

int main(void)
{
	int result = test_missing_pts_derivation();

	if (result != 0)
		return 10 + result;
	result = test_wrap_and_av_offset();
	if (result != 0)
		return 20 + result;
	result = test_discontinuity_is_one_shot();
	if (result != 0)
		return 30 + result;
	result = test_rational_accumulation();
	if (result != 0)
		return 40 + result;
	result = test_first_raw_rebases_derived_track();
	if (result != 0)
		return 50 + result;
	result = test_ordered_output_ignores_reorder_jitter();
	if (result != 0)
		return 60 + result;
	result = test_demux_order_seeds_missing_track_pts();
	if (result != 0)
		return 70 + result;
	return 0;
}
