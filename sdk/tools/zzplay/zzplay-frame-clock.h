/*
 * Monotonic frame clock for animation playback in zzplay.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZPLAY_FRAME_CLOCK_H
#define ZZPLAY_FRAME_CLOCK_H

#include <stddef.h>
#include <stdint.h>

typedef enum ZZPlayFrameAction {
  ZZPLAY_FRAME_ACTION_WAIT = 0,     /* Waiting for frame deadline */
  ZZPLAY_FRAME_ACTION_ADVANCE = 1,  /* Frame deadline reached; advance to next frame */
  ZZPLAY_FRAME_ACTION_RESTART = 2,  /* Animation ended and repeat requested; restart */
  ZZPLAY_FRAME_ACTION_EOF = 3       /* Animation ended (native end); finish item */
} ZZPlayFrameAction;

typedef struct ZZPlayFrameClock {
  uint64_t start_us;
  uint64_t deadline_us;
  uint64_t paused_remaining_us;
  uint64_t total_elapsed_ms;
  uint32_t frame_index;
  uint32_t current_duration_ms;
  uint32_t effective_duration_ms;
  uint32_t max_lag_us;
  uint32_t frames_presented;
  uint32_t drift_resync_count;
  uint8_t started;
  uint8_t paused;
  uint8_t repeat_item;
  uint8_t ended;
} ZZPlayFrameClock;

void zzplay_frame_clock_init(ZZPlayFrameClock *clock, int repeat_item);
void zzplay_frame_clock_start(ZZPlayFrameClock *clock, uint64_t now_us,
                              uint32_t first_duration_ms);
void zzplay_frame_clock_advance(ZZPlayFrameClock *clock, uint64_t now_us,
                                uint32_t duration_ms);
void zzplay_frame_clock_pause(ZZPlayFrameClock *clock, uint64_t now_us);
void zzplay_frame_clock_resume(ZZPlayFrameClock *clock, uint64_t now_us);
uint32_t zzplay_frame_clock_remaining_us(const ZZPlayFrameClock *clock,
                                         uint64_t now_us);
uint32_t zzplay_frame_clock_wait_slice_us(const ZZPlayFrameClock *clock,
                                          uint64_t now_us,
                                          uint32_t max_slice_us);
ZZPlayFrameAction zzplay_frame_clock_poll(const ZZPlayFrameClock *clock,
                                          uint64_t now_us);
void zzplay_frame_clock_on_end(ZZPlayFrameClock *clock);
void zzplay_frame_clock_on_restart(ZZPlayFrameClock *clock, uint64_t now_us,
                                   uint32_t first_duration_ms);
void zzplay_frame_clock_set_repeat(ZZPlayFrameClock *clock, int repeat_item);

#endif /* ZZPLAY_FRAME_CLOCK_H */
