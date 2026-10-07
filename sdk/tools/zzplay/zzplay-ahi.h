/*
 * ahi.device PCM sink for zzplay.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZPLAY_AHI_H
#define ZZPLAY_AHI_H

#include "zzplay-audio-clock.h"

#include <stddef.h>
#include <stdint.h>

struct AHIRequest;
struct MsgPort;

#define ZZPLAY_AHI_BUFFER_COUNT 2U

typedef struct ZZPlayAHIBuffer {
  struct AHIRequest *request;
  void *samples;
  uint32_t frames;
  uint8_t ready;
  uint8_t pending;
} ZZPlayAHIBuffer;

typedef struct ZZPlayAHISink {
  struct MsgPort *port;
  struct AHIRequest *control;
  ZZPlayAHIBuffer buffer[ZZPLAY_AHI_BUFFER_COUNT];
  ZZPlayAudioClock clock;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t frame_bytes;
  uint32_t period_frames;
  /* Fixed-point 0x10000 == 100%: applied to ahir_Volume of every request
   * configured from here on. Already-sent requests keep the volume they
   * were sent with; AHI has no per-request update call. */
  uint32_t volume_fixed;
  uint32_t unit;
  int fill_slot;
  int last_error;
  uint8_t device_open;
  uint8_t underrun_active;
  uint8_t end_of_stream;
} ZZPlayAHISink;

/* Open ahi.device on `unit` (0..3, the unit the AHI preferences editor
 * binds an audio mode to) and allocate the double-buffered sink. */
int zzplay_ahi_prepare(ZZPlayAHISink *sink,
                       uint32_t unit,
                       uint32_t sample_rate,
                       uint32_t channels,
                       uint32_t period_frames);
/* Store the output volume (0..100 percent) used for subsequently sent
 * requests. Values above 100 are clamped. */
void zzplay_ahi_set_volume(ZZPlayAHISink *sink, uint32_t percent);
void *zzplay_ahi_acquire_buffer(ZZPlayAHISink *sink,
                                size_t *capacity_bytes);
int zzplay_ahi_submit_buffer(ZZPlayAHISink *sink, size_t bytes);
int zzplay_ahi_play(ZZPlayAHISink *sink);
int zzplay_ahi_poll(ZZPlayAHISink *sink);
int zzplay_ahi_pause(ZZPlayAHISink *sink);
int zzplay_ahi_resume(ZZPlayAHISink *sink);
int zzplay_ahi_begin_drain(ZZPlayAHISink *sink);
void zzplay_ahi_mark_end_of_stream(ZZPlayAHISink *sink);
int zzplay_ahi_drained(const ZZPlayAHISink *sink);
void zzplay_ahi_stop(ZZPlayAHISink *sink);
void zzplay_ahi_close(ZZPlayAHISink *sink);
uint64_t zzplay_ahi_played_frames(const ZZPlayAHISink *sink);
uint64_t zzplay_ahi_queued_frames(const ZZPlayAHISink *sink);

#endif /* ZZPLAY_AHI_H */
