/*
 * Monotonic frame clock implementation for zzplay.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zzplay-frame-clock.h"
#include <string.h>

void zzplay_frame_clock_init(ZZPlayFrameClock *clock, int repeat_item)
{
  if (!clock) {
    return;
  }
  memset(clock, 0, sizeof(*clock));
  clock->max_lag_us = 1000000U; /* 1 second maximum lag before resync */
  clock->repeat_item = repeat_item ? 1U : 0U;
}

void zzplay_frame_clock_start(ZZPlayFrameClock *clock, uint64_t now_us,
                              uint32_t first_duration_ms)
{
  if (!clock) {
    return;
  }
  clock->start_us = now_us;
  clock->current_duration_ms = first_duration_ms;
  clock->effective_duration_ms = first_duration_ms > 0U ? first_duration_ms : 1U;
  clock->deadline_us = now_us + (uint64_t)clock->effective_duration_ms * 1000ULL;
  clock->frame_index = 0U;
  clock->total_elapsed_ms = 0U;
  clock->frames_presented = 1U;
  clock->started = 1U;
  clock->paused = 0U;
  clock->ended = 0U;
}

void zzplay_frame_clock_advance(ZZPlayFrameClock *clock, uint64_t now_us,
                                uint32_t duration_ms)
{
  uint64_t next_deadline;

  if (!clock || !clock->started) {
    return;
  }
  clock->total_elapsed_ms += clock->effective_duration_ms;
  clock->frame_index++;
  clock->frames_presented++;
  clock->current_duration_ms = duration_ms;
  clock->effective_duration_ms = duration_ms > 0U ? duration_ms : 1U;

  if (clock->paused) {
    clock->paused_remaining_us = (uint64_t)clock->effective_duration_ms * 1000ULL;
    return;
  }

  next_deadline = clock->deadline_us + (uint64_t)clock->effective_duration_ms * 1000ULL;
  if (clock->max_lag_us > 0U && now_us > next_deadline + (uint64_t)clock->max_lag_us) {
    /* Extreme lag spike: resync deadline to now + frame duration */
    clock->deadline_us = now_us + (uint64_t)clock->effective_duration_ms * 1000ULL;
    clock->drift_resync_count++;
  } else {
    /* Cumulative deadline without drift */
    clock->deadline_us = next_deadline;
  }
}

void zzplay_frame_clock_pause(ZZPlayFrameClock *clock, uint64_t now_us)
{
  if (!clock || !clock->started || clock->paused) {
    return;
  }
  clock->paused = 1U;
  if (clock->deadline_us > now_us) {
    clock->paused_remaining_us = clock->deadline_us - now_us;
  } else {
    clock->paused_remaining_us = 0U;
  }
}

void zzplay_frame_clock_resume(ZZPlayFrameClock *clock, uint64_t now_us)
{
  if (!clock || !clock->started || !clock->paused) {
    return;
  }
  clock->paused = 0U;
  clock->deadline_us = now_us + clock->paused_remaining_us;
}

uint32_t zzplay_frame_clock_remaining_us(const ZZPlayFrameClock *clock,
                                         uint64_t now_us)
{
  if (!clock || !clock->started || clock->ended) {
    return 0U;
  }
  if (clock->paused) {
    return clock->paused_remaining_us <= (uint64_t)UINT32_MAX
               ? (uint32_t)clock->paused_remaining_us
               : UINT32_MAX;
  }
  if (clock->deadline_us > now_us) {
    uint64_t diff = clock->deadline_us - now_us;
    return diff <= (uint64_t)UINT32_MAX ? (uint32_t)diff : UINT32_MAX;
  }
  return 0U;
}

uint32_t zzplay_frame_clock_wait_slice_us(const ZZPlayFrameClock *clock,
                                          uint64_t now_us,
                                          uint32_t max_slice_us)
{
  uint32_t rem;

  if (!clock || !clock->started || clock->ended) {
    return 0U;
  }
  if (max_slice_us == 0U) {
    max_slice_us = 2000U; /* Default 2ms poll chunk */
  }
  if (clock->paused) {
    return max_slice_us;
  }
  rem = zzplay_frame_clock_remaining_us(clock, now_us);
  if (rem == 0U) {
    return 0U;
  }
  return rem < max_slice_us ? rem : max_slice_us;
}

ZZPlayFrameAction zzplay_frame_clock_poll(const ZZPlayFrameClock *clock,
                                          uint64_t now_us)
{
  if (!clock || !clock->started) {
    return ZZPLAY_FRAME_ACTION_WAIT;
  }
  if (clock->ended) {
    return clock->repeat_item ? ZZPLAY_FRAME_ACTION_RESTART
                              : ZZPLAY_FRAME_ACTION_EOF;
  }
  if (clock->paused) {
    return ZZPLAY_FRAME_ACTION_WAIT;
  }
  if (now_us >= clock->deadline_us) {
    return ZZPLAY_FRAME_ACTION_ADVANCE;
  }
  return ZZPLAY_FRAME_ACTION_WAIT;
}

void zzplay_frame_clock_on_end(ZZPlayFrameClock *clock)
{
  if (!clock) {
    return;
  }
  clock->ended = 1U;
}

void zzplay_frame_clock_on_restart(ZZPlayFrameClock *clock, uint64_t now_us,
                                   uint32_t first_duration_ms)
{
  if (!clock) {
    return;
  }
  clock->frame_index = 0U;
  clock->current_duration_ms = first_duration_ms;
  clock->effective_duration_ms =
      first_duration_ms > 0U ? first_duration_ms : 1U;
  clock->deadline_us = now_us + (uint64_t)clock->effective_duration_ms * 1000ULL;
  clock->ended = 0U;
}

void zzplay_frame_clock_set_repeat(ZZPlayFrameClock *clock, int repeat_item)
{
  if (!clock) {
    return;
  }
  clock->repeat_item = repeat_item ? 1U : 0U;
}
