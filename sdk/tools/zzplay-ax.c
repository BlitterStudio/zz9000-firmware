/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-ax.h"

#include <string.h>

static int zzplay_ax_audio_valid(
    const ZZPlayAXSink *sink,
    const ZZ9KMediaSessionAudioResult *audio)
{
  return sink && audio && audio->session == sink->session &&
         audio->sample_rate == sink->sample_rate &&
         audio->channels == sink->channels &&
         audio->sample_format == ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE &&
         audio->pcm_produced >= audio->pcm_acknowledged &&
         audio->pcm_produced % sink->frame_bytes == 0U &&
         audio->pcm_acknowledged % sink->frame_bytes == 0U;
}

static int zzplay_ax_apply_audio(
    ZZPlayAXSink *sink,
    const ZZ9KMediaSessionAudioResult *audio)
{
  uint64_t retired;

  if (!zzplay_ax_audio_valid(sink, audio)) {
    return ZZ9K_STATUS_INTERNAL_ERROR;
  }
  retired = audio->pcm_acknowledged / sink->frame_bytes;
  if (sink->bound && retired < sink->retired_frames) {
    return ZZ9K_STATUS_INTERNAL_ERROR;
  }
  sink->audio = *audio;
  sink->retired_frames = retired;
  sink->bound =
      (audio->flags & ZZ9K_MEDIA_SESSION_RESULT_AUDIO_BOUND) != 0U;
  sink->paused =
      sink->bound &&
      (audio->flags & ZZ9K_MEDIA_SESSION_RESULT_AUDIO_PLAYING) == 0U;
  sink->drained =
      (audio->flags & ZZ9K_MEDIA_SESSION_RESULT_AUDIO_DRAINED) != 0U;
  return ZZ9K_STATUS_OK;
}

static int zzplay_ax_control(ZZPlayAXSink *sink, uint32_t flags)
{
  ZZ9KMediaSessionAudioResult audio;
  int status;

  if (!sink || !sink->prepared || !sink->ops ||
      !sink->ops->bind) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  memset(&audio, 0, sizeof(audio));
  status = sink->ops->bind(
      sink->user, sink->session, flags, &audio);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  status = zzplay_ax_apply_audio(sink, &audio);
  if (status != ZZ9K_STATUS_OK || !sink->bound) {
    return status != ZZ9K_STATUS_OK
               ? status
               : ZZ9K_STATUS_INTERNAL_ERROR;
  }
  return ZZ9K_STATUS_OK;
}

void zzplay_ax_init(ZZPlayAXSink *sink, uint32_t session,
                    const ZZPlayAXControlOps *ops, void *user)
{
  if (!sink) {
    return;
  }
  memset(sink, 0, sizeof(*sink));
  sink->session = session;
  sink->ops = ops;
  sink->user = user;
}

int zzplay_ax_prepare(ZZPlayAXSink *sink,
                      const ZZ9KMediaSessionAudioResult *audio)
{
  if (!sink || !audio || sink->session == 0U ||
      audio->session != sink->session ||
      audio->sample_rate == 0U ||
      (audio->channels != 1U && audio->channels != 2U) ||
      audio->sample_format != ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  if (sink->prepared) {
    return audio->sample_rate == sink->sample_rate &&
                   audio->channels == sink->channels
               ? ZZ9K_STATUS_OK
               : ZZ9K_STATUS_UNSUPPORTED;
  }
  sink->sample_rate = audio->sample_rate;
  sink->channels = audio->channels;
  sink->frame_bytes = audio->channels * 2U;
  sink->prepared = 1U;
  return zzplay_ax_apply_audio(sink, audio);
}

int zzplay_ax_play(ZZPlayAXSink *sink)
{
  int status = zzplay_ax_control(sink, 0U);

  if (status != ZZ9K_STATUS_OK || sink->paused) {
    return status != ZZ9K_STATUS_OK
               ? status
               : ZZ9K_STATUS_INTERNAL_ERROR;
  }
  return zzplay_ax_poll(sink);
}

int zzplay_ax_poll(ZZPlayAXSink *sink)
{
  ZZ9KMediaSessionStatusResult result;
  uint64_t retired;
  uint64_t queued;
  uint64_t staged;
  int status;

  if (!sink || !sink->prepared || !sink->bound ||
      !sink->ops || !sink->ops->status) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  memset(&result, 0, sizeof(result));
  status = sink->ops->status(
      sink->user, sink->session, ZZ9K_MEDIA_STATUS_AUDIO_OUTPUT,
      0U, &result);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  retired = result.value[0];
  queued = result.value[1];
  staged = result.value[2];
  if (result.session != sink->session ||
      result.page != ZZ9K_MEDIA_STATUS_AUDIO_OUTPUT ||
      retired < sink->retired_frames || staged < retired ||
      staged - retired != queued) {
    return ZZ9K_STATUS_INTERNAL_ERROR;
  }
  sink->retired_frames = retired;
  sink->dma_queued_frames = queued;
  sink->staged_frames = staged;
  sink->underruns =
      result.value[3] > UINT32_MAX
          ? UINT32_MAX
          : (uint32_t)result.value[3];
  sink->audio.pcm_acknowledged =
      retired * sink->frame_bytes;
  sink->audio.flags = result.flags;
  sink->paused =
      (result.flags & ZZ9K_MEDIA_SESSION_RESULT_AUDIO_PLAYING) == 0U;
  sink->drained =
      (result.flags & ZZ9K_MEDIA_SESSION_RESULT_AUDIO_DRAINED) != 0U;
  if ((result.flags & ZZ9K_MEDIA_SESSION_RESULT_AUDIO_BOUND) == 0U) {
    return ZZ9K_STATUS_INTERNAL_ERROR;
  }
  return ZZ9K_STATUS_OK;
}

int zzplay_ax_pause(ZZPlayAXSink *sink)
{
  int status = zzplay_ax_control(
      sink, ZZ9K_MEDIA_AUDIO_BIND_PAUSE);

  if (status == ZZ9K_STATUS_OK && !sink->paused) {
    return ZZ9K_STATUS_INTERNAL_ERROR;
  }
  if (status == ZZ9K_STATUS_OK) {
    sink->dma_queued_frames = 0U;
    sink->staged_frames = sink->retired_frames;
  }
  return status;
}

int zzplay_ax_resume(ZZPlayAXSink *sink)
{
  int status = zzplay_ax_control(sink, 0U);

  if (status == ZZ9K_STATUS_OK && sink->paused) {
    return ZZ9K_STATUS_INTERNAL_ERROR;
  }
  return status;
}

int zzplay_ax_begin_drain(ZZPlayAXSink *sink)
{
  if (!sink || !sink->bound) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  sink->draining = 1U;
  return zzplay_ax_poll(sink);
}

int zzplay_ax_stop(ZZPlayAXSink *sink)
{
  ZZ9KMediaSessionAudioResult audio;
  int status;

  if (!sink || !sink->prepared) {
    return ZZ9K_STATUS_OK;
  }
  if (!sink->bound) {
    return ZZ9K_STATUS_OK;
  }
  if (!sink->ops || !sink->ops->unbind) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  memset(&audio, 0, sizeof(audio));
  status = sink->ops->unbind(
      sink->user, sink->session, 0U, &audio);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  if (audio.session != sink->session ||
      (audio.flags & ZZ9K_MEDIA_SESSION_RESULT_AUDIO_BOUND) != 0U) {
    return ZZ9K_STATUS_INTERNAL_ERROR;
  }
  sink->audio = audio;
  sink->bound = 0U;
  sink->paused = 0U;
  sink->draining = 0U;
  sink->dma_queued_frames = 0U;
  return ZZ9K_STATUS_OK;
}

int zzplay_ax_close(ZZPlayAXSink *sink)
{
  int status;

  if (!sink) {
    return ZZ9K_STATUS_OK;
  }
  status = zzplay_ax_stop(sink);
  if (status == ZZ9K_STATUS_OK) {
    sink->prepared = 0U;
  }
  return status;
}

uint64_t zzplay_ax_played_frames(const ZZPlayAXSink *sink)
{
  return sink ? sink->retired_frames : 0U;
}

uint64_t zzplay_ax_queued_frames(const ZZPlayAXSink *sink)
{
  return sink ? sink->dma_queued_frames : 0U;
}

uint64_t zzplay_ax_clock_pts(const ZZPlayAXSink *sink,
                             uint64_t origin_pts)
{
  uint64_t whole;
  uint64_t remainder;

  if (!sink || sink->sample_rate == 0U ||
      origin_pts == ZZ9K_MEDIA_NO_PTS) {
    return ZZ9K_MEDIA_NO_PTS;
  }
  whole = sink->retired_frames / sink->sample_rate;
  remainder = sink->retired_frames % sink->sample_rate;
  return origin_pts + whole * UINT64_C(90000) +
         remainder * UINT64_C(90000) / sink->sample_rate;
}

int zzplay_ax_drained(const ZZPlayAXSink *sink)
{
  return sink && sink->draining && sink->drained &&
         sink->dma_queued_frames == 0U;
}
