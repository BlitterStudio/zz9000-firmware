/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-mp3.h"

#include "zz9k/audio.h"
#include "zz9k/caps.h"
#include "zz9k/host.h"
#include "zz9k/shared.h"
#include "zzplay-ahi.h"
#include "zzplay-core.h"
#include "zzplay-mhi.h"
#include "zzplay-mp3-transport.h"

#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZZPLAY_MP3_PCM_CAPACITY ZZPLAY_MP3_DEFAULT_PCM_CAPACITY
#define ZZPLAY_MP3_Z2_PCM_CAPACITY (32UL * 1024UL)
#define ZZPLAY_MP3_Z2_STAGING_CAPACITY (16UL * 1024UL)
#define ZZPLAY_MP3_DRAIN_GUARD 128U

typedef struct ZZPlayMP3Decode {
  ZZ9KContext *ctx;
  FILE *file;
  ZZ9KSharedBuffer compressed;
  ZZ9KSharedBuffer pcm;
  ZZ9KSharedBuffer staging;
  ZZ9KAudioStreamResult result;
  ZZPlayAHISink *ahi;
  const ZZPlayMP3Info *probe;
  const ZZPlayMP3Controls *controls;
  uint32_t produced_seen;
  uint32_t pcm_offset;
  uint32_t pending_ack;
  uint32_t input_bytes;
  uint64_t output_frames;
  uint8_t session_open;
  uint8_t ahi_started;
  uint8_t compact_z2;
} ZZPlayMP3Decode;

static int zzplay_mp3_should_stop(const ZZPlayMP3Decode *decode)
{
  return decode->controls && decode->controls->stop_requested &&
         decode->controls->stop_requested(decode->controls->user);
}

static int zzplay_mp3_is_paused(const ZZPlayMP3Decode *decode)
{
  return decode->controls && decode->controls->paused &&
         decode->controls->paused(decode->controls->user);
}

static int zzplay_mp3_controls_stop(const ZZPlayMP3Controls *controls)
{
  return controls && controls->stop_requested &&
         controls->stop_requested(controls->user);
}

static int zzplay_mp3_controls_paused(const ZZPlayMP3Controls *controls)
{
  return controls && controls->paused &&
         controls->paused(controls->user);
}

static void zzplay_mp3_report_progress(const ZZPlayMP3Controls *controls,
                                       uint32_t elapsed_ms, int exact)
{
  if (controls && controls->progress) {
    controls->progress(controls->user, elapsed_ms, exact);
  }
}

static void zzplay_mp3_report_backend(const ZZPlayMP3Controls *controls,
                                      const char *name)
{
  if (controls && controls->backend) {
    controls->backend(controls->user, name);
  }
}

/* zzplay_mhi_play_file() carries one opaque pointer, so bundle everything
 * the callbacks need behind it. */
typedef struct ZZPlayMHIBridge {
  const ZZPlayMP3Controls *controls;
  const ZZPlayMHISink *sink;
  uint32_t bitrate_kbps;
} ZZPlayMHIBridge;

static int zzplay_mp3_controls_stop_thunk(void *user)
{
  const ZZPlayMHIBridge *bridge = (const ZZPlayMHIBridge *)user;

  if (!bridge) {
    return 0;
  }
  /* MHI reports only bytes handed to the decoder, which run ahead of what
   * has actually been heard by however much is queued - reported as
   * approximate rather than pretending to a sample-accurate position. */
  if (bridge->sink && bridge->bitrate_kbps != 0U) {
    zzplay_mp3_report_progress(
        bridge->controls,
        (uint32_t)((bridge->sink->input_bytes * 8ULL) /
                   bridge->bitrate_kbps),
        0);
  }
  return zzplay_mp3_controls_stop(bridge->controls);
}

static int zzplay_mp3_controls_paused_thunk(void *user)
{
  const ZZPlayMHIBridge *bridge = (const ZZPlayMHIBridge *)user;

  return bridge ? zzplay_mp3_controls_paused(bridge->controls) : 0;
}

static int zzplay_mp3_service_ready(ZZ9KContext *ctx, ZZ9KCaps *caps)
{
  ZZ9KServiceInfo service;

  return caps && zz9k_query_caps(ctx, caps) == ZZ9K_STATUS_OK &&
         (caps->capability_bits & ZZ9K_CAP_AUDIO_DECODE) != 0U &&
         zz9k_query_service(ctx, ZZ9K_SERVICE_AUDIO, &service) ==
             ZZ9K_STATUS_OK &&
         (service.flags & ZZ9K_SERVICE_FLAG_AUDIO_MP3_DECODE) != 0U &&
         (service.flags & ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM) != 0U;
}

static int zzplay_mp3_result_valid(const ZZPlayMP3Decode *decode)
{
  if (decode->result.bytes_produced < decode->produced_seen ||
      decode->result.bytes_produced - decode->produced_seen >
          decode->pcm.length) {
    return 0;
  }
  if (decode->result.sample_rate != 0U &&
      (decode->result.sample_rate != decode->probe->sample_rate ||
       decode->result.channels != decode->probe->channels)) {
    return 0;
  }
  return 1;
}

static int zzplay_mp3_ack(ZZPlayMP3Decode *decode, int force)
{
  int status;

  if (!zzplay_mp3_pcm_read_due(decode->pending_ack,
                               decode->pcm.length, force)) {
    return 1;
  }
  status = zz9k_audio_stream_read(
      decode->ctx, decode->result.session, decode->pending_ack, 0U,
      &decode->result);
  if (status != ZZ9K_STATUS_OK || !zzplay_mp3_result_valid(decode)) {
    return 0;
  }
  decode->pending_ack = 0U;
  decode->pcm_offset = decode->result.pcm_read;
  return 1;
}

static int zzplay_mp3_start_ahi(ZZPlayMP3Decode *decode, int final)
{
  uint64_t target;

  if (!decode->ahi || decode->ahi_started) {
    return 1;
  }
  target = (uint64_t)decode->ahi->period_frames *
           ZZPLAY_AHI_BUFFER_COUNT;
  if (!final && zzplay_ahi_queued_frames(decode->ahi) < target) {
    return 1;
  }
  if (zzplay_ahi_queued_frames(decode->ahi) == 0U) {
    return final;
  }
  if (!zzplay_ahi_play(decode->ahi)) {
    return 0;
  }
  decode->ahi_started = 1U;
  return 1;
}

static void *zzplay_mp3_acquire_ahi(ZZPlayMP3Decode *decode,
                                    size_t *capacity)
{
  void *buffer;

  while ((buffer = zzplay_ahi_acquire_buffer(
              decode->ahi, capacity)) == 0) {
    if (zzplay_mp3_should_stop(decode) ||
        !zzplay_mp3_start_ahi(decode, 0)) {
      return 0;
    }
    /* Reap immediately before retrying, not immediately after: Delay's
     * resolution is a 20 ms tick, and polling first spent that whole tick
     * sitting on a buffer AHI had already returned. */
    Delay(1U);
    if (!zzplay_ahi_poll(decode->ahi)) {
      return 0;
    }
  }
  return buffer;
}

static int zzplay_mp3_copy_ring(const ZZPlayMP3Decode *decode,
                                void *destination,
                                uint32_t bytes)
{
  uint32_t first = bytes;

  if (first > decode->pcm.length - decode->pcm_offset) {
    first = decode->pcm.length - decode->pcm_offset;
  }
  if (!zz9k_shared_copy_from(destination, &decode->pcm,
                             decode->pcm_offset, first)) {
    return 0;
  }
  return first == bytes ||
         zz9k_shared_copy_from((uint8_t *)destination + first,
                               &decode->pcm, 0U, bytes - first);
}

static int zzplay_mp3_pump_pcm(ZZPlayMP3Decode *decode, int final)
{
  uint32_t available;
  uint32_t period_bytes = decode->ahi
                              ? decode->ahi->period_frames *
                                    decode->ahi->frame_bytes
                              : 1U;

  if (!zzplay_mp3_result_valid(decode)) {
    return 0;
  }
  available = decode->result.bytes_produced - decode->produced_seen;
  if (!decode->ahi) {
    decode->produced_seen += available;
    decode->pcm_offset = zzplay_mp3_ring_advance(
        decode->pcm_offset, available, decode->pcm.length);
    decode->pending_ack += available;
    decode->output_frames +=
        available / (decode->probe->channels * 2U);
    return zzplay_mp3_ack(decode, final);
  }
  while (available >= period_bytes || (final && available != 0U)) {
    void *buffer;
    size_t capacity;
    uint32_t bytes = available < period_bytes ? available : period_bytes;

    if (zzplay_mp3_should_stop(decode)) {
      return 0;
    }
    buffer = zzplay_mp3_acquire_ahi(decode, &capacity);
    if (!buffer || bytes > capacity ||
        !zzplay_mp3_copy_ring(decode, buffer, bytes) ||
        !zzplay_ahi_submit_buffer(decode->ahi, bytes)) {
      return 0;
    }
    decode->produced_seen += bytes;
    decode->pcm_offset = zzplay_mp3_ring_advance(
        decode->pcm_offset, bytes, decode->pcm.length);
    decode->pending_ack += bytes;
    decode->output_frames += bytes / decode->ahi->frame_bytes;
    available -= bytes;
    if (!zzplay_mp3_start_ahi(decode, final) ||
        !zzplay_mp3_ack(decode, 0)) {
      return 0;
    }
  }
  return 1;
}

static int zzplay_mp3_finish_ahi(ZZPlayMP3Decode *decode)
{
  if (!decode->ahi) {
    return 1;
  }
  zzplay_ahi_mark_end_of_stream(decode->ahi);
  if (!zzplay_mp3_start_ahi(decode, 1) ||
      !zzplay_ahi_begin_drain(decode->ahi)) {
    return 0;
  }
  while (!zzplay_ahi_drained(decode->ahi)) {
    if (zzplay_mp3_should_stop(decode) ||
        !zzplay_ahi_poll(decode->ahi)) {
      return 0;
    }
    Delay(1U);
  }
  return 1;
}

static int zzplay_mp3_decode_once(ZZPlayMP3Decode *decode)
{
  ZZ9KAudioStreamBeginDesc begin;
  static uint8_t chunk[ZZPLAY_MP3_FEED_MAX_BYTES];
  uint32_t pcm_capacity;
  uint32_t staging_capacity;
  uint32_t host_flags;
  int status;
  unsigned flush_guard;

  memset(&decode->result, 0, sizeof(decode->result));
  pcm_capacity = ZZPLAY_MP3_PCM_CAPACITY;
  staging_capacity = ZZPLAY_MP3_FEED_MAX_BYTES;
  host_flags = 0U;
  if (decode->compact_z2) {
    pcm_capacity = ZZPLAY_MP3_Z2_PCM_CAPACITY;
    staging_capacity = ZZPLAY_MP3_Z2_STAGING_CAPACITY;
    host_flags = ZZ9K_ALLOC_HOST_WINDOW;
  }
  if (zz9k_alloc_shared(decode->ctx, ZZPLAY_MP3_INPUT_CAPACITY,
                        16U, ZZ9K_ALLOC_CARD_ONLY, &decode->compressed) !=
          ZZ9K_STATUS_OK ||
      zz9k_alloc_shared(decode->ctx, pcm_capacity,
                        16U, host_flags, &decode->pcm) != ZZ9K_STATUS_OK ||
      zz9k_alloc_shared(decode->ctx, staging_capacity,
                        16U, host_flags, &decode->staging) != ZZ9K_STATUS_OK) {
    return 0;
  }
  /* The stream decoder has no rate or channel conversion, so the firmware
   * rejects a non-zero output geometry outright. Ask for the file's native
   * rate/channels; zzplay_mp3_result_valid then holds the decoded stream to
   * the geometry the probe already reported. */
  if (!zz9k_audio_build_stream_begin_desc(
          &begin, decode->compressed.handle, decode->compressed.length,
          decode->pcm.handle, decode->pcm.length, 0U, 0U,
          ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE, 0U,
          decode->staging.length, 0U)) {
    return 0;
  }
  status = zz9k_audio_stream_begin(
      decode->ctx, &begin, &decode->result);
  if (status != ZZ9K_STATUS_OK) {
    return 0;
  }
  decode->session_open = 1U;
  decode->pcm_offset = decode->result.pcm_read;

  for (;;) {
    ZZ9KAudioStreamFeedDesc feed;
    size_t got;
    uint32_t flags;
    unsigned retry = 0U;

    if (zzplay_mp3_should_stop(decode)) {
      return 0;
    }
    /* Pause by ceasing to feed. The AHI queue plays out first, so audio
     * stops after at most the queued depth rather than instantly; MHI has a
     * real pause primitive and uses it instead. Keep servicing AHI while
     * held so completed buffers are still reaped. */
    while (zzplay_mp3_is_paused(decode)) {
      if (zzplay_mp3_should_stop(decode)) {
        return 0;
      }
      if (decode->ahi && decode->ahi_started) {
        zzplay_ahi_poll(decode->ahi);
      }
      Delay(1U);
    }
    if (decode->probe->sample_rate != 0U) {
      zzplay_mp3_report_progress(
          decode->controls,
          (uint32_t)((decode->output_frames * 1000ULL) /
                     decode->probe->sample_rate),
          1);
    }
    got = fread(chunk, 1U, decode->staging.length, decode->file);
    if (got == 0U && ferror(decode->file)) {
      return 0;
    }
    flags = got == 0U ? ZZ9K_AUDIO_STREAM_FEED_EOF : 0U;
    if (got != 0U &&
        !zz9k_shared_copy_to(&decode->staging, 0U, chunk,
                             (uint32_t)got)) {
      return 0;
    }
    do {
      if (++retry > 64U ||
          !zz9k_audio_build_stream_feed_desc(
              &feed, decode->result.session, decode->staging.handle,
              0U, (uint32_t)got, flags)) {
        return 0;
      }
      status = zz9k_audio_stream_feed(
          decode->ctx, &feed, &decode->result);
      if (status != ZZ9K_STATUS_OK ||
          !zzplay_mp3_pump_pcm(decode, flags != 0U)) {
        return 0;
      }
      if ((decode->result.flags &
           ZZ9K_AUDIO_STREAM_RESULT_BACKPRESSURE) != 0U &&
          !zzplay_mp3_ack(decode, 1)) {
        return 0;
      }
    } while ((decode->result.flags &
              ZZ9K_AUDIO_STREAM_RESULT_BACKPRESSURE) != 0U);
    decode->input_bytes += (uint32_t)got;
    if (flags != 0U) {
      break;
    }
  }

  for (flush_guard = 0U; flush_guard < ZZPLAY_MP3_DRAIN_GUARD;
       flush_guard++) {
    uint32_t before = decode->produced_seen;

    if (!zzplay_mp3_pump_pcm(decode, 1) ||
        !zzplay_mp3_ack(decode, 1)) {
      return 0;
    }
    if (decode->pending_ack == 0U &&
        decode->result.bytes_produced == decode->produced_seen &&
        ((decode->result.flags & ZZ9K_AUDIO_STREAM_RESULT_DONE) != 0U ||
         before == decode->produced_seen)) {
      break;
    }
  }
  return flush_guard < ZZPLAY_MP3_DRAIN_GUARD &&
         decode->output_frames != 0U &&
         zzplay_mp3_finish_ahi(decode);
}

static void zzplay_mp3_decode_cleanup(ZZPlayMP3Decode *decode)
{
  if (decode->session_open) {
    ZZ9KAudioStreamResult result;
    (void)zz9k_audio_stream_close(
        decode->ctx, decode->result.session, 0U, &result);
  }
  if (decode->staging.handle) {
    (void)zz9k_free_shared(decode->ctx, decode->staging.handle);
  }
  if (decode->pcm.handle) {
    (void)zz9k_free_shared(decode->ctx, decode->pcm.handle);
  }
  if (decode->compressed.handle) {
    (void)zz9k_free_shared(decode->ctx, decode->compressed.handle);
  }
  if (decode->ahi) {
    zzplay_ahi_close(decode->ahi);
  }
  if (decode->file) {
    fclose(decode->file);
  }
}

static int zzplay_mp3_accelerated(
    const char *path,
    const ZZPlayMP3Info *info,
    const ZZPlayOptions *options,
    const ZZPlayMP3Controls *controls)
{
  ZZ9KContext *ctx = 0;
  ZZ9KCaps caps;
  uint32_t repeats = options->loop_count;
  uint32_t completed = 0U;
  int ok = 0;

  memset(&caps, 0, sizeof(caps));
  if (zz9k_open(&ctx) != ZZ9K_STATUS_OK ||
      !zzplay_mp3_service_ready(ctx, &caps)) {
    fprintf(stderr,
            "zzplay: accelerated MP3 streaming is unavailable\n");
    goto done;
  }
  for (;;) {
    ZZPlayMP3Decode decode;
    ZZPlayAHISink ahi;

    memset(&decode, 0, sizeof(decode));
    memset(&ahi, 0, sizeof(ahi));
    ahi.fill_slot = -1;
    decode.ctx = ctx;
    decode.compact_z2 =
        (caps.capability_bits & ZZ9K_CAP_APERTURE_LAYOUT) != 0U;
    decode.probe = info;
    decode.controls = controls;
    decode.file = fopen(path, "rb");
    if (!decode.file) {
      fprintf(stderr, "zzplay: cannot reopen %s\n", path);
      zzplay_mp3_decode_cleanup(&decode);
      goto done;
    }
    if (options->audio_backend != ZZPLAY_AUDIO_NONE) {
      uint32_t period = zzplay_mp3_ahi_period_frames(
          info->sample_rate, ZZPLAY_AHI_BUFFER_COUNT);

      if (period == 0U ||
          !zzplay_ahi_prepare(&ahi, info->sample_rate,
                              info->channels, period)) {
        fprintf(stderr,
                "zzplay: AHI backend acquisition failed "
                "(device error %d)\n",
                ahi.last_error);
        zzplay_mp3_decode_cleanup(&decode);
        goto done;
      }
      decode.ahi = &ahi;
    }
    if (!zzplay_mp3_decode_once(&decode)) {
      int stopped = zzplay_mp3_controls_stop(controls);

      if (!stopped) {
        fprintf(stderr,
                "zzplay: accelerated MP3 decode/output failed\n");
      }
      zzplay_mp3_decode_cleanup(&decode);
      if (stopped) {
        ok = 1;
      }
      goto done;
    }
    zzplay_info("zzplay: MP3 loop %lu: %lu input bytes, %llu audio frames\n",
           (unsigned long)completed,
           (unsigned long)decode.input_bytes,
           (unsigned long long)decode.output_frames);
    zzplay_mp3_decode_cleanup(&decode);
    if (options->loop_mode == ZZPLAY_LOOP_FOREVER) {
      completed++;
      continue;
    }
    if (options->loop_mode == ZZPLAY_LOOP_FINITE && repeats != 0U) {
      repeats--;
      completed++;
      continue;
    }
    ok = 1;
    break;
  }

done:
  if (ctx) {
    zz9k_close(ctx);
  }
  return ok;
}

static int zzplay_mp3_mhi(
    const char *path,
    const ZZPlayMP3Info *info,
    const ZZPlayOptions *options,
    const ZZPlayMP3Controls *controls,
    ZZPlayMHIStatus *open_status)
{
  ZZPlayMHIBridge bridge;
  ZZPlayMHISink sink;
  ZZPlayMHIStatus status;
  uint32_t repeats = options->loop_count;
  uint32_t completed = 0U;
  int ok = 0;

  bridge.controls = controls;
  bridge.sink = &sink;
  bridge.bitrate_kbps = info ? info->bitrate_kbps : 0U;
  status = zzplay_mhi_acquire(&sink);
  *open_status = status;
  if (status != ZZPLAY_MHI_OK) {
    return 0;
  }
  zzplay_info("zzplay: selected audio backend MHI (card-local Layer III)\n");
  /* The player window has to be told too, or it shows "selecting..." for the
   * whole track whenever MHI is the chosen backend. */
  zzplay_mp3_report_backend(controls, "MHI");
  for (;;) {
    FILE *file = fopen(path, "rb");

    if (!file) {
      *open_status = ZZPLAY_MHI_IO_ERROR;
      goto done;
    }
    status = zzplay_mhi_play_file(
        &sink, file, zzplay_mp3_controls_stop_thunk,
        zzplay_mp3_controls_paused_thunk, &bridge);
    fclose(file);
    if (status != ZZPLAY_MHI_OK) {
      *open_status = status;
      goto done;
    }
    zzplay_info("zzplay: MP3 MHI loop %lu: %llu input bytes\n",
           (unsigned long)completed,
           (unsigned long long)sink.input_bytes);
    if (options->loop_mode == ZZPLAY_LOOP_FOREVER) {
      completed++;
      continue;
    }
    if (options->loop_mode == ZZPLAY_LOOP_FINITE && repeats != 0U) {
      repeats--;
      completed++;
      continue;
    }
    zzplay_info("zzplay: MP3 MHI playback complete, %lu loops\n",
           (unsigned long)completed);
    ok = 1;
    break;
  }

done:
  zzplay_mhi_release(&sink);
  return ok;
}

int zzplay_mp3_run(const char *path,
                   const ZZPlayMP3Info *info,
                   const ZZPlayOptions *options,
                   const ZZPlayMP3Controls *controls)
{
  ZZPlayMHIStatus mhi_status = ZZPLAY_MHI_MISSING;

  if (!path || !info || !options) {
    return 0;
  }
  if (options->audio_backend == ZZPLAY_AUDIO_AX) {
    fprintf(stderr,
            "zzplay: direct AX is not a standalone MP3 backend; "
            "use MHI or AHI\n");
    return 0;
  }
  if (options->audio_backend == ZZPLAY_AUDIO_MHI ||
      options->audio_backend == ZZPLAY_AUDIO_AUTO) {
    if (zzplay_mp3_mhi(path, info, options, controls, &mhi_status)) {
      return 1;
    }
    if (mhi_status == ZZPLAY_MHI_STOPPED) {
      return 1;
    }
    if (options->audio_backend == ZZPLAY_AUDIO_MHI ||
        mhi_status == ZZPLAY_MHI_IO_ERROR) {
      fprintf(stderr, "zzplay: MHI playback failed: %s\n",
              zzplay_mhi_status_name(mhi_status));
      return 0;
    }
    zzplay_info("zzplay: MHI unavailable before playback (%s); "
           "AUTO falling back to accelerated decode + AHI\n",
           zzplay_mhi_status_name(mhi_status));
  }
  if (options->audio_backend != ZZPLAY_AUDIO_NONE) {
    zzplay_info("zzplay: selected audio backend AHI "
           "(accelerated Layer III decode, S16BE)\n");
  } else {
    zzplay_info("zzplay: selected audio backend NONE "
           "(accelerated Layer III decode benchmark)\n");
  }
  zzplay_mp3_report_backend(
      controls,
      options->audio_backend != ZZPLAY_AUDIO_NONE ? "AHI" : "none");
  return zzplay_mp3_accelerated(path, info, options, controls);
}
