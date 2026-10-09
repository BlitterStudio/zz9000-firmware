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

/* The descriptor hook runs zz9k_sound_audio_start on the first buffer and
 * zz9k_sound_mp3_pair_at at the audio start; unlike the class scan it must
 * not claim a stream whose first frame is not exactly at that start. */
static void test_hook_recognition(void)
{
  static const uint8_t mpeg2_header[4] = {0xffU, 0xf3U, 0x80U, 0x00U};
  uint8_t stream[TEST_STREAM_BYTES + 1U];
  uint8_t tag[10];
  ZZ9KSoundMp3Envelope envelope;
  uint32_t start;

  make_two_frame_mp3(stream, 0U, TEST_STREAM_BYTES);
  expect_true(zz9k_sound_mp3_pair_at(stream, TEST_STREAM_BYTES, 0),
              "hook accepts a frame pair at the audio start");
  make_two_frame_mp3(stream, 1U, sizeof(stream));
  expect_true(!zz9k_sound_mp3_pair_at(stream, sizeof(stream), 0) &&
                  zz9k_sound_recognize_mp3(stream, sizeof(stream), 0),
              "hook rejects a displaced first frame the class scan accepts");

  memset(tag, 0, sizeof(tag));
  memcpy(tag, "ID3", 3U);
  tag[3] = 4U;
  tag[5] = 0x10U;
  tag[8] = 1U;
  tag[9] = 2U;
  expect_true(zz9k_sound_audio_start(tag, sizeof(tag), &start) &&
                  start == 10U + 130U + 10U,
              "audio start skips a footer-flagged synchsafe ID3v2 tag");
  expect_true(zz9k_sound_audio_start(stream + 1U, 4U, &start) && start == 0U,
              "untagged data starts at byte zero");
  expect_true(!zz9k_sound_audio_start(tag, 9U, &start),
              "a truncated ID3v2 header is rejected");

  expect_true(zz9k_sound_mp3_header(mpeg2_header, 4U, &envelope) &&
                  envelope.frame_bytes == 208U &&
                  envelope.sample_rate == 22050U &&
                  envelope.samples_per_frame == 576U,
              "MPEG-2 Layer III 64 kbit/s 22050 Hz frame is 208 bytes");
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

/* Builds the 42-byte native FLAC prefix: marker, last-block STREAMINFO
 * header, 4096-sample blocks, then rate/channels/bits as packed by
 * STREAMINFO (20-bit rate, 3-bit channels-1, 5-bit bits-1). */
static void set_flac_header(uint8_t *bytes, uint32_t rate,
                            uint32_t channels, uint32_t bits)
{
  memset(bytes, 0, ZZ9K_SOUND_FLAC_HEADER_BYTES);
  memcpy(bytes, "fLaC", 4U);
  bytes[4] = 0x80U;
  bytes[7] = ZZ9K_SOUND_FLAC_STREAMINFO_BYTES;
  bytes[8] = 0x10U;
  bytes[10] = 0x10U;
  bytes[18] = (uint8_t)(rate >> 12);
  bytes[19] = (uint8_t)(rate >> 4);
  bytes[20] = (uint8_t)(((rate & 0x0fU) << 4) | ((channels - 1U) << 1) |
                        ((bits - 1U) >> 4));
  bytes[21] = (uint8_t)(((bits - 1U) & 0x0fU) << 4);
  bytes[25] = 0x10U;
}

static void test_flac_recognition(void)
{
  uint8_t header[ZZ9K_SOUND_FLAC_HEADER_BYTES];
  ZZ9KSoundFlacEnvelope envelope;

  set_flac_header(header, 44100U, 2U, 16U);
  memset(&envelope, 0, sizeof(envelope));
  expect_true(zz9k_sound_recognize_flac(header, sizeof(header), &envelope) &&
                  envelope.sample_rate == 44100U && envelope.channels == 2U &&
                  envelope.bits_per_sample == 16U &&
                  envelope.total_samples == 16U,
              "16-bit stereo FLAC STREAMINFO is decoded");
  set_flac_header(header, 96000U, 1U, 24U);
  expect_true(zz9k_sound_recognize_flac(header, sizeof(header), &envelope) &&
                  envelope.sample_rate == 96000U && envelope.channels == 1U &&
                  envelope.bits_per_sample == 24U,
              "24-bit mono FLAC STREAMINFO is decoded");
  expect_true(!zz9k_sound_recognize_flac(header, sizeof(header) - 1U, 0),
              "a truncated STREAMINFO is rejected");
  set_flac_header(header, 44100U, 3U, 16U);
  expect_true(!zz9k_sound_recognize_flac(header, sizeof(header), 0),
              "multichannel FLAC is not claimed");
  set_flac_header(header, 44100U, 2U, 32U);
  expect_true(!zz9k_sound_recognize_flac(header, sizeof(header), 0),
              "FLAC wider than 24 bits is not claimed");
  set_flac_header(header, 7999U, 2U, 16U);
  expect_true(!zz9k_sound_recognize_flac(header, sizeof(header), 0),
              "rates below the decoder floor are not claimed");
  set_flac_header(header, 192001U, 2U, 16U);
  expect_true(!zz9k_sound_recognize_flac(header, sizeof(header), 0),
              "rates above the decoder ceiling are not claimed");
  set_flac_header(header, 44100U, 2U, 16U);
  header[4] = 0x84U;
  expect_true(!zz9k_sound_recognize_flac(header, sizeof(header), 0),
              "a first block other than STREAMINFO is rejected");
  set_flac_header(header, 44100U, 2U, 16U);
  memcpy(header, "OggS", 4U);
  expect_true(!zz9k_sound_recognize_flac(header, sizeof(header), 0),
              "Ogg-FLAC is not claimed as native FLAC");
}

/* First page of a libvorbis (ffmpeg) stereo 44.1 kHz stream, verbatim: the
 * CRC was written by an independent encoder. */
static const uint8_t vorbis_first_page[ZZ9K_SOUND_VORBIS_HEADER_BYTES] = {
  0x4f, 0x67, 0x67, 0x53, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x12, 0xea, 0x84, 0x19, 0x00, 0x00, 0x00, 0x00, 0xa6, 0xe9,
  0x8d, 0xe1, 0x01, 0x1e, 0x01, 0x76, 0x6f, 0x72, 0x62, 0x69, 0x73, 0x00,
  0x00, 0x00, 0x00, 0x02, 0x44, 0xac, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x80, 0xb5, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb8, 0x01
};

static void vorbis_reseal(uint8_t *page)
{
  uint32_t crc = zz9k_sound_ogg_crc(page, ZZ9K_SOUND_VORBIS_HEADER_BYTES);
  page[22] = (uint8_t)crc;
  page[23] = (uint8_t)(crc >> 8);
  page[24] = (uint8_t)(crc >> 16);
  page[25] = (uint8_t)(crc >> 24);
}

static void test_vorbis_recognition(void)
{
  /* First page of an ffmpeg libopus stream, zero-padded to the same size. */
  static const uint8_t opus_first_page[ZZ9K_SOUND_VORBIS_HEADER_BYTES] = {
    0x4f, 0x67, 0x67, 0x53, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x07, 0x61, 0x23, 0x34, 0x00, 0x00, 0x00, 0x00, 0x04, 0x73,
    0x33, 0x25, 0x01, 0x13, 0x4f, 0x70, 0x75, 0x73, 0x48, 0x65, 0x61, 0x64,
    0x01, 0x01, 0x38, 0x01, 0x80, 0xbb, 0x00, 0x00, 0x00, 0x00, 0x00
  };
  uint8_t page[ZZ9K_SOUND_VORBIS_HEADER_BYTES];
  ZZ9KSoundVorbisEnvelope envelope;

  memset(&envelope, 0, sizeof(envelope));
  expect_true(zz9k_sound_recognize_vorbis(vorbis_first_page, sizeof(page),
                                          &envelope) &&
                  envelope.sample_rate == 44100U && envelope.channels == 2U &&
                  envelope.serial == 0x1984ea12UL &&
                  envelope.nominal_bitrate == 112000U &&
                  envelope.max_block_samples == 2048U,
              "a real Vorbis identification page is recognized");
  expect_true(!zz9k_sound_recognize_vorbis(vorbis_first_page,
                                           sizeof(page) - 1U, 0),
              "a truncated identification page is rejected");
  memcpy(page, vorbis_first_page, sizeof(page));
  page[40] ^= 0x01U;
  expect_true(!zz9k_sound_recognize_vorbis(page, sizeof(page), 0),
              "a page whose CRC does not match is rejected");
  vorbis_reseal(page);
  expect_true(zz9k_sound_recognize_vorbis(page, sizeof(page), &envelope) &&
                  envelope.sample_rate == 44101U,
              "resealing restores the CRC over the changed rate");
  memcpy(page, vorbis_first_page, sizeof(page));
  page[28 + 11] = 3U;
  vorbis_reseal(page);
  expect_true(!zz9k_sound_recognize_vorbis(page, sizeof(page), 0),
              "multichannel Vorbis is not claimed");
  memcpy(page, vorbis_first_page, sizeof(page));
  page[5] = 0x00U;
  vorbis_reseal(page);
  expect_true(!zz9k_sound_recognize_vorbis(page, sizeof(page), 0),
              "a page without the BOS flag is not a stream start");
  memcpy(page, vorbis_first_page, sizeof(page));
  page[28 + 28] = 0x5bU; /* large block 2^5 < small block 2^11 */
  vorbis_reseal(page);
  expect_true(!zz9k_sound_recognize_vorbis(page, sizeof(page), 0),
              "invalid Vorbis block sizes are rejected");
  memcpy(page, vorbis_first_page, sizeof(page));
  page[28 + 29] = 0x00U;
  vorbis_reseal(page);
  expect_true(!zz9k_sound_recognize_vorbis(page, sizeof(page), 0),
              "an identification packet without its framing bit is rejected");
  memcpy(page, vorbis_first_page, sizeof(page));
  page[28 + 12] = 0x3fU; /* 7999 Hz */
  page[28 + 13] = 0x1fU;
  vorbis_reseal(page);
  expect_true(!zz9k_sound_recognize_vorbis(page, sizeof(page), 0),
              "rates below the decoder floor are not claimed");
  expect_true(!zz9k_sound_recognize_vorbis(opus_first_page,
                                           sizeof(opus_first_page), 0),
              "Ogg Opus is not claimed as Vorbis");
}

static void test_wide_legacy_conversion(void)
{
  /* MSB-justified S32BE stereo: left -1.0, right just under +1.0. */
  static const uint8_t s32_stereo[8] = {
    0x80U, 0x00U, 0x12U, 0x00U, 0x7fU, 0xffU, 0xffU, 0x00U
  };
  static const uint8_t s32_mono[4] = {0x40U, 0x01U, 0xabU, 0x00U};
  uint8_t top[4];

  zz9k_sound_frame_top16(s32_stereo, 2U, 4U, top);
  expect_true(top[0] == 0x80U && top[1] == 0x00U && top[2] == 0x7fU &&
                  top[3] == 0xffU,
              "S32 frames keep each channel's top 16 bits for legacy output");
  expect_true(zz9k_sound_legacy_quantize_s16be(top, 2U) == 0,
              "wide stereo is averaged after narrowing");
  zz9k_sound_frame_top16(s32_mono, 1U, 4U, top);
  expect_true(zz9k_sound_legacy_quantize_s16be(top, 1U) == 64,
              "wide mono narrows to the same 8-bit sample as S16");
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
  state.input_handle = 11U;
  state.pcm_handle = 12U;
  state.staging_handle = 13U;
  state.sample = &sample;
  state.session_open = 1U;
  state.input_allocated = 1U;
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
                  !state.input_allocated && !state.pcm_allocated &&
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

  /* A modern stereo sample owns two separate AllocVec planes: both are
   * freed once before publication and neither after it. */
  {
    uint8_t right;
    memset(&state, 0, sizeof(state));
    memset(&trace, 0, sizeof(trace));
    state.sample = &sample;
    state.right_sample = &right;
    zz9k_sound_cleanup_owned(&state, &ops, &trace);
    expect_true(trace.count == 2U && trace.sample == &right &&
                    state.sample == 0 && state.right_sample == 0,
                "unpublished stereo cleanup frees both channel planes once");
    zz9k_sound_cleanup_owned(&state, &ops, &trace);
    expect_true(trace.count == 2U, "stereo plane cleanup is idempotent");
    memset(&trace, 0, sizeof(trace));
    state.sample = &sample;
    state.right_sample = &right;
    state.sample_published = 1U;
    zz9k_sound_cleanup_owned(&state, &ops, &trace);
    expect_true(trace.count == 0U && state.right_sample == &right,
                "published stereo planes remain owned by sound.datatype");
  }
}

int main(void)
{
  test_recognition();
  test_hook_recognition();
  test_legacy_conversion();
  test_flac_recognition();
  test_vorbis_recognition();
  test_wide_legacy_conversion();
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
