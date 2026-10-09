/*
 * Actual-source contract harness for the ZZ9000 MP3 sound DataType.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#ifndef __amigaos__
#include <stdio.h>
#endif
#include <string.h>
#ifdef __amigaos__
void *SysBase;

void *memset(void *dst, int value, size_t bytes)
{
  uint8_t *out = (uint8_t *)dst;
  while (bytes-- != 0U) {
    *out++ = (uint8_t)value;
  }
  return dst;
}

void *memcpy(void *dst, const void *src, size_t bytes)
{
  uint8_t *out = (uint8_t *)dst;
  const uint8_t *in = (const uint8_t *)src;
  while (bytes-- != 0U) {
    *out++ = *in++;
  }
  return dst;
}

static uint32_t test_divmod_u32(uint32_t dividend, uint32_t divisor,
                                uint32_t *remainder)
{
  uint32_t quotient = 0U;
  uint32_t bit = 1U;
  uint32_t shifted = divisor;
  if (divisor == 0U) {
    if (remainder) {
      *remainder = dividend;
    }
    return 0U;
  }
  while (shifted <= (dividend >> 1) && shifted <= 0x7fffffffUL) {
    shifted <<= 1;
    bit <<= 1;
  }
  while (bit != 0U) {
    if (dividend >= shifted) {
      dividend -= shifted;
      quotient |= bit;
    }
    shifted >>= 1;
    bit >>= 1;
  }
  if (remainder) {
    *remainder = dividend;
  }
  return quotient;
}

uint32_t __udivsi3(uint32_t dividend, uint32_t divisor)
{
  return test_divmod_u32(dividend, divisor, 0);
}

uint32_t __umodsi3(uint32_t dividend, uint32_t divisor)
{
  uint32_t remainder;
  (void)test_divmod_u32(dividend, divisor, &remainder);
  return remainder;
}
#endif

#define ZZ9K_SOUND_DATATYPE_TEST 1
#include "../amiga/datatypes/zz9k_sound_datatype.c"

#define TEST_FRAME_BYTES 417U
#define TEST_STREAM_BYTES (TEST_FRAME_BYTES * 2U)

typedef struct CleanupTrace {
  uint32_t events[8];
  uint32_t count;
  void *sample;
} CleanupTrace;

static int failures;

static void expect_true(int condition, const char *message)
{
  if (!condition) {
#ifndef __amigaos__
    printf("FAIL: %s\n", message);
#else
    (void)message;
#endif
    failures++;
  }
}

static void set_mpeg1_layer3_header(uint8_t *bytes)
{
  bytes[0] = 0xffU;
  bytes[1] = 0xfbU;
  bytes[2] = 0x90U;
  bytes[3] = 0x64U;
}

static void make_two_frame_mp3(uint8_t *bytes, uint32_t offset,
                               uint32_t total)
{
  memset(bytes, 0, total);
  set_mpeg1_layer3_header(bytes + offset);
  set_mpeg1_layer3_header(bytes + offset + TEST_FRAME_BYTES);
}

static void test_recognition(void)
{
  uint8_t plain[TEST_STREAM_BYTES];
  uint8_t tagged[10U + TEST_STREAM_BYTES + 128U];
  uint8_t invalid[TEST_STREAM_BYTES];
  uint8_t id3_only[10U];
  ZZ9KSoundMp3Envelope envelope;

  make_two_frame_mp3(plain, 0U, sizeof(plain));
  memset(&envelope, 0, sizeof(envelope));
  expect_true(zz9k_sound_recognize_mp3(plain, sizeof(plain), &envelope),
              "two matching Layer III frames are recognized");
  expect_true(envelope.first_frame == 0U,
              "plain stream starts at the first byte");
  expect_true(envelope.frame_bytes == TEST_FRAME_BYTES,
              "MPEG-1 Layer III frame size is parsed");
  expect_true(envelope.sample_rate == 44100U && envelope.channels == 2U &&
                  envelope.samples_per_frame == 1152U,
              "MPEG-1 Layer III audio envelope is reported");

  make_two_frame_mp3(tagged, 10U, sizeof(tagged));
  tagged[0] = 'I';
  tagged[1] = 'D';
  tagged[2] = '3';
  tagged[3] = 4U;
  tagged[4] = 0U;
  tagged[5] = 0U;
  tagged[6] = tagged[7] = tagged[8] = tagged[9] = 0U;
  memcpy(tagged + sizeof(tagged) - 128U, "TAG", 3U);
  memset(&envelope, 0, sizeof(envelope));
  expect_true(zz9k_sound_recognize_mp3(tagged, sizeof(tagged), &envelope),
              "bounded ID3v2 prefix and ID3v1 suffix preserve recognition");
  expect_true(envelope.first_frame == 10U,
              "recognition skips the complete ID3v2 header");

  memcpy(invalid, plain, sizeof(invalid));
  invalid[1] = 0xfdU;
  invalid[TEST_FRAME_BYTES + 1U] = 0xfdU;
  expect_true(!zz9k_sound_recognize_mp3(invalid, sizeof(invalid), 0),
              "Layer II frames are rejected");
  invalid[1] = 0xffU;
  invalid[TEST_FRAME_BYTES + 1U] = 0xffU;
  expect_true(!zz9k_sound_recognize_mp3(invalid, sizeof(invalid), 0),
              "Layer I frames are rejected");

  memset(id3_only, 0, sizeof(id3_only));
  memcpy(id3_only, "ID3", 3U);
  id3_only[3] = 4U;
  expect_true(!zz9k_sound_recognize_mp3(id3_only, sizeof(id3_only), 0),
              "an arbitrary ID3 header is not accepted as MP3");
  id3_only[6] = 0x80U;
  expect_true(!zz9k_sound_recognize_mp3(id3_only, sizeof(id3_only), 0),
              "non-synchsafe ID3v2 sizes are rejected");
  id3_only[6] = 8U;
  id3_only[7] = id3_only[8] = 0U;
  id3_only[9] = 1U;
  expect_true(!zz9k_sound_recognize_mp3(id3_only, sizeof(id3_only), 0),
              "ID3v2 bodies beyond the recognition ceiling are rejected");

  memset(invalid, 0, sizeof(invalid));
  set_mpeg1_layer3_header(invalid);
  expect_true(!zz9k_sound_recognize_mp3(invalid, sizeof(invalid), 0),
              "one plausible sync word is not a positive MP3 envelope");
}

static void test_legacy_conversion(void)
{
  static const uint8_t negative_full_scale[2] = {0x80U, 0x00U};
  static const uint8_t positive_full_scale[2] = {0x7fU, 0xffU};
  static const uint8_t cancelling_stereo[4] = {
    0x80U, 0x00U, 0x7fU, 0xffU
  };
  static const uint8_t equal_stereo[4] = {
    0x40U, 0x00U, 0x40U, 0x00U
  };

  expect_true(zz9k_sound_legacy_quantize_s16be(negative_full_scale, 1U) ==
                  -128,
              "legacy mono conversion preserves negative full scale");
  expect_true(zz9k_sound_legacy_quantize_s16be(positive_full_scale, 1U) ==
                  127,
              "legacy mono conversion saturates positive full scale");
  expect_true(zz9k_sound_legacy_quantize_s16be(cancelling_stereo, 2U) == 0,
              "legacy stereo conversion averages before S16-to-S8 quantizing");
  expect_true(zz9k_sound_legacy_quantize_s16be(equal_stereo, 2U) == 64,
              "legacy stereo conversion emits one signed mono sample");
  expect_true(zz9k_sound_legacy_quantize_s16be(equal_stereo, 3U) == 0,
              "legacy conversion rejects unsupported channel counts");
}

static void test_growth(void)
{
  uint32_t next;

  next = 0U;
  expect_true(zz9k_sound_next_capacity(0U, 1U, 256U * 1024U, &next) &&
                  next == 64U * 1024U,
              "empty sample storage starts at the bounded initial capacity");
  expect_true(zz9k_sound_next_capacity(64U * 1024U,
                                       64U * 1024U + 1U,
                                       256U * 1024U, &next) &&
                  next == 128U * 1024U,
              "sample storage grows geometrically");
  expect_true(zz9k_sound_next_capacity(200U * 1024U,
                                       250U * 1024U,
                                       256U * 1024U, &next) &&
                  next == 256U * 1024U,
              "growth clamps at the explicit allocation ceiling");
  next = 123U;
  expect_true(!zz9k_sound_next_capacity(64U, 257U, 256U, &next) &&
                  next == 123U,
              "growth rejects requirements beyond the ceiling");
  expect_true(!zz9k_sound_next_capacity(1U, 1U, 1U, 0),
              "growth requires an output destination");
}

static void trace_close(void *ctx, uint32_t session)
{
  CleanupTrace *trace = (CleanupTrace *)ctx;
  trace->events[trace->count++] = 0x10000000UL | session;
}

static void trace_free_shared(void *ctx, uint32_t handle)
{
  CleanupTrace *trace = (CleanupTrace *)ctx;
  trace->events[trace->count++] = 0x20000000UL | handle;
}

static void trace_free_sample(void *ctx, void *sample)
{
  CleanupTrace *trace = (CleanupTrace *)ctx;
  trace->events[trace->count++] = 0x30000000UL;
  trace->sample = sample;
}

static void test_cleanup(void)
{
  static const ZZ9KSoundCleanupOps ops = {
    trace_close, trace_free_shared, trace_free_sample
  };
  ZZ9KSoundOwnedState state;
  CleanupTrace trace;
  uint8_t sample;

  memset(&state, 0, sizeof(state));
  memset(&trace, 0, sizeof(trace));
  state.session = 7U;
  state.mp3_handle = 11U;
  state.pcm_handle = 12U;
  state.staging_handle = 13U;
  state.sample = &sample;
  state.session_open = 1U;
  state.mp3_allocated = 1U;
  state.pcm_allocated = 1U;
  state.staging_allocated = 1U;

  zz9k_sound_cleanup_owned(&state, &ops, &trace);
  expect_true(trace.count == 5U,
              "failure cleanup releases every owned resource exactly once");
  expect_true(trace.events[0] == (0x10000000UL | 7U) &&
                  trace.events[1] == (0x20000000UL | 13U) &&
                  trace.events[2] == (0x20000000UL | 12U) &&
                  trace.events[3] == (0x20000000UL | 11U) &&
                  trace.events[4] == 0x30000000UL,
              "cleanup closes the session before reverse-order resource frees");
  expect_true(trace.sample == &sample && !state.session_open &&
                  !state.mp3_allocated && !state.pcm_allocated &&
                  !state.staging_allocated && state.sample == 0,
              "cleanup clears ownership after releasing resources");
  zz9k_sound_cleanup_owned(&state, &ops, &trace);
  expect_true(trace.count == 5U, "cleanup is idempotent after a partial failure");

  memset(&state, 0, sizeof(state));
  memset(&trace, 0, sizeof(trace));
  state.sample = &sample;
  state.sample_published = 1U;
  zz9k_sound_cleanup_owned(&state, &ops, &trace);
  expect_true(trace.count == 0U && state.sample == &sample,
              "published sample ownership remains with sound.datatype");
}

int main(void)
{
  test_recognition();
  test_legacy_conversion();
  test_growth();
  test_cleanup();

  if (failures != 0) {
#ifndef __amigaos__
    printf("%d sound datatype contract assertion(s) failed\n", failures);
#endif
    return 1;
  }
  return 0;
}
