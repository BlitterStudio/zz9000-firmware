/*
 * Integer MPEG Program Stream timeline mapping.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_MEDIA_TIMELINE_H
#define SDK_MEDIA_TIMELINE_H

#include <stdint.h>

#define SDK_MEDIA_TIMELINE_NO_PTS UINT64_C(0xffffffffffffffff)
#define SDK_MEDIA_TIMELINE_PTS_MODULUS (UINT64_C(1) << 33)

#define SDK_MEDIA_TIMELINE_VIDEO 0U
#define SDK_MEDIA_TIMELINE_AUDIO 1U
#define SDK_MEDIA_TIMELINE_TRACKS 2U

#define SDK_MEDIA_TIMELINE_DERIVED       (1U << 0)
#define SDK_MEDIA_TIMELINE_DISCONTINUITY (1U << 1)
#define SDK_MEDIA_TIMELINE_REBASED       (1U << 2)

struct SDKMediaTimelineTrack {
	uint64_t current;
	uint64_t remainder;
	uint8_t have_current;
};

struct SDKMediaTimeline {
	struct SDKMediaTimelineTrack track[SDK_MEDIA_TIMELINE_TRACKS];
	uint64_t origin;
	uint64_t last_unwrapped_raw;
	uint64_t last_observed_raw;
	uint8_t have_origin;
};

struct SDKMediaTimelineResult {
	uint64_t pts;
	uint64_t origin;
	uint64_t raw_pts;
	uint32_t flags;
};

void sdk_media_timeline_init(struct SDKMediaTimeline *timeline);
void sdk_media_timeline_seed_origin(
	struct SDKMediaTimeline *timeline, uint64_t raw_pts);
int sdk_media_timeline_map(struct SDKMediaTimeline *timeline,
	                       uint32_t track, uint64_t raw_pts,
	                       uint64_t step_units, uint32_t units_per_second,
	                       struct SDKMediaTimelineResult *result);
int sdk_media_timeline_peek_next(
	const struct SDKMediaTimeline *timeline, uint32_t track,
	uint64_t step_units, uint32_t units_per_second, uint64_t *pts);
int sdk_media_timeline_observe_ordered(
	struct SDKMediaTimeline *timeline, uint32_t track, uint64_t raw_pts,
	struct SDKMediaTimelineResult *result);

#endif /* SDK_MEDIA_TIMELINE_H */
