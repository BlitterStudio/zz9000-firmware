/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-ahi.h"

#include <devices/ahi.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include <string.h>

#define ZZPLAY_AHI_VERSION 4U

static int zzplay_ahi_has_pending(const ZZPlayAHISink *sink)
{
  unsigned i;

  for (i = 0U; i < ZZPLAY_AHI_BUFFER_COUNT; i++) {
    if (sink->buffer[i].pending) {
      return 1;
    }
  }
  return 0;
}

static int zzplay_ahi_has_ready(const ZZPlayAHISink *sink)
{
  unsigned i;

  for (i = 0U; i < ZZPLAY_AHI_BUFFER_COUNT; i++) {
    if (sink->buffer[i].ready) {
      return 1;
    }
  }
  return 0;
}

static struct AHIRequest *zzplay_ahi_predecessor(
    ZZPlayAHISink *sink, unsigned slot)
{
  unsigned other;

  for (other = 0U; other < ZZPLAY_AHI_BUFFER_COUNT; other++) {
    if (other != slot && sink->buffer[other].pending) {
      return sink->buffer[other].request;
    }
  }
  return 0;
}

static void zzplay_ahi_configure(ZZPlayAHISink *sink, unsigned slot)
{
  struct AHIRequest *request = sink->buffer[slot].request;

  request->ahir_Std.io_Message.mn_ReplyPort = sink->port;
  request->ahir_Std.io_Command = CMD_WRITE;
  request->ahir_Std.io_Error = 0;
  request->ahir_Std.io_Actual = 0U;
  request->ahir_Std.io_Data = sink->buffer[slot].samples;
  request->ahir_Std.io_Length =
      sink->buffer[slot].frames * sink->frame_bytes;
  request->ahir_Std.io_Offset = 0U;
  request->ahir_Version = ZZPLAY_AHI_VERSION;
  request->ahir_Type =
      sink->channels == 1U ? AHIST_M16S : AHIST_S16S;
  request->ahir_Frequency = sink->sample_rate;
  request->ahir_Volume = 0x10000L;
  request->ahir_Position = 0x8000L;
  request->ahir_Link = zzplay_ahi_predecessor(sink, slot);
}

static void zzplay_ahi_send(ZZPlayAHISink *sink, unsigned slot)
{
  zzplay_ahi_configure(sink, slot);
  sink->buffer[slot].ready = 0U;
  sink->buffer[slot].pending = 1U;
  SendIO((struct IORequest *)sink->buffer[slot].request);
}

static void zzplay_ahi_send_ready(ZZPlayAHISink *sink)
{
  unsigned i;

  for (i = 0U; i < ZZPLAY_AHI_BUFFER_COUNT; i++) {
    if (sink->buffer[i].ready && !sink->buffer[i].pending) {
      zzplay_ahi_send(sink, i);
    }
  }
}

static int zzplay_ahi_control(ZZPlayAHISink *sink, UWORD command)
{
  if (!sink || !sink->control || !sink->device_open) {
    return 0;
  }
  sink->control->ahir_Std.io_Command = command;
  sink->control->ahir_Std.io_Data = 0;
  sink->control->ahir_Std.io_Length = 0U;
  sink->control->ahir_Std.io_Offset = 0U;
  if (DoIO((struct IORequest *)sink->control) != 0) {
    sink->last_error = sink->control->ahir_Std.io_Error;
    return 0;
  }
  return 1;
}

int zzplay_ahi_prepare(ZZPlayAHISink *sink,
                       uint32_t sample_rate,
                       uint32_t channels,
                       uint32_t period_frames)
{
  size_t period_bytes;
  unsigned i;
  int failure = -1;

  if (!sink || sample_rate == 0U ||
      (channels != 1U && channels != 2U) ||
      period_frames == 0U) {
    return 0;
  }
  memset(sink, 0, sizeof(*sink));
  sink->fill_slot = -1;
  sink->sample_rate = sample_rate;
  sink->channels = channels;
  sink->frame_bytes = channels * 2U;
  sink->period_frames = period_frames;
  period_bytes = (size_t)period_frames * sink->frame_bytes;

  sink->port = CreateMsgPort();
  if (!sink->port) {
    goto fail;
  }
  sink->control = (struct AHIRequest *)CreateIORequest(
      sink->port, sizeof(*sink->control));
  if (!sink->control) {
    goto fail;
  }
  sink->control->ahir_Version = ZZPLAY_AHI_VERSION;
  if (OpenDevice((CONST_STRPTR)AHINAME, AHI_DEFAULT_UNIT,
                 (struct IORequest *)sink->control, 0U) != 0) {
    failure = sink->control->ahir_Std.io_Error;
    goto fail;
  }
  sink->device_open = 1U;

  for (i = 0U; i < ZZPLAY_AHI_BUFFER_COUNT; i++) {
    sink->buffer[i].request =
        (struct AHIRequest *)CreateIORequest(
            sink->port, sizeof(*sink->buffer[i].request));
    if (!sink->buffer[i].request) {
      goto fail;
    }
    CopyMem(sink->control, sink->buffer[i].request,
            sizeof(*sink->control));
    sink->buffer[i].request->ahir_Std.io_Message.mn_ReplyPort =
        sink->port;
    sink->buffer[i].samples =
        AllocVec((ULONG)period_bytes, MEMF_PUBLIC);
    if (!sink->buffer[i].samples) {
      goto fail;
    }
  }
  zzplay_audio_clock_prepare(
      &sink->clock, sample_rate,
      period_frames * ZZPLAY_AHI_BUFFER_COUNT);
  return 1;

fail:
  zzplay_ahi_close(sink);
  sink->last_error = failure;
  return 0;
}

void *zzplay_ahi_acquire_buffer(ZZPlayAHISink *sink,
                                size_t *capacity_bytes)
{
  unsigned i;

  if (capacity_bytes) {
    *capacity_bytes = 0U;
  }
  if (!sink || sink->fill_slot >= 0) {
    return 0;
  }
  for (i = 0U; i < ZZPLAY_AHI_BUFFER_COUNT; i++) {
    if (!sink->buffer[i].ready && !sink->buffer[i].pending) {
      sink->fill_slot = (int)i;
      if (capacity_bytes) {
        *capacity_bytes =
            (size_t)sink->period_frames * sink->frame_bytes;
      }
      return sink->buffer[i].samples;
    }
  }
  return 0;
}

int zzplay_ahi_submit_buffer(ZZPlayAHISink *sink, size_t bytes)
{
  ZZPlayAHIBuffer *buffer;
  uint32_t frames;

  if (!sink || sink->fill_slot < 0 || bytes == 0U ||
      bytes > (size_t)sink->period_frames * sink->frame_bytes ||
      bytes % sink->frame_bytes != 0U) {
    return 0;
  }
  buffer = &sink->buffer[(unsigned)sink->fill_slot];
  frames = (uint32_t)(bytes / sink->frame_bytes);
  if (!zzplay_audio_clock_queue(&sink->clock, frames)) {
    return 0;
  }
  buffer->frames = frames;
  buffer->ready = 1U;
  sink->fill_slot = -1;
  sink->underrun_active = 0U;
  if (sink->clock.state == ZZPLAY_AUDIO_CLOCK_PLAYING) {
    zzplay_ahi_send_ready(sink);
  }
  return 1;
}

int zzplay_ahi_play(ZZPlayAHISink *sink)
{
  if (!sink || !zzplay_audio_clock_play(&sink->clock)) {
    return 0;
  }
  zzplay_ahi_send_ready(sink);
  return zzplay_ahi_has_pending(sink);
}

int zzplay_ahi_poll(ZZPlayAHISink *sink)
{
  unsigned i;

  if (!sink) {
    return 0;
  }
  for (i = 0U; i < ZZPLAY_AHI_BUFFER_COUNT; i++) {
    ZZPlayAHIBuffer *buffer = &sink->buffer[i];
    struct AHIRequest *request = buffer->request;
    uint32_t actual_frames;
    uint32_t submitted_frames;

    if (!buffer->pending ||
        !CheckIO((struct IORequest *)request)) {
      continue;
    }
    (void)WaitIO((struct IORequest *)request);
    buffer->pending = 0U;
    submitted_frames = buffer->frames;
    actual_frames =
        request->ahir_Std.io_Actual / sink->frame_bytes;
    if (request->ahir_Std.io_Actual % sink->frame_bytes != 0U) {
      sink->last_error = -1;
      return 0;
    }
    if (actual_frames > submitted_frames) {
      sink->last_error = -1;
      return 0;
    }
    if (!zzplay_audio_clock_complete(
            &sink->clock, submitted_frames, actual_frames)) {
      sink->last_error = -1;
      return 0;
    }
    buffer->frames = 0U;
    if (request->ahir_Std.io_Error != 0) {
      sink->last_error = request->ahir_Std.io_Error;
      return 0;
    }
    if (actual_frames != submitted_frames) {
      sink->last_error = -1;
      return 0;
    }
  }
  if (sink->clock.state == ZZPLAY_AUDIO_CLOCK_PLAYING &&
      !sink->end_of_stream &&
      !zzplay_ahi_has_pending(sink) &&
      !zzplay_ahi_has_ready(sink) &&
      sink->clock.queued_frames == 0U) {
    if (!sink->underrun_active) {
      zzplay_audio_clock_note_underrun(&sink->clock);
      sink->underrun_active = 1U;
    }
  }
  return 1;
}

int zzplay_ahi_pause(ZZPlayAHISink *sink)
{
  if (!sink || sink->clock.state != ZZPLAY_AUDIO_CLOCK_PLAYING ||
      !zzplay_ahi_control(sink, CMD_STOP)) {
    return 0;
  }
  return zzplay_audio_clock_pause(&sink->clock);
}

int zzplay_ahi_resume(ZZPlayAHISink *sink)
{
  if (!sink || sink->clock.state != ZZPLAY_AUDIO_CLOCK_PAUSED ||
      !zzplay_ahi_control(sink, CMD_START)) {
    return 0;
  }
  return zzplay_audio_clock_resume(&sink->clock);
}

int zzplay_ahi_begin_drain(ZZPlayAHISink *sink)
{
  if (!sink || sink->fill_slot >= 0) {
    return 0;
  }
  zzplay_ahi_send_ready(sink);
  return zzplay_audio_clock_begin_drain(&sink->clock);
}

void zzplay_ahi_mark_end_of_stream(ZZPlayAHISink *sink)
{
  if (!sink || sink->end_of_stream) {
    return;
  }
  /* AHI completion can be polled once after the final request empties but
   * before the decoder reports EOF. That terminal empty queue is not a gap. */
  if (sink->underrun_active &&
      !zzplay_ahi_has_pending(sink) &&
      !zzplay_ahi_has_ready(sink) &&
      sink->clock.queued_frames == 0U) {
    zzplay_audio_clock_retract_underrun(&sink->clock);
    sink->underrun_active = 0U;
  }
  sink->end_of_stream = 1U;
}

int zzplay_ahi_drained(const ZZPlayAHISink *sink)
{
  return sink && zzplay_audio_clock_drained(&sink->clock) &&
         !zzplay_ahi_has_pending(sink) &&
         !zzplay_ahi_has_ready(sink);
}

void zzplay_ahi_stop(ZZPlayAHISink *sink)
{
  unsigned i;

  if (!sink) {
    return;
  }
  if (sink->clock.state == ZZPLAY_AUDIO_CLOCK_PAUSED) {
    (void)zzplay_ahi_control(sink, CMD_START);
  }
  for (i = 0U; i < ZZPLAY_AHI_BUFFER_COUNT; i++) {
    ZZPlayAHIBuffer *buffer = &sink->buffer[i];

    if (buffer->pending && buffer->request) {
      if (!CheckIO((struct IORequest *)buffer->request)) {
        AbortIO((struct IORequest *)buffer->request);
      }
      (void)WaitIO((struct IORequest *)buffer->request);
    }
    buffer->pending = 0U;
    buffer->ready = 0U;
    buffer->frames = 0U;
  }
  sink->fill_slot = -1;
  zzplay_audio_clock_stop(&sink->clock);
}

void zzplay_ahi_close(ZZPlayAHISink *sink)
{
  unsigned i;

  if (!sink) {
    return;
  }
  zzplay_ahi_stop(sink);
  for (i = 0U; i < ZZPLAY_AHI_BUFFER_COUNT; i++) {
    if (sink->buffer[i].samples) {
      FreeVec(sink->buffer[i].samples);
    }
    if (sink->buffer[i].request) {
      DeleteIORequest(
          (struct IORequest *)sink->buffer[i].request);
    }
  }
  if (sink->control) {
    if (sink->device_open) {
      CloseDevice((struct IORequest *)sink->control);
    }
    DeleteIORequest((struct IORequest *)sink->control);
  }
  if (sink->port) {
    DeleteMsgPort(sink->port);
  }
  memset(sink, 0, sizeof(*sink));
  sink->fill_slot = -1;
}

uint64_t zzplay_ahi_played_frames(const ZZPlayAHISink *sink)
{
  return sink ? sink->clock.played_frames : 0U;
}

uint64_t zzplay_ahi_queued_frames(const ZZPlayAHISink *sink)
{
  return sink ? sink->clock.queued_frames : 0U;
}
