/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "../tools/zzplay-audio-clock.h"

#include <stdint.h>

static int check_completion_clock(void)
{
  ZZPlayAudioClock clock;

  zzplay_audio_clock_prepare(&clock, 48000U, 4096U);
  if (clock.state != ZZPLAY_AUDIO_CLOCK_PREBUFFERING ||
      !zzplay_audio_clock_queue(&clock, 960U) ||
      clock.played_frames != 0U ||
      zzplay_audio_clock_pts(&clock, 9000U) != 9000U ||
      !zzplay_audio_clock_play(&clock)) {
    return 0;
  }
  if (!zzplay_audio_clock_complete(&clock, 960U, 960U) ||
      clock.played_frames != 960U ||
      clock.queued_frames != 0U ||
      zzplay_audio_clock_pts(&clock, 9000U) != 10800U) {
    return 0;
  }
  return 1;
}

static int check_pause_drain_and_short_completion(void)
{
  ZZPlayAudioClock clock;

  zzplay_audio_clock_prepare(&clock, 44100U, 4096U);
  if (!zzplay_audio_clock_queue(&clock, 1000U) ||
      !zzplay_audio_clock_play(&clock) ||
      !zzplay_audio_clock_pause(&clock) ||
      zzplay_audio_clock_complete(&clock, 1000U, 1000U) ||
      clock.played_frames != 0U ||
      !zzplay_audio_clock_resume(&clock) ||
      !zzplay_audio_clock_complete(&clock, 1000U, 900U) ||
      clock.played_frames != 900U ||
      clock.discarded_frames != 100U ||
      !zzplay_audio_clock_begin_drain(&clock) ||
      !zzplay_audio_clock_drained(&clock)) {
    return 0;
  }
  return 1;
}

static int check_bounds_and_lifecycle(void)
{
  ZZPlayAudioClock clock;

  zzplay_audio_clock_prepare(&clock, 32000U, 100U);
  if (zzplay_audio_clock_queue(&clock, 101U) ||
      !zzplay_audio_clock_queue(&clock, 60U) ||
      zzplay_audio_clock_queue(&clock, 41U) ||
      !zzplay_audio_clock_play(&clock)) {
    return 0;
  }
  zzplay_audio_clock_note_underrun(&clock);
  if (clock.underruns != 1U) {
    return 0;
  }
  zzplay_audio_clock_retract_underrun(&clock);
  zzplay_audio_clock_retract_underrun(&clock);
  if (clock.underruns != 0U) {
    return 0;
  }
  zzplay_audio_clock_stop(&clock);
  if (clock.state != ZZPLAY_AUDIO_CLOCK_STOPPED ||
      clock.queued_frames != 0U ||
      clock.discarded_frames != 60U) {
    return 0;
  }
  zzplay_audio_clock_close(&clock);
  return clock.state == ZZPLAY_AUDIO_CLOCK_CLOSED;
}

static int check_smooth_bounded_presentation(void)
{
  ZZPlayAudioClock clock;

  zzplay_audio_clock_prepare(&clock, 48000U, 9600U);
  if (!zzplay_audio_clock_queue(&clock, 9600U) ||
      !zzplay_audio_clock_play(&clock)) {
    return 0;
  }
  zzplay_audio_clock_start_presentation(&clock, 1000000U);
  zzplay_audio_clock_update_presentation(&clock, 1025000U);
  if (clock.presentation_frames != 1200U ||
      zzplay_audio_clock_presentation_pts(&clock, 9000U) != 11250U) {
    return 0;
  }
  zzplay_audio_clock_update_presentation(&clock, 1100000U);
  if (clock.presentation_frames != 4800U ||
      !zzplay_audio_clock_complete(&clock, 4800U, 4800U) ||
      clock.presentation_frames != 4800U) {
    return 0;
  }
  zzplay_audio_clock_update_presentation(&clock, 1150000U);
  return clock.presentation_frames == 7200U &&
         zzplay_audio_clock_presentation_pts(&clock, 9000U) == 22500U;
}

static int check_presentation_discards_starved_time(void)
{
  ZZPlayAudioClock clock;

  zzplay_audio_clock_prepare(&clock, 1000U, 200U);
  if (!zzplay_audio_clock_queue(&clock, 100U) ||
      !zzplay_audio_clock_play(&clock)) {
    return 0;
  }
  zzplay_audio_clock_start_presentation(&clock, 0U);
  zzplay_audio_clock_update_presentation(&clock, 200000U);
  if (clock.presentation_frames != 100U ||
      !zzplay_audio_clock_queue(&clock, 100U)) {
    return 0;
  }
  zzplay_audio_clock_update_presentation(&clock, 250000U);
  if (clock.presentation_frames != 150U) {
    return 0;
  }
  zzplay_audio_clock_update_presentation(&clock, 400000U);
  return clock.presentation_frames == 200U;
}

static int check_completion_reconciles_presentation(void)
{
  ZZPlayAudioClock clock;

  zzplay_audio_clock_prepare(&clock, 1000U, 1000U);
  if (!zzplay_audio_clock_queue(&clock, 1000U) ||
      !zzplay_audio_clock_play(&clock)) {
    return 0;
  }
  zzplay_audio_clock_start_presentation(&clock, 0U);
  zzplay_audio_clock_update_presentation(&clock, 800000U);
  if (clock.presentation_frames != 800U ||
      !zzplay_audio_clock_complete(&clock, 1000U, 900U)) {
    return 0;
  }
  return clock.presentation_frames == 900U &&
         zzplay_audio_clock_presentation_pts(&clock, 0U) == 81000U;
}

static int check_pause_excludes_wall_time(void)
{
  ZZPlayAudioClock clock;

  zzplay_audio_clock_prepare(&clock, 1000U, 1000U);
  if (!zzplay_audio_clock_queue(&clock, 1000U) ||
      !zzplay_audio_clock_play(&clock)) {
    return 0;
  }
  zzplay_audio_clock_start_presentation(&clock, 1000000U);
  zzplay_audio_clock_update_presentation(&clock, 1200000U);
  if (clock.presentation_frames != 200U ||
      !zzplay_audio_clock_pause(&clock)) {
    return 0;
  }
  zzplay_audio_clock_update_presentation(&clock, 11200000U);
  if (clock.presentation_frames != 200U ||
      !zzplay_audio_clock_resume(&clock)) {
    return 0;
  }
  zzplay_audio_clock_start_presentation(&clock, 11200000U);
  zzplay_audio_clock_update_presentation(&clock, 11300000U);
  return clock.presentation_frames == 300U;
}

int main(void)
{
  if (!check_completion_clock()) {
    return 1;
  }
  if (!check_pause_drain_and_short_completion()) {
    return 2;
  }
  if (!check_bounds_and_lifecycle()) {
    return 3;
  }
  if (!check_smooth_bounded_presentation()) {
    return 4;
  }
  if (!check_presentation_discards_starved_time()) {
    return 5;
  }
  if (!check_completion_reconciles_presentation()) {
    return 6;
  }
  if (!check_pause_excludes_wall_time()) {
    return 7;
  }
  return 0;
}
