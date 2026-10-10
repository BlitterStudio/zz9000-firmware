/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "../tools/zzplay/zzplay-sync.h"

#include <stdint.h>

static int check_periods(void)
{
  return zzplay_frame_period_pts(25U, 1U) == 3600U &&
         zzplay_frame_period_pts(30000U, 1001U) == 3003U &&
         zzplay_frame_period_pts(0U, 1U) == 0U;
}

static int check_boundaries(void)
{
  ZZPlaySyncPolicy policy;
  int64_t drift = 0;

  zzplay_sync_policy_init(&policy, 25U, 1U);
  if (policy.hold_ahead_pts != 1800U ||
      policy.drop_late_pts != 3600U) {
    return 0;
  }
  if (zzplay_sync_decide(&policy, 11800U, 10000U, &drift) !=
          ZZPLAY_SYNC_PRESENT ||
      drift != 1800 ||
      zzplay_sync_decide(&policy, 11801U, 10000U, &drift) !=
          ZZPLAY_SYNC_HOLD ||
      zzplay_sync_decide(&policy, 6400U, 10000U, &drift) !=
          ZZPLAY_SYNC_PRESENT ||
      drift != -3600 ||
      zzplay_sync_decide(&policy, 6399U, 10000U, &drift) !=
          ZZPLAY_SYNC_DISCARD) {
    return 0;
  }
  return 1;
}

static int check_saturation(void)
{
  ZZPlaySyncPolicy policy;
  int64_t drift;

  zzplay_sync_policy_init(&policy, 60U, 1U);
  if (zzplay_sync_decide(&policy, UINT64_MAX, 0U, &drift) !=
          ZZPLAY_SYNC_HOLD ||
      drift != INT64_MAX ||
      zzplay_sync_decide(&policy, 0U, UINT64_MAX, &drift) !=
          ZZPLAY_SYNC_DISCARD ||
      drift != INT64_MIN) {
    return 0;
  }
  return 1;
}

static int check_initial_offset(void)
{
  return !zzplay_sync_audio_may_start(9000U, 18000U, 3600U) &&
         zzplay_sync_audio_may_start(15000U, 18000U, 3600U) &&
         zzplay_sync_audio_may_start(19000U, 18000U, 3600U) &&
         zzplay_sync_audio_may_start(
             UINT64_MAX, 18000U, 3600U);
}

static int check_audio_starvation_recovery(void)
{
  return zzplay_sync_resolve_audio_starvation(
             ZZPLAY_SYNC_HOLD, 0U, 0U) == ZZPLAY_SYNC_PRESENT &&
         zzplay_sync_resolve_audio_starvation(
             ZZPLAY_SYNC_HOLD, 1U, 0U) == ZZPLAY_SYNC_HOLD &&
         zzplay_sync_resolve_audio_starvation(
             ZZPLAY_SYNC_HOLD, 960U, 960U) == ZZPLAY_SYNC_PRESENT &&
         zzplay_sync_resolve_audio_starvation(
             ZZPLAY_SYNC_HOLD, 961U, 960U) == ZZPLAY_SYNC_HOLD &&
         zzplay_sync_resolve_audio_starvation(
             ZZPLAY_SYNC_PRESENT, 0U, 960U) == ZZPLAY_SYNC_PRESENT &&
         zzplay_sync_resolve_audio_starvation(
             ZZPLAY_SYNC_DISCARD, 0U, 960U) == ZZPLAY_SYNC_DISCARD;
}

static int check_keyframe_skip(void)
{
  /* 500 ms is 45000 ticks. Only a larger lag skips. */
  return !zzplay_sync_needs_keyframe_skip(0) &&
         !zzplay_sync_needs_keyframe_skip(1800) &&
         !zzplay_sync_needs_keyframe_skip(-3600) &&
         !zzplay_sync_needs_keyframe_skip(-45000) &&
         zzplay_sync_needs_keyframe_skip(-45001) &&
         zzplay_sync_needs_keyframe_skip(INT64_MIN);
}

/* A decoder that only just keeps pace leaves video 40-500 ms behind for
 * good; discarding decoded frames does not win that time back, so without
 * a cap every frame was discarded (720p VP9 on the A4000: 874 of 900). */
static int check_discard_run(void)
{
  const int64_t late = -9000;   /* 100 ms behind */
  const int64_t far = -45001;   /* past the keyframe skip */

  return zzplay_sync_limit_discard_run(ZZPLAY_SYNC_DISCARD, 0U, late, 1) ==
             ZZPLAY_SYNC_DISCARD &&
         zzplay_sync_limit_discard_run(ZZPLAY_SYNC_DISCARD, 1U, late, 1) ==
             ZZPLAY_SYNC_DISCARD &&
         /* the third late frame in a row is shown */
         zzplay_sync_limit_discard_run(ZZPLAY_SYNC_DISCARD, 2U, late, 1) ==
             ZZPLAY_SYNC_PRESENT &&
         zzplay_sync_limit_discard_run(ZZPLAY_SYNC_DISCARD, 7U, late, 0) ==
             ZZPLAY_SYNC_PRESENT &&
         /* past the skip the discard asks the card to skip decoding,
          * which is how video catches up */
         zzplay_sync_limit_discard_run(ZZPLAY_SYNC_DISCARD, 2U, far, 1) ==
             ZZPLAY_SYNC_DISCARD &&
         /* without a keyframe skip it would only lose the frame */
         zzplay_sync_limit_discard_run(ZZPLAY_SYNC_DISCARD, 2U, far, 0) ==
             ZZPLAY_SYNC_PRESENT &&
         zzplay_sync_limit_discard_run(ZZPLAY_SYNC_HOLD, 5U, 9000, 1) ==
             ZZPLAY_SYNC_HOLD &&
         zzplay_sync_limit_discard_run(ZZPLAY_SYNC_PRESENT, 5U, 0, 1) ==
             ZZPLAY_SYNC_PRESENT;
}

int main(void)
{
  if (!check_periods()) {
    return 1;
  }
  if (!check_boundaries()) {
    return 2;
  }
  if (!check_saturation()) {
    return 3;
  }
  if (!check_initial_offset()) {
    return 4;
  }
  if (!check_audio_starvation_recovery()) {
    return 5;
  }
  if (!check_keyframe_skip()) {
    return 6;
  }
  if (!check_discard_run()) {
    return 7;
  }
  return 0;
}
