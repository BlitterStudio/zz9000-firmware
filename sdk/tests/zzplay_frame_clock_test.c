/*
 * Host unit tests for zzplay monotonic frame clock (zzplay-frame-clock.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "../tools/zzplay/zzplay-frame-clock.h"

static int test_zero_duration_min_deadline(void)
{
  ZZPlayFrameClock clock;
  uint64_t now = 1000000ULL;

  zzplay_frame_clock_init(&clock, 0);
  /* Duration 0 ms must be clamped to max(duration, 1) = 1 ms = 1000 us */
  zzplay_frame_clock_start(&clock, now, 0U);

  if (clock.effective_duration_ms != 1U) return 1;
  if (clock.deadline_us != now + 1000ULL) return 2;
  if (zzplay_frame_clock_remaining_us(&clock, now) != 1000U) return 3;

  /* At now + 999 us, still waiting */
  if (zzplay_frame_clock_poll(&clock, now + 999ULL) != ZZPLAY_FRAME_ACTION_WAIT) return 4;
  /* At now + 1000 us, advance is ready */
  if (zzplay_frame_clock_poll(&clock, now + 1000ULL) != ZZPLAY_FRAME_ACTION_ADVANCE) return 5;

  return 0;
}

static int test_cumulative_no_drift(void)
{
  ZZPlayFrameClock clock;
  uint64_t now = 5000000ULL;
  unsigned i;

  zzplay_frame_clock_init(&clock, 0);
  /* 40 ms frame = 40,000 us */
  zzplay_frame_clock_start(&clock, now, 40U);
  if (clock.deadline_us != now + 40000ULL) return 1;

  /* Simulate 10 frames with varied execution jitter (e.g. 50-100 us late) */
  for (i = 1; i <= 10; i++) {
    now = clock.deadline_us + 75ULL; /* 75 us jitter */
    zzplay_frame_clock_advance(&clock, now, 40U);
  }

  /* Cumulative deadline after 11 frames (1 start + 10 advances):
   * total duration = 11 * 40,000 us = 440,000 us.
   * Jitter MUST NOT accumulate in deadline_us! */
  if (clock.deadline_us != 5000000ULL + 440000ULL) return 2;
  if (clock.frame_index != 10U) return 3;
  if (clock.total_elapsed_ms != 400U) return 4;
  if (clock.drift_resync_count != 0U) return 5;

  return 0;
}

static int test_pause_resume_freeze(void)
{
  ZZPlayFrameClock clock;
  uint64_t now = 1000000ULL;

  zzplay_frame_clock_init(&clock, 0);
  /* 100 ms frame */
  zzplay_frame_clock_start(&clock, now, 100U);

  /* Advance 40 ms into the frame: 60 ms (60,000 us) remaining */
  now += 40000ULL;
  if (zzplay_frame_clock_remaining_us(&clock, now) != 60000U) return 1;

  /* Pause at now */
  zzplay_frame_clock_pause(&clock, now);
  if (!clock.paused) return 2;
  if (clock.paused_remaining_us != 60000ULL) return 3;

  /* While paused, poll returns WAIT regardless of time passed */
  if (zzplay_frame_clock_poll(&clock, now + 5000000ULL) != ZZPLAY_FRAME_ACTION_WAIT) return 4;
  /* Remaining time stays frozen */
  if (zzplay_frame_clock_remaining_us(&clock, now + 5000000ULL) != 60000U) return 5;
  /* Wait slice during pause returns polling chunk so input/gui pumps */
  if (zzplay_frame_clock_wait_slice_us(&clock, now + 5000000ULL, 2000U) != 2000U) return 6;

  /* Resume 10 seconds later */
  now += 10000000ULL;
  zzplay_frame_clock_resume(&clock, now);
  if (clock.paused) return 7;
  /* Deadline must be shifted to now + 60,000 us */
  if (clock.deadline_us != now + 60000ULL) return 8;
  if (zzplay_frame_clock_remaining_us(&clock, now) != 60000U) return 9;

  return 0;
}

static int test_native_end_vs_explicit_repeat(void)
{
  ZZPlayFrameClock clock;
  uint64_t now = 1000000ULL;

  /* 1. Native finite end without repeat: finishes item (EOF) */
  zzplay_frame_clock_init(&clock, 0);
  zzplay_frame_clock_start(&clock, now, 50U);
  zzplay_frame_clock_on_end(&clock);

  if (zzplay_frame_clock_poll(&clock, now + 50000ULL) != ZZPLAY_FRAME_ACTION_EOF) return 1;

  /* 2. Explicit repeat enabled: triggers RESTART */
  zzplay_frame_clock_set_repeat(&clock, 1);
  if (zzplay_frame_clock_poll(&clock, now + 50000ULL) != ZZPLAY_FRAME_ACTION_RESTART) return 2;

  /* Restart resets the clock */
  zzplay_frame_clock_on_restart(&clock, now + 50000ULL, 50U);
  if (clock.ended) return 3;
  if (clock.frame_index != 0U) return 4;
  if (clock.deadline_us != now + 100000ULL) return 5;
  if (zzplay_frame_clock_poll(&clock, now + 50000ULL) != ZZPLAY_FRAME_ACTION_WAIT) return 6;

  return 0;
}

static int test_wait_slice_no_busy_spin(void)
{
  ZZPlayFrameClock clock;
  uint64_t now = 1000000ULL;

  zzplay_frame_clock_init(&clock, 0);
  zzplay_frame_clock_start(&clock, now, 50U); /* 50,000 us */

  /* Remaining 50,000 us, max chunk 2,000 us -> returns 2,000 us */
  if (zzplay_frame_clock_wait_slice_us(&clock, now, 2000U) != 2000U) return 1;

  /* When remaining is 1,500 us, returns 1,500 us */
  if (zzplay_frame_clock_wait_slice_us(&clock, now + 48500ULL, 2000U) != 1500U) return 2;

  /* When deadline reached, returns 0 us */
  if (zzplay_frame_clock_wait_slice_us(&clock, now + 50000ULL, 2000U) != 0U) return 3;

  return 0;
}

static int test_lag_drift_resync(void)
{
  ZZPlayFrameClock clock;
  uint64_t now = 1000000ULL;

  zzplay_frame_clock_init(&clock, 0);
  zzplay_frame_clock_start(&clock, now, 40U);

  /* Simulate an extreme 2-second stall (exceeds default 1s max_lag) */
  now += 2000000ULL;
  zzplay_frame_clock_advance(&clock, now, 40U);

  /* Deadline should have resynced to now + 40,000 us, not now - 1,920,000 us */
  if (clock.drift_resync_count != 1U) return 1;
  if (clock.deadline_us != now + 40000ULL) return 2;

  return 0;
}

int main(void)
{
  int rc;

  rc = test_zero_duration_min_deadline();
  if (rc) {
    fprintf(stderr, "test_zero_duration_min_deadline failed: %d\n", rc);
    return 1;
  }
  rc = test_cumulative_no_drift();
  if (rc) {
    fprintf(stderr, "test_cumulative_no_drift failed: %d\n", rc);
    return 2;
  }
  rc = test_pause_resume_freeze();
  if (rc) {
    fprintf(stderr, "test_pause_resume_freeze failed: %d\n", rc);
    return 3;
  }
  rc = test_native_end_vs_explicit_repeat();
  if (rc) {
    fprintf(stderr, "test_native_end_vs_explicit_repeat failed: %d\n", rc);
    return 4;
  }
  rc = test_wait_slice_no_busy_spin();
  if (rc) {
    fprintf(stderr, "test_wait_slice_no_busy_spin failed: %d\n", rc);
    return 5;
  }
  rc = test_lag_drift_resync();
  if (rc) {
    fprintf(stderr, "test_lag_drift_resync failed: %d\n", rc);
    return 6;
  }

  printf("zzplay_frame_clock_test: all tests passed\n");
  return 0;
}
