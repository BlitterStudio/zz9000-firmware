/* Playback pacing arithmetic for zzplay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_SYNC_H
#define ZZPLAY_SYNC_H

#include <stdint.h>

typedef enum ZZPlaySyncDecision {
  ZZPLAY_SYNC_HOLD = 0,
  ZZPLAY_SYNC_PRESENT,
  ZZPLAY_SYNC_DISCARD
} ZZPlaySyncDecision;

typedef struct ZZPlaySyncPolicy {
  uint64_t hold_ahead_pts;
  uint64_t drop_late_pts;
} ZZPlaySyncPolicy;

uint32_t zzplay_frame_period_us(uint32_t frame_rate_milli);
uint32_t zzplay_pacing_wait_us(uint32_t frame_period_us,
                               uint32_t elapsed_us,
                               int uncapped);
uint64_t zzplay_frame_period_pts(uint32_t frame_rate_num,
                                 uint32_t frame_rate_den);
void zzplay_sync_policy_init(ZZPlaySyncPolicy *policy,
                             uint32_t frame_rate_num,
                             uint32_t frame_rate_den);
ZZPlaySyncDecision zzplay_sync_decide(
    const ZZPlaySyncPolicy *policy,
    uint64_t video_pts,
    uint64_t master_pts,
    int64_t *drift_pts);
ZZPlaySyncDecision zzplay_sync_resolve_audio_starvation(
    ZZPlaySyncDecision decision,
    uint64_t queued_audio_frames,
    uint64_t low_water_frames);
int zzplay_sync_audio_may_start(uint64_t video_pts,
                                uint64_t audio_origin_pts,
                                uint64_t allowed_lead_pts);

#endif /* ZZPLAY_SYNC_H */
