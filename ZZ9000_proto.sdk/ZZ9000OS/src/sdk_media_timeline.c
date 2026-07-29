/*
 * Integer MPEG Program Stream timeline mapping.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_media_timeline.h"

#include <string.h>

#define SDK_MEDIA_TIMELINE_DISCONTINUITY_TICKS UINT64_C(90000)

static uint64_t unwrap_pts(uint64_t previous, uint64_t raw_pts)
{
	const uint64_t mask = SDK_MEDIA_TIMELINE_PTS_MODULUS - 1U;
	const uint64_t half = SDK_MEDIA_TIMELINE_PTS_MODULUS >> 1;
	uint64_t candidate = (previous & ~mask) | (raw_pts & mask);

	if (candidate < previous && previous - candidate > half)
		candidate += SDK_MEDIA_TIMELINE_PTS_MODULUS;
	else if (candidate > previous && candidate - previous > half &&
	         candidate >= SDK_MEDIA_TIMELINE_PTS_MODULUS)
		candidate -= SDK_MEDIA_TIMELINE_PTS_MODULUS;
	return candidate;
}

static uint64_t advance_pts(const struct SDKMediaTimelineTrack *track,
	                        uint64_t step_units, uint32_t units_per_second,
	                        uint64_t *remainder)
{
	uint64_t scaled = step_units * UINT64_C(90000) + track->remainder;

	*remainder = scaled % units_per_second;
	return track->current + scaled / units_per_second;
}

void sdk_media_timeline_init(struct SDKMediaTimeline *timeline)
{
	if (!timeline)
		return;
	memset(timeline, 0, sizeof(*timeline));
	timeline->origin = SDK_MEDIA_TIMELINE_NO_PTS;
	timeline->last_unwrapped_raw = SDK_MEDIA_TIMELINE_NO_PTS;
	timeline->last_observed_raw = SDK_MEDIA_TIMELINE_NO_PTS;
}

void sdk_media_timeline_seed_origin(
	struct SDKMediaTimeline *timeline, uint64_t raw_pts)
{
	if (!timeline || timeline->have_origin ||
	    raw_pts == SDK_MEDIA_TIMELINE_NO_PTS)
		return;
	timeline->origin =
		raw_pts & (SDK_MEDIA_TIMELINE_PTS_MODULUS - 1U);
	timeline->have_origin = 1U;
}

int sdk_media_timeline_map(struct SDKMediaTimeline *timeline,
	                       uint32_t track_index, uint64_t raw_pts,
	                       uint64_t step_units, uint32_t units_per_second,
	                       struct SDKMediaTimelineResult *result)
{
	struct SDKMediaTimelineTrack *track;
	uint32_t flags = 0U;

	if (!timeline || !result ||
	    track_index >= SDK_MEDIA_TIMELINE_TRACKS ||
	    units_per_second == 0U)
		return 0;
	track = &timeline->track[track_index];

	if (raw_pts != SDK_MEDIA_TIMELINE_NO_PTS) {
		uint64_t unwrapped = raw_pts &
			(SDK_MEDIA_TIMELINE_PTS_MODULUS - 1U);
		uint8_t first_origin = !timeline->have_origin;

		if (timeline->last_unwrapped_raw != SDK_MEDIA_TIMELINE_NO_PTS)
			unwrapped = unwrap_pts(timeline->last_unwrapped_raw, raw_pts);
		timeline->last_unwrapped_raw = unwrapped;
		timeline->last_observed_raw =
			raw_pts & (SDK_MEDIA_TIMELINE_PTS_MODULUS - 1U);
		if (first_origin) {
			timeline->origin = unwrapped;
			timeline->have_origin = 1U;
		}
		if (track->have_current && first_origin) {
			/* A track may have advanced from rate-derived time before the
			 * first packet carrying PTS arrives. Rebase that provisional
			 * clock explicitly and report the transition once. */
			flags |= SDK_MEDIA_TIMELINE_DISCONTINUITY |
			         SDK_MEDIA_TIMELINE_REBASED;
		} else if (track->have_current) {
			uint64_t remainder;
			uint64_t expected = advance_pts(
				track, step_units, units_per_second, &remainder);
			uint64_t delta = expected > unwrapped
				? expected - unwrapped : unwrapped - expected;

			if (delta > SDK_MEDIA_TIMELINE_DISCONTINUITY_TICKS)
				flags |= SDK_MEDIA_TIMELINE_DISCONTINUITY |
				         SDK_MEDIA_TIMELINE_REBASED;
		}
		track->current = unwrapped;
		track->remainder = 0U;
		track->have_current = 1U;
	} else {
		if (track->have_current) {
			track->current = advance_pts(
				track, step_units, units_per_second,
				&track->remainder);
		} else {
			track->current = timeline->have_origin
				? timeline->origin : 0U;
			track->remainder = 0U;
			track->have_current = 1U;
		}
		flags |= SDK_MEDIA_TIMELINE_DERIVED;
	}

	memset(result, 0, sizeof(*result));
	result->pts = track->current;
	result->origin = timeline->origin;
	result->raw_pts = timeline->last_observed_raw;
	result->flags = flags;
	return 1;
}

int sdk_media_timeline_peek_next(
	const struct SDKMediaTimeline *timeline, uint32_t track_index,
	uint64_t step_units, uint32_t units_per_second, uint64_t *pts)
{
	uint64_t remainder;

	if (!timeline || !pts ||
	    track_index >= SDK_MEDIA_TIMELINE_TRACKS ||
	    units_per_second == 0U ||
	    !timeline->track[track_index].have_current)
		return 0;
	*pts = advance_pts(
		&timeline->track[track_index], step_units, units_per_second,
		&remainder);
	return 1;
}

int sdk_media_timeline_observe_ordered(
	struct SDKMediaTimeline *timeline, uint32_t track_index,
	uint64_t raw_pts, struct SDKMediaTimelineResult *result)
{
	struct SDKMediaTimelineTrack *track;
	uint64_t unwrapped;
	uint64_t delta;
	uint32_t flags = 0U;

	if (!timeline || !result ||
	    track_index >= SDK_MEDIA_TIMELINE_TRACKS ||
	    raw_pts == SDK_MEDIA_TIMELINE_NO_PTS)
		return 0;
	track = &timeline->track[track_index];
	unwrapped = raw_pts & (SDK_MEDIA_TIMELINE_PTS_MODULUS - 1U);
	if (timeline->last_unwrapped_raw != SDK_MEDIA_TIMELINE_NO_PTS)
		unwrapped = unwrap_pts(timeline->last_unwrapped_raw, raw_pts);
	timeline->last_unwrapped_raw = unwrapped;
	timeline->last_observed_raw =
		raw_pts & (SDK_MEDIA_TIMELINE_PTS_MODULUS - 1U);
	if (!timeline->have_origin) {
		timeline->origin = unwrapped;
		timeline->have_origin = 1U;
		if (track->have_current) {
			track->current = unwrapped;
			track->remainder = 0U;
			flags |= SDK_MEDIA_TIMELINE_DISCONTINUITY |
			         SDK_MEDIA_TIMELINE_REBASED;
		} else {
			track->current = unwrapped;
			track->remainder = 0U;
			track->have_current = 1U;
		}
	} else if (track->have_current) {
		delta = track->current > unwrapped
			? track->current - unwrapped : unwrapped - track->current;
		if (delta > SDK_MEDIA_TIMELINE_DISCONTINUITY_TICKS) {
			track->current = unwrapped;
			track->remainder = 0U;
			flags |= SDK_MEDIA_TIMELINE_DISCONTINUITY |
			         SDK_MEDIA_TIMELINE_REBASED;
		}
	} else {
		track->current = unwrapped;
		track->remainder = 0U;
		track->have_current = 1U;
	}
	memset(result, 0, sizeof(*result));
	result->pts = track->current;
	result->origin = timeline->origin;
	result->raw_pts = timeline->last_observed_raw;
	result->flags = flags;
	return 1;
}
