/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-sync.h"

#include <limits.h>

#define ZZPLAY_PTS_HZ 90000U
#define ZZPLAY_STEADY_MAX_TICKS 3600U

uint32_t zzplay_frame_period_us(uint32_t frame_rate_milli)
{
  return frame_rate_milli == 0U
             ? 0U
             : 1000000000U / frame_rate_milli;
}

uint32_t zzplay_pacing_wait_us(uint32_t frame_period_us,
                               uint32_t elapsed_us,
                               int uncapped)
{
  if (uncapped || elapsed_us >= frame_period_us) {
    return 0U;
  }
  return frame_period_us - elapsed_us;
}

uint64_t zzplay_frame_period_pts(uint32_t frame_rate_num,
                                 uint32_t frame_rate_den)
{
  if (frame_rate_num == 0U || frame_rate_den == 0U) {
    return 0U;
  }
  return ((uint64_t)ZZPLAY_PTS_HZ * frame_rate_den +
          frame_rate_num / 2U) / frame_rate_num;
}

void zzplay_sync_policy_init(ZZPlaySyncPolicy *policy,
                             uint32_t frame_rate_num,
                             uint32_t frame_rate_den)
{
  uint64_t period;

  if (!policy) {
    return;
  }
  period = zzplay_frame_period_pts(frame_rate_num, frame_rate_den);
  policy->hold_ahead_pts = period / 2U;
  policy->drop_late_pts =
      period > ZZPLAY_STEADY_MAX_TICKS
          ? period
          : ZZPLAY_STEADY_MAX_TICKS;
}

static int64_t zzplay_pts_delta(uint64_t video_pts,
                                uint64_t master_pts)
{
  uint64_t magnitude;

  if (video_pts >= master_pts) {
    magnitude = video_pts - master_pts;
    return magnitude > (uint64_t)INT64_MAX
               ? INT64_MAX
               : (int64_t)magnitude;
  }
  magnitude = master_pts - video_pts;
  return magnitude > (uint64_t)INT64_MAX
             ? INT64_MIN
             : -(int64_t)magnitude;
}

ZZPlaySyncDecision zzplay_sync_decide(
    const ZZPlaySyncPolicy *policy,
    uint64_t video_pts,
    uint64_t master_pts,
    int64_t *drift_pts)
{
  int64_t drift;

  if (!policy) {
    return ZZPLAY_SYNC_PRESENT;
  }
  drift = zzplay_pts_delta(video_pts, master_pts);
  if (drift_pts) {
    *drift_pts = drift;
  }
  if (drift > 0 &&
      (uint64_t)drift > policy->hold_ahead_pts) {
    return ZZPLAY_SYNC_HOLD;
  }
  if (drift < 0 &&
      (drift == INT64_MIN ||
       (uint64_t)(-drift) > policy->drop_late_pts)) {
    return ZZPLAY_SYNC_DISCARD;
  }
  return ZZPLAY_SYNC_PRESENT;
}

ZZPlaySyncDecision zzplay_sync_resolve_audio_starvation(
    ZZPlaySyncDecision decision,
    uint64_t queued_audio_frames,
    uint64_t low_water_frames)
{
  if (decision == ZZPLAY_SYNC_HOLD &&
      queued_audio_frames <= low_water_frames) {
    return ZZPLAY_SYNC_PRESENT;
  }
  return decision;
}

int zzplay_sync_audio_may_start(uint64_t video_pts,
                                uint64_t audio_origin_pts,
                                uint64_t allowed_lead_pts)
{
  if (video_pts == UINT64_MAX ||
      audio_origin_pts == UINT64_MAX ||
      video_pts >= audio_origin_pts) {
    return 1;
  }
  return audio_origin_pts - video_pts <= allowed_lead_pts;
}
