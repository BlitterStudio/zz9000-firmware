/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-mp3.h"

#include "zz9k/audio.h"
#include "zz9k/caps.h"
#include "zz9k/host.h"
#include "zz9k/shared.h"
#include "zzplay-ahi.h"
#include "zzplay-core.h"
#include "zzplay-gui.h"
#include "zzplay-launch.h"
#include "zzplay-mhi.h"
#include "zzplay-mp3-transport.h"
#include "zzplay-tags.h"

#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZZPLAY_MP3_PCM_CAPACITY ZZPLAY_MP3_DEFAULT_PCM_CAPACITY
#define ZZPLAY_MP3_Z2_PCM_CAPACITY (32UL * 1024UL)
#define ZZPLAY_MP3_Z2_STAGING_CAPACITY (16UL * 1024UL)
#define ZZPLAY_MP3_DRAIN_GUARD 128U

/* Everything both backends need about the item being played. */
typedef struct ZZPlayMP3Engine {
  const ZZPlayEngineRun *run;
  ZZPlayController *ctl;
  /* Audio payload boundaries: after the ID3v2 tag, before the ID3v1
   * trailer. Seeking and the duration estimate work inside this range. */
  uint64_t audio_start;
  uint64_t audio_end;
  const ZZPlayMP3Info *info;
  /* Stream the accelerated path decodes: MP3 geometry comes from the frame
   * probe, FLAC from STREAMINFO. pcm_width is the bytes per channel
   * sample of pcm_format (2 for S16BE, 4 for MSB-justified S32BE). */
  uint32_t codec;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t pcm_format;
  uint32_t pcm_width;
  /* Largest PCM unit the decoder writes at once (an MP3 frame, a FLAC
   * block, a Vorbis packet); the firmware needs room for it whole. */
  uint32_t min_pcm_bytes;
  /* Largest compressed unit the decoder buffers whole before decoding
   * (a FLAC frame); 0 when the codec has no such requirement. */
  uint32_t max_input_unit;
  const char *codec_name;
  /* MP3 restarts a pass at any frame boundary; native FLAC is decoded
   * from its stream header, so it always starts at audio_start. */
  int seekable;
  uint32_t total_ms;
  /* Backend descriptor without the muted marker ("MHI <driver>",
   * "AHI unit N"). */
  char output_base[40];
  /* The resolved backend is NONE (AUDIO=none, --benchmark, or a saved
   * MP3OUTPUT=NONE): decode without opening AHI. */
  int muted;
} ZZPlayMP3Engine;

static void zzplay_mp3_pump(const ZZPlayMP3Engine *engine)
{
  if (engine->run->pump) {
    engine->run->pump(engine->run->pump_user);
  }
}


/* Output text marks a requested mute only when the active backend honours
 * volume; a saved zero remains available for a later capable backend. */
static void zzplay_mp3_refresh_output(const ZZPlayMP3Engine *engine)
{
  char text[ZZPLAY_NOW_OUTPUT_MAX];

  if (engine->ctl->volume == 0U && engine->ctl->now.volume_supported) {
    sprintf(text, "%s, muted", engine->output_base);
  } else {
    strcpy(text, engine->output_base);
  }
  zzplay_controller_set_output(engine->ctl, text);
}

static void zzplay_mp3_set_output(ZZPlayMP3Engine *engine,
                                  const char *output)
{
  strncpy(engine->output_base, output, sizeof(engine->output_base) - 1U);
  engine->output_base[sizeof(engine->output_base) - 1U] = '\0';
  zzplay_mp3_refresh_output(engine);
}

/* One accelerated-decode pass over the file (the AHI backend). */
typedef struct ZZPlayMP3Decode {
  ZZ9KContext *ctx;
  FILE *file;
  ZZ9KSharedBuffer compressed;
  ZZ9KSharedBuffer pcm;
  ZZ9KSharedBuffer staging;
  ZZ9KAudioStreamResult result;
  ZZPlayAHISink *ahi;
  const ZZPlayMP3Engine *engine;
  uint32_t produced_seen;
  uint32_t pcm_offset;
  uint32_t pending_ack;
  uint32_t input_bytes;
  uint64_t output_frames;
  /* Where in the item this pass started; positions are reported from
   * here plus the frames this pass has played. */
  uint32_t position_base_ms;
  /* Last non-OK status of a Feed or Read; UNSUPPORTED means the card
   * refused the stream's layout (a chained or multiplexed Ogg link, or a
   * geometry it cannot decode), not a transport failure. */
  int stream_status;
  uint8_t session_open;
  uint8_t ahi_started;
  uint8_t compact_z2;
} ZZPlayMP3Decode;

typedef enum ZZPlayMP3PassResult {
  ZZPLAY_MP3_PASS_FAILED = 0,
  ZZPLAY_MP3_PASS_COMPLETED,
  ZZPLAY_MP3_PASS_STOPPED,
  ZZPLAY_MP3_PASS_UNSUPPORTED
} ZZPlayMP3PassResult;

static int zzplay_mp3_should_stop(const ZZPlayMP3Decode *decode)
{
  return zzplay_controller_item_should_end(decode->engine->ctl);
}

/* Live controls for the AHI path: volume is applied here; any request
 * ends the pass (returns 1) and stays pending. zzplay_mp3_accelerated
 * turns a SEEK into a fresh pass at the new offset, so neither compressed
 * data the card already accepted nor PCM queued to AHI from the old
 * position is played after the jump. */
static int zzplay_mp3_service_controls(ZZPlayMP3Decode *decode)
{
  ZZPlayController *ctl = decode->engine->ctl;
  uint32_t volume;

  if (zzplay_controller_take_volume(ctl, &volume)) {
    if (decode->ahi) {
      zzplay_ahi_set_volume(decode->ahi, volume);
    }
    zzplay_mp3_refresh_output(decode->engine);
  }
  return zzplay_controller_item_should_end(ctl);
}

static void zzplay_mp3_report_progress(const ZZPlayMP3Decode *decode)
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
static int zzplay_mp3_service_ready(ZZ9KContext *ctx, ZZ9KCaps *caps,
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

static int zzplay_mp3_result_valid(const ZZPlayMP3Decode *decode)
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
  if (status != ZZ9K_STATUS_OK) {
    decode->stream_status = status;
    return 0;
  }
  if (!zzplay_mp3_result_valid(decode)) {
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
    zzplay_mp3_pump(decode->engine);
  }
  return buffer;
}

static int zzplay_mp3_copy_ring(const ZZPlayMP3Decode *decode,
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
static int zzplay_mp3_copy_to_ahi(const ZZPlayMP3Decode *decode,
                                  uint8_t *destination,
                                  uint32_t ring_bytes)
{
  static uint8_t bounce[4096];
  uint32_t offset = decode->pcm_offset;

  if (decode->engine->pcm_width == 2U) {
    return zzplay_mp3_copy_ring(decode, offset, destination, ring_bytes);
  }
  while (ring_bytes != 0U) {
    uint32_t bytes = ring_bytes < sizeof(bounce) ? ring_bytes
                                                 : (uint32_t)sizeof(bounce);
    uint32_t i;

    if (!zzplay_mp3_copy_ring(decode, offset, bounce, bytes)) {
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

static int zzplay_mp3_pump_pcm(ZZPlayMP3Decode *decode, int final)
{
  uint32_t available;
  uint32_t ring_frame = decode->engine->channels * decode->engine->pcm_width;
  uint32_t period_bytes = decode->ahi
                              ? decode->ahi->period_frames * ring_frame
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
    decode->output_frames += available / ring_frame;
    return zzplay_mp3_ack(decode, final);
  }
  while (available >= period_bytes || (final && available >= ring_frame)) {
    void *buffer;
    size_t capacity;
    uint32_t bytes = available < period_bytes ? available : period_bytes;
    uint32_t frames = bytes / ring_frame;
    uint32_t ahi_bytes = frames * decode->ahi->frame_bytes;

    bytes = frames * ring_frame;
    if (zzplay_mp3_should_stop(decode)) {
      return 0;
    }
    buffer = zzplay_mp3_acquire_ahi(decode, &capacity);
    if (!buffer || ahi_bytes > capacity ||
        !zzplay_mp3_copy_to_ahi(decode, (uint8_t *)buffer, bytes) ||
        !zzplay_ahi_submit_buffer(decode->ahi, ahi_bytes)) {
      return 0;
    }
    decode->produced_seen += bytes;
    decode->pcm_offset = zzplay_mp3_ring_advance(
        decode->pcm_offset, bytes, decode->pcm.length);
    decode->pending_ack += bytes;
    decode->output_frames += frames;
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
    zzplay_mp3_report_progress(decode);
    zzplay_mp3_pump(decode->engine);
    Delay(1U);
  }
  return 1;
}

/* Why a pass ended early. When the card refused the stream's layout (for
 * Ogg, a further chained or multiplexed link after the first one played),
 * the audio already queued to AHI plays out before the item fails with an
 * explicit unsupported-stream error, so it is never reported as complete. */
static ZZPlayMP3PassResult zzplay_mp3_pass_failure(ZZPlayMP3Decode *decode)
{
  if (zzplay_mp3_should_stop(decode)) {
    return ZZPLAY_MP3_PASS_STOPPED;
  }
  if (decode->stream_status != ZZ9K_STATUS_UNSUPPORTED) {
    return ZZPLAY_MP3_PASS_FAILED;
  }
  (void)zzplay_mp3_finish_ahi(decode);
  return zzplay_mp3_should_stop(decode) ? ZZPLAY_MP3_PASS_STOPPED
                                        : ZZPLAY_MP3_PASS_UNSUPPORTED;
}

/* The stream decoder has no rate or channel conversion, so the firmware
 * rejects a non-zero output geometry outright. Ask for the file's native
 * rate/channels; zzplay_mp3_result_valid then holds the decoded stream to
 * the geometry the probe already reported. MP3 keeps the original Begin so
 * it runs on firmware that predates the codec-aware BeginEx; its high
 * water stays the staging size. The BeginEx codecs pass no high water, as
 * the sound class does, so per-call decode is never capped below one FLAC
 * block or Vorbis packet. */
static int zzplay_mp3_begin(ZZPlayMP3Decode *decode)
{
  const ZZPlayMP3Engine *engine = decode->engine;

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

static uint32_t zzplay_mp3_round_4k(uint32_t bytes)
{
  return (bytes + 4095UL) & ~4095UL;
}

/* PCM ring for one pass: the bus profile's default, raised to twice the
 * decoder's largest PCM unit so a whole AHI period (at most half the ring)
 * and one more unit fit together. */
static uint32_t zzplay_mp3_pcm_capacity(const ZZPlayMP3Engine *engine,
                                        int compact_z2)
{
  uint32_t base = compact_z2 ? ZZPLAY_MP3_Z2_PCM_CAPACITY
                             : ZZPLAY_MP3_PCM_CAPACITY;
  uint32_t want = zzplay_mp3_round_4k(2U * engine->min_pcm_bytes);

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
static int zzplay_mp3_alloc_rings(ZZPlayMP3Decode *decode)
{
  const ZZPlayMP3Engine *engine = decode->engine;
  uint32_t host_flags = decode->compact_z2 ? ZZ9K_ALLOC_HOST_WINDOW : 0U;
  uint32_t staging_max = decode->compact_z2 ? ZZPLAY_MP3_Z2_STAGING_CAPACITY
                                            : ZZPLAY_MP3_FEED_MAX_BYTES;
  uint32_t pcm = zzplay_mp3_pcm_capacity(engine, decode->compact_z2);
  uint32_t pcm_floor = zzplay_mp3_round_4k(engine->min_pcm_bytes);
  uint32_t input = ZZPLAY_MP3_INPUT_CAPACITY;

  if (pcm_floor < 8192U) {
    pcm_floor = 8192U;
  }
  for (;;) {
    if (zz9k_alloc_shared(decode->ctx, pcm, 16U, host_flags,
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
    }
    memset(&decode->pcm, 0, sizeof(decode->pcm));
    if (pcm <= pcm_floor) {
      return 0;
    }
    pcm = zzplay_mp3_round_4k(pcm / 2U);
    if (pcm < pcm_floor) {
      pcm = pcm_floor;
    }
  }

input_ring:
  /* A 16384-sample 24-bit FLAC frame is over 70 KB: with a 64 KiB chunk
   * behind it, the default ring cannot hold both and the decoder faults. */
  if (engine->max_input_unit != 0U &&
      engine->max_input_unit + decode->staging.length > input) {
    input = zzplay_mp3_round_4k(engine->max_input_unit +
                                decode->staging.length);
  }
  if (zz9k_alloc_shared(decode->ctx, input, 16U, ZZ9K_ALLOC_CARD_ONLY,
                        &decode->compressed) != ZZ9K_STATUS_OK) {
    memset(&decode->compressed, 0, sizeof(decode->compressed));
    return 0;
  }
  return 1;
}

static ZZPlayMP3PassResult zzplay_mp3_decode_once(ZZPlayMP3Decode *decode)
{
  static uint8_t chunk[ZZPLAY_MP3_FEED_MAX_BYTES];
  int status;
  unsigned flush_guard;

  memset(&decode->result, 0, sizeof(decode->result));
  status = zzplay_mp3_begin(decode);
  if (status != ZZ9K_STATUS_OK) {
    return ZZPLAY_MP3_PASS_FAILED;
  }
  decode->session_open = 1U;
  decode->pcm_offset = decode->result.pcm_read;

  for (;;) {
    ZZ9KAudioStreamFeedDesc feed;
    size_t got;
    uint32_t flags;
    unsigned retry = 0U;

    if (zzplay_mp3_service_controls(decode)) {
      return ZZPLAY_MP3_PASS_STOPPED;
    }
    zzplay_mp3_pump(decode->engine);
    /* Pause by ceasing to feed, confirmed once the hold is in place so
     * modal operations may run. The AHI queue plays out first, so audio
     * stops after at most the queued depth rather than instantly. Keep
     * servicing AHI while held so completed buffers are still reaped. */
    while (decode->engine->ctl->paused) {
      if (zzplay_mp3_service_controls(decode)) {
        return ZZPLAY_MP3_PASS_STOPPED;
      }
      zzplay_controller_set_engine_paused(decode->engine->ctl, 1);
      if (decode->ahi && decode->ahi_started) {
        zzplay_ahi_poll(decode->ahi);
      }
      zzplay_mp3_pump(decode->engine);
      Delay(1U);
    }
    zzplay_controller_set_engine_paused(decode->engine->ctl, 0);
    zzplay_mp3_report_progress(decode);
    got = fread(chunk, 1U, decode->staging.length, decode->file);
    if (got == 0U && ferror(decode->file)) {
      return ZZPLAY_MP3_PASS_FAILED;
    }
    flags = got == 0U ? ZZ9K_AUDIO_STREAM_FEED_EOF : 0U;
    if (got != 0U &&
        !zz9k_shared_copy_to(&decode->staging, 0U, chunk,
                             (uint32_t)got)) {
      return ZZPLAY_MP3_PASS_FAILED;
    }
    do {
      if (++retry > 64U ||
          !zz9k_audio_build_stream_feed_desc(
              &feed, decode->result.session, decode->staging.handle,
              0U, (uint32_t)got, flags)) {
        return ZZPLAY_MP3_PASS_FAILED;
      }
      status = zz9k_audio_stream_feed(
          decode->ctx, &feed, &decode->result);
      if (status != ZZ9K_STATUS_OK) {
        decode->stream_status = status;
      }
      if (status != ZZ9K_STATUS_OK ||
          !zzplay_mp3_pump_pcm(decode, flags != 0U)) {
        return zzplay_mp3_pass_failure(decode);
      }
      /* Under backpressure hand AHI whatever whole frames are waiting, even
       * less than a period, before the forced Read: a decoder that needs
       * room for a whole block must never wait on a partial period. */
      if ((decode->result.flags &
           ZZ9K_AUDIO_STREAM_RESULT_BACKPRESSURE) != 0U &&
          (!zzplay_mp3_pump_pcm(decode, 1) || !zzplay_mp3_ack(decode, 1))) {
        return zzplay_mp3_pass_failure(decode);
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
      return zzplay_mp3_pass_failure(decode);
    }
    if (decode->pending_ack == 0U &&
        decode->result.bytes_produced == decode->produced_seen &&
        ((decode->result.flags & ZZ9K_AUDIO_STREAM_RESULT_DONE) != 0U ||
         before == decode->produced_seen)) {
      break;
    }
  }
  if (flush_guard >= ZZPLAY_MP3_DRAIN_GUARD ||
      decode->output_frames == 0U) {
    return ZZPLAY_MP3_PASS_FAILED;
  }
  return zzplay_mp3_finish_ahi(decode) ? ZZPLAY_MP3_PASS_COMPLETED
                                       : ZZPLAY_MP3_PASS_FAILED;
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
static ZZPlayEngineResult zzplay_mp3_accelerated(
    ZZPlayMP3Engine *engine, int *unavailable)
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
      !zzplay_mp3_service_ready(ctx, &caps, engine->codec)) {
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
    ZZPlayMP3Decode decode;
    ZZPlayMP3PassResult pass;

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
      zzplay_mp3_decode_cleanup(&decode);
      goto done;
    }
    if (fseek(decode.file, (long)offset, SEEK_SET) != 0) {
      zzplay_launch_reportf(engine->ctl, engine->run->options,
                            "cannot seek in %s", run->path);
      zzplay_mp3_decode_cleanup(&decode);
      goto done;
    }
    if (!zzplay_mp3_alloc_rings(&decode)) {
      if (unavailable && !started) {
        *unavailable = 1;
      } else {
        zzplay_launch_reportf(
            engine->ctl, engine->run->options,
            "not enough shared card memory for %s streaming",
            engine->codec_name);
      }
      zzplay_mp3_decode_cleanup(&decode);
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
        zzplay_mp3_decode_cleanup(&decode);
        goto done;
      }
      zzplay_ahi_set_volume(&ahi, engine->ctl->volume);
      decode.ahi = &ahi;
    }
    started = 1;
    pass = zzplay_mp3_decode_once(&decode);
    /* Saturating casts: libnix printf has no %ll length modifier. */
    zzplay_info("zzplay: %s loop %lu: %lu input bytes, %lu audio frames\n",
           engine->codec_name, (unsigned long)completed,
           (unsigned long)decode.input_bytes,
           (unsigned long)(decode.output_frames > 0xffffffffULL
                               ? 0xffffffffULL
                               : decode.output_frames));
    zzplay_mp3_decode_cleanup(&decode);
    if (pass == ZZPLAY_MP3_PASS_STOPPED &&
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
    if (pass == ZZPLAY_MP3_PASS_STOPPED) {
      result = ZZPLAY_ENGINE_STOPPED;
      goto done;
    }
    if (pass == ZZPLAY_MP3_PASS_UNSUPPORTED) {
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
    if (pass != ZZPLAY_MP3_PASS_COMPLETED) {
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

/* zzplay_mhi_play_file() carries one opaque pointer, so bundle everything
 * the callbacks need behind it. */
typedef struct ZZPlayMHIBridge {
  ZZPlayMP3Engine *engine;
  ZZPlayMHISink *sink;
  /* Where the current pass started, in bytes and in time; the reported
   * MHI position is derived from the bytes handed to the decoder since
   * then and therefore runs slightly ahead (shown as approximate). */
  uint64_t offset;
  uint32_t base_ms;
  /* A consumed SEEK ended the pass; restart at the new offset. */
  int seek_restart;
} ZZPlayMHIBridge;

static int zzplay_mp3_mhi_stop_thunk(void *user)
{
  ZZPlayMHIBridge *bridge = (ZZPlayMHIBridge *)user;
  ZZPlayController *ctl;
  uint32_t volume;

  if (!bridge) {
    return 0;
  }
  ctl = bridge->engine->ctl;
  /* Runs once per driver poll: this is where the MHI path pumps the GUI,
   * reports its position and applies live controls. */
  zzplay_mp3_pump(bridge->engine);
  if (bridge->engine->info->bitrate_kbps != 0U) {
    zzplay_controller_set_position(
        ctl,
        bridge->base_ms +
            (uint32_t)((bridge->sink->input_bytes * 8ULL) /
                       bridge->engine->info->bitrate_kbps),
        0);
  }
  if (zzplay_controller_take_volume(ctl, &volume)) {
    (void)zzplay_mhi_set_volume(bridge->sink, volume);
    zzplay_mp3_refresh_output(bridge->engine);
  }
  if (ctl->request == ZZPLAY_REQUEST_SEEK) {
    uint32_t seek_ms = 0U;

    (void)zzplay_controller_take_request(ctl, 0, &seek_ms);
    bridge->offset = zzplay_mp3_seek_offset(
        bridge->engine->audio_start, bridge->engine->audio_end,
        bridge->engine->total_ms, seek_ms);
    bridge->base_ms = seek_ms;
    bridge->seek_restart = 1;
    return 1;
  }
  return zzplay_controller_item_should_end(ctl);
}

static int zzplay_mp3_mhi_paused_thunk(void *user)
{
  ZZPlayMHIBridge *bridge = (ZZPlayMHIBridge *)user;
  ZZPlayController *ctl;

  if (!bridge) {
    return 0;
  }
  ctl = bridge->engine->ctl;
  /* play_file drives the sink's real MHI pause off this answer. Confirm
   * the hold once the sink reports it, so modal operations may run. */
  if (bridge->sink->paused) {
    zzplay_controller_set_engine_paused(ctl, 1);
  } else if (!ctl->paused) {
    zzplay_controller_set_engine_paused(ctl, 0);
  }
  return ctl->paused != 0;
}

static ZZPlayEngineResult zzplay_mp3_mhi(ZZPlayMP3Engine *engine,
                                         ZZPlayMHIStatus *open_status)
{
  const ZZPlayEngineRun *run = engine->run;
  ZZPlayMHIBridge bridge;
  ZZPlayMHISink sink;
  ZZPlayMHIStatus status;
  uint32_t repeats = run->options->loop_count;
  uint32_t completed = 0U;
  ZZPlayEngineResult result = ZZPLAY_ENGINE_FAILED;

  memset(&bridge, 0, sizeof(bridge));
  memset(&sink, 0, sizeof(sink));
  bridge.engine = engine;
  bridge.sink = &sink;
  bridge.offset = zzplay_mp3_seek_offset(
      engine->audio_start, engine->audio_end, engine->total_ms,
      run->seek_ms);
  bridge.base_ms = run->seek_ms;
  status = zzplay_mhi_acquire(&sink, run->prefs->mhi_driver);
  *open_status = status;
  if (status != ZZPLAY_MHI_OK) {
    return ZZPLAY_ENGINE_FAILED;
  }
  zzplay_controller_set_capabilities(
      run->ctl, 1, sink.volume_supported != 0, 0);
  {
    char output[ZZPLAY_NOW_OUTPUT_MAX];

    sprintf(output, "MHI %s", zzplay_mhi_driver_name(&sink));
    zzplay_mp3_set_output(engine, output);
  }
  (void)zzplay_mhi_set_volume(&sink, run->ctl->volume);
  zzplay_info("zzplay: selected audio backend MHI (card-local Layer III)\n");
  for (;;) {
    FILE *file = fopen(run->path, "rb");

    if (!file) {
      *open_status = ZZPLAY_MHI_IO_ERROR;
      goto done;
    }
    if (fseek(file, (long)bridge.offset, SEEK_SET) != 0) {
      fclose(file);
      *open_status = ZZPLAY_MHI_IO_ERROR;
      goto done;
    }
    bridge.seek_restart = 0;
    status = zzplay_mhi_play_file(
        &sink, file, zzplay_mp3_mhi_stop_thunk,
        zzplay_mp3_mhi_paused_thunk, &bridge);
    fclose(file);
    if (bridge.seek_restart) {
      /* The seek was consumed inside the poll; restart the pass at the
       * new offset with the decoder still held. */
      continue;
    }
    if (status == ZZPLAY_MHI_STOPPED) {
      result = ZZPLAY_ENGINE_STOPPED;
      goto done;
    }
    if (status != ZZPLAY_MHI_OK) {
      *open_status = status;
      goto done;
    }
    zzplay_info("zzplay: MP3 MHI loop %lu: %lu input bytes\n",
           (unsigned long)completed,
           (unsigned long)(sink.input_bytes > 0xffffffffULL
                               ? 0xffffffffULL
                               : sink.input_bytes));
    if (zzplay_controller_loop_item(engine->ctl) ||
        (run->options->loop_mode == ZZPLAY_LOOP_FINITE &&
         repeats != 0U)) {
      if (run->options->loop_mode == ZZPLAY_LOOP_FINITE) {
        repeats--;
      }
      bridge.offset = engine->audio_start;
      bridge.base_ms = 0U;
      completed++;
      continue;
    }
    zzplay_info("zzplay: MP3 MHI playback complete, %lu loops\n",
           (unsigned long)completed);
    result = ZZPLAY_ENGINE_EOF;
    break;
  }

done:
  zzplay_mhi_release(&sink);
  return result;
}

ZZPlayEngineResult zzplay_mp3_run(const ZZPlayEngineRun *run)
{
  ZZPlayMP3Engine engine;
  ZZPlayMHIStatus mhi_status = ZZPLAY_MHI_MISSING;
  ZZPlayAudioBackend requested;
  int strict = 0;

  if (!run || !run->ctl || !run->path || !run->probe ||
      !run->options || !run->prefs) {
    return ZZPLAY_ENGINE_FAILED;
  }
  memset(&engine, 0, sizeof(engine));
  engine.run = run;
  engine.ctl = run->ctl;
  engine.info = &run->probe->mp3;
  engine.codec = ZZ9K_AUDIO_CODEC_MP3;
  engine.sample_rate = engine.info->sample_rate;
  engine.channels = engine.info->channels;
  engine.pcm_format = ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE;
  engine.pcm_width = 2U;
  engine.min_pcm_bytes = 1152U * 2U * 2U;
  engine.codec_name = "MP3";
  engine.seekable = 1;

  /* The application read these boundaries with the probing handle, so this
   * engine needs no metadata-only open before its streaming handles. */
  engine.audio_start = run->audio_start;
  engine.audio_end = run->file_size > (uint64_t)run->trailer_bytes
                         ? run->file_size - run->trailer_bytes
                         : engine.audio_start;
  engine.total_ms = zzplay_mp3_duration_ms(
      engine.audio_end - engine.audio_start,
      engine.info->bitrate_kbps);
  zzplay_controller_set_duration(run->ctl, engine.total_ms);
  zzplay_controller_set_capabilities(run->ctl, 1, 0, 0);
  {
    char format[ZZPLAY_NOW_TEXT_MAX];

    sprintf(format, "MP3 %lu Hz, %s, %lu kbps",
            (unsigned long)engine.info->sample_rate,
            engine.info->channels == 1U ? "mono" : "stereo",
            (unsigned long)engine.info->bitrate_kbps);
    zzplay_controller_set_format(run->ctl, format);
  }

  requested = zzplay_prefs_requested_backend(
      run->prefs, run->options, ZZPLAY_MEDIA_AUDIO_MP3, &strict);
  if (requested == ZZPLAY_AUDIO_AX) {
    if (strict) {
      zzplay_launch_reportf(
          engine.ctl, engine.run->options,
          "direct AX is not a standalone MP3 backend; use MHI or AHI");
      return ZZPLAY_ENGINE_FAILED;
    }
    /* A saved (non-strict) AX preference falls back like AUTO. */
    requested = ZZPLAY_AUDIO_AUTO;
  }
  if (requested == ZZPLAY_AUDIO_MHI || requested == ZZPLAY_AUDIO_AUTO) {
    ZZPlayEngineResult mhi_result = zzplay_mp3_mhi(&engine, &mhi_status);

    if (mhi_result != ZZPLAY_ENGINE_FAILED) {
      return mhi_result;
    }
    if (run->ctl->request != ZZPLAY_REQUEST_NONE) {
      return ZZPLAY_ENGINE_STOPPED;
    }
    if (strict && requested == ZZPLAY_AUDIO_MHI) {
      zzplay_launch_reportf(engine.ctl, engine.run->options,
                            "MHI playback failed: %s",
                            zzplay_mhi_status_name(mhi_status));
      return ZZPLAY_ENGINE_FAILED;
    }
    if (mhi_status == ZZPLAY_MHI_IO_ERROR) {
      zzplay_launch_reportf(engine.ctl, engine.run->options,
                            "MHI playback failed: %s",
                            zzplay_mhi_status_name(mhi_status));
      return ZZPLAY_ENGINE_FAILED;
    }
    zzplay_info("zzplay: MHI unavailable before playback (%s); "
           "AUTO falling back to accelerated decode + AHI\n",
           zzplay_mhi_status_name(mhi_status));
  }
  if (requested == ZZPLAY_AUDIO_NONE) {
    zzplay_info("zzplay: selected audio backend NONE "
           "(accelerated Layer III decode benchmark)\n");
    engine.muted = 1;
    zzplay_controller_set_capabilities(run->ctl, 1, 0, 0);
    zzplay_mp3_set_output(&engine, "none");
  } else {
    zzplay_info("zzplay: selected audio backend AHI "
           "(accelerated Layer III decode, S16BE)\n");
    zzplay_controller_set_capabilities(run->ctl, 1, 1, 0);
    sprintf(engine.output_base, "AHI unit %lu",
            (unsigned long)run->prefs->ahi_unit);
    zzplay_mp3_refresh_output(&engine);
  }
  if (requested == ZZPLAY_AUDIO_AHI && !strict) {
    /* A saved AHI preference falls back like AUTO when its path cannot
     * be had before playback; an explicit --audio=ahi does not. */
    int unavailable = 0;
    ZZPlayEngineResult result = zzplay_mp3_accelerated(&engine, &unavailable);

    if (!unavailable || run->ctl->request != ZZPLAY_REQUEST_NONE) {
      return result;
    }
    zzplay_info("zzplay: saved AHI output unavailable before playback; "
           "falling back to MHI\n");
    result = zzplay_mp3_mhi(&engine, &mhi_status);
    if (result != ZZPLAY_ENGINE_FAILED) {
      return result;
    }
    if (run->ctl->request != ZZPLAY_REQUEST_NONE) {
      return ZZPLAY_ENGINE_STOPPED;
    }
    zzplay_launch_reportf(engine.ctl, engine.run->options,
                          "AHI output unavailable; MHI playback failed: %s",
                          zzplay_mhi_status_name(mhi_status));
    return ZZPLAY_ENGINE_FAILED;
  }
  return zzplay_mp3_accelerated(&engine, 0);
}

/* Backend policy shared by the codecs that only the card's stream decoder
 * plays (FLAC, Ogg Vorbis): accelerated decode + AHI, or NONE. No saved
 * per-media output applies (MP3OUTPUT names an MHI path they cannot use);
 * an explicit AUDIO= option still wins. */
static ZZPlayEngineResult zzplay_card_stream_run(ZZPlayMP3Engine *engine)
{
  const ZZPlayEngineRun *run = engine->run;
  ZZPlayAudioBackend requested;
  int strict = 0;

  zzplay_controller_set_duration(run->ctl, engine->total_ms);
  zzplay_controller_set_capabilities(run->ctl, 0, 0, 0);
  requested = zzplay_prefs_requested_backend(
      run->prefs, run->options, ZZPLAY_MEDIA_AUDIO_NONE, &strict);
  if (strict && (requested == ZZPLAY_AUDIO_MHI ||
                 requested == ZZPLAY_AUDIO_AX)) {
    zzplay_launch_reportf(engine->ctl, run->options,
                          "%s plays through accelerated decode + AHI only",
                          engine->codec_name);
    return ZZPLAY_ENGINE_FAILED;
  }
  if (requested == ZZPLAY_AUDIO_NONE) {
    zzplay_info("zzplay: selected audio backend NONE "
           "(accelerated %s decode benchmark)\n", engine->codec_name);
    engine->muted = 1;
    zzplay_mp3_set_output(engine, "none");
  } else {
    zzplay_info("zzplay: selected audio backend AHI "
           "(accelerated %s decode, %s)\n", engine->codec_name,
           engine->pcm_width == 4U ? "S32BE narrowed to S16" : "S16BE");
    zzplay_controller_set_capabilities(run->ctl, 0, 1, 0);
    sprintf(engine->output_base, "AHI unit %lu",
            (unsigned long)run->prefs->ahi_unit);
    zzplay_mp3_refresh_output(engine);
  }
  return zzplay_mp3_accelerated(engine, 0);
}

static int zzplay_card_stream_engine(const ZZPlayEngineRun *run,
                                     ZZPlayMP3Engine *engine)
{
  if (!run || !run->ctl || !run->path || !run->probe ||
      !run->options || !run->prefs) {
    return 0;
  }
  memset(engine, 0, sizeof(*engine));
  engine->run = run;
  engine->ctl = run->ctl;
  engine->pcm_format = ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE;
  engine->pcm_width = 2U;
  engine->audio_start = 0U;
  engine->audio_end = run->file_size;
  return 1;
}

ZZPlayEngineResult zzplay_flac_run(const ZZPlayEngineRun *run)
{
  ZZPlayMP3Engine engine;
  const ZZPlayFLACInfo *info;
  char format[ZZPLAY_NOW_TEXT_MAX];

  if (!zzplay_card_stream_engine(run, &engine)) {
    return ZZPLAY_ENGINE_FAILED;
  }
  info = &run->probe->flac;
  engine.codec = ZZ9K_AUDIO_CODEC_FLAC;
  engine.codec_name = "FLAC";
  engine.max_input_unit = info->max_frame_bytes;
  engine.sample_rate = info->sample_rate;
  engine.channels = info->channels;
  /* The firmware refuses to truncate: wider sources need the
   * MSB-justified S32BE container, narrowed to S16 for AHI here. */
  if (info->bits_per_sample > 16U) {
    engine.pcm_format = ZZ9K_AUDIO_SAMPLE_FORMAT_S32BE;
    engine.pcm_width = 4U;
  }
  engine.min_pcm_bytes =
      info->max_block_size * info->channels * engine.pcm_width;
  engine.total_ms = info->sample_rate != 0U
                        ? (uint32_t)(info->total_samples * 1000ULL /
                                     info->sample_rate)
                        : 0U;
  sprintf(format, "FLAC %lu Hz, %s, %lu-bit",
          (unsigned long)info->sample_rate,
          info->channels == 1U ? "mono" : "stereo",
          (unsigned long)info->bits_per_sample);
  zzplay_controller_set_format(run->ctl, format);
  return zzplay_card_stream_run(&engine);
}

ZZPlayEngineResult zzplay_vorbis_run(const ZZPlayEngineRun *run)
{
  ZZPlayMP3Engine engine;
  const ZZPlayVorbisInfo *info;
  char format[ZZPLAY_NOW_TEXT_MAX];
  uint64_t granule = 0U;
  FILE *file;

  if (!zzplay_card_stream_engine(run, &engine)) {
    return ZZPLAY_ENGINE_FAILED;
  }
  info = &run->probe->vorbis;
  engine.codec = ZZ9K_AUDIO_CODEC_VORBIS;
  engine.codec_name = "Ogg Vorbis";
  /* Overlap-add emits at most half the long block per packet. */
  engine.min_pcm_bytes = info->max_block_samples / 2U * info->channels * 2U;
  engine.sample_rate = info->sample_rate;
  engine.channels = info->channels;
  /* The duration is the last page's granule position (the stream's sample
   * count) when that page belongs to the same logical stream; otherwise it
   * stays unknown rather than estimated. */
  file = fopen(run->path, "rb");
  if (file) {
    if (zzplay_ogg_last_granule(file, info->serial, &granule) &&
        info->sample_rate != 0U) {
      engine.total_ms = (uint32_t)(granule * 1000ULL / info->sample_rate);
    }
    fclose(file);
  }
  if (info->nominal_bitrate != 0U) {
    sprintf(format, "Ogg Vorbis %lu Hz, %s, ~%lu kbps",
            (unsigned long)info->sample_rate,
            info->channels == 1U ? "mono" : "stereo",
            (unsigned long)(info->nominal_bitrate / 1000U));
  } else {
    sprintf(format, "Ogg Vorbis %lu Hz, %s",
            (unsigned long)info->sample_rate,
            info->channels == 1U ? "mono" : "stereo");
  }
  zzplay_controller_set_format(run->ctl, format);
  return zzplay_card_stream_run(&engine);
}
