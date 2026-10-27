/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-audio-clock.h"

#include <string.h>

#define ZZPLAY_PTS_HZ 90000U
#define ZZPLAY_US_PER_SECOND 1000000U

static uint64_t zzplay_audio_frames_to_pts(uint64_t frames,
                                           uint32_t sample_rate,
                                           uint64_t origin_pts)
{
  uint64_t seconds;
  uint64_t remainder;

  if (sample_rate == 0U) {
    return origin_pts;
  }
  seconds = frames / sample_rate;
  remainder = frames % sample_rate;
  return origin_pts + seconds * ZZPLAY_PTS_HZ +
         (remainder * ZZPLAY_PTS_HZ) / sample_rate;
}

static uint64_t zzplay_audio_clock_presentation_horizon(
    const ZZPlayAudioClock *clock)
{
  return clock->submitted_frames >= clock->discarded_frames
             ? clock->submitted_frames - clock->discarded_frames
             : 0U;
}

void zzplay_audio_clock_prepare(ZZPlayAudioClock *clock,
                                uint32_t sample_rate,
                                uint32_t queue_limit_frames)
{
  if (!clock) {
    return;
  }
  memset(clock, 0, sizeof(*clock));
  clock->sample_rate = sample_rate;
  clock->queue_limit_frames = queue_limit_frames;
  clock->state = sample_rate == 0U
                     ? ZZPLAY_AUDIO_CLOCK_CLOSED
                     : ZZPLAY_AUDIO_CLOCK_PREBUFFERING;
}

int zzplay_audio_clock_queue(ZZPlayAudioClock *clock, uint32_t frames)
{
  if (!clock || frames == 0U ||
      (clock->state != ZZPLAY_AUDIO_CLOCK_PREBUFFERING &&
       clock->state != ZZPLAY_AUDIO_CLOCK_PLAYING &&
       clock->state != ZZPLAY_AUDIO_CLOCK_PAUSED)) {
    return 0;
  }
  if (clock->queue_limit_frames != 0U &&
      ((uint64_t)frames > clock->queue_limit_frames ||
       clock->queued_frames >
           (uint64_t)clock->queue_limit_frames - frames)) {
    return 0;
  }
  clock->submitted_frames += frames;
  clock->queued_frames += frames;
  return 1;
}

int zzplay_audio_clock_play(ZZPlayAudioClock *clock)
{
  if (!clock || clock->state != ZZPLAY_AUDIO_CLOCK_PREBUFFERING ||
      clock->queued_frames == 0U) {
    return 0;
  }
  clock->state = ZZPLAY_AUDIO_CLOCK_PLAYING;
  return 1;
}

int zzplay_audio_clock_pause(ZZPlayAudioClock *clock)
{
  if (!clock || clock->state != ZZPLAY_AUDIO_CLOCK_PLAYING) {
    return 0;
  }
  clock->presentation_active = 0U;
  clock->state = ZZPLAY_AUDIO_CLOCK_PAUSED;
  return 1;
}

int zzplay_audio_clock_resume(ZZPlayAudioClock *clock)
{
  if (!clock || clock->state != ZZPLAY_AUDIO_CLOCK_PAUSED) {
    return 0;
  }
  clock->state = ZZPLAY_AUDIO_CLOCK_PLAYING;
  return 1;
}

int zzplay_audio_clock_begin_drain(ZZPlayAudioClock *clock)
{
  if (!clock ||
      (clock->state != ZZPLAY_AUDIO_CLOCK_PLAYING &&
       clock->state != ZZPLAY_AUDIO_CLOCK_PREBUFFERING)) {
    return 0;
  }
  clock->state = ZZPLAY_AUDIO_CLOCK_DRAINING;
  return 1;
}

int zzplay_audio_clock_complete(ZZPlayAudioClock *clock,
                                uint32_t submitted_frames,
                                uint32_t played_frames)
{
  uint32_t discarded;
  uint64_t horizon;

  if (!clock || submitted_frames == 0U ||
      played_frames > submitted_frames ||
      clock->queued_frames < submitted_frames ||
      (clock->state != ZZPLAY_AUDIO_CLOCK_PLAYING &&
       clock->state != ZZPLAY_AUDIO_CLOCK_DRAINING)) {
    return 0;
  }
  discarded = submitted_frames - played_frames;
  clock->queued_frames -= submitted_frames;
  clock->played_frames += played_frames;
  clock->discarded_frames += discarded;
  horizon = zzplay_audio_clock_presentation_horizon(clock);
  if (clock->presentation_frames < clock->played_frames) {
    clock->presentation_frames = clock->played_frames;
  }
  if (clock->presentation_frames > horizon) {
    clock->presentation_frames = horizon;
    clock->presentation_fraction = 0U;
  }
  return 1;
}

void zzplay_audio_clock_start_presentation(ZZPlayAudioClock *clock,
                                           uint64_t now_us)
{
  if (!clock || clock->state != ZZPLAY_AUDIO_CLOCK_PLAYING) {
    return;
  }
  if (clock->presentation_frames < clock->played_frames) {
    clock->presentation_frames = clock->played_frames;
  }
  clock->presentation_last_us = now_us;
  clock->presentation_fraction = 0U;
  clock->presentation_active = 1U;
}

void zzplay_audio_clock_update_presentation(ZZPlayAudioClock *clock,
                                            uint64_t now_us)
{
  uint64_t elapsed_us;
  uint64_t elapsed_seconds;
  uint64_t elapsed_remainder;
  uint64_t scaled_remainder;
  uint64_t advance;
  uint64_t horizon;
  uint64_t room;

  if (!clock || !clock->presentation_active ||
      (clock->state != ZZPLAY_AUDIO_CLOCK_PLAYING &&
       clock->state != ZZPLAY_AUDIO_CLOCK_DRAINING)) {
    return;
  }
  if (now_us <= clock->presentation_last_us) {
    clock->presentation_last_us = now_us;
    return;
  }
  elapsed_us = now_us - clock->presentation_last_us;
  clock->presentation_last_us = now_us;
  elapsed_seconds = elapsed_us / ZZPLAY_US_PER_SECOND;
  elapsed_remainder = elapsed_us % ZZPLAY_US_PER_SECOND;
  advance = elapsed_seconds * clock->sample_rate;
  scaled_remainder =
      elapsed_remainder * clock->sample_rate +
      clock->presentation_fraction;
  advance += scaled_remainder / ZZPLAY_US_PER_SECOND;
  scaled_remainder %= ZZPLAY_US_PER_SECOND;

  horizon = zzplay_audio_clock_presentation_horizon(clock);
  if (clock->presentation_frames < clock->played_frames) {
    clock->presentation_frames = clock->played_frames;
  }
  if (clock->presentation_frames >= horizon) {
    clock->presentation_frames = horizon;
    clock->presentation_fraction = 0U;
    return;
  }
  room = horizon - clock->presentation_frames;
  if (advance >= room) {
    clock->presentation_frames = horizon;
    clock->presentation_fraction = 0U;
    return;
  }
  clock->presentation_frames += advance;
  clock->presentation_fraction = (uint32_t)scaled_remainder;
}

void zzplay_audio_clock_note_underrun(ZZPlayAudioClock *clock)
{
  if (clock && clock->state == ZZPLAY_AUDIO_CLOCK_PLAYING) {
    clock->underruns++;
  }
}

void zzplay_audio_clock_retract_underrun(ZZPlayAudioClock *clock)
{
  if (clock && clock->underruns != 0U) {
    clock->underruns--;
  }
}

int zzplay_audio_clock_drained(const ZZPlayAudioClock *clock)
{
  return clock && clock->state == ZZPLAY_AUDIO_CLOCK_DRAINING &&
         clock->queued_frames == 0U;
}

void zzplay_audio_clock_stop(ZZPlayAudioClock *clock)
{
  if (!clock || clock->state == ZZPLAY_AUDIO_CLOCK_CLOSED) {
    return;
  }
  clock->discarded_frames += clock->queued_frames;
  clock->queued_frames = 0U;
  clock->presentation_active = 0U;
  clock->state = ZZPLAY_AUDIO_CLOCK_STOPPED;
}

void zzplay_audio_clock_close(ZZPlayAudioClock *clock)
{
  if (!clock) {
    return;
  }
  zzplay_audio_clock_stop(clock);
  clock->state = ZZPLAY_AUDIO_CLOCK_CLOSED;
}

uint64_t zzplay_audio_clock_pts(const ZZPlayAudioClock *clock,
                                uint64_t origin_pts)
{
  if (!clock || clock->sample_rate == 0U) {
    return origin_pts;
  }
  return zzplay_audio_frames_to_pts(
      clock->played_frames, clock->sample_rate, origin_pts);
}

uint64_t zzplay_audio_clock_presentation_pts(
    const ZZPlayAudioClock *clock, uint64_t origin_pts)
{
  if (!clock || clock->sample_rate == 0U) {
    return origin_pts;
  }
  return zzplay_audio_frames_to_pts(
      clock->presentation_frames, clock->sample_rate, origin_pts);
}

uint32_t zzplay_output_elapsed_ms(uint64_t written_frames,
                                  uint64_t lead_frames,
                                  uint64_t origin_frames,
                                  uint32_t output_rate)
{
  uint64_t played;
  uint64_t milliseconds;

  if (output_rate == 0U || written_frames < lead_frames) {
    return 0U;
  }
  played = written_frames - lead_frames;
  if (played < origin_frames) {
    return 0U;
  }
  played -= origin_frames;
  milliseconds = played * 1000ULL / output_rate;
  return milliseconds > 0xffffffffULL ? 0xffffffffU
                                      : (uint32_t)milliseconds;
}

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
    defined(__VBCC__)
#include "zz9k/host.h"

int zzplay_output_clock_elapsed(struct ZZ9KContext *ctx,
                                uint64_t *origin_frames, int *armed,
                                uint32_t base_ms, uint32_t *elapsed_ms)
{
  ZZ9KAudioOutputClockResult clock;
  uint64_t written;
  int status;

  if (!ctx || !origin_frames || !armed || !elapsed_ms) {
    return 0;
  }
  status = zz9k_audio_output_clock_get(ctx, &clock);
  if (status != ZZ9K_STATUS_OK || clock.output_rate == 0U) {
    return 0;
  }
  if (!*armed) {
    *origin_frames = clock.retired_frames;
    *armed = 1;
  }
  written = clock.retired_frames + clock.lead_frames;
  *elapsed_ms = base_ms + zzplay_output_elapsed_ms(
      written, clock.lead_frames, *origin_frames, clock.output_rate);
  return 1;
}
#endif
