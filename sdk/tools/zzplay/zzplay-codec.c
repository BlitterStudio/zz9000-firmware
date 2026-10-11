/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-codec.h"

#include "zz9k/audio.h"
#include "zz9k/caps.h"
#include "zz9k/host.h"
#include "zz9k/shared.h"
#include "zzplay-ahi.h"
#include "zzplay-core.h"
#include "zzplay-launch.h"
#include "zzplay-mp3-transport.h"

#include <proto/dos.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZZPLAY_CODEC_PCM_CAPACITY ZZPLAY_MP3_DEFAULT_PCM_CAPACITY
#define ZZPLAY_CODEC_Z2_PCM_CAPACITY (32UL * 1024UL)
#define ZZPLAY_CODEC_Z2_STAGING_CAPACITY (16UL * 1024UL)
#define ZZPLAY_CODEC_DRAIN_GUARD 128U

/* One staging chunk. The AHI and on-card passes never run together. */
uint8_t zzplay_codec_chunk[ZZPLAY_MP3_FEED_MAX_BYTES];

void zzplay_codec_pump(const ZZPlayCodecEngine *engine)
{
  if (engine->run->pump) {
    engine->run->pump(engine->run->pump_user);
  }
}


/* Output text marks a requested mute only when the active backend honours
 * volume; a saved zero remains available for a later capable backend. */
void zzplay_codec_refresh_output(const ZZPlayCodecEngine *engine)
{
  char text[ZZPLAY_NOW_OUTPUT_MAX];

  if (engine->ctl->volume == 0U && engine->ctl->now.volume_supported) {
    sprintf(text, "%s, muted", engine->output_base);
  } else {
    strcpy(text, engine->output_base);
  }
  zzplay_controller_set_output(engine->ctl, text);
}

void zzplay_codec_set_output(ZZPlayCodecEngine *engine,
                             const char *output)
{
  strncpy(engine->output_base, output, sizeof(engine->output_base) - 1U);
  engine->output_base[sizeof(engine->output_base) - 1U] = '\0';
  zzplay_codec_refresh_output(engine);
}

int zzplay_codec_should_stop(const ZZPlayCodecDecode *decode)
{
  return zzplay_controller_item_should_end(decode->engine->ctl);
}

/* Live controls for the AHI path: volume is applied here; any request
 * ends the pass (returns 1) and stays pending. zzplay_codec_accelerated
 * turns a SEEK into a fresh pass at the new offset, so neither compressed
 * data the card already accepted nor PCM queued to AHI from the old
 * position is played after the jump. */
static int zzplay_codec_service_controls(ZZPlayCodecDecode *decode)
{
  ZZPlayController *ctl = decode->engine->ctl;
  uint32_t volume;

  if (zzplay_controller_take_volume(ctl, &volume)) {
    if (decode->ahi) {
      zzplay_ahi_set_volume(decode->ahi, volume);
    }
    zzplay_codec_refresh_output(decode->engine);
  }
  return zzplay_controller_item_should_end(ctl);
}

static void zzplay_codec_report_progress(const ZZPlayCodecDecode *decode)
{
  /* Count what AHI has played, not what was queued to it: the queue holds
   * a few hundred milliseconds, which would put the position ahead of the
   * sound. Without a sink (muted) output is consumed as it is produced. */
  uint64_t frames = decode->ahi ? zzplay_ahi_played_frames(decode->ahi)
                                : decode->output_frames;

  if (decode->engine->sample_rate != 0U) {
    zzplay_controller_set_position(
        decode->engine->ctl,
        decode->position_base_ms +
            (uint32_t)(frames * 1000ULL / decode->engine->sample_rate),
        1);
  }
}

/* MP3 needs the Layer III decode capability and its two stream flags; the
 * BeginEx codecs need only their own stream flag. */
int zzplay_codec_service_ready(ZZ9KContext *ctx, ZZ9KCaps *caps,
                               uint32_t codec)
{
  ZZ9KServiceInfo service;
  uint32_t required;

  switch (codec) {
  case ZZ9K_AUDIO_CODEC_MP3:
    required = ZZ9K_SERVICE_FLAG_AUDIO_MP3_DECODE |
               ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM;
    break;
  case ZZ9K_AUDIO_CODEC_FLAC:
    required = ZZ9K_SERVICE_FLAG_AUDIO_FLAC_STREAM;
    break;
  case ZZ9K_AUDIO_CODEC_VORBIS:
    required = ZZ9K_SERVICE_FLAG_AUDIO_VORBIS_STREAM;
    break;
  default:
    return 0;
  }
  return caps && zz9k_query_caps(ctx, caps) == ZZ9K_STATUS_OK &&
         (codec != ZZ9K_AUDIO_CODEC_MP3 ||
          (caps->capability_bits & ZZ9K_CAP_AUDIO_DECODE) != 0U) &&
         zz9k_query_service(ctx, ZZ9K_SERVICE_AUDIO, &service) ==
             ZZ9K_STATUS_OK &&
         (service.flags & required) == required;
}

static int zzplay_codec_result_valid(const ZZPlayCodecDecode *decode)
{
  if (decode->result.bytes_produced < decode->produced_seen ||
      decode->result.bytes_produced - decode->produced_seen >
          decode->pcm.length) {
    return 0;
  }
  if (decode->result.sample_rate != 0U &&
      (decode->result.sample_rate != decode->engine->sample_rate ||
       decode->result.channels != decode->engine->channels)) {
    return 0;
  }
  return 1;
}

static int zzplay_codec_ack(ZZPlayCodecDecode *decode, int force)
{
  int status;

  if (!zzplay_mp3_pcm_read_due(decode->pending_ack,
                               decode->pcm.length, force)) {
    return 1;
  }
  status = zz9k_audio_stream_read(
      decode->ctx, decode->result.session, decode->pending_ack, 0U,
      &decode->result);
  if (status != ZZ9K_STATUS_OK) {
    decode->stream_status = status;
    return 0;
  }
  if (!zzplay_codec_result_valid(decode)) {
    return 0;
  }
  decode->pending_ack = 0U;
  decode->pcm_offset = decode->result.pcm_read;
  return 1;
}

static int zzplay_codec_start_ahi(ZZPlayCodecDecode *decode, int final)
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

static void *zzplay_codec_acquire_ahi(ZZPlayCodecDecode *decode,
                                      size_t *capacity)
{
  void *buffer;

  while ((buffer = zzplay_ahi_acquire_buffer(
              decode->ahi, capacity)) == 0) {
    if (zzplay_codec_should_stop(decode) ||
        !zzplay_codec_start_ahi(decode, 0)) {
      return 0;
    }
    /* Reap immediately before retrying, not immediately after: Delay's
     * resolution is a 20 ms tick, and polling first spent that whole tick
     * sitting on a buffer AHI had already returned. */
    Delay(1U);
    if (!zzplay_ahi_poll(decode->ahi)) {
      return 0;
    }
    zzplay_codec_pump(decode->engine);
  }
  return buffer;
}

static int zzplay_codec_copy_ring(const ZZPlayCodecDecode *decode,
                                  uint32_t offset, void *destination,
                                  uint32_t bytes)
{
  uint32_t first = bytes;

  if (first > decode->pcm.length - offset) {
    first = decode->pcm.length - offset;
  }
  if (!zz9k_shared_copy_from(destination, &decode->pcm, offset, first)) {
    return 0;
  }
  return first == bytes ||
         zz9k_shared_copy_from((uint8_t *)destination + first,
                               &decode->pcm, 0U, bytes - first);
}

/* Copies `ring_bytes` of whole frames into an AHI buffer. AHI plays S16:
 * an S32BE stream is narrowed to the top 16 bits of each sample through a
 * small bounce buffer. */
static int zzplay_codec_copy_to_ahi(const ZZPlayCodecDecode *decode,
                                    uint8_t *destination,
                                    uint32_t ring_bytes)
{
  static uint8_t bounce[4096];
  uint32_t offset = decode->pcm_offset;

  if (decode->engine->pcm_width == 2U) {
    return zzplay_codec_copy_ring(decode, offset, destination, ring_bytes);
  }
  while (ring_bytes != 0U) {
    uint32_t bytes = ring_bytes < sizeof(bounce) ? ring_bytes
                                                 : (uint32_t)sizeof(bounce);
    uint32_t i;

    if (!zzplay_codec_copy_ring(decode, offset, bounce, bytes)) {
      return 0;
    }
    for (i = 0U; i < bytes; i += 4U) {
      *destination++ = bounce[i];
      *destination++ = bounce[i + 1U];
    }
    offset = zzplay_mp3_ring_advance(offset, bytes, decode->pcm.length);
    ring_bytes -= bytes;
  }
  return 1;
}

static int zzplay_codec_pump_pcm(ZZPlayCodecDecode *decode, int final)
{
  uint32_t available;
  uint32_t ring_frame = decode->engine->channels * decode->engine->pcm_width;
  uint32_t period_bytes = decode->ahi
                              ? decode->ahi->period_frames * ring_frame
                              : 1U;

  if (!zzplay_codec_result_valid(decode)) {
    return 0;
  }
  available = decode->result.bytes_produced - decode->produced_seen;
  if (!decode->ahi) {
    decode->produced_seen += available;
    decode->pcm_offset = zzplay_mp3_ring_advance(
        decode->pcm_offset, available, decode->pcm.length);
    decode->pending_ack += available;
    decode->output_frames += available / ring_frame;
    return zzplay_codec_ack(decode, final);
  }
  while (available >= period_bytes || (final && available >= ring_frame)) {
    void *buffer;
    size_t capacity;
    uint32_t bytes = available < period_bytes ? available : period_bytes;
    uint32_t frames = bytes / ring_frame;
    uint32_t ahi_bytes = frames * decode->ahi->frame_bytes;

    bytes = frames * ring_frame;
    if (zzplay_codec_should_stop(decode)) {
      return 0;
    }
    buffer = zzplay_codec_acquire_ahi(decode, &capacity);
    if (!buffer || ahi_bytes > capacity ||
        !zzplay_codec_copy_to_ahi(decode, (uint8_t *)buffer, bytes) ||
        !zzplay_ahi_submit_buffer(decode->ahi, ahi_bytes)) {
      return 0;
    }
    decode->produced_seen += bytes;
    decode->pcm_offset = zzplay_mp3_ring_advance(
        decode->pcm_offset, bytes, decode->pcm.length);
    decode->pending_ack += bytes;
    decode->output_frames += frames;
    available -= bytes;
    if (!zzplay_codec_start_ahi(decode, final) ||
        !zzplay_codec_ack(decode, 0)) {
      return 0;
    }
  }
  return 1;
}

static int zzplay_codec_finish_ahi(ZZPlayCodecDecode *decode)
{
  if (!decode->ahi) {
    return 1;
  }
  zzplay_ahi_mark_end_of_stream(decode->ahi);
  if (!zzplay_codec_start_ahi(decode, 1) ||
      !zzplay_ahi_begin_drain(decode->ahi)) {
    return 0;
  }
  while (!zzplay_ahi_drained(decode->ahi)) {
    if (zzplay_codec_should_stop(decode) ||
        !zzplay_ahi_poll(decode->ahi)) {
      return 0;
    }
    zzplay_codec_report_progress(decode);
    zzplay_codec_pump(decode->engine);
    Delay(1U);
  }
  return 1;
}

/* Why a pass ended early. When the card refused the stream's layout (for
 * Ogg, a further chained or multiplexed link after the first one played),
 * the audio already queued to AHI plays out before the item fails with an
 * explicit unsupported-stream error, so it is never reported as complete. */
static ZZPlayCodecPassResult zzplay_codec_pass_failure(
    ZZPlayCodecDecode *decode)
{
  if (zzplay_codec_should_stop(decode)) {
    return ZZPLAY_CODEC_PASS_STOPPED;
  }
  if (decode->stream_status != ZZ9K_STATUS_UNSUPPORTED) {
    return ZZPLAY_CODEC_PASS_FAILED;
  }
  (void)zzplay_codec_finish_ahi(decode);
  return zzplay_codec_should_stop(decode) ? ZZPLAY_CODEC_PASS_STOPPED
                                          : ZZPLAY_CODEC_PASS_UNSUPPORTED;
}

/* The stream decoder has no rate or channel conversion, so the firmware
 * rejects a non-zero output geometry outright. Ask for the file's native
 * rate/channels; zzplay_codec_result_valid then holds the decoded stream to
 * the geometry the probe already reported. MP3 keeps the original Begin so
 * it runs on firmware that predates the codec-aware BeginEx; its high
 * water stays the staging size. The BeginEx codecs pass no high water, as
 * the sound class does, so per-call decode is never capped below one FLAC
 * block or Vorbis packet. */
static int zzplay_codec_begin(ZZPlayCodecDecode *decode)
{
  const ZZPlayCodecEngine *engine = decode->engine;

  if (engine->codec == ZZ9K_AUDIO_CODEC_MP3) {
    ZZ9KAudioStreamBeginDesc begin;

    if (!zz9k_audio_build_stream_begin_desc(
            &begin, decode->compressed.handle, decode->compressed.length,
            decode->pcm.handle, decode->pcm.length, 0U, 0U,
            engine->pcm_format, 0U, decode->staging.length, 0U)) {
      return ZZ9K_STATUS_BAD_REQUEST;
    }
    return zz9k_audio_stream_begin(decode->ctx, &begin, &decode->result);
  } else {
    ZZ9KAudioStreamBeginExDesc begin;

    if (!zz9k_audio_build_stream_begin_ex_desc(
            &begin, engine->codec, decode->compressed.handle,
            decode->compressed.length, decode->pcm.handle,
            decode->pcm.length, 0U, 0U, engine->pcm_format, 0U, 0U, 0U)) {
      return ZZ9K_STATUS_BAD_REQUEST;
    }
    return zz9k_audio_stream_begin_ex(decode->ctx, &begin, &decode->result);
  }
}

static uint32_t zzplay_codec_round_4k(uint32_t bytes)
{
  return (bytes + 4095UL) & ~4095UL;
}

/* PCM ring for one pass: the bus profile's default, raised to twice the
 * decoder's largest PCM unit so a whole AHI period (at most half the ring)
 * and one more unit fit together. Card-only rings skip the Zorro II host
 * cap: the pump reads them on the card, so they never enter the window. */
static uint32_t zzplay_codec_pcm_capacity(uint32_t unit, int compact_z2,
                                          int card_only)
{
  uint32_t base = (!card_only && compact_z2) ? ZZPLAY_CODEC_Z2_PCM_CAPACITY
                                             : ZZPLAY_CODEC_PCM_CAPACITY;
  uint32_t want = zzplay_codec_round_4k(2U * unit);

  return want > base ? want : base;
}

/* Host-visible PCM ring and staging buffer, then the card-only compressed
 * ring. On failure the host pair shrinks in bounded halving steps (KTD6: a
 * compact Zorro II host window may hold only 16 KiB), staging first; the
 * PCM ring never drops below the decoder's largest unit and staging stays
 * below half the PCM ring (high_water must be smaller than the ring). The
 * compressed ring then holds one whole buffered input unit plus a staged
 * chunk. Runs before AHI is prepared so the period can be capped against
 * the ring obtained. */
int zzplay_codec_alloc_rings(ZZPlayCodecDecode *decode, uint32_t pcm_unit,
                             int pcm_card_only)
{
  const ZZPlayCodecEngine *engine = decode->engine;
  uint32_t host_flags = decode->compact_z2 ? ZZ9K_ALLOC_HOST_WINDOW : 0U;
  uint32_t pcm_flags = pcm_card_only ? ZZ9K_ALLOC_CARD_ONLY : host_flags;
  uint32_t unit = pcm_unit != 0U ? pcm_unit : engine->min_pcm_bytes;
  uint32_t staging_max = decode->compact_z2 ? ZZPLAY_CODEC_Z2_STAGING_CAPACITY
                                            : ZZPLAY_MP3_FEED_MAX_BYTES;
  uint32_t pcm = zzplay_codec_pcm_capacity(unit, decode->compact_z2,
                                           pcm_card_only);
  uint32_t pcm_floor = zzplay_codec_round_4k(unit);
  uint32_t input = ZZPLAY_MP3_INPUT_CAPACITY;

  if (pcm_floor < 8192U) {
    pcm_floor = 8192U;
  }
  for (;;) {
    if (zz9k_alloc_shared(decode->ctx, pcm, 16U, pcm_flags,
                          &decode->pcm) == ZZ9K_STATUS_OK) {
      uint32_t staging = staging_max < pcm / 2U ? staging_max : pcm / 2U;

      for (; staging >= 4096U; staging /= 2U) {
        if (zz9k_alloc_shared(decode->ctx, staging, 16U, host_flags,
                              &decode->staging) == ZZ9K_STATUS_OK) {
          goto input_ring;
        }
        memset(&decode->staging, 0, sizeof(decode->staging));
      }
      (void)zz9k_free_shared(decode->ctx, decode->pcm.handle);
      /* Staging is the only host-window buffer on the card path; a
       * smaller PCM ring does not free any of that window. */
      if (pcm_card_only) {
        memset(&decode->pcm, 0, sizeof(decode->pcm));
        return 0;
      }
    }
    memset(&decode->pcm, 0, sizeof(decode->pcm));
    if (pcm <= pcm_floor) {
      return 0;
    }
    pcm = zzplay_codec_round_4k(pcm / 2U);
    if (pcm < pcm_floor) {
      pcm = pcm_floor;
    }
  }

input_ring:
  /* A 16384-sample 24-bit FLAC frame is over 70 KB: with a 64 KiB chunk
   * behind it, the default ring cannot hold both and the decoder faults. */
  if (engine->max_input_unit != 0U &&
      engine->max_input_unit + decode->staging.length > input) {
    input = zzplay_codec_round_4k(engine->max_input_unit +
                                  decode->staging.length);
  }
  if (zz9k_alloc_shared(decode->ctx, input, 16U, ZZ9K_ALLOC_CARD_ONLY,
                        &decode->compressed) != ZZ9K_STATUS_OK) {
    memset(&decode->compressed, 0, sizeof(decode->compressed));
    return 0;
  }
  return 1;
}

static ZZPlayCodecPassResult zzplay_codec_decode_once(ZZPlayCodecDecode *decode)
{
  int status;
  unsigned flush_guard;

  memset(&decode->result, 0, sizeof(decode->result));
  status = zzplay_codec_begin(decode);
  if (status != ZZ9K_STATUS_OK) {
    return ZZPLAY_CODEC_PASS_FAILED;
  }
  decode->session_open = 1U;
  decode->pcm_offset = decode->result.pcm_read;

  for (;;) {
    ZZ9KAudioStreamFeedDesc feed;
    size_t got;
    uint32_t flags;
    unsigned retry = 0U;

    if (zzplay_codec_service_controls(decode)) {
      return ZZPLAY_CODEC_PASS_STOPPED;
    }
    zzplay_codec_pump(decode->engine);
    /* Pause by ceasing to feed, confirmed once the hold is in place so
     * modal operations may run. The AHI queue plays out first, so audio
     * stops after at most the queued depth rather than instantly. Keep
     * servicing AHI while held so completed buffers are still reaped. */
    while (decode->engine->ctl->paused) {
      if (zzplay_codec_service_controls(decode)) {
        return ZZPLAY_CODEC_PASS_STOPPED;
      }
      zzplay_controller_set_engine_paused(decode->engine->ctl, 1);
      if (decode->ahi && decode->ahi_started) {
        zzplay_ahi_poll(decode->ahi);
      }
      zzplay_codec_pump(decode->engine);
      Delay(1U);
    }
    zzplay_controller_set_engine_paused(decode->engine->ctl, 0);
    zzplay_codec_report_progress(decode);
    got = fread(zzplay_codec_chunk, 1U, decode->staging.length, decode->file);
    if (got == 0U && ferror(decode->file)) {
      return ZZPLAY_CODEC_PASS_FAILED;
    }
    flags = got == 0U ? ZZ9K_AUDIO_STREAM_FEED_EOF : 0U;
    if (got != 0U &&
        !zz9k_shared_copy_to(&decode->staging, 0U, zzplay_codec_chunk,
                             (uint32_t)got)) {
      return ZZPLAY_CODEC_PASS_FAILED;
    }
    do {
      if (++retry > 64U ||
          !zz9k_audio_build_stream_feed_desc(
              &feed, decode->result.session, decode->staging.handle,
              0U, (uint32_t)got, flags)) {
        return ZZPLAY_CODEC_PASS_FAILED;
      }
      status = zz9k_audio_stream_feed(
          decode->ctx, &feed, &decode->result);
      if (status != ZZ9K_STATUS_OK) {
        decode->stream_status = status;
      }
      if (status != ZZ9K_STATUS_OK ||
          !zzplay_codec_pump_pcm(decode, flags != 0U)) {
        return zzplay_codec_pass_failure(decode);
      }
      /* Under backpressure hand AHI whatever whole frames are waiting, even
       * less than a period, before the forced Read: a decoder that needs
       * room for a whole block must never wait on a partial period. */
      if ((decode->result.flags &
           ZZ9K_AUDIO_STREAM_RESULT_BACKPRESSURE) != 0U &&
          (!zzplay_codec_pump_pcm(decode, 1) || !zzplay_codec_ack(decode, 1))) {
        return zzplay_codec_pass_failure(decode);
      }
    } while ((decode->result.flags &
              ZZ9K_AUDIO_STREAM_RESULT_BACKPRESSURE) != 0U);
    decode->input_bytes += (uint32_t)got;
    if (flags != 0U) {
      break;
    }
  }

  for (flush_guard = 0U; flush_guard < ZZPLAY_CODEC_DRAIN_GUARD;
       flush_guard++) {
    uint32_t before = decode->produced_seen;

    if (!zzplay_codec_pump_pcm(decode, 1) ||
        !zzplay_codec_ack(decode, 1)) {
      return zzplay_codec_pass_failure(decode);
    }
    if (decode->pending_ack == 0U &&
        decode->result.bytes_produced == decode->produced_seen &&
        ((decode->result.flags & ZZ9K_AUDIO_STREAM_RESULT_DONE) != 0U ||
         before == decode->produced_seen)) {
      break;
    }
  }
  if (flush_guard >= ZZPLAY_CODEC_DRAIN_GUARD ||
      decode->output_frames == 0U) {
    return ZZPLAY_CODEC_PASS_FAILED;
  }
  return zzplay_codec_finish_ahi(decode) ? ZZPLAY_CODEC_PASS_COMPLETED
                                         : ZZPLAY_CODEC_PASS_FAILED;
}

void zzplay_codec_decode_cleanup(ZZPlayCodecDecode *decode)
{
  if (decode->session_open) {
    ZZ9KAudioStreamResult result;
    unsigned tries;
    int status = ZZ9K_STATUS_OK;

    /* A bound pump still owns the session; Stop is idempotent if it
     * is not. Close answers BUSY while a PCM refill is in flight. */
    if (decode->ax_bound && decode->result.session != 0U) {
      (void)zz9k_audio_stream_stop(
          decode->ctx, decode->result.session, 0U, &result);
      decode->ax_bound = 0U;
    }
    for (tries = 0U; tries < 25U; tries++) {
      status = zz9k_audio_stream_close(
          decode->ctx, decode->result.session, 0U, &result);
      if (status != ZZ9K_STATUS_BUSY) {
        break;
      }
      Delay(1);
    }
    decode->session_open = 0U;
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
    decode->ahi = 0;
  }
  if (decode->file) {
    fclose(decode->file);
    decode->file = 0;
  }
}

/* `unavailable`, when given, is a caller that can still fall back: if the
 * accelerated service or the AHI unit cannot be had before anything has
 * played, it is set and nothing is reported. Without it these failures are
 * reported like any other. */
ZZPlayEngineResult zzplay_codec_accelerated(
ZZPlayCodecEngine *engine, int *unavailable)
{
  const ZZPlayEngineRun *run = engine->run;
  ZZ9KContext *ctx = 0;
  ZZ9KCaps caps;
  uint32_t base_ms = engine->seekable ? run->seek_ms : 0U;
  uint64_t offset = engine->seekable
                        ? zzplay_mp3_seek_offset(engine->audio_start,
                                                 engine->audio_end,
                                                 engine->total_ms, base_ms)
                        : engine->audio_start;
  uint32_t repeats = run->options->loop_count;
  uint32_t completed = 0U;
  int started = 0;
  ZZPlayEngineResult result = ZZPLAY_ENGINE_FAILED;
  ZZPlayAHISink ahi;

  memset(&caps, 0, sizeof(caps));
  if (zz9k_open(&ctx) != ZZ9K_STATUS_OK ||
      !zzplay_codec_service_ready(ctx, &caps, engine->codec)) {
    if (unavailable) {
      *unavailable = 1;
    } else {
      zzplay_launch_reportf(engine->ctl, engine->run->options,
                            "accelerated %s streaming is unavailable",
                            engine->codec_name);
    }
    goto done;
  }
  for (;;) {
    ZZPlayCodecDecode decode;
    ZZPlayCodecPassResult pass;

    memset(&decode, 0, sizeof(decode));
    memset(&ahi, 0, sizeof(ahi));
    ahi.fill_slot = -1;
    decode.ctx = ctx;
    decode.compact_z2 =
        (caps.capability_bits & ZZ9K_CAP_APERTURE_LAYOUT) != 0U;
    decode.engine = engine;
    decode.position_base_ms = base_ms;
    decode.file = fopen(run->path, "rb");
    if (!decode.file) {
      zzplay_launch_reportf(engine->ctl, engine->run->options,
                            "cannot reopen %s", run->path);
      zzplay_codec_decode_cleanup(&decode);
      goto done;
    }
    if (fseek(decode.file, (long)offset, SEEK_SET) != 0) {
      zzplay_launch_reportf(engine->ctl, engine->run->options,
                            "cannot seek in %s", run->path);
      zzplay_codec_decode_cleanup(&decode);
      goto done;
    }
    if (!zzplay_codec_alloc_rings(&decode, 0U, 0)) {
      if (unavailable && !started) {
        *unavailable = 1;
      } else {
        zzplay_launch_reportf(
            engine->ctl, engine->run->options,
            "not enough shared card memory for %s streaming",
            engine->codec_name);
      }
      zzplay_codec_decode_cleanup(&decode);
      goto done;
    }
    if (!engine->muted) {
      /* An AHI period never exceeds half the PCM ring, so a full period
       * and one more decoder unit always fit (44.1 kHz stereo is otherwise
       * larger than the 32 KiB compact Zorro II ring). */
      uint32_t ring_frame = engine->channels * engine->pcm_width;
      uint32_t period_cap = decode.pcm.length / 2U / ring_frame;
      uint32_t period = zzplay_mp3_ahi_period_frames(
          engine->sample_rate, ZZPLAY_AHI_BUFFER_COUNT);

      if (period > period_cap) {
        period = period_cap;
      }

      if (period == 0U ||
          !zzplay_ahi_prepare(&ahi, run->prefs->ahi_unit,
                              engine->sample_rate, engine->channels,
                              period)) {
        if (unavailable && !started) {
          *unavailable = 1;
        } else {
          zzplay_launch_reportf(
              engine->ctl, engine->run->options,
              "AHI backend acquisition failed (device error %d)",
              ahi.last_error);
        }
        zzplay_codec_decode_cleanup(&decode);
        goto done;
      }
      zzplay_ahi_set_volume(&ahi, engine->ctl->volume);
      decode.ahi = &ahi;
    }
    started = 1;
    pass = zzplay_codec_decode_once(&decode);
    /* Saturating casts: libnix printf has no %ll length modifier. */
    zzplay_info("zzplay: %s loop %lu: %lu input bytes, %lu audio frames\n",
           engine->codec_name, (unsigned long)completed,
           (unsigned long)decode.input_bytes,
           (unsigned long)(decode.output_frames > 0xffffffffULL
                               ? 0xffffffffULL
                               : decode.output_frames));
    zzplay_codec_decode_cleanup(&decode);
    if (pass == ZZPLAY_CODEC_PASS_STOPPED &&
        engine->ctl->request == ZZPLAY_REQUEST_SEEK) {
      uint32_t seek_ms = 0U;

      /* Cleanup closed the stream session and the AHI queue, dropping
       * everything decoded from the old position; start a fresh pass
       * there. Any stop point may have ended the pass on the seek. */
      (void)zzplay_controller_take_request(engine->ctl, 0, &seek_ms);
      offset = zzplay_mp3_seek_offset(engine->audio_start,
                                      engine->audio_end, engine->total_ms,
                                      seek_ms);
      base_ms = seek_ms;
      continue;
    }
    if (pass == ZZPLAY_CODEC_PASS_STOPPED) {
      result = ZZPLAY_ENGINE_STOPPED;
      goto done;
    }
    if (pass == ZZPLAY_CODEC_PASS_UNSUPPORTED) {
      zzplay_launch_reportf(
          engine->ctl, engine->run->options,
          decode.output_frames != 0U
              ? "the card cannot decode the rest of this %s stream (a "
                "chained or multiplexed Ogg link follows)"
              : "the card cannot decode this %s stream (unsupported "
                "layout)",
          engine->codec_name);
      goto done;
    }
    if (pass != ZZPLAY_CODEC_PASS_COMPLETED) {
      zzplay_launch_reportf(engine->ctl, engine->run->options,
                            "accelerated %s decode/output failed",
                            engine->codec_name);
      goto done;
    }
    if (zzplay_controller_loop_item(engine->ctl) ||
        (run->options->loop_mode == ZZPLAY_LOOP_FINITE &&
         repeats != 0U)) {
      if (run->options->loop_mode == ZZPLAY_LOOP_FINITE) {
        repeats--;
      }
      offset = engine->audio_start;
      base_ms = 0U;
      completed++;
      continue;
    }
    result = ZZPLAY_ENGINE_EOF;
    break;
  }

done:
  if (ctx) {
    zz9k_close(ctx);
  }
  return result;
}
