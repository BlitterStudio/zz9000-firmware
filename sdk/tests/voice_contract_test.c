/*
 * Contract tests for the wavetable voice engine SDK surface: wire layouts
 * (offsets, sizes, byte order), record and status helpers, the high-low-high
 * status reader, the ring write discipline, and the host session calls
 * (open, upload, write, status, reset, close) against a mock firmware that
 * serves the mailbox from real mapped board memory.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#if !defined(_WIN32)
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#endif

#include "zz9k/host.h"
#include "zz9k/reply.h"
#include "zz9k/request.h"
#include "zz9k/voice.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#if defined(MAP_ANONYMOUS)
#define ZZ9K_MMAP_ANONYMOUS MAP_ANONYMOUS
#elif defined(MAP_ANON)
#define ZZ9K_MMAP_ANONYMOUS MAP_ANON
#else
#error "anonymous mmap is required for this host test"
#endif
#endif

/* Test seam exported (undeclared) by host/src/zz9k_host.c. */
void zz9k_set_idle_hook_for_test(void (*hook)(void));

static int g_failures;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      g_failures++;                                                      \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
    }                                                                    \
  } while (0)

/* ---- Wire layout pins (the firmware reads and writes exactly these) ---- */

static void test_layouts(void)
{
  CHECK(ZZ9K_OP_AUDIO_VOICE_OPEN == 0x0518U);
  CHECK(ZZ9K_OP_AUDIO_VOICE_CLOSE == 0x0519U);
  CHECK(ZZ9K_OP_AUDIO_VOICE_RESET == 0x051aU);
  CHECK(ZZ9K_OP_AUDIO_VOICE_UPLOAD == 0x051bU);
  CHECK(ZZ9K_SERVICE_FLAG_AUDIO_VOICE == (1U << 28));
  CHECK(ZZ9K_SERVICE_FLAG_AUDIO_VOICE_STEREO == (1U << 29));

  CHECK(sizeof(ZZ9KAudioVoiceOpenPayload) == 48U);
  CHECK(offsetof(ZZ9KAudioVoiceOpenPayload, ring_handle) == 0U);
  CHECK(offsetof(ZZ9KAudioVoiceOpenPayload, status_handle) == 4U);
  CHECK(offsetof(ZZ9KAudioVoiceOpenPayload, staging_handle) == 8U);
  CHECK(offsetof(ZZ9KAudioVoiceOpenPayload, bank_bytes) == 12U);
  CHECK(offsetof(ZZ9KAudioVoiceOpenPayload, lead_frames) == 16U);
  CHECK(offsetof(ZZ9KAudioVoiceOpenPayload, flags) == 20U);
  CHECK(offsetof(ZZ9KAudioVoiceOpenPayload, voices) == 24U);
  CHECK(offsetof(ZZ9KAudioVoiceOpenPayload, reserved) == 28U);

  CHECK(sizeof(ZZ9KAudioVoiceOpenResultPayload) == 48U);
  CHECK(offsetof(ZZ9KAudioVoiceOpenResultPayload, session) == 0U);
  CHECK(offsetof(ZZ9KAudioVoiceOpenResultPayload, voices) == 4U);
  CHECK(offsetof(ZZ9KAudioVoiceOpenResultPayload, bank_bytes) == 8U);
  CHECK(offsetof(ZZ9KAudioVoiceOpenResultPayload, lead_frames) == 12U);
  CHECK(offsetof(ZZ9KAudioVoiceOpenResultPayload, lead_floor_frames) == 16U);
  CHECK(offsetof(ZZ9KAudioVoiceOpenResultPayload, lead_max_frames) == 20U);
  CHECK(offsetof(ZZ9KAudioVoiceOpenResultPayload, ring_records) == 24U);
  CHECK(offsetof(ZZ9KAudioVoiceOpenResultPayload, output_rate) == 28U);
  CHECK(offsetof(ZZ9KAudioVoiceOpenResultPayload, max_samples) == 32U);
  CHECK(offsetof(ZZ9KAudioVoiceOpenResultPayload, reserved) == 36U);

  CHECK(sizeof(ZZ9KAudioVoiceSessionPayload) == 48U);
  CHECK(offsetof(ZZ9KAudioVoiceSessionPayload, session) == 0U);
  CHECK(offsetof(ZZ9KAudioVoiceSessionPayload, reserved) == 4U);

  CHECK(sizeof(ZZ9KAudioVoiceUploadPayload) == 48U);
  CHECK(offsetof(ZZ9KAudioVoiceUploadPayload, session) == 0U);
  CHECK(offsetof(ZZ9KAudioVoiceUploadPayload, flags) == 4U);
  CHECK(offsetof(ZZ9KAudioVoiceUploadPayload, staging_offset) == 8U);
  CHECK(offsetof(ZZ9KAudioVoiceUploadPayload, bank_offset) == 12U);
  CHECK(offsetof(ZZ9KAudioVoiceUploadPayload, length) == 16U);
  CHECK(offsetof(ZZ9KAudioVoiceUploadPayload, sample_id) == 20U);
  CHECK(offsetof(ZZ9KAudioVoiceUploadPayload, sample_offset) == 24U);
  CHECK(offsetof(ZZ9KAudioVoiceUploadPayload, sample_length) == 28U);
  CHECK(offsetof(ZZ9KAudioVoiceUploadPayload, format) == 32U);
  CHECK(offsetof(ZZ9KAudioVoiceUploadPayload, reserved) == 36U);

  CHECK(sizeof(ZZ9KVoiceRingHeader) == 16U);
  CHECK(offsetof(ZZ9KVoiceRingHeader, write_cursor) == 0U);
  CHECK(offsetof(ZZ9KVoiceRingHeader, heartbeat) == 4U);
  CHECK(ZZ9K_VOICE_RING_WRITE_CURSOR_OFFSET == 0U);
  CHECK(ZZ9K_VOICE_RING_HEARTBEAT_OFFSET == 4U);

  CHECK(sizeof(ZZ9KVoiceRecordWire) == 16U);
  CHECK(offsetof(ZZ9KVoiceRecordWire, stamp) == 0U);
  CHECK(offsetof(ZZ9KVoiceRecordWire, voice) == 4U);
  CHECK(offsetof(ZZ9KVoiceRecordWire, op) == 5U);
  CHECK(offsetof(ZZ9KVoiceRecordWire, flags) == 6U);
  CHECK(offsetof(ZZ9KVoiceRecordWire, arg0) == 8U);
  CHECK(offsetof(ZZ9KVoiceRecordWire, arg1) == 12U);

  CHECK(sizeof(ZZ9KVoiceStatusWire) == 64U);
  CHECK(offsetof(ZZ9KVoiceStatusWire, playing_frame) == 0U);
  CHECK(offsetof(ZZ9KVoiceStatusWire, horizon) == 4U);
  CHECK(offsetof(ZZ9KVoiceStatusWire, read_cursor) == 8U);
  CHECK(offsetof(ZZ9KVoiceStatusWire, state) == 10U);
  CHECK(offsetof(ZZ9KVoiceStatusWire, late) == 12U);
  CHECK(offsetof(ZZ9KVoiceStatusWire, rejected) == 16U);
  CHECK(offsetof(ZZ9KVoiceStatusWire, underruns) == 20U);
  CHECK(offsetof(ZZ9KVoiceStatusWire, dropped) == 24U);
  CHECK(offsetof(ZZ9KVoiceStatusWire, session) == 28U);
  CHECK(offsetof(ZZ9KVoiceStatusWire, margin_min_us) == 32U);
  CHECK(offsetof(ZZ9KVoiceStatusWire, poll_gap_max_us) == 36U);
  CHECK(offsetof(ZZ9KVoiceStatusWire, reserved) == 40U);
  CHECK(ZZ9K_VOICE_STATUS_PLAYING_FRAME_OFFSET == 0U);
  CHECK(ZZ9K_VOICE_STATUS_HORIZON_OFFSET == 4U);
  CHECK(ZZ9K_VOICE_STATUS_READ_CURSOR_OFFSET == 8U);
  CHECK(ZZ9K_VOICE_STATUS_STATE_OFFSET == 10U);
  CHECK(ZZ9K_VOICE_STATUS_LATE_OFFSET == 12U);
  CHECK(ZZ9K_VOICE_STATUS_REJECTED_OFFSET == 16U);
  CHECK(ZZ9K_VOICE_STATUS_UNDERRUNS_OFFSET == 20U);
  CHECK(ZZ9K_VOICE_STATUS_DROPPED_OFFSET == 24U);
  CHECK(ZZ9K_VOICE_STATUS_SESSION_OFFSET == 28U);
  CHECK(ZZ9K_VOICE_STATUS_MARGIN_OFFSET == 32U);
  CHECK(ZZ9K_VOICE_STATUS_POLL_GAP_OFFSET == 36U);
  CHECK(ZZ9K_VOICE_STATUS_FEATURES_OFFSET == 40U);
  CHECK(ZZ9K_VOICE_STATUS_ACTIVE_MASK_OFFSET == 44U);
  CHECK(ZZ9K_VOICE_STATUS_ENDS_OFFSET == 48U);
  CHECK(ZZ9K_VOICE_STATUS_OUTPUT_DELAY_OFFSET == 52U);
  /* the extended words sit in the reserved bytes of the wire struct */
  CHECK(ZZ9K_VOICE_STATUS_OUTPUT_DELAY_OFFSET + 2U <=
        ZZ9K_VOICE_STATUS_BYTES);
  CHECK(ZZ9K_VOICE_FEATURE_RAMP == 0x01U && ZZ9K_VOICE_FEATURE_LIMITER == 0x02U &&
        ZZ9K_VOICE_FEATURE_CUBIC == 0x04U &&
        ZZ9K_VOICE_FEATURE_VOICE_INTERP == 0x08U &&
        ZZ9K_VOICE_FEATURE_OVERSAMPLE == 0x10U &&
        ZZ9K_VOICE_FEATURE_ENVELOPE == 0x20U &&
        ZZ9K_VOICE_FEATURE_EVENTS == 0x40U &&
        ZZ9K_VOICE_FEATURE_CONFIG == 0x80U);
  CHECK(ZZ9K_VOICE_OP_MODE == 7U && ZZ9K_VOICE_OP_ENVELOPE == 8U &&
        ZZ9K_VOICE_OP_CONFIG == 9U);
  CHECK(ZZ9K_VOICE_CONFIG_RAMP == 1U && ZZ9K_VOICE_RAMP_MAX_FRAMES == 512U &&
        ZZ9K_VOICE_FADE_SLOTS == 32U);
  CHECK(ZZ9K_VOICE_JITTER_NONE == 0xFFFFFFFFU);

  CHECK(ZZ9K_VOICE_OP_START == 1U && ZZ9K_VOICE_OP_LOOP == 2U &&
        ZZ9K_VOICE_OP_SET == 3U && ZZ9K_VOICE_OP_STOP == 4U &&
        ZZ9K_VOICE_OP_MASTER == 5U && ZZ9K_VOICE_OP_DIRECTION == 6U);
  CHECK(ZZ9K_VOICE_SET_RATE == 1U && ZZ9K_VOICE_SET_VOLUME == 2U &&
        ZZ9K_VOICE_SET_RATE_HZ == 4U && ZZ9K_VOICE_DIR_BACKWARD == 1U);
  CHECK(ZZ9K_VOICE_STATE_CLOSED == 0U && ZZ9K_VOICE_STATE_OPEN == 1U &&
        ZZ9K_VOICE_STATE_LIVE == 2U);
  CHECK(ZZ9K_VOICE_OPEN_FLAG_NTSC == 1U && ZZ9K_VOICE_OPEN_FLAG_LINEAR == 2U &&
        ZZ9K_VOICE_OPEN_FLAG_LOW_LATENCY == 4U);
  CHECK(ZZ9K_VOICE_UPLOAD_FLAG_COPY == 1U &&
        ZZ9K_VOICE_UPLOAD_FLAG_DEFINE == 2U);
  CHECK(ZZ9K_VOICE_FORMAT_S8 == 0U && ZZ9K_VOICE_FORMAT_S16 == 1U);
  CHECK(ZZ9K_VOICE_FORMAT_S8S == 2U && ZZ9K_VOICE_FORMAT_S16S == 3U &&
        ZZ9K_VOICE_FORMAT_COUNT == 4U);
  CHECK(ZZ9K_VOICE_MAX_VOICES == 32U && ZZ9K_VOICE_MAX_SAMPLES == 256U &&
        ZZ9K_VOICE_BANK_MAX_BYTES == 32U * 1024U * 1024U);
  CHECK(ZZ9K_VOICE_RING_DEFAULT_BYTES == 2048U &&
        ZZ9K_VOICE_STAGING_DEFAULT_BYTES == 4096U &&
        ZZ9K_VOICE_STATUS_BYTES == 64U);
}

/* ---- Stereo formats and the timing helpers ---- */

static void test_stereo_formats(void)
{
  ZZ9KRequest req;
  uint32_t f;

  CHECK(zz9k_voice_format_frame_shift(ZZ9K_VOICE_FORMAT_S8) == 0U);
  CHECK(zz9k_voice_format_frame_shift(ZZ9K_VOICE_FORMAT_S16) == 1U);
  CHECK(zz9k_voice_format_frame_shift(ZZ9K_VOICE_FORMAT_S8S) == 1U);
  CHECK(zz9k_voice_format_frame_shift(ZZ9K_VOICE_FORMAT_S16S) == 2U);
  for (f = 0U; f < ZZ9K_VOICE_FORMAT_COUNT; f++) {
    CHECK(zz9k_voice_format_valid(f) != 0U);
  }
  CHECK(zz9k_voice_format_valid(ZZ9K_VOICE_FORMAT_COUNT) == 0U);
  CHECK(zz9k_voice_format_valid(0xFFFFFFFFU) == 0U);

  for (f = 0U; f < ZZ9K_VOICE_FORMAT_COUNT; f++) {
    CHECK(zz9k_request_audio_voice_upload(&req, 7U,
                                          ZZ9K_VOICE_UPLOAD_FLAG_DEFINE, 0, 0,
                                          0, 3U, 0, 16U, f) ==
          ZZ9K_STATUS_OK);
    CHECK(zz9k_get_be32(req.entry.payload.inline_data + 32) == f);
  }
  CHECK(zz9k_request_audio_voice_upload(&req, 7U,
                                        ZZ9K_VOICE_UPLOAD_FLAG_DEFINE, 0, 0, 0,
                                        3U, 0, 16U, 4U) ==
        ZZ9K_STATUS_BAD_REQUEST);
  /* The format is ignored when no DEFINE is requested. */
  CHECK(zz9k_request_audio_voice_upload(&req, 7U,
                                        ZZ9K_VOICE_UPLOAD_FLAG_COPY, 0, 0, 16U,
                                        0, 0, 0, 99U) == ZZ9K_STATUS_OK);
}

/* The audio service flag set: bit 29 is the stereo flag and no other audio
 * flag shares it. */
static void test_stereo_flag(void)
{
  static const uint32_t others[] = {
      ZZ9K_SERVICE_FLAG_AUDIO_MP3_DECODE, ZZ9K_SERVICE_FLAG_AUDIO_PCM_MIX,
      ZZ9K_SERVICE_FLAG_AUDIO_RESAMPLE, ZZ9K_SERVICE_FLAG_AUDIO_PCM16_STEREO,
      ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM, ZZ9K_SERVICE_FLAG_AUDIO_CONTROL,
      ZZ9K_SERVICE_FLAG_AUDIO_FABRIC, ZZ9K_SERVICE_FLAG_AUDIO_FABRIC_RATE,
      ZZ9K_SERVICE_FLAG_AUDIO_STREAM_GAIN,
      ZZ9K_SERVICE_FLAG_AUDIO_FLAC_STREAM,
      ZZ9K_SERVICE_FLAG_AUDIO_VORBIS_STREAM,
      ZZ9K_SERVICE_FLAG_AUDIO_STREAM_REDUCE, ZZ9K_SERVICE_FLAG_AUDIO_VOICE,
      ZZ9K_SERVICE_FLAG_FIRMWARE, ZZ9K_SERVICE_FLAG_MODULE,
      ZZ9K_SERVICE_FLAG_ASYNC, ZZ9K_SERVICE_FLAG_ZERO_COPY};
  size_t i;

  for (i = 0U; i < sizeof(others) / sizeof(others[0]); i++) {
    CHECK((others[i] & ZZ9K_SERVICE_FLAG_AUDIO_VOICE_STEREO) == 0U);
  }
}

static void test_rate_step(void)
{
  CHECK(ZZ9K_VOICE_RATE_MAX_HZ == 192000U);
  CHECK(zz9k_voice_rate_step(0U) == 0U);
  CHECK(zz9k_voice_rate_step(1U) == 1U);
  CHECK(zz9k_voice_rate_step(8000U) == 10922U);
  CHECK(zz9k_voice_rate_step(8363U) == 11418U);
  CHECK(zz9k_voice_rate_step(22050U) == 30105U);
  CHECK(zz9k_voice_rate_step(44100U) == 60211U);
  CHECK(zz9k_voice_rate_step(48000U) == 65536U);
  CHECK(zz9k_voice_rate_step(96000U) == 131072U);
  CHECK(zz9k_voice_rate_step(192000U) == 262144U);
  CHECK(zz9k_voice_rate_step(192001U) == 262144U);
  CHECK(zz9k_voice_rate_step(200000U) == 262144U);
  CHECK(zz9k_voice_rate_step(0xFFFFFFFFU) == 262144U);
}

/* An independent model of the engine's per-frame advance (audio_voice.c,
 * advance()): counts the frames rendered until the voice wraps or stops. */
static uint32_t model_pass_frames(int backward, uint32_t pos, uint32_t frac,
                                  uint32_t bound, uint32_t step,
                                  uint32_t limit)
{
  uint32_t k;

  for (k = 1U; k <= limit; k++) {
    if (!backward) {
      uint32_t f = frac + (step & 0xFFFFU);

      pos += (step >> 16) + (f >> 16);
      frac = f & 0xFFFFU;
      if (pos >= bound) {
        return k;
      }
    } else {
      uint32_t sub = step & 0xFFFFU;
      uint32_t ip = step >> 16;

      if (frac < sub) {
        frac = frac + 0x10000U - sub;
        ip++;
      } else {
        frac -= sub;
      }
      if (ip > pos) {
        return k; /* below index 0: passes below any loop start */
      }
      pos -= ip;
      if (pos < bound) {
        return k;
      }
    }
  }
  return ZZ9K_VOICE_PASS_NEVER;
}

static void test_pass_frames_fixed(void)
{
  CHECK(ZZ9K_VOICE_PASS_NEVER == 0xFFFFFFFFU);

  /* Forward, step 1.0. */
  CHECK(zz9k_voice_pass_frames(0, 0U, 0U, 100U, 65536U) == 100U);
  CHECK(zz9k_voice_pass_frames(0, 99U, 0U, 100U, 65536U) == 1U);
  CHECK(zz9k_voice_pass_frames(0, 99U, 0xFFFFU, 100U, 65536U) == 1U);
  /* Forward, step 0.5, zero and non-zero fraction. */
  CHECK(zz9k_voice_pass_frames(0, 0U, 0U, 10U, 0x8000U) == 20U);
  CHECK(zz9k_voice_pass_frames(0, 0U, 0x8000U, 10U, 0x8000U) == 19U);
  CHECK(zz9k_voice_pass_frames(0, 0U, 0x7FFFU, 10U, 0x8000U) == 20U);
  CHECK(zz9k_voice_pass_frames(0, 5U, 0xFFFEU, 6U, 1U) == 2U);
  CHECK(zz9k_voice_pass_frames(0, 5U, 0xFFFFU, 6U, 1U) == 1U);
  /* Forward, step above 1.0. */
  CHECK(zz9k_voice_pass_frames(0, 0U, 0U, 10U, 3U * 65536U) == 4U);
  CHECK(zz9k_voice_pass_frames(0, 0U, 0U, 9U, 3U * 65536U) == 3U);
  CHECK(zz9k_voice_pass_frames(0, 0U, 0U, 9U, 3U * 65536U + 1U) == 3U);
  CHECK(zz9k_voice_pass_frames(0, 0U, 1U, 9U, 3U * 65536U) == 3U);
  CHECK(zz9k_voice_pass_frames(0, 0U, 0U, 9U, 3U * 65536U - 1U) == 4U);
  CHECK(zz9k_voice_pass_frames(0, 0U, 0U, 1000U, 262144U) == 250U);
  /* Forward at or past the bound: the next frame passes. */
  CHECK(zz9k_voice_pass_frames(0, 10U, 0U, 10U, 1U) == 1U);
  CHECK(zz9k_voice_pass_frames(0, 11U, 5U, 10U, 1U) == 1U);

  /* Backward, step 1.0, loop start 0 (a step below index 0 passes). */
  CHECK(zz9k_voice_pass_frames(1, 10U, 0U, 0U, 65536U) == 11U);
  CHECK(zz9k_voice_pass_frames(1, 0U, 0U, 0U, 65536U) == 1U);
  CHECK(zz9k_voice_pass_frames(1, 0U, 0x8000U, 0U, 0x8000U) == 2U);
  CHECK(zz9k_voice_pass_frames(1, 0U, 0x7FFFU, 0U, 0x8000U) == 1U);
  CHECK(zz9k_voice_pass_frames(1, 0U, 0x8000U, 0U, 0x7FFFU) == 2U);
  CHECK(zz9k_voice_pass_frames(1, 10U, 0U, 5U, 65536U) == 6U);
  /* Backward, step 0.5 and fractions. */
  CHECK(zz9k_voice_pass_frames(1, 10U, 0U, 10U, 0x8000U) == 1U);
  CHECK(zz9k_voice_pass_frames(1, 10U, 0x8000U, 10U, 0x8000U) == 2U);
  CHECK(zz9k_voice_pass_frames(1, 10U, 0U, 5U, 0x8000U) == 11U);
  /* Backward, step above 1.0. */
  CHECK(zz9k_voice_pass_frames(1, 30U, 0U, 0U, 3U * 65536U) == 11U);
  CHECK(zz9k_voice_pass_frames(1, 30U, 0U, 6U, 3U * 65536U) == 9U);
  CHECK(zz9k_voice_pass_frames(1, 31U, 0U, 6U, 3U * 65536U) == 9U);
  /* Backward at or below the bound: the next frame passes. */
  CHECK(zz9k_voice_pass_frames(1, 4U, 0xFFFFU, 5U, 1U) == 1U);
  CHECK(zz9k_voice_pass_frames(1, 5U, 0U, 5U, 1U) == 1U);

  /* A voice with no rate never passes, and a pass longer than 32 bits is
   * reported as never. */
  CHECK(zz9k_voice_pass_frames(0, 0U, 0U, 100U, 0U) == ZZ9K_VOICE_PASS_NEVER);
  CHECK(zz9k_voice_pass_frames(1, 100U, 0U, 0U, 0U) == ZZ9K_VOICE_PASS_NEVER);
  CHECK(zz9k_voice_pass_frames(0, 0U, 0U, 0x7FFFFFFFU, 1U) ==
        ZZ9K_VOICE_PASS_NEVER);
  CHECK(zz9k_voice_pass_frames(1, 0x7FFFFFFFU, 0U, 0U, 1U) ==
        ZZ9K_VOICE_PASS_NEVER);
  /* The largest pass that fits: 2^32 - 2 frames. */
  CHECK(zz9k_voice_pass_frames(0, 0U, 0U, 0xFFFFU, 1U) == 0xFFFF0000U);
}

/* Every combination in a grid, then random values, against the model. */
static void test_pass_frames_vs_model(void)
{
  static const uint32_t steps[] = {
      0x400U,  0x7FFFU, 0x8000U, 0x8001U, 10922U,  11418U,
      30105U,  60211U,  0xFFFFU, 65536U,  65537U,  98304U,
      131072U, 196609U, 262144U, 0x123456U};
  static const uint32_t fracs[] = {0U, 1U, 0x7FFFU, 0x8000U, 0xFFFFU};
  uint32_t seed = 0x2f6e2b1U;
  size_t si;
  size_t fi;
  uint32_t pos;
  uint32_t i;

  for (si = 0U; si < sizeof(steps) / sizeof(steps[0]); si++) {
    for (fi = 0U; fi < sizeof(fracs) / sizeof(fracs[0]); fi++) {
      for (pos = 0U; pos < 40U; pos++) {
        uint32_t bound;

        for (bound = 0U; bound < 50U; bound++) {
          int dir;

          for (dir = 0; dir < 2; dir++) {
            uint32_t want = model_pass_frames(dir, pos, fracs[fi], bound,
                                              steps[si], 1U << 22);
            uint32_t got = zz9k_voice_pass_frames(dir, pos, fracs[fi], bound,
                                                  steps[si]);

            if (got != want) {
              g_failures++;
              printf("FAIL pass_frames dir=%d pos=%u frac=%u bound=%u "
                     "step=%u got=%u want=%u\n",
                     dir, (unsigned)pos, (unsigned)fracs[fi],
                     (unsigned)bound, (unsigned)steps[si], (unsigned)got,
                     (unsigned)want);
              return;
            }
          }
        }
      }
    }
  }
  for (i = 0U; i < 20000U; i++) {
    uint32_t p;
    uint32_t fr;
    uint32_t b;
    uint32_t st;
    int dir;
    uint32_t want;
    uint32_t got;

    seed = seed * 1664525U + 1013904223U;
    p = (seed >> 8) % 500U;
    seed = seed * 1664525U + 1013904223U;
    fr = (seed >> 8) & 0xFFFFU;
    seed = seed * 1664525U + 1013904223U;
    b = (seed >> 8) % 500U;
    seed = seed * 1664525U + 1013904223U;
    st = 64U + (seed >> 8) % (6U * 65536U);
    dir = (int)((seed >> 4) & 1U);
    want = model_pass_frames(dir, p, fr, b, st, 1U << 22);
    got = zz9k_voice_pass_frames(dir, p, fr, b, st);
    if (got != want) {
      g_failures++;
      printf("FAIL pass_frames dir=%d pos=%u frac=%u bound=%u step=%u "
             "got=%u want=%u\n",
             dir, (unsigned)p, (unsigned)fr, (unsigned)b, (unsigned)st,
             (unsigned)got, (unsigned)want);
      return;
    }
  }
}

/* ---- Record encoding and builders ---- */

static void test_records(void)
{
  ZZ9KVoiceRecord r;
  ZZ9KVoiceRecord back;
  ZZ9KVoiceRecordWire w;
  const uint8_t *b = (const uint8_t *)&w;

  zz9k_voice_build_start(&r, 0x01020304UL, 7U, 0x0105U, 0x0a0b0c0dUL,
                         0x11223344UL);
  zz9k_voice_record_encode(&w, &r);
  /* stamp | voice op | flags | arg0 | arg1, all big-endian */
  CHECK(b[0] == 0x01 && b[1] == 0x02 && b[2] == 0x03 && b[3] == 0x04);
  CHECK(b[4] == 7U && b[5] == ZZ9K_VOICE_OP_START);
  CHECK(b[6] == 0x01 && b[7] == 0x05);
  CHECK(b[8] == 0x0a && b[9] == 0x0b && b[10] == 0x0c && b[11] == 0x0d);
  CHECK(b[12] == 0x11 && b[13] == 0x22 && b[14] == 0x33 && b[15] == 0x44);
  zz9k_voice_record_decode(&back, &w);
  CHECK(memcmp(&back, &r, sizeof(r)) == 0);

  zz9k_voice_build_loop(&r, 5U, 3U, 100U, 200U);
  CHECK(r.stamp == 5U && r.voice == 3U && r.op == ZZ9K_VOICE_OP_LOOP &&
        r.flags == 0U && r.arg0 == 100U && r.arg1 == 200U);
  zz9k_voice_build_set(&r, 6U, 4U,
                       ZZ9K_VOICE_SET_RATE | ZZ9K_VOICE_SET_VOLUME, 428U,
                       0x1234U, 0xabcdU);
  CHECK(r.op == ZZ9K_VOICE_OP_SET && r.flags == 3U && r.arg0 == 428U &&
        r.arg1 == 0x1234abcdUL);
  zz9k_voice_build_stop(&r, 7U, 9U);
  CHECK(r.op == ZZ9K_VOICE_OP_STOP && r.voice == 9U && r.arg0 == 0U);
  zz9k_voice_build_master(&r, 8U, 0x180U);
  CHECK(r.op == ZZ9K_VOICE_OP_MASTER && r.arg0 == 0x180U);
  zz9k_voice_build_direction(&r, 9U, 2U, 1);
  CHECK(r.op == ZZ9K_VOICE_OP_DIRECTION && r.flags == 1U);
  zz9k_voice_build_direction(&r, 9U, 2U, 0);
  CHECK(r.flags == 0U);
  zz9k_voice_build_config_ramp(&r, 10U, 256U);
  CHECK(r.stamp == 10U && r.voice == 0U && r.op == ZZ9K_VOICE_OP_CONFIG &&
        r.flags == ZZ9K_VOICE_CONFIG_RAMP && r.arg0 == 256U && r.arg1 == 0U);
  zz9k_voice_build_config(&r, 11U, 0x0003U, 5U, 6U);
  CHECK(r.op == ZZ9K_VOICE_OP_CONFIG && r.flags == 3U && r.arg0 == 5U &&
        r.arg1 == 6U);
  zz9k_voice_record_encode(&w, &r);
  CHECK(b[4] == 0U && b[5] == 9U && b[6] == 0x00 && b[7] == 0x03);
}

/* ---- Status reader ---- */

static void put_status_word(uint8_t *block, uint32_t offset, uint32_t value)
{
  zz9k_put_be32(block + offset, value);
}

static void test_status_block_read(void)
{
  uint8_t block[ZZ9K_VOICE_STATUS_BYTES];
  ZZ9KVoiceStatus s;

  memset(block, 0, sizeof(block));
  put_status_word(block, 0, 0x00010002UL);
  put_status_word(block, 4, 0x00030004UL);
  zz9k_put_be16(block + 8, 0xfffeU);
  zz9k_put_be16(block + 10, ZZ9K_VOICE_STATE_LIVE);
  put_status_word(block, 12, 0x11111111UL);
  put_status_word(block, 16, 0x22222222UL);
  put_status_word(block, 20, 0x33333333UL);
  put_status_word(block, 24, 0x44444444UL);
  put_status_word(block, 28, 0x55555555UL);
  zz9k_voice_status_read(block, &s);
  CHECK(s.playing_frame == 0x00010002UL);
  CHECK(s.horizon == 0x00030004UL);
  CHECK(s.read_cursor == 0xfffeU);
  CHECK(s.state == ZZ9K_VOICE_STATE_LIVE);
  CHECK(s.late == 0x11111111UL);
  CHECK(s.rejected == 0x22222222UL);
  CHECK(s.underruns == 0x33333333UL);
  CHECK(s.dropped == 0x44444444UL);
  CHECK(s.session == 0x55555555UL);
}

static void test_jitter_read(void)
{
  uint8_t block[ZZ9K_VOICE_STATUS_BYTES];
  ZZ9KVoiceJitter j;
  ZZ9KVoiceStatus s;

  /* A block from firmware that predates the telemetry: both words zero. */
  memset(block, 0, sizeof(block));
  zz9k_voice_jitter_read(block, &j);
  CHECK(j.margin_min_us == 0U && j.poll_gap_max_us == 0U);

  put_status_word(block, 28, 0x55555555UL);
  put_status_word(block, ZZ9K_VOICE_STATUS_MARGIN_OFFSET, 0x00012345UL);
  put_status_word(block, ZZ9K_VOICE_STATUS_POLL_GAP_OFFSET, 0x00067890UL);
  zz9k_voice_jitter_read(block, &j);
  CHECK(j.margin_min_us == 0x00012345UL);
  CHECK(j.poll_gap_max_us == 0x00067890UL);
  /* The status decode leaves the other words alone. */
  zz9k_voice_status_read(block, &s);
  CHECK(s.session == 0x55555555UL);

  put_status_word(block, ZZ9K_VOICE_STATUS_MARGIN_OFFSET,
                  ZZ9K_VOICE_JITTER_NONE);
  zz9k_voice_jitter_read(block, &j);
  CHECK(j.margin_min_us == ZZ9K_VOICE_JITTER_NONE);
}

static void test_ext_status_read(void)
{
  uint8_t block[ZZ9K_VOICE_STATUS_BYTES];
  ZZ9KVoiceExtStatus x;
  ZZ9KVoiceJitter j;
  ZZ9KVoiceStatus s;

  /* A block from firmware that predates the features word, or a zeroed one:
   * every extended word reads 0, which means no features. */
  memset(block, 0, sizeof(block));
  memset(&x, 0xAA, sizeof(x));
  zz9k_voice_ext_status_read(block, &x);
  CHECK(x.features == 0U && x.active_mask == 0U && x.ends == 0U &&
        x.output_delay_frames == 0U);

  /* Each word at its offset, big-endian, and the older words untouched. */
  put_status_word(block, 28, 0x55555555UL);
  put_status_word(block, ZZ9K_VOICE_STATUS_POLL_GAP_OFFSET, 0x00067890UL);
  put_status_word(block, ZZ9K_VOICE_STATUS_FEATURES_OFFSET, 0x00000081UL);
  put_status_word(block, ZZ9K_VOICE_STATUS_ACTIVE_MASK_OFFSET, 0x80000001UL);
  put_status_word(block, ZZ9K_VOICE_STATUS_ENDS_OFFSET, 0x01020304UL);
  zz9k_put_be16(block + ZZ9K_VOICE_STATUS_OUTPUT_DELAY_OFFSET, 0x0123U);
  zz9k_voice_ext_status_read(block, &x);
  CHECK(x.features == 0x81UL);
  CHECK((x.features & ZZ9K_VOICE_FEATURE_CONFIG) != 0U &&
        (x.features & ZZ9K_VOICE_FEATURE_RAMP) != 0U &&
        (x.features & ZZ9K_VOICE_FEATURE_CUBIC) == 0U);
  CHECK(x.active_mask == 0x80000001UL);
  CHECK(x.ends == 0x01020304UL);
  CHECK(x.output_delay_frames == 0x0123U);
  zz9k_voice_status_read(block, &s);
  zz9k_voice_jitter_read(block, &j);
  CHECK(s.session == 0x55555555UL && j.poll_gap_max_us == 0x00067890UL);
}

/* A scripted 16-bit source that returns a queued sequence, so a tear (the
 * high half changing between the first and last read) can be simulated. */
struct ScriptedRead {
  uint16_t values[64];
  uint32_t offsets[64];
  uint32_t count;
  uint32_t next;
};

static uint16_t scripted_read16(void *user, uint32_t offset)
{
  struct ScriptedRead *s = (struct ScriptedRead *)user;
  uint16_t value;

  if (s->next >= s->count || s->offsets[s->next] != offset) {
    g_failures++;
    printf("FAIL scripted read %u at offset %u\n", (unsigned)s->next,
           (unsigned)offset);
    return 0U;
  }
  value = s->values[s->next];
  s->next++;
  return value;
}

static void script(struct ScriptedRead *s, uint32_t offset, uint16_t value)
{
  s->offsets[s->count] = offset;
  s->values[s->count] = value;
  s->count++;
}

static void test_read32_tear(void)
{
  struct ScriptedRead s;
  uint32_t value;

  /* No tear: high, low, high, one pass. */
  memset(&s, 0, sizeof(s));
  script(&s, 8U, 0x0001U);
  script(&s, 10U, 0xffffU);
  script(&s, 8U, 0x0001U);
  value = zz9k_voice_read32_with(scripted_read16, &s, 8U);
  CHECK(value == 0x0001ffffUL);
  CHECK(s.next == 3U);

  /* The counter rolls from 0x0001ffff to 0x00020000 between the reads: the
   * first pass saw high 0x0001 then low 0x0000 then high 0x0002 (a torn
   * 0x00010000 that was never a real value). The retry reads a stable
   * 0x00020000. */
  memset(&s, 0, sizeof(s));
  script(&s, 8U, 0x0001U);
  script(&s, 10U, 0x0000U);
  script(&s, 8U, 0x0002U);
  script(&s, 8U, 0x0002U);
  script(&s, 10U, 0x0000U);
  script(&s, 8U, 0x0002U);
  value = zz9k_voice_read32_with(scripted_read16, &s, 8U);
  CHECK(value == 0x00020000UL);
  CHECK(value != 0x00010000UL);
  CHECK(s.next == 6U);

  /* Two torn passes in a row still converge on the stable pass. */
  memset(&s, 0, sizeof(s));
  script(&s, 0U, 0x0000U);
  script(&s, 2U, 0xffffU);
  script(&s, 0U, 0x0001U);
  script(&s, 0U, 0x0001U);
  script(&s, 2U, 0xffffU);
  script(&s, 0U, 0x0002U);
  script(&s, 0U, 0x0002U);
  script(&s, 2U, 0x0005U);
  script(&s, 0U, 0x0002U);
  value = zz9k_voice_read32_with(scripted_read16, &s, 0U);
  CHECK(value == 0x00020005UL);
  CHECK(s.next == 9U);

  /* The real-memory reader reads the same big-endian words. */
  {
    uint8_t mem[8] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};

    CHECK(zz9k_voice_read32(mem, 0U) == 0x12345678UL);
    CHECK(zz9k_voice_read32(mem, 4U) == 0x9abcdef0UL);
  }
}

/* ---- Ring math ---- */

static void test_ring_math(void)
{
  CHECK(zz9k_voice_ring_records_for_bytes(0U) == 0U);
  CHECK(zz9k_voice_ring_records_for_bytes(271U) == 0U);
  CHECK(zz9k_voice_ring_records_for_bytes(272U) == 16U);
  CHECK(zz9k_voice_ring_records_for_bytes(2047U) == 64U);
  CHECK(zz9k_voice_ring_records_for_bytes(2048U) == 64U);
  CHECK(zz9k_voice_ring_records_for_bytes(16U + 128U * 16U - 1U) == 64U);
  CHECK(zz9k_voice_ring_records_for_bytes(16U + 128U * 16U) == 128U);
  CHECK(zz9k_voice_ring_records_for_bytes(ZZ9K_VOICE_RING_MAX_BYTES) == 4096U);
  CHECK(zz9k_voice_ring_records_for_bytes(0x00100000UL) == 4096U);

  CHECK(zz9k_voice_ring_free(64U, 0U, 0U) == 64U);
  CHECK(zz9k_voice_ring_free(64U, 64U, 0U) == 0U);
  CHECK(zz9k_voice_ring_free(64U, 70U, 10U) == 4U);
  /* The cursors are mod 65536: write wrapped past read. */
  CHECK(zz9k_voice_ring_free(64U, 5U, 65530U) == 64U - 11U);
  /* A read cursor ahead of the write cursor is corrupt: no space. */
  CHECK(zz9k_voice_ring_free(64U, 0U, 1U) == 0U);
}

/* ---- Staging and publishing against plain ring memory ---- */

static void fill_session(ZZ9KVoiceSession *session, volatile uint8_t *ring,
                         uint32_t records)
{
  memset(session, 0, sizeof(*session));
  session->grant.ring_records = records;
  session->ring = ring;
  session->ring_bytes = ZZ9K_VOICE_RING_HEADER_BYTES +
                        records * ZZ9K_VOICE_RECORD_BYTES;
  session->mapped = 1U;
}

static void test_stage_then_publish(void)
{
  uint8_t ring[ZZ9K_VOICE_RING_HEADER_BYTES + 16U * ZZ9K_VOICE_RECORD_BYTES];
  ZZ9KVoiceSession session;
  ZZ9KVoiceRecord recs[40];
  ZZ9KVoiceRecordWire wire;
  uint32_t i;
  uint32_t staged;

  memset(ring, 0, sizeof(ring));
  fill_session(&session, ring, 16U);
  for (i = 0U; i < 40U; i++) {
    zz9k_voice_build_stop(&recs[i], 100U + i, (uint8_t)(i & 31U));
  }

  /* Staging copies records but must not move the shared cursor: that store
   * is what lets the firmware see them. */
  staged = zz9k_voice_ring_stage(&session, 0U, recs, 5U);
  CHECK(staged == 5U);
  CHECK(zz9k_get_be16(ring) == 0U);
  CHECK(session.write_cursor == 5U);
  for (i = 0U; i < 5U; i++) {
    zz9k_voice_record_encode(&wire, &recs[i]);
    CHECK(memcmp(ring + 16U + i * 16U, &wire, 16U) == 0);
  }
  zz9k_voice_ring_publish(&session);
  CHECK(zz9k_get_be16(ring) == 5U);

  /* Never more than the free space: capacity 16, read cursor 2, five
   * outstanding from 2..5 means 16 - 3 = 13 free. */
  staged = zz9k_voice_ring_stage(&session, 2U, recs, 40U);
  CHECK(staged == 13U);
  CHECK(session.write_cursor == 18U);
  CHECK(zz9k_get_be16(ring) == 5U); /* still the old cursor */
  zz9k_voice_ring_publish(&session);
  CHECK(zz9k_get_be16(ring) == 18U);

  /* Cursor 16 wrapped onto slot 0: the twelfth record of that batch. */
  zz9k_voice_record_encode(&wire, &recs[11]);
  CHECK(memcmp(ring + 16U + 0U, &wire, 16U) == 0);

  /* Full: nothing staged, nothing changes. */
  staged = zz9k_voice_ring_stage(&session, 2U, recs, 1U);
  CHECK(staged == 0U);
  CHECK(session.write_cursor == 18U);

  /* The 16-bit cursor wraps at 65536 and still lands in the right slot. */
  session.write_cursor = 65535U;
  staged = zz9k_voice_ring_stage(&session, 65535U, recs, 2U);
  CHECK(staged == 2U);
  CHECK((uint16_t)session.write_cursor == 1U);
  zz9k_voice_ring_publish(&session);
  CHECK(zz9k_get_be16(ring) == 1U);
  zz9k_voice_record_encode(&wire, &recs[0]);
  CHECK(memcmp(ring + 16U + 15U * 16U, &wire, 16U) == 0); /* 65535 & 15 */
  zz9k_voice_record_encode(&wire, &recs[1]);
  CHECK(memcmp(ring + 16U + 0U, &wire, 16U) == 0);

  /* A dead session or missing records stage nothing. */
  session.mapped = 0U;
  CHECK(zz9k_voice_ring_stage(&session, 0U, recs, 1U) == 0U);
  session.mapped = 1U;
  CHECK(zz9k_voice_ring_stage(&session, 0U, 0, 1U) == 0U);
}

/* ---- Request builders and the OPEN reply decoder ---- */

static void test_request_builders(void)
{
  ZZ9KRequest req;
  ZZ9KVoiceOpenDesc desc;
  const uint8_t *p;

  memset(&desc, 0, sizeof(desc));
  desc.voices = 9U;
  desc.bank_bytes = 0x00100000UL;
  desc.lead_frames = 9000U;
  desc.flags = ZZ9K_VOICE_OPEN_FLAG_NTSC | ZZ9K_VOICE_OPEN_FLAG_LINEAR;

  CHECK(zz9k_request_audio_voice_open(&req, 0x11U, 0x22U, 0x33U, &desc) ==
        ZZ9K_STATUS_OK);
  CHECK(req.entry.opcode == ZZ9K_OP_AUDIO_VOICE_OPEN);
  CHECK(req.entry.payload_len == 48U);
  p = req.entry.payload.inline_data;
  CHECK(zz9k_get_be32(p + 0) == 0x11U);
  CHECK(zz9k_get_be32(p + 4) == 0x22U);
  CHECK(zz9k_get_be32(p + 8) == 0x33U);
  CHECK(zz9k_get_be32(p + 12) == 0x00100000UL);
  CHECK(zz9k_get_be32(p + 16) == 9000U);
  CHECK(zz9k_get_be32(p + 20) == 3U);
  CHECK(zz9k_get_be32(p + 24) == 9U);
  {
    uint32_t i;
    int zero = 1;

    for (i = 28U; i < 48U; i++) {
      zero &= p[i] == 0U;
    }
    CHECK(zero);
  }

  /* Refusals before anything reaches the card. */
  CHECK(zz9k_request_audio_voice_open(&req, 0x11U, 0x11U, 0x33U, &desc) ==
        ZZ9K_STATUS_BAD_REQUEST);
  CHECK(zz9k_request_audio_voice_open(&req, ZZ9K_INVALID_HANDLE, 0x22U, 0x33U,
                                      &desc) == ZZ9K_STATUS_BAD_REQUEST);
  desc.voices = 33U;
  CHECK(zz9k_request_audio_voice_open(&req, 1U, 2U, 3U, &desc) ==
        ZZ9K_STATUS_BAD_REQUEST);
  desc.voices = 0U;
  CHECK(zz9k_request_audio_voice_open(&req, 1U, 2U, 3U, &desc) ==
        ZZ9K_STATUS_BAD_REQUEST);
  desc.voices = 4U;
  desc.bank_bytes = ZZ9K_VOICE_BANK_MAX_BYTES + 1U;
  CHECK(zz9k_request_audio_voice_open(&req, 1U, 2U, 3U, &desc) ==
        ZZ9K_STATUS_BAD_REQUEST);
  desc.bank_bytes = 0U;
  CHECK(zz9k_request_audio_voice_open(&req, 1U, 2U, 3U, &desc) ==
        ZZ9K_STATUS_BAD_REQUEST);
  desc.bank_bytes = 4096U;
  desc.flags = ZZ9K_VOICE_OPEN_FLAG_LOW_LATENCY;
  CHECK(zz9k_request_audio_voice_open(&req, 1U, 2U, 3U, &desc) ==
        ZZ9K_STATUS_OK);
  CHECK(zz9k_get_be32(req.entry.payload.inline_data + 20) == 4U);
  desc.flags = 8U;
  CHECK(zz9k_request_audio_voice_open(&req, 1U, 2U, 3U, &desc) ==
        ZZ9K_STATUS_BAD_REQUEST);

  CHECK(zz9k_request_audio_voice_session(&req, ZZ9K_OP_AUDIO_VOICE_CLOSE,
                                         0xdeadbeefUL) == ZZ9K_STATUS_OK);
  CHECK(req.entry.opcode == ZZ9K_OP_AUDIO_VOICE_CLOSE);
  CHECK(req.entry.payload_len == 48U);
  CHECK(zz9k_get_be32(req.entry.payload.inline_data) == 0xdeadbeefUL);
  CHECK(zz9k_request_audio_voice_session(&req, ZZ9K_OP_AUDIO_VOICE_RESET, 5U) ==
        ZZ9K_STATUS_OK);
  CHECK(req.entry.opcode == ZZ9K_OP_AUDIO_VOICE_RESET);
  CHECK(zz9k_request_audio_voice_session(&req, ZZ9K_OP_AUDIO_VOICE_CLOSE, 0U) ==
        ZZ9K_STATUS_BAD_REQUEST);
  CHECK(zz9k_request_audio_voice_session(&req, ZZ9K_OP_AUDIO_VOICE_OPEN, 1U) ==
        ZZ9K_STATUS_BAD_REQUEST);

  CHECK(zz9k_request_audio_voice_upload(
            &req, 7U, ZZ9K_VOICE_UPLOAD_FLAG_COPY | ZZ9K_VOICE_UPLOAD_FLAG_DEFINE,
            0x10U, 0x20U, 0x30U, 200U, 0x40U, 0x50U,
            ZZ9K_VOICE_FORMAT_S16) == ZZ9K_STATUS_OK);
  CHECK(req.entry.opcode == ZZ9K_OP_AUDIO_VOICE_UPLOAD);
  CHECK(req.entry.payload_len == 48U);
  p = req.entry.payload.inline_data;
  CHECK(zz9k_get_be32(p + 0) == 7U);
  CHECK(zz9k_get_be32(p + 4) == 3U);
  CHECK(zz9k_get_be32(p + 8) == 0x10U);
  CHECK(zz9k_get_be32(p + 12) == 0x20U);
  CHECK(zz9k_get_be32(p + 16) == 0x30U);
  CHECK(zz9k_get_be32(p + 20) == 200U);
  CHECK(zz9k_get_be32(p + 24) == 0x40U);
  CHECK(zz9k_get_be32(p + 28) == 0x50U);
  CHECK(zz9k_get_be32(p + 32) == 1U);
  CHECK(zz9k_request_audio_voice_upload(&req, 7U, 0U, 0, 0, 0, 0, 0, 0, 0) ==
        ZZ9K_STATUS_BAD_REQUEST);
  CHECK(zz9k_request_audio_voice_upload(&req, 7U, 4U, 0, 0, 1, 0, 0, 1, 0) ==
        ZZ9K_STATUS_BAD_REQUEST);
  CHECK(zz9k_request_audio_voice_upload(&req, 7U, 2U, 0, 0, 0, 256U, 0, 1, 0) ==
        ZZ9K_STATUS_BAD_REQUEST);
  CHECK(zz9k_request_audio_voice_upload(&req, 7U, 2U, 0, 0, 0, 0, 0, 1, 3U) ==
        ZZ9K_STATUS_OK);
  CHECK(zz9k_request_audio_voice_upload(&req, 7U, 2U, 0, 0, 0, 0, 0, 1, 4U) ==
        ZZ9K_STATUS_BAD_REQUEST);
}

static void test_open_reply_decoder(void)
{
  ZZ9KMailboxEntry reply;
  ZZ9KVoiceGrant grant;
  uint8_t *p;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_AUDIO_VOICE_OPEN;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = 48U;
  p = reply.payload.inline_data;
  zz9k_put_be32(p + 0, 0xcafe0001UL);
  zz9k_put_be32(p + 4, 8U);
  zz9k_put_be32(p + 8, 0x20000UL);
  zz9k_put_be32(p + 12, 9000U);
  zz9k_put_be32(p + 16, 7680U);
  zz9k_put_be32(p + 20, 24000U);
  zz9k_put_be32(p + 24, 64U);
  zz9k_put_be32(p + 28, 48000U);
  zz9k_put_be32(p + 32, 256U);
  CHECK(zz9k_reply_audio_voice_open_result(&reply, &grant) == ZZ9K_STATUS_OK);
  CHECK(grant.session == 0xcafe0001UL && grant.voices == 8U &&
        grant.bank_bytes == 0x20000UL && grant.lead_frames == 9000U &&
        grant.lead_floor_frames == 7680U && grant.lead_max_frames == 24000U &&
        grant.ring_records == 64U && grant.output_rate == 48000U &&
        grant.max_samples == 256U);

  /* A low-latency grant: the lower floor decodes, and a lead under it is
   * still a firmware fault. */
  zz9k_put_be32(p + 12, 2880U);
  zz9k_put_be32(p + 16, 2880U);
  CHECK(zz9k_reply_audio_voice_open_result(&reply, &grant) == ZZ9K_STATUS_OK);
  CHECK(grant.lead_frames == 2880U && grant.lead_floor_frames == 2880U);
  zz9k_put_be32(p + 12, 2879U);
  CHECK(zz9k_reply_audio_voice_open_result(&reply, &grant) ==
        ZZ9K_STATUS_INTERNAL_ERROR);
  zz9k_put_be32(p + 12, 9000U);
  zz9k_put_be32(p + 16, 7680U);

  /* A grant that breaks the contract is a firmware fault, zeroed. */
  zz9k_put_be32(p + 24, 48U); /* not a power of two */
  CHECK(zz9k_reply_audio_voice_open_result(&reply, &grant) ==
        ZZ9K_STATUS_INTERNAL_ERROR);
  CHECK(grant.session == 0U);
  zz9k_put_be32(p + 24, 64U);
  zz9k_put_be32(p + 0, 0U);
  CHECK(zz9k_reply_audio_voice_open_result(&reply, &grant) ==
        ZZ9K_STATUS_INTERNAL_ERROR);
  zz9k_put_be32(p + 0, 1U);
  reply.payload_len = 40U;
  CHECK(zz9k_reply_audio_voice_open_result(&reply, &grant) ==
        ZZ9K_STATUS_INTERNAL_ERROR);
  reply.payload_len = 48U;
  reply.status = ZZ9K_STATUS_BAD_REQUEST;
  CHECK(zz9k_reply_audio_voice_open_result(&reply, &grant) ==
        ZZ9K_STATUS_BAD_REQUEST);
}

/* ---- Mock firmware ---- */

#define MOCK_RING_ENTRIES 8U
#define MOCK_MAX_BUFS 16U
#define MOCK_MAX_LOG 64U
#define MOCK_BUF_BASE 0x00020000UL

struct TestMailbox {
  ZZ9KMailboxDescriptor descriptor;
  ZZ9KMailboxWireEntry request_ring[MOCK_RING_ENTRIES];
  ZZ9KMailboxWireEntry completion_ring[MOCK_RING_ENTRIES];
};

struct MockBuf {
  uint32_t handle;
  uint32_t arm_addr;
  uint32_t board_offset;
  uint32_t length;
  uint32_t flags;
  int live;
  int pinned;
  uint16_t owner;
};

struct MockLog {
  uint16_t opcode;
  uint8_t payload[48];
  uint16_t client;
};

struct MockSample {
  uint32_t offset;
  uint32_t length;
  uint32_t format;
  int defined;
};

#define MOCK_BANK_MAX 0x00040000UL

static void *g_window;
static uint32_t g_window_size;
static uint32_t g_window_addr;

static struct {
  struct TestMailbox mailbox;
  ZZ9KBoard board;
  uint32_t served_tail;
  struct MockBuf bufs[MOCK_MAX_BUFS];
  uint32_t buf_count;
  uint32_t next_handle;
  uint32_t next_offset;
  int allocs;
  int frees;
  int fail_alloc_at;           /* 1-based alloc call that fails, 0 = none */
  uint16_t open_status;        /* status OPEN answers with */
  uint32_t grant_ring_override;/* 0 = honest ring_records */
  uint32_t session;
  uint16_t session_owner;
  uint32_t release_calls;
  uint32_t open_calls;
  int session_open;
  uint32_t fail_copy_at;       /* 1-based COPY that fails, 0 = none */
  uint32_t copies;
  uint32_t defines;
  uint16_t close_status;
  uint32_t bank_bytes;
  uint8_t bank[MOCK_BANK_MAX];
  struct MockSample samples[ZZ9K_VOICE_MAX_SAMPLES];
  struct MockLog log[MOCK_MAX_LOG];
  uint32_t log_count;
  uint32_t last_alloc_flags[MOCK_MAX_BUFS];
} g_mock;

static void *mock_window_alloc(uint32_t size)
{
  static const uintptr_t low_bases[] = {
      UINT32_C(0x10000000), UINT32_C(0x18000000), UINT32_C(0x28000000),
      UINT32_C(0x30000000)};
  void *p;
  size_t i;

#if defined(_WIN32)
  for (i = 0U; i < sizeof(low_bases) / sizeof(low_bases[0]); i++) {
    p = VirtualAlloc((LPVOID)low_bases[i], (SIZE_T)size,
                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p && (uint64_t)(uintptr_t)p + size <= UINT64_C(0x100000000)) {
      return p;
    }
    if (p) {
      VirtualFree(p, 0, MEM_RELEASE);
    }
  }
#else
  int flags = MAP_PRIVATE | ZZ9K_MMAP_ANONYMOUS;

#if defined(MAP_32BIT)
  p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, flags | MAP_32BIT, -1,
           0);
  if (p != MAP_FAILED) {
    if ((uint64_t)(uintptr_t)p + size <= UINT64_C(0x100000000)) {
      return p;
    }
    munmap(p, (size_t)size);
  }
#endif
  for (i = 0U; i < sizeof(low_bases) / sizeof(low_bases[0]); i++) {
#if defined(MAP_FIXED_NOREPLACE)
    p = mmap((void *)low_bases[i], (size_t)size, PROT_READ | PROT_WRITE,
             flags | MAP_FIXED_NOREPLACE, -1, 0);
#else
    p = mmap((void *)low_bases[i], (size_t)size, PROT_READ | PROT_WRITE,
             flags, -1, 0);
#endif
    if (p != MAP_FAILED) {
      if ((uint64_t)(uintptr_t)p + size <= UINT64_C(0x100000000)) {
        return p;
      }
      munmap(p, (size_t)size);
    }
  }
#endif
  return NULL;
}

static void mock_window_free(void *p, uint32_t size)
{
  if (!p) {
    return;
  }
#if defined(_WIN32)
  (void)size;
  VirtualFree(p, 0, MEM_RELEASE);
#else
  munmap(p, (size_t)size);
#endif
}

static uint8_t *mock_ptr(uint32_t board_offset)
{
  return (uint8_t *)(uintptr_t)(g_window_addr + board_offset);
}

static void mock_reset(uint16_t zorro_version)
{
  struct TestMailbox *m;

  memset(g_window, 0, g_window_size);
  memset(&g_mock, 0, sizeof(g_mock));
  m = &g_mock.mailbox;
  zz9k_put_be32(m->descriptor.magic, ZZ9K_ABI_MAGIC);
  zz9k_put_be16(m->descriptor.abi_major, ZZ9K_ABI_VERSION_MAJOR);
  zz9k_put_be16(m->descriptor.abi_minor, ZZ9K_ABI_VERSION_MINOR);
  zz9k_put_be32(m->descriptor.descriptor_size, (uint32_t)sizeof(m->descriptor));
  zz9k_put_be32(m->descriptor.request_ring_offset,
                (uint32_t)offsetof(struct TestMailbox, request_ring));
  zz9k_put_be32(m->descriptor.request_ring_entries, MOCK_RING_ENTRIES);
  zz9k_put_be32(m->descriptor.completion_ring_offset,
                (uint32_t)offsetof(struct TestMailbox, completion_ring));
  zz9k_put_be32(m->descriptor.completion_ring_entries, MOCK_RING_ENTRIES);
  zz9k_put_be32(m->descriptor.capability_bits, ZZ9K_CAP_MAILBOX);
  g_mock.board.zorro_version = zorro_version;
  g_mock.board.board_addr = g_window_addr;
  g_mock.board.board_size = g_window_size;
  g_mock.board.product = zorro_version == 2U ? ZZ9K_PRODUCT_Z2 : ZZ9K_PRODUCT_Z3;
  g_mock.next_handle = 0x40000001UL;
  g_mock.next_offset = MOCK_BUF_BASE;
  g_mock.open_status = ZZ9K_STATUS_OK;
  g_mock.close_status = ZZ9K_STATUS_OK;
}

static struct MockBuf *mock_find(uint32_t handle)
{
  uint32_t i;

  for (i = 0U; i < g_mock.buf_count; i++) {
    if (g_mock.bufs[i].handle == handle && g_mock.bufs[i].live) {
      return &g_mock.bufs[i];
    }
  }
  return NULL;
}

static ZZ9KMailboxWireEntry *mock_complete(const ZZ9KMailboxWireEntry *req,
                                           uint16_t status,
                                           uint16_t payload_len)
{
  struct TestMailbox *m = &g_mock.mailbox;
  ZZ9KMailboxWireEntry *reply;
  uint32_t tail = zz9k_get_be32(m->descriptor.completion_tail);

  reply = &m->completion_ring[tail % MOCK_RING_ENTRIES];
  memset((void *)reply->payload, 0, sizeof(reply->payload));
  zz9k_put_be32(reply->request_id, zz9k_get_be32(req->request_id));
  zz9k_put_be16(reply->opcode, zz9k_get_be16(req->opcode));
  zz9k_put_be16(reply->status, status);
  zz9k_put_be16(reply->payload_len, payload_len);
  zz9k_put_be32(reply->user_cookie, zz9k_get_be32(req->user_cookie));
  zz9k_put_be32(m->descriptor.completion_tail, (tail + 1U) % MOCK_RING_ENTRIES);
  zz9k_put_be32(m->descriptor.request_head,
                (zz9k_get_be32(m->descriptor.request_head) + 1U) %
                    MOCK_RING_ENTRIES);
  return reply;
}

static void mock_word(ZZ9KMailboxWireEntry *reply, uint32_t index,
                      uint32_t value)
{
  zz9k_put_be32(&reply->payload[index * 4U], value);
}

static uint32_t mock_req_word(const ZZ9KMailboxWireEntry *req, uint32_t index)
{
  return zz9k_get_be32(&req->payload[index * 4U]);
}

static uint16_t mock_req_client(const ZZ9KMailboxWireEntry *req)
{
  return (uint16_t)((zz9k_get_be16(req->flags) & ZZ9K_ENTRY_CLIENT_ID_MASK) >>
                    ZZ9K_ENTRY_CLIENT_ID_SHIFT);
}

/* The firmware's voice_session_owned_by(): the session id of another client
 * is no handle. */
static int mock_owns_session(const ZZ9KMailboxWireEntry *req)
{
  return g_mock.session_open && mock_req_word(req, 0) == g_mock.session &&
         g_mock.session_owner == mock_req_client(req);
}

static void mock_log(const ZZ9KMailboxWireEntry *req)
{
  if (g_mock.log_count < MOCK_MAX_LOG) {
    g_mock.log[g_mock.log_count].opcode = zz9k_get_be16(req->opcode);
    g_mock.log[g_mock.log_count].client = mock_req_client(req);
    memcpy(g_mock.log[g_mock.log_count].payload, (const void *)req->payload,
           48U);
    g_mock.log_count++;
  }
}

static uint32_t mock_ring_records(uint32_t ring_bytes)
{
  return zz9k_voice_ring_records_for_bytes(ring_bytes);
}

static void mock_serve_open(const ZZ9KMailboxWireEntry *req)
{
  struct MockBuf *ring = mock_find(mock_req_word(req, 0));
  struct MockBuf *status = mock_find(mock_req_word(req, 1));
  struct MockBuf *staging = mock_find(mock_req_word(req, 2));
  ZZ9KMailboxWireEntry *reply;
  uint32_t bank = mock_req_word(req, 3);
  uint32_t lead = mock_req_word(req, 4);
  uint32_t voices = mock_req_word(req, 6);
  uint32_t flags = mock_req_word(req, 5);
  uint32_t floor_frames =
      (flags & ZZ9K_VOICE_OPEN_FLAG_LOW_LATENCY) != 0U ? 2880U : 7680U;

  g_mock.open_calls++;
  if (g_mock.open_status != ZZ9K_STATUS_OK) {
    (void)mock_complete(req, g_mock.open_status, 0U);
    return;
  }
  /* The firmware takes only buffers the opening client allocated. */
  if (!ring || !status || !staging || g_mock.session_open ||
      ring == status || ring == staging || status == staging ||
      ring->owner != mock_req_client(req) ||
      status->owner != mock_req_client(req) ||
      staging->owner != mock_req_client(req) ||
      status->length < ZZ9K_VOICE_STATUS_BYTES || bank > MOCK_BANK_MAX) {
    (void)mock_complete(req, ZZ9K_STATUS_BAD_HANDLE, 0U);
    return;
  }
  ring->pinned = status->pinned = staging->pinned = 1;
  g_mock.session_open = 1;
  g_mock.session_owner = mock_req_client(req);
  g_mock.session = 0xa5000000UL + g_mock.open_calls;
  g_mock.bank_bytes = bank;
  memset(g_mock.bank, 0, sizeof(g_mock.bank));
  memset(g_mock.samples, 0, sizeof(g_mock.samples));
  if (lead < floor_frames) {
    lead = floor_frames;
  }
  if (lead > 24000U) {
    lead = 24000U;
  }
  reply = mock_complete(req, ZZ9K_STATUS_OK, 48U);
  mock_word(reply, 0, g_mock.session);
  mock_word(reply, 1, voices);
  mock_word(reply, 2, bank);
  mock_word(reply, 3, lead);
  mock_word(reply, 4, floor_frames);
  mock_word(reply, 5, 24000U);
  mock_word(reply, 6, g_mock.grant_ring_override != 0U
                          ? g_mock.grant_ring_override
                          : mock_ring_records(ring->length));
  mock_word(reply, 7, 48000U);
  mock_word(reply, 8, 256U);
}

static void mock_unpin(void)
{
  uint32_t i;

  for (i = 0U; i < g_mock.buf_count; i++) {
    g_mock.bufs[i].pinned = 0;
  }
  g_mock.session_open = 0;
}

static void mock_serve_upload(const ZZ9KMailboxWireEntry *req)
{
  uint32_t flags = mock_req_word(req, 1);
  uint32_t staging_offset = mock_req_word(req, 2);
  uint32_t bank_offset = mock_req_word(req, 3);
  uint32_t length = mock_req_word(req, 4);
  uint32_t id = mock_req_word(req, 5);
  uint32_t sample_offset = mock_req_word(req, 6);
  uint32_t sample_length = mock_req_word(req, 7);
  uint32_t format = mock_req_word(req, 8);
  struct MockBuf *staging = NULL;
  uint32_t i;

  if (!mock_owns_session(req)) {
    (void)mock_complete(req, ZZ9K_STATUS_BAD_HANDLE, 0U);
    return;
  }
  for (i = 0U; i < g_mock.buf_count; i++) {
    /* The staging buffer is the third buffer of the session. */
    if (g_mock.bufs[i].pinned) {
      staging = &g_mock.bufs[i];
    }
  }
  if ((flags & ZZ9K_VOICE_UPLOAD_FLAG_COPY) != 0U) {
    g_mock.copies++;
    if (g_mock.fail_copy_at != 0U && g_mock.copies == g_mock.fail_copy_at) {
      (void)mock_complete(req, ZZ9K_STATUS_INTERNAL_ERROR, 0U);
      return;
    }
    if (!staging || staging_offset > staging->length ||
        length > staging->length - staging_offset ||
        bank_offset > g_mock.bank_bytes ||
        length > g_mock.bank_bytes - bank_offset) {
      (void)mock_complete(req, ZZ9K_STATUS_BAD_REQUEST, 0U);
      return;
    }
    memcpy(g_mock.bank + bank_offset,
           mock_ptr(staging->board_offset) + staging_offset, length);
  }
  if ((flags & ZZ9K_VOICE_UPLOAD_FLAG_DEFINE) != 0U) {
    g_mock.defines++;
    if (id >= ZZ9K_VOICE_MAX_SAMPLES ||
        sample_offset > g_mock.bank_bytes ||
        sample_length > g_mock.bank_bytes - sample_offset) {
      (void)mock_complete(req, ZZ9K_STATUS_BAD_REQUEST, 0U);
      return;
    }
    g_mock.samples[id].offset = sample_offset;
    g_mock.samples[id].length = sample_length;
    g_mock.samples[id].format = format;
    g_mock.samples[id].defined = 1;
  }
  (void)mock_complete(req, ZZ9K_STATUS_OK, 0U);
}

static void mock_respond(void)
{
  struct TestMailbox *m = &g_mock.mailbox;
  const ZZ9KMailboxWireEntry *req;
  uint32_t head = zz9k_get_be32(m->descriptor.request_head);
  uint32_t tail = zz9k_get_be32(m->descriptor.request_tail);
  uint16_t opcode;
  ZZ9KMailboxWireEntry *reply;

  if (head == tail || g_mock.served_tail == tail) {
    return;
  }
  g_mock.served_tail = tail;
  req = &m->request_ring[head];
  opcode = zz9k_get_be16(req->opcode);
  if (opcode >= ZZ9K_OP_AUDIO_VOICE_OPEN &&
      opcode <= ZZ9K_OP_AUDIO_VOICE_UPLOAD) {
    mock_log(req);
  }

  switch (opcode) {
  case ZZ9K_OP_QUERY_CAPS:
    reply = mock_complete(req, ZZ9K_STATUS_OK, 40U);
    mock_word(reply, 0, ZZ9K_ABI_MAGIC);
    mock_word(reply, 1, ((uint32_t)ZZ9K_ABI_VERSION_MAJOR << 16) |
                            ZZ9K_ABI_VERSION_MINOR);
    mock_word(reply, 2, zz9k_get_be32(m->descriptor.capability_bits));
    mock_word(reply, 3, 48U);
    mock_word(reply, 4, 32U);
    mock_word(reply, 7, MOCK_RING_ENTRIES);
    mock_word(reply, 8, MOCK_RING_ENTRIES);
    break;
  case ZZ9K_OP_ALLOC_SHARED: {
    uint32_t length = mock_req_word(req, 0);
    uint32_t alignment = mock_req_word(req, 1);
    uint32_t flags = mock_req_word(req, 2);
    struct MockBuf *b;

    g_mock.allocs++;
    if ((g_mock.fail_alloc_at != 0 && g_mock.allocs == g_mock.fail_alloc_at) ||
        g_mock.buf_count >= MOCK_MAX_BUFS) {
      (void)mock_complete(req, ZZ9K_STATUS_NO_MEMORY, 0U);
      break;
    }
    if (alignment < 64U) {
      alignment = 64U;
    }
    g_mock.next_offset = (g_mock.next_offset + alignment - 1U) &
                         ~(alignment - 1U);
    b = &g_mock.bufs[g_mock.buf_count++];
    b->handle = g_mock.next_handle++;
    b->board_offset = g_mock.next_offset;
    b->arm_addr = (uint32_t)ZZ9K_ARM_MEMORY_START +
                  (b->board_offset - (uint32_t)ZZ9K_AMIGA_MEMORY_OFFSET);
    b->length = length;
    b->flags = flags;
    b->owner = mock_req_client(req);
    b->live = 1;
    g_mock.last_alloc_flags[g_mock.buf_count - 1U] = flags;
    g_mock.next_offset += length;
    reply = mock_complete(req, ZZ9K_STATUS_OK, 16U);
    mock_word(reply, 0, b->handle);
    mock_word(reply, 1, b->arm_addr);
    mock_word(reply, 2, b->length);
    mock_word(reply, 3, flags);
    break;
  }
  case ZZ9K_OP_FREE_SHARED: {
    struct MockBuf *b = mock_find(mock_req_word(req, 0));

    g_mock.frees++;
    if (!b || b->pinned) {
      (void)mock_complete(req, ZZ9K_STATUS_BAD_HANDLE, 0U);
      break;
    }
    b->live = 0;
    (void)mock_complete(req, ZZ9K_STATUS_OK, 0U);
    break;
  }
  case ZZ9K_OP_CLIENT_RELEASE: {
    uint16_t client = mock_req_client(req);
    uint32_t i;

    g_mock.release_calls++;
    if (client == 0U) {
      (void)mock_complete(req, ZZ9K_STATUS_BAD_REQUEST, 0U);
      break;
    }
    if (g_mock.session_open && g_mock.session_owner == client) {
      mock_unpin();
    }
    for (i = 0U; i < g_mock.buf_count; i++) {
      if (g_mock.bufs[i].live && g_mock.bufs[i].owner == client) {
        g_mock.bufs[i].live = 0;
      }
    }
    (void)mock_complete(req, ZZ9K_STATUS_OK, 0U);
    break;
  }
  case ZZ9K_OP_AUDIO_VOICE_OPEN:
    mock_serve_open(req);
    break;
  case ZZ9K_OP_AUDIO_VOICE_CLOSE:
    if (!mock_owns_session(req)) {
      (void)mock_complete(req, ZZ9K_STATUS_BAD_HANDLE, 0U);
      break;
    }
    mock_unpin();
    (void)mock_complete(req, g_mock.close_status, 0U);
    break;
  case ZZ9K_OP_AUDIO_VOICE_RESET:
    if (!mock_owns_session(req)) {
      (void)mock_complete(req, ZZ9K_STATUS_BAD_HANDLE, 0U);
      break;
    }
    (void)mock_complete(req, ZZ9K_STATUS_OK, 0U);
    break;
  case ZZ9K_OP_AUDIO_VOICE_UPLOAD:
    mock_serve_upload(req);
    break;
  default:
    (void)mock_complete(req, ZZ9K_STATUS_UNSUPPORTED, 0U);
    break;
  }
}

static int mock_live_buffers(void)
{
  uint32_t i;
  int live = 0;

  for (i = 0U; i < g_mock.buf_count; i++) {
    live += g_mock.bufs[i].live;
  }
  return live;
}

static ZZ9KContext *mock_attach(uint16_t zorro_version)
{
  ZZ9KContext *ctx = NULL;

  mock_reset(zorro_version);
  zz9k_set_idle_hook_for_test(mock_respond);
  if (zz9k_attach_mailbox(&ctx, &g_mock.board, &g_mock.mailbox.descriptor, 0,
                          0) != ZZ9K_STATUS_OK) {
    zz9k_set_idle_hook_for_test(0);
    return NULL;
  }
  return ctx;
}

static void mock_detach(ZZ9KContext *ctx)
{
  zz9k_close(ctx);
  zz9k_set_idle_hook_for_test(0);
}

static void default_desc(ZZ9KVoiceOpenDesc *desc)
{
  memset(desc, 0, sizeof(*desc));
  desc->voices = 8U;
  desc->bank_bytes = 0x20000UL;
  desc->lead_frames = 9000U;
  desc->flags = ZZ9K_VOICE_OPEN_FLAG_LINEAR;
}

/* ---- Host session tests ---- */

static void test_open_success(uint16_t zorro_version)
{
  ZZ9KContext *ctx = mock_attach(zorro_version);
  ZZ9KVoiceOpenDesc desc;
  ZZ9KVoiceSession s;
  struct MockLog *open;

  CHECK(ctx != NULL);
  if (!ctx) {
    return;
  }
  default_desc(&desc);
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_OK);
  CHECK(s.mapped == 1U);
  CHECK(g_mock.allocs == 3);
  CHECK(g_mock.frees == 0);
  CHECK(g_mock.open_calls == 1U);

  /* Sizes: ring 2 KiB, status 64 bytes, staging 4 KiB, all cache-line
   * aligned and, on Zorro II, requested from the host window. */
  CHECK(g_mock.bufs[0].length == ZZ9K_VOICE_RING_DEFAULT_BYTES);
  CHECK(g_mock.bufs[1].length == ZZ9K_VOICE_STATUS_BYTES);
  CHECK(g_mock.bufs[2].length == ZZ9K_VOICE_STAGING_DEFAULT_BYTES);
  CHECK((g_mock.bufs[0].board_offset & 63U) == 0U);
  CHECK((g_mock.bufs[1].board_offset & 63U) == 0U);
  CHECK((g_mock.bufs[2].board_offset & 63U) == 0U);
  if (zorro_version == 2U) {
    CHECK((g_mock.last_alloc_flags[0] & ZZ9K_ALLOC_HOST_WINDOW) != 0U);
    CHECK((g_mock.last_alloc_flags[1] & ZZ9K_ALLOC_HOST_WINDOW) != 0U);
    CHECK((g_mock.last_alloc_flags[2] & ZZ9K_ALLOC_HOST_WINDOW) != 0U);
  }

  /* The OPEN request names the three handles and the descriptor. */
  CHECK(g_mock.log_count == 1U);
  open = &g_mock.log[0];
  CHECK(open->opcode == ZZ9K_OP_AUDIO_VOICE_OPEN);
  CHECK(zz9k_get_be32(open->payload + 0) == g_mock.bufs[0].handle);
  CHECK(zz9k_get_be32(open->payload + 4) == g_mock.bufs[1].handle);
  CHECK(zz9k_get_be32(open->payload + 8) == g_mock.bufs[2].handle);
  CHECK(zz9k_get_be32(open->payload + 12) == 0x20000UL);
  CHECK(zz9k_get_be32(open->payload + 16) == 9000U);
  CHECK(zz9k_get_be32(open->payload + 20) == ZZ9K_VOICE_OPEN_FLAG_LINEAR);
  CHECK(zz9k_get_be32(open->payload + 24) == 8U);

  /* The session carries the grant and Amiga-visible pointers. */
  CHECK(s.grant.session == g_mock.session);
  CHECK(s.grant.voices == 8U);
  CHECK(s.grant.bank_bytes == 0x20000UL);
  CHECK(s.grant.lead_frames == 9000U);
  CHECK(s.grant.lead_floor_frames == 7680U);
  CHECK(s.grant.lead_max_frames == 24000U);
  CHECK(s.grant.ring_records == 64U);
  CHECK(s.grant.output_rate == 48000U);
  CHECK(s.grant.max_samples == 256U);
  CHECK(s.ring_handle == g_mock.bufs[0].handle);
  CHECK(s.status_handle == g_mock.bufs[1].handle);
  CHECK(s.staging_handle == g_mock.bufs[2].handle);
  CHECK(s.ring == mock_ptr(g_mock.bufs[0].board_offset));
  CHECK(s.status == mock_ptr(g_mock.bufs[1].board_offset));
  CHECK(s.staging == mock_ptr(g_mock.bufs[2].board_offset));
  CHECK(s.ring_bytes == 2048U && s.staging_bytes == 4096U);
  CHECK(s.write_cursor == 0U);
  /* Ring header clean, heartbeat started, status block clean. */
  CHECK(zz9k_get_be16(s.ring) == 0U);
  CHECK(zz9k_get_be32(s.ring + 4) == 1U);
  {
    uint32_t i;
    int clean = 1;

    for (i = 0U; i < 64U; i++) {
      clean &= s.status[i] == 0U;
    }
    CHECK(clean);
  }

  CHECK(zz9k_voice_close(ctx, &s) == ZZ9K_STATUS_OK);
  CHECK(s.mapped == 0U);
  mock_detach(ctx);
}

/* A low-latency open asks for the short floor: the request carries the flag
 * and the grant reports the lower floor, whatever lead was asked. */
static void test_open_low_latency(void)
{
  ZZ9KContext *ctx = mock_attach(3U);
  ZZ9KVoiceOpenDesc desc;
  ZZ9KVoiceSession s;

  CHECK(ctx != NULL);
  if (!ctx) {
    return;
  }
  default_desc(&desc);
  desc.flags = ZZ9K_VOICE_OPEN_FLAG_LOW_LATENCY;
  desc.lead_frames = 0U;
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_OK);
  CHECK(zz9k_get_be32(g_mock.log[0].payload + 20) ==
        ZZ9K_VOICE_OPEN_FLAG_LOW_LATENCY);
  CHECK(s.grant.lead_floor_frames == 2880U);
  CHECK(s.grant.lead_frames == 2880U);
  CHECK(zz9k_voice_close(ctx, &s) == ZZ9K_STATUS_OK);

  default_desc(&desc);
  desc.flags = ZZ9K_VOICE_OPEN_FLAG_LOW_LATENCY | ZZ9K_VOICE_OPEN_FLAG_LINEAR;
  desc.lead_frames = 1000U;
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_OK);
  CHECK(s.grant.lead_floor_frames == 2880U && s.grant.lead_frames == 2880U);
  CHECK(zz9k_voice_close(ctx, &s) == ZZ9K_STATUS_OK);

  /* A request above the short floor is granted as asked. */
  default_desc(&desc);
  desc.flags = ZZ9K_VOICE_OPEN_FLAG_LOW_LATENCY;
  desc.lead_frames = 5000U;
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_OK);
  CHECK(s.grant.lead_floor_frames == 2880U && s.grant.lead_frames == 5000U);
  CHECK(zz9k_voice_close(ctx, &s) == ZZ9K_STATUS_OK);
  mock_detach(ctx);
}

static void test_open_caller_sizes(void)
{
  ZZ9KContext *ctx = mock_attach(3U);
  ZZ9KVoiceOpenDesc desc;
  ZZ9KVoiceSession s;

  CHECK(ctx != NULL);
  if (!ctx) {
    return;
  }
  default_desc(&desc);
  desc.ring_bytes = 16U + 256U * 16U;
  desc.staging_bytes = 8192U;
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_OK);
  CHECK(g_mock.bufs[0].length == 16U + 256U * 16U);
  CHECK(g_mock.bufs[2].length == 8192U);
  CHECK(s.grant.ring_records == 256U);
  CHECK(zz9k_voice_close(ctx, &s) == ZZ9K_STATUS_OK);
  mock_detach(ctx);
}

static void test_open_refused_frees_everything(void)
{
  ZZ9KContext *ctx = mock_attach(3U);
  ZZ9KVoiceOpenDesc desc;
  ZZ9KVoiceSession s;
  uint32_t i;

  CHECK(ctx != NULL);
  if (!ctx) {
    return;
  }
  default_desc(&desc);
  g_mock.open_status = ZZ9K_STATUS_BUSY;
  memset(&s, 0xee, sizeof(s));
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_BUSY);
  CHECK(g_mock.allocs == 3);
  CHECK(g_mock.frees == 3);
  CHECK(mock_live_buffers() == 0);
  CHECK(g_mock.open_calls == 1U);
  for (i = 0U; i < sizeof(s); i++) {
    if (((const uint8_t *)&s)[i] != 0U) {
      CHECK(0 && "session not zeroed after a refused open");
      break;
    }
  }

  /* Any other refusal frees the same way. */
  g_mock.open_status = ZZ9K_STATUS_NO_MEMORY;
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_NO_MEMORY);
  CHECK(mock_live_buffers() == 0);
  CHECK(g_mock.allocs == g_mock.frees);

  /* The next open on the same card still works. */
  g_mock.open_status = ZZ9K_STATUS_OK;
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_OK);
  CHECK(zz9k_voice_close(ctx, &s) == ZZ9K_STATUS_OK);
  CHECK(mock_live_buffers() == 0);
  mock_detach(ctx);
}

static void test_open_partial_allocation_failure(void)
{
  ZZ9KContext *ctx = mock_attach(3U);
  ZZ9KVoiceOpenDesc desc;
  ZZ9KVoiceSession s;

  CHECK(ctx != NULL);
  if (!ctx) {
    return;
  }
  default_desc(&desc);

  /* The third allocation fails: the first two are given back. */
  g_mock.fail_alloc_at = 3;
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_NO_MEMORY);
  CHECK(g_mock.allocs == 3 && g_mock.frees == 2);
  CHECK(mock_live_buffers() == 0);
  CHECK(g_mock.open_calls == 0U);

  /* The second allocation fails: the first is given back. */
  mock_detach(ctx);
  ctx = mock_attach(3U);
  CHECK(ctx != NULL);
  if (!ctx) {
    return;
  }
  g_mock.fail_alloc_at = 2;
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_NO_MEMORY);
  CHECK(g_mock.allocs == 2 && g_mock.frees == 1);
  CHECK(mock_live_buffers() == 0);
  mock_detach(ctx);
}

static void test_open_bad_grant_closes_and_frees(void)
{
  ZZ9KContext *ctx = mock_attach(3U);
  ZZ9KVoiceOpenDesc desc;
  ZZ9KVoiceSession s;

  CHECK(ctx != NULL);
  if (!ctx) {
    return;
  }
  default_desc(&desc);

  /* A ring capacity that disagrees with the buffer is a firmware fault; the
   * session it opened is closed (unpinning the buffers) before they are
   * freed, so nothing leaks card-side. */
  g_mock.grant_ring_override = 128U;
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_INTERNAL_ERROR);
  CHECK(g_mock.session_open == 0);
  CHECK(mock_live_buffers() == 0);
  CHECK(s.mapped == 0U);
  {
    uint32_t i;
    int closed = 0;

    for (i = 0U; i < g_mock.log_count; i++) {
      closed += g_mock.log[i].opcode == ZZ9K_OP_AUDIO_VOICE_CLOSE;
    }
    CHECK(closed == 1);
  }
  mock_detach(ctx);
}

static void test_open_rejects_bad_desc_before_allocating(void)
{
  ZZ9KContext *ctx = mock_attach(3U);
  ZZ9KVoiceOpenDesc desc;
  ZZ9KVoiceSession s;

  CHECK(ctx != NULL);
  if (!ctx) {
    return;
  }
  default_desc(&desc);
  desc.voices = 33U;
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_BAD_REQUEST);
  default_desc(&desc);
  desc.ring_bytes = 100U;
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_BAD_REQUEST);
  default_desc(&desc);
  desc.ring_bytes = ZZ9K_VOICE_RING_MAX_BYTES + 1U;
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_BAD_REQUEST);
  default_desc(&desc);
  desc.flags = 0x80U;
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_BAD_REQUEST);
  default_desc(&desc);
  desc.flags = 8U; /* the bit after the low-latency flag is unknown */
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_BAD_REQUEST);
  CHECK(zz9k_voice_open(ctx, &desc, NULL) == ZZ9K_STATUS_BAD_REQUEST);
  CHECK(g_mock.allocs == 0);
  mock_detach(ctx);
}

static void make_pattern(uint8_t *data, uint32_t length)
{
  uint32_t i;

  for (i = 0U; i < length; i++) {
    data[i] = (uint8_t)(i * 7U + 3U);
  }
}

static void test_upload_chunks_then_one_define(void)
{
  ZZ9KContext *ctx = mock_attach(3U);
  ZZ9KVoiceOpenDesc desc;
  ZZ9KVoiceSession s;
  ZZ9KVoiceUploadDesc up;
  uint8_t data[10000];
  uint32_t i;
  uint32_t copies = 0U;
  uint32_t defines = 0U;
  uint32_t expect_len[3] = {4096U, 4096U, 1808U};
  uint32_t seen_define_at = 0U;

  CHECK(ctx != NULL);
  if (!ctx) {
    return;
  }
  default_desc(&desc);
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_OK);
  make_pattern(data, sizeof(data));
  memset(&up, 0, sizeof(up));
  up.sample_id = 17U;
  up.bank_offset = 100U;
  up.data = data;
  up.length = sizeof(data);
  up.format = ZZ9K_VOICE_FORMAT_S16;
  g_mock.log_count = 0U;
  CHECK(zz9k_voice_upload(ctx, &s, &up) == ZZ9K_STATUS_OK);

  /* 10000 bytes through a 4096-byte staging buffer: three COPY requests,
   * then exactly one DEFINE, and nothing else. */
  for (i = 0U; i < g_mock.log_count; i++) {
    const struct MockLog *l = &g_mock.log[i];
    uint32_t flags = zz9k_get_be32(l->payload + 4);

    CHECK(l->opcode == ZZ9K_OP_AUDIO_VOICE_UPLOAD);
    CHECK(zz9k_get_be32(l->payload + 0) == s.grant.session);
    if (flags == ZZ9K_VOICE_UPLOAD_FLAG_COPY) {
      CHECK(defines == 0U); /* every COPY precedes the DEFINE */
      CHECK(copies < 3U);
      if (copies < 3U) {
        CHECK(zz9k_get_be32(l->payload + 8) == 0U);
        CHECK(zz9k_get_be32(l->payload + 12) == 100U + copies * 4096U);
        CHECK(zz9k_get_be32(l->payload + 16) == expect_len[copies]);
      }
      copies++;
    } else if (flags == ZZ9K_VOICE_UPLOAD_FLAG_DEFINE) {
      defines++;
      seen_define_at = i;
      CHECK(zz9k_get_be32(l->payload + 20) == 17U);
      CHECK(zz9k_get_be32(l->payload + 24) == 100U);
      CHECK(zz9k_get_be32(l->payload + 28) == 10000U);
      CHECK(zz9k_get_be32(l->payload + 32) == ZZ9K_VOICE_FORMAT_S16);
    } else {
      CHECK(0 && "unexpected upload flags");
    }
  }
  CHECK(copies == 3U && defines == 1U);
  CHECK(g_mock.log_count == 4U && seen_define_at == 3U);

  /* The bytes arrived intact and in place. */
  CHECK(memcmp(g_mock.bank + 100U, data, sizeof(data)) == 0);
  CHECK(g_mock.samples[17].defined == 1);
  CHECK(g_mock.samples[17].offset == 100U);
  CHECK(g_mock.samples[17].length == 10000U);
  CHECK(g_mock.samples[17].format == ZZ9K_VOICE_FORMAT_S16);

  /* A sample that fits the staging buffer is one COPY and one DEFINE. */
  g_mock.log_count = 0U;
  up.sample_id = 3U;
  up.bank_offset = 0U;
  up.length = 300U;
  up.format = ZZ9K_VOICE_FORMAT_S8;
  CHECK(zz9k_voice_upload(ctx, &s, &up) == ZZ9K_STATUS_OK);
  CHECK(g_mock.log_count == 2U);
  CHECK(zz9k_get_be32(g_mock.log[0].payload + 4) == ZZ9K_VOICE_UPLOAD_FLAG_COPY);
  CHECK(zz9k_get_be32(g_mock.log[1].payload + 4) ==
        ZZ9K_VOICE_UPLOAD_FLAG_DEFINE);
  CHECK(memcmp(g_mock.bank, data, 300U) == 0);

  CHECK(zz9k_voice_close(ctx, &s) == ZZ9K_STATUS_OK);
  mock_detach(ctx);
}

static void test_upload_small_staging_and_failures(void)
{
  ZZ9KContext *ctx = mock_attach(3U);
  ZZ9KVoiceOpenDesc desc;
  ZZ9KVoiceSession s;
  ZZ9KVoiceUploadDesc up;
  uint8_t data[200];

  CHECK(ctx != NULL);
  if (!ctx) {
    return;
  }
  default_desc(&desc);
  desc.staging_bytes = 70U; /* not a multiple of 4: chunks round down to 68 */
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_OK);
  make_pattern(data, sizeof(data));
  memset(&up, 0, sizeof(up));
  up.sample_id = 1U;
  up.bank_offset = 10U;
  up.data = data;
  up.length = sizeof(data);
  g_mock.log_count = 0U;
  CHECK(zz9k_voice_upload(ctx, &s, &up) == ZZ9K_STATUS_OK);
  /* 200 = 68 + 68 + 64: three COPY and one DEFINE. */
  CHECK(g_mock.log_count == 4U);
  CHECK(zz9k_get_be32(g_mock.log[0].payload + 16) == 68U);
  CHECK(zz9k_get_be32(g_mock.log[1].payload + 12) == 10U + 68U);
  CHECK(zz9k_get_be32(g_mock.log[2].payload + 12) == 10U + 136U);
  CHECK(zz9k_get_be32(g_mock.log[2].payload + 16) == 64U);
  CHECK(memcmp(g_mock.bank + 10U, data, sizeof(data)) == 0);

  /* A COPY the firmware refuses stops the upload before any DEFINE. */
  g_mock.log_count = 0U;
  g_mock.defines = 0U;
  g_mock.copies = 0U;
  g_mock.fail_copy_at = 2U;
  CHECK(zz9k_voice_upload(ctx, &s, &up) == ZZ9K_STATUS_INTERNAL_ERROR);
  CHECK(g_mock.defines == 0U);
  CHECK(g_mock.log_count == 2U);
  g_mock.fail_copy_at = 0U;

  /* Range and id checks happen before a single byte moves. */
  g_mock.log_count = 0U;
  up.bank_offset = s.grant.bank_bytes - 100U; /* 200 bytes do not fit */
  CHECK(zz9k_voice_upload(ctx, &s, &up) == ZZ9K_STATUS_BAD_REQUEST);
  up.bank_offset = 0U;
  up.sample_id = 256U;
  CHECK(zz9k_voice_upload(ctx, &s, &up) == ZZ9K_STATUS_BAD_REQUEST);
  up.sample_id = 1U;
  up.format = 4U;
  CHECK(zz9k_voice_upload(ctx, &s, &up) == ZZ9K_STATUS_BAD_REQUEST);
  up.format = 0U;
  up.length = 0U;
  CHECK(zz9k_voice_upload(ctx, &s, &up) == ZZ9K_STATUS_BAD_REQUEST);
  up.length = sizeof(data);
  up.data = NULL;
  CHECK(zz9k_voice_upload(ctx, &s, &up) == ZZ9K_STATUS_BAD_REQUEST);
  CHECK(g_mock.log_count == 0U);

  CHECK(zz9k_voice_close(ctx, &s) == ZZ9K_STATUS_OK);
  mock_detach(ctx);
}

static void put_read_cursor(ZZ9KVoiceSession *s, uint16_t value)
{
  zz9k_put_be16((volatile uint8_t *)s->status + 8, value);
}

static void test_write_bounded_by_free_space(void)
{
  ZZ9KContext *ctx = mock_attach(3U);
  ZZ9KVoiceOpenDesc desc;
  ZZ9KVoiceSession s;
  ZZ9KVoiceRecord recs[100];
  ZZ9KVoiceRecordWire wire;
  uint32_t i;

  CHECK(ctx != NULL);
  if (!ctx) {
    return;
  }
  default_desc(&desc);
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_OK);
  CHECK(s.grant.ring_records == 64U);
  for (i = 0U; i < 100U; i++) {
    zz9k_voice_build_start(&recs[i], 1000U + i, (uint8_t)(i & 31U), 5U, i,
                           2U * i);
  }

  /* 100 records into a 64-record ring: exactly the free space. */
  CHECK(zz9k_voice_write(&s, recs, 100U) == 64U);
  CHECK(zz9k_get_be16(s.ring) == 64U);
  for (i = 0U; i < 64U; i++) {
    zz9k_voice_record_encode(&wire, &recs[i]);
    CHECK(memcmp((const void *)(s.ring + 16U + i * 16U), &wire, 16U) == 0);
  }
  /* Full: zero written and the cursor does not move. */
  CHECK(zz9k_voice_write(&s, recs, 1U) == 0U);
  CHECK(zz9k_get_be16(s.ring) == 64U);

  /* The firmware takes 10: ten more fit, and they wrap onto the oldest
   * slots; the 11th does not. */
  put_read_cursor(&s, 10U);
  CHECK(zz9k_voice_write(&s, &recs[64], 36U) == 10U);
  CHECK(zz9k_get_be16(s.ring) == 74U);
  for (i = 0U; i < 10U; i++) {
    zz9k_voice_record_encode(&wire, &recs[64U + i]);
    CHECK(memcmp((const void *)(s.ring + 16U + i * 16U), &wire, 16U) == 0);
  }
  CHECK(zz9k_voice_write(&s, &recs[74], 1U) == 0U);

  /* The cursors wrap at 65536: park both just below it. */
  s.write_cursor = 65530U;
  zz9k_voice_ring_publish(&s);
  put_read_cursor(&s, 65530U);
  CHECK(zz9k_voice_write(&s, recs, 20U) == 20U);
  CHECK(zz9k_get_be16(s.ring) == (uint16_t)(65530U + 20U));
  CHECK(zz9k_get_be16(s.ring) == 14U);

  /* Bad arguments write nothing. */
  CHECK(zz9k_voice_write(&s, recs, 0U) == 0U);
  CHECK(zz9k_voice_write(&s, NULL, 4U) == 0U);
  CHECK(zz9k_voice_write(NULL, recs, 4U) == 0U);

  put_read_cursor(&s, 14U);
  CHECK(zz9k_voice_close(ctx, &s) == ZZ9K_STATUS_OK);
  CHECK(zz9k_voice_write(&s, recs, 1U) == 0U); /* closed session */
  mock_detach(ctx);
}

static void test_status_and_heartbeat(void)
{
  ZZ9KContext *ctx = mock_attach(3U);
  ZZ9KVoiceOpenDesc desc;
  ZZ9KVoiceSession s;
  ZZ9KVoiceStatus st;

  CHECK(ctx != NULL);
  if (!ctx) {
    return;
  }
  default_desc(&desc);
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_OK);

  zz9k_put_be32((volatile uint8_t *)s.status + 0, 0x00001000UL);
  zz9k_put_be32((volatile uint8_t *)s.status + 4, 0x00003000UL);
  zz9k_put_be16((volatile uint8_t *)s.status + 8, 9U);
  zz9k_put_be16((volatile uint8_t *)s.status + 10, ZZ9K_VOICE_STATE_LIVE);
  zz9k_put_be32((volatile uint8_t *)s.status + 12, 1U);
  zz9k_put_be32((volatile uint8_t *)s.status + 16, 2U);
  zz9k_put_be32((volatile uint8_t *)s.status + 20, 3U);
  zz9k_put_be32((volatile uint8_t *)s.status + 24, 4U);
  zz9k_put_be32((volatile uint8_t *)s.status + 28, s.grant.session);

  CHECK(zz9k_get_be32(s.ring + 4) == 1U);
  CHECK(zz9k_voice_status(&s, &st) == ZZ9K_STATUS_OK);
  CHECK(st.playing_frame == 0x1000UL && st.horizon == 0x3000UL);
  CHECK(st.read_cursor == 9U && st.state == ZZ9K_VOICE_STATE_LIVE);
  CHECK(st.late == 1U && st.rejected == 2U && st.underruns == 3U &&
        st.dropped == 4U && st.session == s.grant.session);
  /* Each status call bumps the heartbeat word in the ring header. */
  CHECK(zz9k_get_be32(s.ring + 4) == 2U);
  CHECK(zz9k_voice_status(&s, &st) == ZZ9K_STATUS_OK);
  CHECK(zz9k_get_be32(s.ring + 4) == 3U);

  CHECK(zz9k_voice_status(&s, NULL) == ZZ9K_STATUS_BAD_REQUEST);
  CHECK(zz9k_voice_status(NULL, &st) == ZZ9K_STATUS_BAD_REQUEST);
  CHECK(zz9k_voice_close(ctx, &s) == ZZ9K_STATUS_OK);
  mock_detach(ctx);
}

static void test_reset_and_close(void)
{
  ZZ9KContext *ctx = mock_attach(3U);
  ZZ9KVoiceOpenDesc desc;
  ZZ9KVoiceSession s;
  uint32_t session;

  CHECK(ctx != NULL);
  if (!ctx) {
    return;
  }
  default_desc(&desc);
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_OK);
  session = s.grant.session;
  g_mock.log_count = 0U;
  CHECK(zz9k_voice_reset(ctx, &s) == ZZ9K_STATUS_OK);
  CHECK(g_mock.log_count == 1U);
  CHECK(g_mock.log[0].opcode == ZZ9K_OP_AUDIO_VOICE_RESET);
  CHECK(zz9k_get_be32(g_mock.log[0].payload) == session);
  CHECK(s.mapped == 1U); /* a reset keeps the session */

  CHECK(zz9k_voice_close(ctx, &s) == ZZ9K_STATUS_OK);
  CHECK(g_mock.log[1].opcode == ZZ9K_OP_AUDIO_VOICE_CLOSE);
  CHECK(zz9k_get_be32(g_mock.log[1].payload) == session);
  CHECK(mock_live_buffers() == 0);
  CHECK(g_mock.frees == 3);
  CHECK(s.mapped == 0U);
  /* A closed session is dead to every call. */
  CHECK(zz9k_voice_reset(ctx, &s) == ZZ9K_STATUS_BAD_REQUEST);
  CHECK(zz9k_voice_close(ctx, &s) == ZZ9K_STATUS_BAD_REQUEST);

  /* A stale session (the firmware already closed it) still frees the
   * buffers and reports the firmware's answer. */
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_OK);
  mock_unpin();
  CHECK(zz9k_voice_close(ctx, &s) == ZZ9K_STATUS_BAD_HANDLE);
  CHECK(mock_live_buffers() == 0);
  CHECK(s.mapped == 0U);
  mock_detach(ctx);
}

/* A mailbox call the way ZZ9KCall issues it: submit, then collect the
 * completion. The mock answers between the two. */
static int submit_and_collect(ZZ9KContext *ctx, ZZ9KRequest *request,
                              ZZ9KMailboxEntry *reply)
{
  uint32_t request_id = 0U;
  int status = zz9k_submit(ctx, request, &request_id);

  if (status != ZZ9K_STATUS_QUEUED) {
    return status;
  }
  mock_respond();
  status = zz9k_poll(ctx, reply);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  return reply->status;
}

static int submit_define(ZZ9KContext *ctx, const ZZ9KVoiceSession *s)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;

  memset(&reply, 0, sizeof(reply));
  if (zz9k_request_audio_voice_upload(&request, s->grant.session,
                                      ZZ9K_VOICE_UPLOAD_FLAG_DEFINE, 0U, 0U,
                                      0U, 1U, 0U, 16U,
                                      ZZ9K_VOICE_FORMAT_S16) !=
      ZZ9K_STATUS_OK) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  return submit_and_collect(ctx, &request, &reply);
}

/* Every request a context sends, through zz9k_call or zz9k_submit, carries
 * the context's client id: the Voice API calls own the session and its
 * buffers under that id, so a ZZ9KCall-style submit on the same session is
 * accepted. */
static void test_client_id_on_voice_calls(void)
{
  ZZ9KContext *ctx = mock_attach(3U);
  ZZ9KVoiceOpenDesc desc;
  ZZ9KVoiceSession s;
  uint32_t i;

  CHECK(ctx != NULL);
  if (!ctx) {
    return;
  }
  CHECK(zz9k_set_client_id(ctx, 7U) == ZZ9K_STATUS_OK);
  default_desc(&desc);
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_OK);
  CHECK(g_mock.buf_count == 3U);
  for (i = 0U; i < g_mock.buf_count; i++) {
    CHECK(g_mock.bufs[i].owner == 7U);
  }
  CHECK(g_mock.session_open == 1);
  CHECK(g_mock.session_owner == 7U);
  CHECK(g_mock.log_count == 1U);
  CHECK(g_mock.log[0].opcode == ZZ9K_OP_AUDIO_VOICE_OPEN);
  CHECK(g_mock.log[0].client == 7U);

  /* A submit on the session the Voice API opened. */
  CHECK(submit_define(ctx, &s) == ZZ9K_STATUS_OK);
  CHECK(g_mock.defines == 1U);
  /* And the Voice API on the same session after the submit. */
  CHECK(zz9k_voice_reset(ctx, &s) == ZZ9K_STATUS_OK);
  CHECK(zz9k_voice_close(ctx, &s) == ZZ9K_STATUS_OK);
  CHECK(g_mock.session_open == 0);
  CHECK(mock_live_buffers() == 0);
  for (i = 0U; i < g_mock.log_count; i++) {
    CHECK(g_mock.log[i].client == 7U);
  }

  /* Another client sees no handle in the session id. */
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_OK);
  CHECK(zz9k_set_client_id(ctx, 8U) == ZZ9K_STATUS_OK);
  CHECK(submit_define(ctx, &s) == ZZ9K_STATUS_BAD_HANDLE);
  CHECK(zz9k_set_client_id(ctx, 7U) == ZZ9K_STATUS_OK);
  CHECK(zz9k_voice_close(ctx, &s) == ZZ9K_STATUS_OK);
  mock_detach(ctx);
}

/* A context with no client id sends 0 on every path, as the AHI voice worker
 * does, and its session is owned by client 0. */
static void test_client_id_zero_unchanged(void)
{
  ZZ9KContext *ctx = mock_attach(3U);
  ZZ9KVoiceOpenDesc desc;
  ZZ9KVoiceSession s;
  uint32_t i;

  CHECK(ctx != NULL);
  if (!ctx) {
    return;
  }
  default_desc(&desc);
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_OK);
  CHECK(g_mock.session_owner == 0U);
  CHECK(submit_define(ctx, &s) == ZZ9K_STATUS_OK);
  CHECK(zz9k_voice_close(ctx, &s) == ZZ9K_STATUS_OK);
  for (i = 0U; i < g_mock.log_count; i++) {
    CHECK(g_mock.log[i].client == 0U);
  }
  mock_detach(ctx);
}

/* CLIENT_RELEASE for the opener's id ends its voice session and frees its
 * buffers; another client's release leaves them alone. */
static void test_client_release_closes_voice_session(void)
{
  ZZ9KContext *ctx = mock_attach(3U);
  ZZ9KVoiceOpenDesc desc;
  ZZ9KVoiceSession s;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;

  CHECK(ctx != NULL);
  if (!ctx) {
    return;
  }
  CHECK(zz9k_set_client_id(ctx, 9U) == ZZ9K_STATUS_OK);
  default_desc(&desc);
  CHECK(zz9k_voice_open(ctx, &desc, &s) == ZZ9K_STATUS_OK);
  CHECK(g_mock.session_open == 1);

  zz9k_request_init(&request, ZZ9K_OP_CLIENT_RELEASE);
  CHECK(zz9k_set_client_id(ctx, 10U) == ZZ9K_STATUS_OK);
  CHECK(zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS) ==
        ZZ9K_STATUS_OK);
  CHECK(g_mock.session_open == 1);
  CHECK(mock_live_buffers() == 3);

  zz9k_request_init(&request, ZZ9K_OP_CLIENT_RELEASE);
  CHECK(zz9k_set_client_id(ctx, 9U) == ZZ9K_STATUS_OK);
  CHECK(zz9k_call(ctx, &request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS) ==
        ZZ9K_STATUS_OK);
  CHECK(g_mock.release_calls == 2U);
  CHECK(g_mock.session_open == 0);
  CHECK(mock_live_buffers() == 0);
  mock_detach(ctx);
}

int main(void)
{
  g_window_size = 0x00400000UL;
  g_window = mock_window_alloc(g_window_size);
  if (!g_window) {
    printf("voice_contract_test: cannot map a low board window\n");
    return 2;
  }
  g_window_addr = (uint32_t)(uintptr_t)g_window;

  test_stereo_formats();
  test_stereo_flag();
  test_rate_step();
  test_pass_frames_fixed();
  test_pass_frames_vs_model();
  test_layouts();
  test_records();
  test_status_block_read();
  test_jitter_read();
  test_ext_status_read();
  test_read32_tear();
  test_ring_math();
  test_stage_then_publish();
  test_request_builders();
  test_open_reply_decoder();

  test_open_success(2U);
  test_open_success(3U);
  test_open_low_latency();
  test_open_caller_sizes();
  test_open_refused_frees_everything();
  test_open_partial_allocation_failure();
  test_open_bad_grant_closes_and_frees();
  test_open_rejects_bad_desc_before_allocating();
  test_upload_chunks_then_one_define();
  test_upload_small_staging_and_failures();
  test_write_bounded_by_free_space();
  test_status_and_heartbeat();
  test_reset_and_close();
  test_client_id_on_voice_calls();
  test_client_id_zero_unchanged();
  test_client_release_closes_voice_session();

  mock_window_free(g_window, g_window_size);
  if (g_failures) {
    printf("voice_contract_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  printf("voice_contract_test: all contract invariants verified\n");
  return 0;
}
