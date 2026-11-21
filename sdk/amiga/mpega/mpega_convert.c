/*
 * PCM handling of the ZZ9000 mpega.library, see mpega_convert.h.
 *
 * MPGA_decode_to follows ambmpega.library's interface (MIT, (c) 2026 Brian
 * King): the formats, the ring and index rules, the peak and the dither of
 * one 8-bit step are described in <libraries/mpega.h>. The code here is
 * written for the PCM this library gets from the card.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mpega_convert.h"

#include <string.h>

#include "zz9k/abi.h"

/* The ring is read as big-endian words from a buffer that is 2-byte aligned.
 * The 68k reads them as they are; other hosts compose them. */
#if defined(__m68k__)
#define MPEGA_BE16(p) (*(const int16_t *)(p))
#else
#define MPEGA_BE16(p) \
  ((int16_t)(uint16_t)(((uint16_t)(p)[0] << 8) | (uint16_t)(p)[1]))
#endif

static void mpega_read_run(uint8_t *dst, const volatile uint8_t *src,
                           uint32_t n)
{
  while (n != 0U && ((uintptr_t)src & 3U) != 0U) {
    *dst++ = *src++;
    n--;
  }

  if (((uintptr_t)dst & 3U) == 0U) {
    while (n >= 4U) {
      *(uint32_t *)(void *)dst = *(const volatile uint32_t *)(const volatile void *)src;
      dst += 4;
      src += 4;
      n -= 4U;
    }
  } else {
    while (n >= 4U) {
      uint32_t word = *(const volatile uint32_t *)(const volatile void *)src;

      memcpy(dst, &word, 4U);
      dst += 4;
      src += 4;
      n -= 4U;
    }
  }

  while (n != 0U) {
    *dst++ = *src++;
    n--;
  }
}

void mpega_ring_read(uint8_t *dst, const volatile uint8_t *ring,
                     uint32_t ring_length, uint32_t offset, uint32_t bytes)
{
  uint32_t first;

  if (bytes == 0U || ring_length == 0U || offset >= ring_length) {
    return;
  }

  first = ring_length - offset;
  if (bytes <= first) {
    mpega_read_run(dst, ring + offset, bytes);
    return;
  }

  mpega_read_run(dst, ring + offset, first);
  mpega_read_run(dst + first, ring, bytes - first);
}

uint32_t mpega_ring_advance(uint32_t offset, uint32_t bytes,
                            uint32_t ring_length)
{
  if (ring_length == 0U) {
    return 0U;
  }
  if (bytes >= ring_length - offset) {
    return bytes - (ring_length - offset);
  }
  return offset + bytes;
}

static int16_t mpega_scale_clamp(int32_t value, int32_t scale_percent,
                                 uint32_t *clipped)
{
  if (scale_percent != 100) {
    value = (value * scale_percent) / 100;
  }
  if (value > 32767) {
    (*clipped)++;
    return 32767;
  }
  if (value < -32768) {
    (*clipped)++;
    return -32768;
  }
  return (int16_t)value;
}

uint32_t mpega_pcm_prepare(const uint8_t *src, uint32_t src_frames,
                           uint32_t src_channels, uint32_t out_channels,
                           uint32_t div, int32_t scale_percent,
                           int16_t *dst0, int16_t *dst1, uint32_t *clipped)
{
  uint32_t source_frame;
  uint32_t out_frame;
  uint32_t frame_bytes;

  if (div == 0U) {
    div = 1U;
  }
  frame_bytes = src_channels * 2U;
  out_frame = 0U;

  if (out_channels == 1U) {
    for (source_frame = 0U; source_frame < src_frames;
         source_frame += div, out_frame++) {
      const uint8_t *frame = src + source_frame * frame_bytes;
      int32_t value = MPEGA_BE16(frame);

      if (src_channels == 2U) {
        value = (value + (int32_t)MPEGA_BE16(frame + 2)) / 2;
      }
      dst0[out_frame] = mpega_scale_clamp(value, scale_percent, clipped);
    }
  } else if (src_channels == 1U) {
    for (source_frame = 0U; source_frame < src_frames;
         source_frame += div, out_frame++) {
      int16_t value = mpega_scale_clamp(
          MPEGA_BE16(src + source_frame * 2U), scale_percent, clipped);

      dst0[out_frame] = value;
      dst1[out_frame] = value;
    }
  } else if (scale_percent == 100) {
    for (source_frame = 0U; source_frame < src_frames;
         source_frame += div, out_frame++) {
      const uint8_t *frame = src + source_frame * 4U;

      dst0[out_frame] = MPEGA_BE16(frame);
      dst1[out_frame] = MPEGA_BE16(frame + 2);
    }
  } else {
    for (source_frame = 0U; source_frame < src_frames;
         source_frame += div, out_frame++) {
      const uint8_t *frame = src + source_frame * 4U;

      dst0[out_frame] =
          mpega_scale_clamp(MPEGA_BE16(frame), scale_percent, clipped);
      dst1[out_frame] =
          mpega_scale_clamp(MPEGA_BE16(frame + 2), scale_percent, clipped);
    }
  }

  return out_frame;
}

uint16_t mpega_pcm_peak(const int16_t *samples, uint32_t count)
{
  int32_t peak = 0;
  uint32_t i;

  for (i = 0U; i < count; i++) {
    int32_t value = samples[i];

    if (value < 0) {
      value = -value;
    }
    if (value > peak) {
      peak = value;
    }
  }

  return (uint16_t)(peak > 32767 ? 32767 : peak);
}

int mpega_out_valid(const struct MpgaOut *out, uint32_t channels)
{
  if (!out || out->mo_Format > MPGA_FMT_S14 || !out->mo_Buffer[0]) {
    return 0;
  }
  if (out->mo_Format != MPGA_FMT_S16I && channels > 1U && !out->mo_Buffer[1]) {
    return 0;
  }
  if (out->mo_Format == MPGA_FMT_S14 &&
      (!out->mo_Buffer[2] || (channels > 1U && !out->mo_Buffer[3]))) {
    return 0;
  }
  return 1;
}

/* A uniform 0 to 255 from a linear congruential generator. */
static int32_t mpega_dither_byte(uint32_t *state)
{
  *state = *state * 1664525UL + 1013904223UL;
  return (int32_t)(*state >> 24);
}

void mpega_pcm_write_out(struct MpgaOut *out, const int16_t *left,
                         const int16_t *right, uint32_t frames,
                         uint32_t channels, uint32_t *dither)
{
  const int16_t *source[2];
  uint32_t mask = out->mo_Mask;
  uint32_t channel;
  uint32_t i;

  source[0] = left;
  source[1] = right;
  for (channel = 0U; channel < channels; channel++) {
    const int16_t *samples = source[channel];
    uint32_t at = out->mo_Index;

    out->mo_Peak[channel] = mpega_pcm_peak(samples, frames);

    switch (out->mo_Format) {
    case MPGA_FMT_S16: {
      int16_t *dst = (int16_t *)out->mo_Buffer[channel];

      for (i = 0U; i < frames; i++) {
        dst[(at + i) & mask] = samples[i];
      }
      break;
    }
    case MPGA_FMT_S16I: {
      int16_t *dst = (int16_t *)out->mo_Buffer[0];
      uint32_t position = at * channels + channel;

      for (i = 0U; i < frames; i++) {
        dst[position & mask] = samples[i];
        position += channels;
      }
      break;
    }
    case MPGA_FMT_S8: {
      int8_t *dst = (int8_t *)out->mo_Buffer[channel];

      for (i = 0U; i < frames; i++) {
        dst[(at + i) & mask] = (int8_t)(samples[i] >> 8);
      }
      break;
    }
    case MPGA_FMT_S8D: {
      int8_t *dst = (int8_t *)out->mo_Buffer[channel];

      for (i = 0U; i < frames; i++) {
        int32_t noise = mpega_dither_byte(dither);
        int32_t value;

        noise -= mpega_dither_byte(dither);
        value = (int32_t)samples[i] + noise + 128;
        if (value > 32767) {
          value = 32767;
        } else if (value < -32768) {
          value = -32768;
        }
        dst[(at + i) & mask] = (int8_t)(value >> 8);
      }
      break;
    }
    default: {
      int8_t *high = (int8_t *)out->mo_Buffer[channel];
      uint8_t *low = (uint8_t *)out->mo_Buffer[2U + channel];

      for (i = 0U; i < frames; i++) {
        uint32_t position = (at + i) & mask;

        high[position] = (int8_t)(samples[i] >> 8);
        low[position] = (uint8_t)((samples[i] >> 2) & 0x3f);
      }
      break;
    }
    }
  }

  if (channels == 1U) {
    out->mo_Peak[1] = out->mo_Peak[0];
  }
  out->mo_Index += frames;
}

void mpega_reduce_plan(MPEGAReducePlan *plan, uint32_t service_flags,
                       int header_probed, uint32_t stream_channels,
                       int force_mono, uint32_t probed_rate,
                       uint32_t freq_div)
{
  plan->output_hz = 0U;
  plan->output_channels = 0U;

  if ((service_flags & ZZ9K_SERVICE_FLAG_AUDIO_STREAM_REDUCE) == 0U ||
      !header_probed) {
    return;
  }
  if (force_mono && stream_channels == 2U) {
    plan->output_channels = 1U;
  }
  if (freq_div > 1U && probed_rate / freq_div != 0U) {
    plan->output_hz = probed_rate / freq_div;
  }
}

int mpega_reduce_begin(MPEGAReducePlan *plan, MPEGAReduceBeginFn begin,
                       void *context)
{
  int status = begin(context, plan->output_hz, plan->output_channels);

  if (status == ZZ9K_STATUS_UNSUPPORTED &&
      (plan->output_hz != 0U || plan->output_channels != 0U)) {
    plan->output_hz = 0U;
    plan->output_channels = 0U;
    status = begin(context, 0U, 0U);
  }

  return status;
}

#define MPEGA_EOF_STALL_LIMIT 4U

int mpega_feed_until_pcm(MPEGAFeedState *state, const MPEGAFeedOps *ops,
                         uint32_t max_attempts)
{
  void *context = ops->context;
  uint32_t attempts;

  for (attempts = 0; attempts < max_attempts; attempts++) {
    uint32_t got;

    if (ops->pcm_available(context)) {
      return MPEGA_ERR_NONE;
    }
    if (!ops->ack(context)) {
      return MPEGA_ERR_BADFRAME;
    }
    if ((ops->result_flags(context) & ZZ9K_AUDIO_STREAM_RESULT_DONE) != 0U) {
      return MPEGA_ERR_EOF;
    }

    if (state->staged_pending != 0U) {
      if (!ops->feed(context, state->staged_pending, 0U)) {
        return MPEGA_ERR_BADFRAME;
      }
      if ((ops->result_flags(context) &
           ZZ9K_AUDIO_STREAM_RESULT_BACKPRESSURE) == 0U) {
        state->staged_pending = 0U;
      } else if (!ops->kick(context)) {
        return MPEGA_ERR_BADFRAME;
      }
      continue;
    }

    if (state->eof_sent) {
      uint32_t consumed = ops->bytes_consumed(context);
      uint32_t produced = ops->bytes_produced(context);

      if (!ops->kick(context)) {
        return MPEGA_ERR_BADFRAME;
      }
      if (ops->bytes_consumed(context) != consumed ||
          ops->bytes_produced(context) != produced) {
        state->eof_stalls = 0U;
      } else if (++state->eof_stalls >= MPEGA_EOF_STALL_LIMIT) {
        return MPEGA_ERR_EOF;
      }
      continue;
    }

    if (!ops->read_stage(context, &got)) {
      return MPEGA_ERR_BADFRAME;
    }
    if (got == 0U) {
      state->eof_sent = 1U;
      state->eof_stalls = 0U;
      if (!ops->feed(context, 0U, ZZ9K_AUDIO_STREAM_FEED_EOF)) {
        return MPEGA_ERR_BADFRAME;
      }
      continue;
    }
    if (!ops->feed(context, got, 0U)) {
      return MPEGA_ERR_BADFRAME;
    }
    if ((ops->result_flags(context) & ZZ9K_AUDIO_STREAM_RESULT_BACKPRESSURE) !=
        0U) {
      state->staged_pending = got;
    }
  }

  return MPEGA_FEED_NO_PCM;
}
