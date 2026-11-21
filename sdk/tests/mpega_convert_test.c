/*
 * Host tests of the ZZ9000 mpega.library PCM code (amiga/mpega/mpega_convert.c
 * is compiled into this test as it is built for the Amiga).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../amiga/mpega/mpega_convert.h"
#include "zz9k/abi.h"

static int failures;

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      printf("%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);  \
      failures++;                                                           \
    }                                                                       \
  } while (0)

/* ---------------------------------------------------------------- ring */

static void test_ring_read(void)
{
  enum { ring_length = 37 };
  static const uint32_t lengths[] = {0, 1, 2, 3, 4, 5, 8, 16, 36, 37};
  uint32_t ring_words[(ring_length + 4 + 3) / 4 + 1];
  uint32_t dst_words[(ring_length + 8) / 4 + 1];
  uint32_t skew;
  uint32_t dst_skew;
  uint32_t offset;
  size_t l;

  for (skew = 0; skew < 4; skew++) {
    for (dst_skew = 0; dst_skew < 4; dst_skew++) {
      for (offset = 0; offset < ring_length; offset++) {
        for (l = 0; l < sizeof(lengths) / sizeof(lengths[0]); l++) {
          uint8_t *ring = (uint8_t *)ring_words + skew;
          uint8_t *dst = (uint8_t *)dst_words + dst_skew;
          uint32_t i;
          int same = 1;

          memset(ring_words, 0xee, sizeof(ring_words));
          memset(dst_words, 0xdd, sizeof(dst_words));
          for (i = 0; i < ring_length; i++) {
            ring[i] = (uint8_t)(i + 1);
          }

          mpega_ring_read(dst, ring, ring_length, offset, lengths[l]);

          for (i = 0; i < lengths[l]; i++) {
            if (dst[i] != (uint8_t)((offset + i) % ring_length + 1)) {
              same = 0;
            }
          }
          /* Nothing before or after the requested bytes is touched. */
          CHECK(dst_skew == 0 || dst[-1] == 0xdd);
          CHECK(dst[lengths[l]] == 0xdd);
          if (!same) {
            printf("ring read skew %u dst %u offset %u length %u\n",
                   (unsigned)skew, (unsigned)dst_skew, (unsigned)offset,
                   (unsigned)lengths[l]);
          }
          CHECK(same);
        }
      }
    }
  }
}

static void test_ring_advance(void)
{
  CHECK(mpega_ring_advance(0, 4, 64) == 4);
  CHECK(mpega_ring_advance(60, 4, 64) == 0);
  CHECK(mpega_ring_advance(60, 8, 64) == 4);
  CHECK(mpega_ring_advance(63, 1, 64) == 0);
  CHECK(mpega_ring_advance(10, 0, 64) == 10);
}

/* ------------------------------------------------------------- prepare */

static void put_be16(uint8_t *p, int16_t value)
{
  p[0] = (uint8_t)((uint16_t)value >> 8);
  p[1] = (uint8_t)value;
}

static void test_prepare_stereo(void)
{
  uint8_t src[9 * 4];
  int16_t left[9];
  int16_t right[9];
  uint32_t clipped = 0;
  uint32_t i;

  for (i = 0; i < 9; i++) {
    put_be16(src + i * 4, (int16_t)(100 * (int)i - 300));
    put_be16(src + i * 4 + 2, (int16_t)(-50 * (int)i));
  }

  CHECK(mpega_pcm_prepare(src, 9, 2, 2, 1, 100, left, right, &clipped) == 9);
  for (i = 0; i < 9; i++) {
    CHECK(left[i] == 100 * (int)i - 300);
    CHECK(right[i] == -50 * (int)i);
  }
  CHECK(clipped == 0);

  /* Every div-th frame: 9 frames give 3 at div 4, 5 at div 2. */
  CHECK(mpega_pcm_prepare(src, 9, 2, 2, 4, 100, left, right, &clipped) == 3);
  CHECK(left[0] == -300 && left[1] == 100 && left[2] == 500);
  CHECK(right[0] == 0 && right[1] == -200 && right[2] == -400);
  CHECK(mpega_pcm_prepare(src, 9, 2, 2, 2, 100, left, right, &clipped) == 5);
  CHECK(left[4] == 500 && right[4] == -400);
  CHECK(mpega_pcm_prepare(src, 8, 2, 2, 4, 100, left, right, &clipped) == 2);
  CHECK(mpega_pcm_prepare(src, 0, 2, 2, 1, 100, left, right, &clipped) == 0);
}

static void test_prepare_mono_mix(void)
{
  uint8_t src[4 * 4];
  int16_t out[4];
  uint32_t clipped = 0;

  put_be16(src + 0, 3);
  put_be16(src + 2, 0);
  put_be16(src + 4, -3);
  put_be16(src + 6, 0);
  put_be16(src + 8, 32767);
  put_be16(src + 10, 32767);
  put_be16(src + 12, -32768);
  put_be16(src + 14, -32768);

  CHECK(mpega_pcm_prepare(src, 4, 2, 1, 1, 100, out, 0, &clipped) == 4);
  /* (L + R) / 2 rounds toward zero. */
  CHECK(out[0] == 1);
  CHECK(out[1] == -1);
  CHECK(out[2] == 32767);
  CHECK(out[3] == -32768);
  CHECK(clipped == 0);

  /* Mono mixed down and decimated by 2. */
  CHECK(mpega_pcm_prepare(src, 4, 2, 1, 2, 100, out, 0, &clipped) == 2);
  CHECK(out[0] == 1 && out[1] == 32767);
}

static void test_prepare_mono_source(void)
{
  uint8_t src[3 * 2];
  int16_t left[3];
  int16_t right[3];
  uint32_t clipped = 0;

  put_be16(src + 0, 10);
  put_be16(src + 2, -20);
  put_be16(src + 4, 30);

  CHECK(mpega_pcm_prepare(src, 3, 1, 1, 1, 100, left, 0, &clipped) == 3);
  CHECK(left[0] == 10 && left[1] == -20 && left[2] == 30);

  CHECK(mpega_pcm_prepare(src, 3, 1, 2, 1, 100, left, right, &clipped) == 3);
  CHECK(left[2] == 30 && right[2] == 30 && right[1] == -20);
}

static void test_prepare_scale_and_clip(void)
{
  uint8_t src[4 * 4];
  int16_t left[4];
  int16_t right[4];
  uint32_t clipped = 0;

  put_be16(src + 0, 1001);
  put_be16(src + 2, -1001);
  put_be16(src + 4, 20000);
  put_be16(src + 6, -20000);
  put_be16(src + 8, 16000);
  put_be16(src + 10, 0);
  put_be16(src + 12, 32767);
  put_be16(src + 14, -32768);

  /* Half: truncated toward zero. */
  CHECK(mpega_pcm_prepare(src, 4, 2, 2, 1, 50, left, right, &clipped) == 4);
  CHECK(left[0] == 500 && right[0] == -500);
  CHECK(left[1] == 10000 && right[1] == -10000);
  CHECK(clipped == 0);

  /* Double: 20000 and -20000 clip, 16000 does not, 32767 and -32768 do. */
  CHECK(mpega_pcm_prepare(src, 4, 2, 2, 1, 200, left, right, &clipped) == 4);
  CHECK(left[0] == 2002 && right[0] == -2002);
  CHECK(left[1] == 32767 && right[1] == -32768);
  CHECK(left[2] == 32000 && right[2] == 0);
  CHECK(left[3] == 32767 && right[3] == -32768);
  CHECK(clipped == 4);

  /* The count keeps adding to what it holds. */
  CHECK(mpega_pcm_prepare(src, 4, 2, 2, 1, 10000, left, right, &clipped) == 4);
  CHECK(clipped == 4 + 7);

  /* Mono: the mix is scaled once. */
  clipped = 0;
  CHECK(mpega_pcm_prepare(src, 2, 2, 1, 1, 300, left, 0, &clipped) == 2);
  CHECK(left[0] == 0 && left[1] == 0);
  CHECK(clipped == 0);
}

static void test_peak(void)
{
  int16_t samples[5] = {10, -300, 299, 0, 7};
  int16_t full[2] = {-32768, 5};

  CHECK(mpega_pcm_peak(samples, 5) == 300);
  CHECK(mpega_pcm_peak(samples, 0) == 0);
  CHECK(mpega_pcm_peak(full, 2) == 32767);
}

/* --------------------------------------------------------------- write */

static int out_valid(uint32_t format, int b0, int b1, int b2, int b3,
                     uint32_t channels)
{
  struct MpgaOut out;
  int16_t buffers[4];

  memset(&out, 0, sizeof(out));
  out.mo_Format = format;
  out.mo_Buffer[0] = b0 ? &buffers[0] : 0;
  out.mo_Buffer[1] = b1 ? &buffers[1] : 0;
  out.mo_Buffer[2] = b2 ? &buffers[2] : 0;
  out.mo_Buffer[3] = b3 ? &buffers[3] : 0;
  return mpega_out_valid(&out, channels);
}

static void test_out_valid(void)
{
  CHECK(!mpega_out_valid(0, 2));
  CHECK(out_valid(MPGA_FMT_S16, 1, 1, 0, 0, 2));
  CHECK(!out_valid(MPGA_FMT_S16, 1, 0, 0, 0, 2));
  CHECK(out_valid(MPGA_FMT_S16, 1, 0, 0, 0, 1));
  CHECK(!out_valid(MPGA_FMT_S16, 0, 1, 0, 0, 1));
  CHECK(out_valid(MPGA_FMT_S16I, 1, 0, 0, 0, 2));
  CHECK(out_valid(MPGA_FMT_S8, 1, 1, 0, 0, 2));
  CHECK(out_valid(MPGA_FMT_S8D, 1, 1, 0, 0, 2));
  CHECK(!out_valid(MPGA_FMT_S14, 1, 1, 1, 0, 2));
  CHECK(out_valid(MPGA_FMT_S14, 1, 1, 1, 1, 2));
  CHECK(out_valid(MPGA_FMT_S14, 1, 0, 1, 0, 1));
  CHECK(!out_valid(MPGA_FMT_S14, 1, 0, 0, 0, 1));
  CHECK(!out_valid(MPGA_FMT_S14 + 1, 1, 1, 1, 1, 2));
}

static void test_write_s16(void)
{
  int16_t left_ring[8];
  int16_t right_ring[8];
  int16_t left[5] = {100, -200, 300, -400, 500};
  int16_t right[5] = {-1, -32768, 3, 4, 5};
  struct MpgaOut out;
  uint32_t dither = 1;
  int i;

  for (i = 0; i < 8; i++) {
    left_ring[i] = 0x1111;
    right_ring[i] = 0x2222;
  }
  memset(&out, 0, sizeof(out));
  out.mo_Format = MPGA_FMT_S16;
  out.mo_Buffer[0] = left_ring;
  out.mo_Buffer[1] = right_ring;
  out.mo_Mask = 7;
  out.mo_Index = 6;

  mpega_pcm_write_out(&out, left, right, 5, 2, &dither);

  /* Positions 6, 7, 0, 1, 2: the ring wraps. */
  CHECK(left_ring[6] == 100 && left_ring[7] == -200 && left_ring[0] == 300 &&
        left_ring[1] == -400 && left_ring[2] == 500);
  CHECK(right_ring[6] == -1 && right_ring[7] == -32768 && right_ring[2] == 5);
  CHECK(left_ring[3] == 0x1111 && left_ring[4] == 0x1111 &&
        left_ring[5] == 0x1111);
  CHECK(right_ring[5] == 0x2222);
  CHECK(out.mo_Index == 11);
  CHECK(out.mo_Peak[0] == 500);
  CHECK(out.mo_Peak[1] == 32767);

  /* A mask of all ones does not wrap. */
  {
    int16_t flat[16];

    memset(flat, 0, sizeof(flat));
    out.mo_Buffer[0] = flat;
    out.mo_Mask = ~0U;
    out.mo_Index = 9;
    mpega_pcm_write_out(&out, left, right, 3, 1, &dither);
    CHECK(flat[9] == 100 && flat[10] == -200 && flat[11] == 300);
    CHECK(flat[8] == 0 && flat[12] == 0);
    CHECK(out.mo_Index == 12);
    /* One channel: the right peak copies the left. */
    CHECK(out.mo_Peak[0] == 300 && out.mo_Peak[1] == 300);
  }
}

static void test_write_s16_interleaved(void)
{
  int16_t ring[16];
  int16_t left[5] = {1, 2, 3, 4, 5};
  int16_t right[5] = {-1, -2, -3, -4, -5};
  struct MpgaOut out;
  uint32_t dither = 1;
  int i;

  for (i = 0; i < 16; i++) {
    ring[i] = 0x7777;
  }
  memset(&out, 0, sizeof(out));
  out.mo_Format = MPGA_FMT_S16I;
  out.mo_Buffer[0] = ring;
  out.mo_Mask = 15; /* WORDs: eight sample frames */
  out.mo_Index = 6; /* sample frames */

  mpega_pcm_write_out(&out, left, right, 5, 2, &dither);

  /* Frames 6, 7, 0, 1, 2 at word positions 12, 14, 0, 2, 4. */
  CHECK(ring[12] == 1 && ring[13] == -1);
  CHECK(ring[14] == 2 && ring[15] == -2);
  CHECK(ring[0] == 3 && ring[1] == -3);
  CHECK(ring[2] == 4 && ring[3] == -4);
  CHECK(ring[4] == 5 && ring[5] == -5);
  CHECK(ring[6] == 0x7777 && ring[11] == 0x7777);
  CHECK(out.mo_Index == 11);
  CHECK(out.mo_Peak[0] == 5 && out.mo_Peak[1] == 5);

  /* Mono: one word a frame, mo_Index counts those frames. */
  for (i = 0; i < 16; i++) {
    ring[i] = 0x7777;
  }
  out.mo_Index = 14;
  mpega_pcm_write_out(&out, left, 0, 4, 1, &dither);
  CHECK(ring[14] == 1 && ring[15] == 2 && ring[0] == 3 && ring[1] == 4);
  CHECK(ring[2] == 0x7777 && ring[13] == 0x7777);
  CHECK(out.mo_Index == 18);
}

static void test_write_s8(void)
{
  int8_t left_ring[4] = {9, 9, 9, 9};
  int8_t right_ring[4] = {9, 9, 9, 9};
  int16_t left[3] = {0x1234, -1, 0x7fff};
  int16_t right[3] = {-32768, 0x00ff, 0x0100};
  struct MpgaOut out;
  uint32_t dither = 1;

  memset(&out, 0, sizeof(out));
  out.mo_Format = MPGA_FMT_S8;
  out.mo_Buffer[0] = left_ring;
  out.mo_Buffer[1] = right_ring;
  out.mo_Mask = 3;
  out.mo_Index = 3;

  mpega_pcm_write_out(&out, left, right, 3, 2, &dither);

  /* The high byte, arithmetic: positions 3, 0, 1. */
  CHECK(left_ring[3] == 0x12 && left_ring[0] == -1 && left_ring[1] == 0x7f);
  CHECK(right_ring[3] == -128 && right_ring[0] == 0 && right_ring[1] == 1);
  CHECK(left_ring[2] == 9 && right_ring[2] == 9);
  CHECK(out.mo_Index == 6);
  CHECK(out.mo_Peak[0] == 32767 && out.mo_Peak[1] == 32767);
}

static void test_write_s8_dithered(void)
{
  enum { n = 1152 };
  static int8_t ring_a[n];
  static int8_t ring_b[n];
  static int16_t constant[n];
  static int16_t extremes[n];
  struct MpgaOut out;
  uint32_t dither_a = 12345;
  uint32_t dither_b = 12345;
  uint32_t i;
  long sum = 0;
  int bad = 0;

  for (i = 0; i < n; i++) {
    constant[i] = 0x0a40; /* 10.25 of an 8-bit step */
    extremes[i] = (i & 1U) ? 32767 : -32768;
  }
  memset(&out, 0, sizeof(out));
  out.mo_Format = MPGA_FMT_S8D;
  out.mo_Buffer[0] = ring_a;
  out.mo_Mask = ~0U;

  mpega_pcm_write_out(&out, constant, 0, n, 1, &dither_a);
  for (i = 0; i < n; i++) {
    /* One step of noise either way around the rounded value. */
    if (ring_a[i] < 9 || ring_a[i] > 12) {
      bad++;
    }
    sum += ring_a[i];
  }
  CHECK(bad == 0);
  /* Triangular dither keeps the mean at the sample: 2624 / 256 = 10.25. */
  CHECK(sum > (long)(10.10 * n) && sum < (long)(10.40 * n));
  CHECK(dither_a != 12345);

  /* The same noise state gives the same bytes. */
  out.mo_Buffer[0] = ring_b;
  out.mo_Index = 0;
  mpega_pcm_write_out(&out, constant, 0, n, 1, &dither_b);
  CHECK(memcmp(ring_a, ring_b, sizeof(ring_a)) == 0);
  CHECK(dither_a == dither_b);

  /* The 16-bit ends stay inside the byte range. */
  out.mo_Index = 0;
  mpega_pcm_write_out(&out, extremes, 0, n, 1, &dither_b);
  for (i = 0; i < n; i += 2) {
    CHECK(ring_b[i] == -128 || ring_b[i] == -127);
    CHECK(ring_b[i + 1] == 127);
  }
}

static void test_write_s14(void)
{
  int8_t hi_left[4];
  int8_t hi_right[4];
  uint8_t lo_left[4];
  uint8_t lo_right[4];
  int16_t left[3] = {0x1234, -2, 32767};
  int16_t right[3] = {-32768, 3, 0};
  struct MpgaOut out;
  uint32_t dither = 1;
  int i;

  memset(hi_left, 0x55, sizeof(hi_left));
  memset(hi_right, 0x55, sizeof(hi_right));
  memset(lo_left, 0x55, sizeof(lo_left));
  memset(lo_right, 0x55, sizeof(lo_right));
  memset(&out, 0, sizeof(out));
  out.mo_Format = MPGA_FMT_S14;
  out.mo_Buffer[0] = hi_left;
  out.mo_Buffer[1] = hi_right;
  out.mo_Buffer[2] = lo_left;
  out.mo_Buffer[3] = lo_right;
  out.mo_Mask = 3;
  out.mo_Index = 2;

  mpega_pcm_write_out(&out, left, right, 3, 2, &dither);

  /* Positions 2, 3, 0. High byte of the sample, then its next 6 bits. */
  CHECK(hi_left[2] == 0x12 && lo_left[2] == 0x0d);
  CHECK(hi_left[3] == -1 && lo_left[3] == 63);
  CHECK(hi_left[0] == 0x7f && lo_left[0] == 63);
  CHECK(hi_right[2] == -128 && lo_right[2] == 0);
  CHECK(hi_right[3] == 0 && lo_right[3] == 0);
  CHECK(hi_left[1] == 0x55 && lo_right[1] == 0x55);
  CHECK(out.mo_Index == 5);

  /* The two bytes rebuild the sample without its two lowest bits. */
  for (i = 0; i < 3; i++) {
    int pos = (2 + i) & 3;

    CHECK(((int)hi_left[pos] * 64 + (int)lo_left[pos]) == (left[i] >> 2));
    CHECK(((int)hi_right[pos] * 64 + (int)lo_right[pos]) == (right[i] >> 2));
  }
}

/* ------------------------------------------------------------- reduce */

static void test_reduce_plan(void)
{
  MPEGAReducePlan plan;
  const uint32_t reduce = ZZ9K_SERVICE_FLAG_AUDIO_STREAM_REDUCE;
  const uint32_t other = ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM;

  /* Without the flag nothing is asked of the card. */
  mpega_reduce_plan(&plan, other, 1, 2, 1, 44100, 2);
  CHECK(plan.output_hz == 0 && plan.output_channels == 0);

  /* Without a probed header the stream's rate and layout are guesses. */
  mpega_reduce_plan(&plan, reduce | other, 0, 2, 1, 44100, 2);
  CHECK(plan.output_hz == 0 && plan.output_channels == 0);

  mpega_reduce_plan(&plan, reduce | other, 1, 2, 0, 44100, 1);
  CHECK(plan.output_hz == 0 && plan.output_channels == 0);

  mpega_reduce_plan(&plan, reduce | other, 1, 2, 1, 44100, 1);
  CHECK(plan.output_hz == 0 && plan.output_channels == 1);

  /* A mono stream has nothing to mix down. */
  mpega_reduce_plan(&plan, reduce | other, 1, 1, 1, 44100, 1);
  CHECK(plan.output_hz == 0 && plan.output_channels == 0);

  mpega_reduce_plan(&plan, reduce, 1, 2, 0, 44100, 2);
  CHECK(plan.output_hz == 22050 && plan.output_channels == 0);

  mpega_reduce_plan(&plan, reduce, 1, 2, 0, 32000, 4);
  CHECK(plan.output_hz == 8000 && plan.output_channels == 0);

  mpega_reduce_plan(&plan, reduce, 1, 2, 1, 48000, 4);
  CHECK(plan.output_hz == 12000 && plan.output_channels == 1);
}

typedef struct FakeCard {
  int supports_reduce;
  int calls;
  uint32_t hz[4];
  uint32_t channels[4];
  int fail_status;
} FakeCard;

static int fake_begin(void *context, uint32_t output_hz,
                      uint32_t output_channels)
{
  FakeCard *card = (FakeCard *)context;

  card->hz[card->calls] = output_hz;
  card->channels[card->calls] = output_channels;
  card->calls++;
  if (card->fail_status != ZZ9K_STATUS_OK) {
    return card->fail_status;
  }
  if (!card->supports_reduce && (output_hz != 0 || output_channels != 0)) {
    return ZZ9K_STATUS_UNSUPPORTED;
  }
  return ZZ9K_STATUS_OK;
}

static void test_reduce_begin(void)
{
  MPEGAReducePlan plan;
  FakeCard card;

  /* A card that reduces gets the plan once and keeps it. */
  memset(&card, 0, sizeof(card));
  card.supports_reduce = 1;
  plan.output_hz = 22050;
  plan.output_channels = 1;
  CHECK(mpega_reduce_begin(&plan, fake_begin, &card) == ZZ9K_STATUS_OK);
  CHECK(card.calls == 1 && card.hz[0] == 22050 && card.channels[0] == 1);
  CHECK(plan.output_hz == 22050 && plan.output_channels == 1);

  /* A card that rejects it is asked again with zeros and the plan clears. */
  memset(&card, 0, sizeof(card));
  plan.output_hz = 22050;
  plan.output_channels = 1;
  CHECK(mpega_reduce_begin(&plan, fake_begin, &card) == ZZ9K_STATUS_OK);
  CHECK(card.calls == 2);
  CHECK(card.hz[0] == 22050 && card.channels[0] == 1);
  CHECK(card.hz[1] == 0 && card.channels[1] == 0);
  CHECK(plan.output_hz == 0 && plan.output_channels == 0);

  /* An empty plan is begun once, and a refusal of it is final. */
  memset(&card, 0, sizeof(card));
  card.fail_status = ZZ9K_STATUS_UNSUPPORTED;
  plan.output_hz = 0;
  plan.output_channels = 0;
  CHECK(mpega_reduce_begin(&plan, fake_begin, &card) ==
        ZZ9K_STATUS_UNSUPPORTED);
  CHECK(card.calls == 1);

  /* Other failures do not retry. */
  memset(&card, 0, sizeof(card));
  card.fail_status = ZZ9K_STATUS_NO_MEMORY;
  plan.output_hz = 11025;
  plan.output_channels = 0;
  CHECK(mpega_reduce_begin(&plan, fake_begin, &card) ==
        ZZ9K_STATUS_NO_MEMORY);
  CHECK(card.calls == 1);
  CHECK(plan.output_hz == 11025);

  /* A retry that fails too reports its status. */
  memset(&card, 0, sizeof(card));
  card.supports_reduce = 0;
  plan.output_hz = 0;
  plan.output_channels = 1;
  card.fail_status = ZZ9K_STATUS_OK;
  CHECK(mpega_reduce_begin(&plan, fake_begin, &card) == ZZ9K_STATUS_OK);
  CHECK(card.calls == 2);
}

int main(void)
{
  test_ring_read();
  test_ring_advance();
  test_prepare_stereo();
  test_prepare_mono_mix();
  test_prepare_mono_source();
  test_prepare_scale_and_clip();
  test_peak();
  test_out_valid();
  test_write_s16();
  test_write_s16_interleaved();
  test_write_s8();
  test_write_s8_dithered();
  test_write_s14();
  test_reduce_plan();
  test_reduce_begin();

  if (failures != 0) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
