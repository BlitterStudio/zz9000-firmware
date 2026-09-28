/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "../tools/zzplay-ax.h"

#include <string.h>

typedef struct MockAX {
  uint64_t produced_bytes;
  uint64_t retired_frames;
  uint64_t queued_frames;
  uint64_t staged_frames;
  uint32_t underruns;
  uint32_t bind_calls;
  uint32_t unbind_calls;
  uint32_t status_calls;
  int next_status;
  uint8_t bound;
  uint8_t paused;
  uint8_t drained;
} MockAX;

static void mock_audio_result(const MockAX *mock,
                              ZZ9KMediaSessionAudioResult *result)
{
  memset(result, 0, sizeof(*result));
  result->session = 7U;
  result->state = ZZ9K_MEDIA_SESSION_STATE_READY;
  result->sample_rate = 44100U;
  result->channels = 2U;
  result->sample_format = ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE;
  result->pcm_produced = mock->produced_bytes;
  result->pcm_acknowledged = mock->retired_frames * 4U;
  result->audio_pts =
      54000U + mock->retired_frames * UINT64_C(90000) / 44100U;
  if (mock->bound) {
    result->flags |= ZZ9K_MEDIA_SESSION_RESULT_AUDIO_BOUND;
    if (!mock->paused) {
      result->flags |= ZZ9K_MEDIA_SESSION_RESULT_AUDIO_PLAYING;
    }
    if (mock->drained) {
      result->flags |= ZZ9K_MEDIA_SESSION_RESULT_AUDIO_DRAINED;
    }
    if (mock->underruns != 0U) {
      result->flags |= ZZ9K_MEDIA_SESSION_RESULT_AUDIO_UNDERRUN;
    }
  }
}

static int mock_bind(void *user, uint32_t session, uint32_t flags,
                     ZZ9KMediaSessionAudioResult *result)
{
  MockAX *mock = (MockAX *)user;

  if (session != 7U ||
      (flags & ~ZZ9K_MEDIA_AUDIO_BIND_PAUSE) != 0U) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (mock->next_status != ZZ9K_STATUS_OK) {
    int status = mock->next_status;

    mock->next_status = ZZ9K_STATUS_OK;
    return status;
  }
  mock->bind_calls++;
  mock->bound = 1U;
  mock->paused =
      (flags & ZZ9K_MEDIA_AUDIO_BIND_PAUSE) != 0U;
  if (mock->paused) {
    mock->queued_frames = 0U;
    mock->staged_frames = mock->retired_frames;
  }
  mock_audio_result(mock, result);
  return ZZ9K_STATUS_OK;
}

static int mock_unbind(void *user, uint32_t session, uint32_t flags,
                       ZZ9KMediaSessionAudioResult *result)
{
  MockAX *mock = (MockAX *)user;

  if (session != 7U || flags != 0U) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  mock->unbind_calls++;
  mock->bound = 0U;
  mock->paused = 0U;
  mock_audio_result(mock, result);
  return ZZ9K_STATUS_OK;
}

static int mock_status(void *user, uint32_t session, uint32_t page,
                       uint32_t flags,
                       ZZ9KMediaSessionStatusResult *result)
{
  MockAX *mock = (MockAX *)user;

  if (session != 7U || page != ZZ9K_MEDIA_STATUS_AUDIO_OUTPUT ||
      flags != 0U) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  mock->status_calls++;
  memset(result, 0, sizeof(*result));
  result->session = session;
  result->state = ZZ9K_MEDIA_SESSION_STATE_READY;
  result->page = page;
  result->value[0] = mock->retired_frames;
  result->value[1] = mock->queued_frames;
  result->value[2] = mock->staged_frames;
  result->value[3] = mock->underruns;
  if (mock->bound) {
    result->flags |= ZZ9K_MEDIA_SESSION_RESULT_AUDIO_BOUND;
    if (!mock->paused) {
      result->flags |= ZZ9K_MEDIA_SESSION_RESULT_AUDIO_PLAYING;
    }
    if (mock->drained) {
      result->flags |= ZZ9K_MEDIA_SESSION_RESULT_AUDIO_DRAINED;
    }
    if (mock->underruns != 0U) {
      result->flags |= ZZ9K_MEDIA_SESSION_RESULT_AUDIO_UNDERRUN;
    }
  }
  return ZZ9K_STATUS_OK;
}

static const ZZPlayAXControlOps mock_ops = {
  mock_bind,
  mock_unbind,
  mock_status
};

static int test_lifecycle_and_clock(void)
{
  ZZPlayAXSink sink;
  ZZ9KMediaSessionAudioResult audio;
  MockAX mock;
  uint64_t expected_pts;

  memset(&mock, 0, sizeof(mock));
  mock.produced_bytes = 9216U;
  mock.next_status = ZZ9K_STATUS_OK;
  mock_audio_result(&mock, &audio);
  zzplay_ax_init(&sink, 7U, &mock_ops, &mock);
  if (zzplay_ax_prepare(&sink, &audio) != ZZ9K_STATUS_OK ||
      sink.frame_bytes != 4U || sink.bound)
    return 1;
  if (zzplay_ax_play(&sink) != ZZ9K_STATUS_OK ||
      !sink.bound || sink.paused || mock.bind_calls != 1U ||
      mock.status_calls != 1U)
    return 2;

  mock.retired_frames = 576U;
  mock.queued_frames = 960U;
  mock.staged_frames = 1536U;
  mock.underruns = 1U;
  if (zzplay_ax_poll(&sink) != ZZ9K_STATUS_OK ||
      zzplay_ax_played_frames(&sink) != 576U ||
      zzplay_ax_queued_frames(&sink) != 960U ||
      sink.underruns != 1U)
    return 3;
  expected_pts =
      54000U + 576U * UINT64_C(90000) / 44100U;
  if (zzplay_ax_clock_pts(&sink, 54000U) != expected_pts)
    return 4;

  if (zzplay_ax_pause(&sink) != ZZ9K_STATUS_OK ||
      !sink.paused || sink.dma_queued_frames != 0U ||
      sink.staged_frames != sink.retired_frames)
    return 5;
  if (zzplay_ax_resume(&sink) != ZZ9K_STATUS_OK || sink.paused)
    return 6;

  mock.queued_frames = 0U;
  mock.staged_frames = mock.retired_frames;
  mock.drained = 1U;
  if (zzplay_ax_begin_drain(&sink) != ZZ9K_STATUS_OK ||
      !zzplay_ax_drained(&sink))
    return 7;
  if (zzplay_ax_stop(&sink) != ZZ9K_STATUS_OK || sink.bound ||
      mock.unbind_calls != 1U ||
      zzplay_ax_close(&sink) != ZZ9K_STATUS_OK ||
      mock.unbind_calls != 1U)
    return 8;
  return 0;
}

static int test_rejections(void)
{
  ZZPlayAXSink sink;
  ZZ9KMediaSessionAudioResult audio;
  MockAX mock;

  memset(&mock, 0, sizeof(mock));
  mock.produced_bytes = 9216U;
  mock.next_status = ZZ9K_STATUS_OK;
  mock_audio_result(&mock, &audio);
  zzplay_ax_init(&sink, 7U, &mock_ops, &mock);
  audio.sample_format = ZZ9K_AUDIO_SAMPLE_FORMAT_S16LE;
  if (zzplay_ax_prepare(&sink, &audio) != ZZ9K_STATUS_UNSUPPORTED)
    return 1;
  mock_audio_result(&mock, &audio);
  if (zzplay_ax_prepare(&sink, &audio) != ZZ9K_STATUS_OK)
    return 2;
  mock.next_status = ZZ9K_STATUS_BUSY;
  if (zzplay_ax_play(&sink) != ZZ9K_STATUS_BUSY || sink.bound)
    return 3;
  if (zzplay_ax_play(&sink) != ZZ9K_STATUS_OK)
    return 4;
  mock.staged_frames = 10U;
  mock.queued_frames = 9U;
  if (zzplay_ax_poll(&sink) != ZZ9K_STATUS_INTERNAL_ERROR)
    return 5;
  return 0;
}

int main(void)
{
  int status;

  status = test_lifecycle_and_clock();
  if (status != 0) return 10 + status;
  status = test_rejections();
  if (status != 0) return 30 + status;
  return 0;
}
