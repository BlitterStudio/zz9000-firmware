/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-stats.h"

#include <string.h>

void zzplay_stats_reset(ZZPlayStatsCore *stats)
{
  if (stats) {
    memset(stats, 0, sizeof(*stats));
  }
}

void zzplay_stats_record_frame(ZZPlayStatsCore *stats,
                               uint32_t wall_us,
                               uint32_t decode_us)
{
  if (!stats) {
    return;
  }
  stats->wall_us += wall_us;
  stats->decode_us += decode_us;
  stats->report_decode_us += decode_us;
  stats->report_frames++;
  stats->total_frames++;
}

void zzplay_stats_reset_report(ZZPlayStatsCore *stats)
{
  if (!stats) {
    return;
  }
  stats->report_decode_us = 0U;
  stats->report_frames = 0U;
}

void zzplay_stats_record_sync(ZZPlayStatsCore *stats,
                              ZZPlaySyncDecision decision,
                              int64_t drift_pts)
{
  uint64_t magnitude;

  if (!stats) {
    return;
  }
  stats->current_drift_pts = drift_pts;
  magnitude = drift_pts < 0
                  ? (drift_pts == INT64_MIN
                         ? (UINT64_C(1) << 63)
                         : (uint64_t)(-drift_pts))
                  : (uint64_t)drift_pts;
  if (magnitude > stats->max_abs_drift_pts) {
    stats->max_abs_drift_pts = magnitude;
  }
  if (decision == ZZPLAY_SYNC_HOLD) {
    stats->hold_events++;
  } else if (decision == ZZPLAY_SYNC_DISCARD) {
    stats->discarded_frames++;
    stats->late_frames++;
  } else {
    stats->presented_frames++;
  }
}

void zzplay_stats_record_profile(ZZPlayStatsCore *stats,
                                 ZZPlayProfileCategory category,
                                 uint32_t elapsed_us)
{
  if (!stats || category < 0 || category >= ZZPLAY_PROFILE_COUNT) {
    return;
  }
  stats->profile[category].elapsed_us += elapsed_us;
  stats->profile[category].calls++;
}

uint32_t zzplay_stats_profile_average_us(
    const ZZPlayStatsCore *stats, ZZPlayProfileCategory category)
{
  uint64_t average;

  if (!stats || category < 0 || category >= ZZPLAY_PROFILE_COUNT ||
      stats->profile[category].calls == 0U) {
    return 0U;
  }
  average = stats->profile[category].elapsed_us /
            stats->profile[category].calls;
  return average > 0xffffffffULL ? 0xffffffffU : (uint32_t)average;
}

uint64_t zzplay_stats_profile_total_us(const ZZPlayStatsCore *stats)
{
  uint64_t total = 0U;
  int category;

  if (!stats) {
    return 0U;
  }
  for (category = 0; category < ZZPLAY_PROFILE_COUNT; category++) {
    total += stats->profile[category].elapsed_us;
  }
  return total;
}

uint32_t zzplay_fps_milli(uint32_t frames, uint64_t elapsed_us)
{
  uint64_t value;

  if (frames == 0U || elapsed_us == 0U) {
    return 0U;
  }
  value = ((uint64_t)frames * 1000000000ULL + elapsed_us / 2U) /
          elapsed_us;
  return value > 0xffffffffULL ? 0xffffffffU : (uint32_t)value;
}
