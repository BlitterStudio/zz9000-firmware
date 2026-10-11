/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-codec-stream.h"

#include "zz9k/audio.h"
#include "zz9k/caps.h"
#include "zz9k/host.h"
#include "zz9k/shared.h"
#include "zzplay-codec.h"
#include "zzplay-core.h"
#include "zzplay-launch.h"

#include <proto/dos.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* On-card FLAC/Vorbis. The window label is the same one video uses for
 * the ZZ9000AX output, so the player shows which path actually ran.
 * Position counts PCM the pump has staged (pcm_read is that cursor modulo
 * the ring; bytes_produced is the cumulative write). Seeking stays off. */
#define ZZPLAY_CARD_OUTPUT "ZZ9000AX direct"
#define ZZPLAY_CARD_WAIT_GUARD 4000U

typedef struct ZZPlayCardAttempt {
  ZZPlayEngineResult result;
  ZZPlayCardAnswer answer;
  int reported;
} ZZPlayCardAttempt;

static uint32_t zzplay_card_played_bytes(const ZZ9KAudioStreamResult *result,
                                         uint32_t capacity)
{
  uint32_t read;
  uint32_t used;

  if (!result || capacity == 0U) {
    return 0U;
  }
  read = result->pcm_read % capacity;
  used = (result->pcm_write + capacity - read) % capacity;
  if (used > result->bytes_produced) {
    return 0U;
  }
  return result->bytes_produced - used;
}

static void zzplay_card_report_position(const ZZPlayCodecDecode *decode)
{
  uint32_t rate = decode->result.sample_rate != 0U
                      ? decode->result.sample_rate
                      : decode->engine->sample_rate;
  uint32_t channels = decode->result.channels != 0U
                          ? decode->result.channels
                          : decode->engine->channels;
  uint32_t frame = channels * 2U;
  uint32_t played;

  if (rate == 0U || frame == 0U) {
    return;
  }
  played = zzplay_card_played_bytes(&decode->result, decode->pcm.length);
  zzplay_controller_set_position(
      decode->engine->ctl,
      (uint32_t)(((uint64_t)played / frame) * 1000ULL / rate), 1);
}

static void zzplay_card_apply_gain(ZZPlayCodecDecode *decode)
{
  ZZ9KAudioStreamResult result;
  uint32_t gain;

  if (!decode->gain_supported || decode->result.session == 0U) {
    return;
  }
  gain = zzplay_card_stream_gain(decode->engine->ctl->volume);
  if (zz9k_audio_stream_set_gain(decode->ctx, decode->result.session, gain,
                                 0U, &result) == ZZ9K_STATUS_OK) {
    decode->result = result;
  }
}

static void zzplay_card_show_ax(ZZPlayCodecEngine *engine, int *shown)
{
  if (!shown || *shown) {
    return;
  }
  *shown = 1;
  zzplay_codec_set_output(engine, ZZPLAY_CARD_OUTPUT);
  zzplay_info("zzplay: selected audio backend AX (on-card %s, S16LE)\n",
              engine->codec_name);
}

static int zzplay_card_service(ZZPlayCodecDecode *decode)
{
  ZZPlayController *ctl = decode->engine->ctl;
  uint32_t volume;

  if (zzplay_controller_take_volume(ctl, &volume)) {
    zzplay_card_apply_gain(decode);
    zzplay_codec_refresh_output(decode->engine);
  }
  return zzplay_controller_item_should_end(ctl);
}

static uint32_t zzplay_card_pcm_unit(const ZZPlayCodecEngine *engine)
{
  uint32_t unit = engine->min_pcm_bytes;

  /* AHI narrows S32BE on the host. The card path asks for S16LE, so the
   * ring only has to hold the narrowed block. */
  if (engine->pcm_width > 2U) {
    unit = (unit / engine->pcm_width) * 2U;
  }
  return unit == 0U ? 4U : unit;
}

static int zzplay_card_begin(ZZPlayCodecDecode *decode)
{
  ZZ9KAudioStreamBeginExDesc begin;

  /* low_water is the firmware pump's refill trigger: decode the next
   * input when half the PCM ring has played, as mhizz9000 does. At 0
   * the pump would refill only once the ring had run dry. */
  if (!zz9k_audio_build_stream_begin_ex_desc(
          &begin, decode->engine->codec, decode->compressed.handle,
          decode->compressed.length, decode->pcm.handle,
          decode->pcm.length, 0U, 0U, ZZ9K_AUDIO_SAMPLE_FORMAT_S16LE,
          decode->pcm.length / 2U, 0U, 0U)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  return zz9k_audio_stream_begin_ex(decode->ctx, &begin, &decode->result);
}

static int zzplay_card_gain_flag(ZZ9KContext *ctx)
{
  ZZ9KServiceInfo service;

  return zz9k_query_service(ctx, ZZ9K_SERVICE_AUDIO, &service) ==
             ZZ9K_STATUS_OK &&
         (service.flags & ZZ9K_SERVICE_FLAG_AUDIO_STREAM_GAIN) != 0U;
}

/* Bind once the decoder has published a sample rate. Play before that is
 * BAD_REQUEST. A later Play on an already-bound session is the status
 * probe; Stop then Play resumes without closing. */
static int zzplay_card_bind(ZZPlayCodecDecode *decode, int *shown,
                            ZZPlayCardAnswer *refusal)
{
  int status;

  if (decode->ax_bound || decode->engine->ctl->paused) {
    return 1;
  }
  if (decode->result.sample_rate == 0U && decode->result.session != 0U) {
    status = zz9k_audio_stream_read(decode->ctx, decode->result.session, 0U,
                                     0U, &decode->result);
    if (status != ZZ9K_STATUS_OK) {
      decode->stream_status = status;
      return 0;
    }
  }
  if (decode->result.sample_rate == 0U) {
    return decode->eof_sent ? 0 : 1;
  }
  status = zz9k_audio_stream_play(decode->ctx, decode->result.session, 0U,
                                   &decode->result);
  if ((status == ZZ9K_STATUS_UNSUPPORTED || status == ZZ9K_STATUS_BUSY) &&
      !decode->ax_committed) {
    *refusal = status == ZZ9K_STATUS_BUSY ? ZZPLAY_CARD_BUSY
                                          : ZZPLAY_CARD_UNSUPPORTED;
    decode->stream_status = status;
    return 0;
  }
  if (status != ZZ9K_STATUS_OK) {
    decode->stream_status = status;
    return 0;
  }
  decode->ax_bound = 1U;
  decode->ax_committed = 1U;
  zzplay_controller_set_capabilities(decode->engine->ctl, 0,
                                     decode->gain_supported != 0U, 0);
  zzplay_card_show_ax(decode->engine, shown);
  zzplay_card_apply_gain(decode);
  return 1;
}

static int zzplay_card_pause(ZZPlayCodecDecode *decode, int *shown,
                             ZZPlayCardAnswer *refusal)
{
  ZZPlayController *ctl = decode->engine->ctl;
  int status;

  if (!ctl->paused) {
    if (!decode->ax_resume) {
      zzplay_controller_set_engine_paused(ctl, 0);
    }
    return 0;
  }
  if (decode->ax_bound) {
    ZZ9KAudioStreamResult result;

    if (zz9k_audio_stream_stop(decode->ctx, decode->result.session, 0U,
                               &result) == ZZ9K_STATUS_OK) {
      decode->result = result;
    }
    decode->ax_bound = 0U;
    decode->ax_resume = 1U;
  }
  zzplay_controller_set_engine_paused(ctl, 1);
  while (ctl->paused) {
    if (zzplay_card_service(decode)) {
      return 1;
    }
    zzplay_codec_pump(decode->engine);
    Delay(1);
  }
  zzplay_controller_set_engine_paused(ctl, 0);
  if (!decode->ax_resume) {
    return 0;
  }
  decode->ax_resume = 0U;
  status = zzplay_card_bind(decode, shown, refusal);
  return status ? 0 : -1;
}

/* A BACKPRESSURE feed kept none of the chunk: the input ring had no room.
 * The flag stays set until the next accepted feed, so it cannot say when
 * to retry. Room returns as the pump plays and the refill decodes, which
 * the result reports as bytes_consumed. Wait for room for this chunk,
 * then feed it again. The guard counts only polls without progress. */
static int zzplay_card_wait_room(ZZPlayCodecDecode *decode, uint32_t chunk,
                                 int *shown, ZZPlayCardAnswer *refusal)
{
  uint32_t consumed = decode->result.bytes_consumed;
  unsigned idle = 0U;

  while (decode->input_bytes - decode->result.bytes_consumed + chunk >
         decode->compressed.length) {
    int held;
    int status;

    if (zzplay_card_service(decode)) {
      return 0;
    }
    held = zzplay_card_pause(decode, shown, refusal);
    if (held != 0 || !zzplay_card_bind(decode, shown, refusal)) {
      return 0;
    }
    if (!decode->ax_bound) {
      /* The ring is full and Play has not taken it. Reading the PCM
       * back would drop samples the pump has not played. */
      return 0;
    }
    status = zz9k_audio_stream_play(decode->ctx, decode->result.session, 0U,
                                     &decode->result);
    if (status != ZZ9K_STATUS_OK) {
      decode->stream_status = status;
      return 0;
    }
    if (decode->result.bytes_consumed != consumed) {
      consumed = decode->result.bytes_consumed;
      idle = 0U;
    } else if (++idle > ZZPLAY_CARD_WAIT_GUARD) {
      return 0;
    }
    zzplay_card_report_position(decode);
    zzplay_codec_pump(decode->engine);
    Delay(1);
  }
  return 1;
}

static int zzplay_card_drain(ZZPlayCodecDecode *decode, int *shown,
                             ZZPlayCardAnswer *refusal)
{
  unsigned guard;

  for (guard = 0U; guard < ZZPLAY_CARD_WAIT_GUARD; guard++) {
    int held;
    int status;

    if (zzplay_card_service(decode)) {
      return 0;
    }
    held = zzplay_card_pause(decode, shown, refusal);
    if (held != 0) {
      return 0;
    }
    if ((decode->result.flags & ZZ9K_AUDIO_STREAM_RESULT_DONE) != 0U) {
      zzplay_card_report_position(decode);
      return 1;
    }
    if (decode->ax_bound) {
      status = zz9k_audio_stream_play(decode->ctx, decode->result.session,
                                       0U, &decode->result);
      if (status != ZZ9K_STATUS_OK) {
        decode->stream_status = status;
        return 0;
      }
    }
    zzplay_card_report_position(decode);
    zzplay_codec_pump(decode->engine);
    Delay(1);
  }
  return 0;
}

static ZZPlayCodecPassResult zzplay_card_stop_or_fail(ZZPlayCodecDecode *decode)
{
  return zzplay_codec_should_stop(decode) ? ZZPLAY_CODEC_PASS_STOPPED
                                          : ZZPLAY_CODEC_PASS_FAILED;
}

static ZZPlayCodecPassResult zzplay_card_play_once(ZZPlayCodecDecode *decode,
                                                   int *shown,
                                                   ZZPlayCardAnswer *refusal)
{
  int status;

  *refusal = ZZPLAY_CARD_OK;
  memset(&decode->result, 0, sizeof(decode->result));
  status = zzplay_card_begin(decode);
  if (status == ZZ9K_STATUS_UNSUPPORTED || status == ZZ9K_STATUS_BUSY) {
    *refusal = status == ZZ9K_STATUS_BUSY ? ZZPLAY_CARD_BUSY
                                          : ZZPLAY_CARD_UNSUPPORTED;
    return ZZPLAY_CODEC_PASS_FAILED;
  }
  if (status != ZZ9K_STATUS_OK) {
    decode->stream_status = status;
    return ZZPLAY_CODEC_PASS_FAILED;
  }
  decode->session_open = 1U;
  zzplay_card_apply_gain(decode);

  for (;;) {
    ZZ9KAudioStreamFeedDesc feed;
    size_t got = 0U;
    uint32_t flags = 0U;
    int held;
    unsigned retry;

    if (zzplay_card_service(decode)) {
      return ZZPLAY_CODEC_PASS_STOPPED;
    }
    held = zzplay_card_pause(decode, shown, refusal);
    if (held > 0) {
      return ZZPLAY_CODEC_PASS_STOPPED;
    }
    if (held < 0) {
      return zzplay_card_stop_or_fail(decode);
    }
    zzplay_card_report_position(decode);
    zzplay_codec_pump(decode->engine);

    if (!decode->eof_sent) {
      got = fread(zzplay_codec_chunk, 1U, decode->staging.length,
                  decode->file);
      if (got == 0U && ferror(decode->file)) {
        return ZZPLAY_CODEC_PASS_FAILED;
      }
      flags = got == 0U ? ZZ9K_AUDIO_STREAM_FEED_EOF : 0U;
      if (got != 0U &&
          !zz9k_shared_copy_to(&decode->staging, 0U, zzplay_codec_chunk,
                               (uint32_t)got)) {
        return ZZPLAY_CODEC_PASS_FAILED;
      }
      for (retry = 0U;; retry++) {
        if (retry > 64U ||
            !zz9k_audio_build_stream_feed_desc(
                &feed, decode->result.session, decode->staging.handle, 0U,
                (uint32_t)got, flags)) {
          return ZZPLAY_CODEC_PASS_FAILED;
        }
        status = zz9k_audio_stream_feed(decode->ctx, &feed, &decode->result);
        if (status == ZZ9K_STATUS_UNSUPPORTED) {
          decode->stream_status = status;
          if (decode->ax_committed) {
            (void)zzplay_card_drain(decode, shown, refusal);
          }
          return zzplay_codec_should_stop(decode)
                     ? ZZPLAY_CODEC_PASS_STOPPED
                     : ZZPLAY_CODEC_PASS_UNSUPPORTED;
        }
        if (status != ZZ9K_STATUS_OK) {
          decode->stream_status = status;
          return zzplay_card_stop_or_fail(decode);
        }
        if ((decode->result.flags &
             ZZ9K_AUDIO_STREAM_RESULT_BACKPRESSURE) == 0U) {
          break;
        }
        if (!zzplay_card_wait_room(decode, (uint32_t)got, shown, refusal)) {
          if (*refusal == ZZPLAY_CARD_UNSUPPORTED ||
              *refusal == ZZPLAY_CARD_BUSY) {
            return ZZPLAY_CODEC_PASS_FAILED;
          }
          return zzplay_card_stop_or_fail(decode);
        }
      }
      decode->input_bytes += (uint32_t)got;
      if (flags != 0U) {
        decode->eof_sent = 1U;
      }
    }

    if (!zzplay_card_bind(decode, shown, refusal)) {
      if (*refusal == ZZPLAY_CARD_UNSUPPORTED ||
          *refusal == ZZPLAY_CARD_BUSY) {
        return ZZPLAY_CODEC_PASS_FAILED;
      }
      return zzplay_card_stop_or_fail(decode);
    }
    if (!decode->eof_sent) {
      continue;
    }
    if (!decode->ax_bound) {
      return ZZPLAY_CODEC_PASS_FAILED;
    }
    if (!zzplay_card_drain(decode, shown, refusal)) {
      return zzplay_card_stop_or_fail(decode);
    }
    if (zzplay_card_played_bytes(&decode->result, decode->pcm.length) ==
        0U) {
      return ZZPLAY_CODEC_PASS_FAILED;
    }
    return ZZPLAY_CODEC_PASS_COMPLETED;
  }
}

static ZZPlayCardAttempt zzplay_card_ax_run(ZZPlayCodecEngine *engine)
{
  const ZZPlayEngineRun *run = engine->run;
  ZZPlayCardAttempt attempt;
  ZZ9KContext *ctx = 0;
  ZZ9KCaps caps;
  uint32_t repeats = run->options->loop_count;
  uint32_t completed = 0U;
  int shown = 0;
  int gain = 0;

  memset(&attempt, 0, sizeof(attempt));
  memset(&caps, 0, sizeof(caps));
  attempt.answer = ZZPLAY_CARD_OK;
  attempt.result = ZZPLAY_ENGINE_FAILED;
  if (zz9k_open(&ctx) != ZZ9K_STATUS_OK ||
      !zzplay_codec_service_ready(ctx, &caps, engine->codec)) {
    zzplay_launch_reportf(engine->ctl, run->options,
                          "accelerated %s streaming is unavailable",
                          engine->codec_name);
    attempt.reported = 1;
    attempt.answer = ZZPLAY_CARD_FAILED;
    goto done;
  }
  gain = zzplay_card_gain_flag(ctx);
  for (;;) {
    ZZPlayCodecDecode decode;
    ZZPlayCardAnswer refusal = ZZPLAY_CARD_OK;
    ZZPlayCodecPassResult pass;
    uint32_t played;

    memset(&decode, 0, sizeof(decode));
    decode.ctx = ctx;
    decode.engine = engine;
    decode.gain_supported = (uint8_t)(gain ? 1U : 0U);
    decode.compact_z2 =
        (caps.capability_bits & ZZ9K_CAP_APERTURE_LAYOUT) != 0U;
    decode.file = fopen(run->path, "rb");
    if (!decode.file) {
      zzplay_launch_reportf(engine->ctl, run->options, "cannot reopen %s",
                            run->path);
      attempt.reported = 1;
      attempt.answer = ZZPLAY_CARD_FAILED;
      zzplay_codec_decode_cleanup(&decode);
      goto done;
    }
    if (fseek(decode.file, (long)engine->audio_start, SEEK_SET) != 0) {
      zzplay_launch_reportf(engine->ctl, run->options, "cannot seek in %s",
                            run->path);
      attempt.reported = 1;
      attempt.answer = ZZPLAY_CARD_FAILED;
      zzplay_codec_decode_cleanup(&decode);
      goto done;
    }
    if (!zzplay_codec_alloc_rings(&decode, zzplay_card_pcm_unit(engine), 1)) {
      zzplay_launch_reportf(engine->ctl, run->options,
                            "not enough shared card memory for %s streaming",
                            engine->codec_name);
      attempt.reported = 1;
      attempt.answer = ZZPLAY_CARD_FAILED;
      zzplay_codec_decode_cleanup(&decode);
      goto done;
    }
    pass = zzplay_card_play_once(&decode, &shown, &refusal);
    played = zzplay_card_played_bytes(&decode.result, decode.pcm.length);
    zzplay_info("zzplay: %s on-card loop %lu: %lu input bytes, %lu played "
                "bytes\n",
                engine->codec_name, (unsigned long)completed,
                (unsigned long)decode.input_bytes, (unsigned long)played);
    zzplay_codec_decode_cleanup(&decode);
    if (pass == ZZPLAY_CODEC_PASS_STOPPED) {
      attempt.result = ZZPLAY_ENGINE_STOPPED;
      goto done;
    }
    if (pass == ZZPLAY_CODEC_PASS_UNSUPPORTED) {
      zzplay_launch_reportf(
          engine->ctl, run->options,
          played != 0U
              ? "the card cannot decode the rest of this %s stream (a "
                "chained or multiplexed Ogg link follows)"
              : "the card cannot decode this %s stream (unsupported layout)",
          engine->codec_name);
      attempt.reported = 1;
      goto done;
    }
    if (refusal == ZZPLAY_CARD_UNSUPPORTED || refusal == ZZPLAY_CARD_BUSY) {
      attempt.answer = refusal;
      attempt.result = ZZPLAY_ENGINE_FAILED;
      goto done;
    }
    if (pass != ZZPLAY_CODEC_PASS_COMPLETED) {
      zzplay_launch_reportf(engine->ctl, run->options,
                            "on-card %s playback failed", engine->codec_name);
      attempt.reported = 1;
      attempt.answer = ZZPLAY_CARD_FAILED;
      goto done;
    }
    if (zzplay_controller_loop_item(engine->ctl) ||
        (run->options->loop_mode == ZZPLAY_LOOP_FINITE && repeats != 0U)) {
      if (run->options->loop_mode == ZZPLAY_LOOP_FINITE) {
        repeats--;
      }
      completed++;
      continue;
    }
    attempt.result = ZZPLAY_ENGINE_EOF;
    break;
  }

done:
  if (ctx) {
    zz9k_close(ctx);
  }
  return attempt;
}

static ZZPlayEngineResult zzplay_card_stream_ahi(ZZPlayCodecEngine *engine)
{
  const ZZPlayEngineRun *run = engine->run;

  if (engine->muted) {
    zzplay_info("zzplay: selected audio backend NONE "
                "(accelerated %s decode benchmark)\n",
                engine->codec_name);
    zzplay_controller_set_capabilities(run->ctl, 0, 0, 0);
    zzplay_codec_set_output(engine, "none");
  } else {
    zzplay_info("zzplay: selected audio backend AHI "
                "(accelerated %s decode, %s)\n",
                engine->codec_name,
                engine->pcm_width == 4U ? "S32BE narrowed to S16" : "S16BE");
    zzplay_controller_set_capabilities(run->ctl, 0, 1, 0);
    sprintf(engine->output_base, "AHI unit %lu",
            (unsigned long)run->prefs->ahi_unit);
    zzplay_codec_refresh_output(engine);
  }
  return zzplay_codec_accelerated(engine, 0);
}

/* FLAC and Ogg Vorbis. No saved per-format output applies (MP3OUTPUT is an
 * MHI path they cannot use); an explicit AUDIO= option is strict unless it
 * is AUTO. AUTO and a non-strict card preference try on-card playback and
 * fall back to AHI when BeginEx(S16LE) or Play is refused or the AX output
 * is busy. */
static ZZPlayEngineResult zzplay_card_stream_run(ZZPlayCodecEngine *engine)
{
  const ZZPlayEngineRun *run = engine->run;
  ZZPlayAudioBackend requested;
  ZZPlayCardDecision plan;
  int strict = 0;

  zzplay_controller_set_duration(run->ctl, engine->total_ms);
  zzplay_controller_set_capabilities(run->ctl, 0, 0, 0);
  requested = zzplay_prefs_requested_backend(
      run->prefs, run->options, ZZPLAY_MEDIA_AUDIO_NONE, &strict);
  plan = zzplay_card_stream_decide(requested, strict, ZZPLAY_CARD_NOT_ASKED);
  if (plan.path == ZZPLAY_CARD_PATH_REFUSED) {
    if (plan.message) {
      zzplay_launch_reportf(engine->ctl, run->options, plan.message,
                            engine->codec_name);
    }
    return ZZPLAY_ENGINE_FAILED;
  }
  if (plan.path == ZZPLAY_CARD_PATH_NONE) {
    engine->muted = 1;
    return zzplay_card_stream_ahi(engine);
  }
  if (plan.path == ZZPLAY_CARD_PATH_AHI) {
    return zzplay_card_stream_ahi(engine);
  }
  {
    ZZPlayCardAttempt attempt = zzplay_card_ax_run(engine);
    ZZPlayCardDecision after;

    if (zzplay_controller_item_should_end(run->ctl)) {
      return ZZPLAY_ENGINE_STOPPED;
    }
    if (attempt.answer != ZZPLAY_CARD_UNSUPPORTED &&
        attempt.answer != ZZPLAY_CARD_BUSY) {
      return attempt.result;
    }
    after = zzplay_card_stream_decide(requested, strict, attempt.answer);
    if (after.fell_back && after.path == ZZPLAY_CARD_PATH_AHI) {
      zzplay_info("zzplay: on-card %s %s; falling back to AHI\n",
                  engine->codec_name,
                  attempt.answer == ZZPLAY_CARD_BUSY ? "busy"
                                                     : "unsupported");
      return zzplay_card_stream_ahi(engine);
    }
    if (!attempt.reported && after.message) {
      zzplay_launch_reportf(engine->ctl, run->options, after.message,
                            engine->codec_name);
    }
    return ZZPLAY_ENGINE_FAILED;
  }
}

static int zzplay_card_stream_engine(const ZZPlayEngineRun *run,
                                     ZZPlayCodecEngine *engine)
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
  ZZPlayCodecEngine engine;
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
  ZZPlayCodecEngine engine;
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
