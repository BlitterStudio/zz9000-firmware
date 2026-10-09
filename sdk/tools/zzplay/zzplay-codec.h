/* The compressed-audio engine shared by zzplay's MP3 engine (zzplay-mp3.c)
 * and its FLAC/Ogg Vorbis engine (zzplay-codec-stream.c): the per-item
 * engine state, one decode pass over the file, and the accelerated decode +
 * AHI backend both use. Implemented in zzplay-mp3.c, where that backend
 * first served MP3; private to those two files.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_CODEC_H
#define ZZPLAY_CODEC_H

#include "zz9k/audio.h"
#include "zz9k/caps.h"
#include "zz9k/host.h"
#include "zz9k/shared.h"
#include "zzplay-ahi.h"
#include "zzplay-engine.h"
#include "zzplay-mp3-transport.h"

#include <stdint.h>
#include <stdio.h>

/* Everything the backends need about the item being played: MP3 (MHI or
 * AHI), FLAC and Ogg Vorbis (on-card or AHI). */
typedef struct ZZPlayCodecEngine {
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
} ZZPlayCodecEngine;

/* One decode pass over the file: accelerated decode feeding AHI, or the
 * on-card pass (zzplay-codec-stream.c) whose PCM the card pump plays. */
typedef struct ZZPlayCodecDecode {
  ZZ9KContext *ctx;
  FILE *file;
  ZZ9KSharedBuffer compressed;
  ZZ9KSharedBuffer pcm;
  ZZ9KSharedBuffer staging;
  ZZ9KAudioStreamResult result;
  ZZPlayAHISink *ahi;
  ZZPlayCodecEngine *engine;
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
  /* On-card FLAC/Vorbis: the pump, not a Read, consumes the PCM ring. */
  uint8_t ax_bound;
  uint8_t ax_resume;
  uint8_t gain_supported;
  uint8_t ax_committed;
  uint8_t eof_sent;
} ZZPlayCodecDecode;

typedef enum ZZPlayCodecPassResult {
  ZZPLAY_CODEC_PASS_FAILED = 0,
  ZZPLAY_CODEC_PASS_COMPLETED,
  ZZPLAY_CODEC_PASS_STOPPED,
  ZZPLAY_CODEC_PASS_UNSUPPORTED
} ZZPlayCodecPassResult;

/* One staging chunk. The AHI and on-card passes never run together. */
extern uint8_t zzplay_codec_chunk[ZZPLAY_MP3_FEED_MAX_BYTES];

/* Calls the application pump so the GUI stays live while a pass blocks. */
void zzplay_codec_pump(const ZZPlayCodecEngine *engine);
/* Republishes engine->output_base, marking a requested mute. */
void zzplay_codec_refresh_output(const ZZPlayCodecEngine *engine);
/* Sets the backend descriptor ("AHI unit N", ...) and publishes it. */
void zzplay_codec_set_output(ZZPlayCodecEngine *engine, const char *output);
/* 1 when a controller request ends the item. */
int zzplay_codec_should_stop(const ZZPlayCodecDecode *decode);
/* 1 when the card advertises the stream service for `codec`. */
int zzplay_codec_service_ready(ZZ9KContext *ctx, ZZ9KCaps *caps,
                               uint32_t codec);
/* Allocates one pass's PCM, staging and compressed rings; `pcm_unit` 0
 * means engine->min_pcm_bytes. `pcm_card_only` keeps the PCM ring out of
 * the host window (the card pump reads it). */
int zzplay_codec_alloc_rings(ZZPlayCodecDecode *decode, uint32_t pcm_unit,
                             int pcm_card_only);
/* Closes the pass's session and releases everything it holds. */
void zzplay_codec_decode_cleanup(ZZPlayCodecDecode *decode);
/* Plays the item through accelerated decode + AHI (or muted). With
 * `unavailable` set, a service or AHI unit missing before anything played
 * is flagged there instead of reported, so the caller can fall back. */
ZZPlayEngineResult zzplay_codec_accelerated(ZZPlayCodecEngine *engine,
                                            int *unavailable);

#endif /* ZZPLAY_CODEC_H */
