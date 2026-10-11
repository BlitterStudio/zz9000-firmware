/*
 * Allocation-free MPEG audio Layer III recognition shared by the ZZ9000 sound
 * DataType class and its descriptor recognition hook.
 *
 * Copyright (C) 2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The code has no Amiga or C-library dependency and uses no hardware divide:
 * the descriptor hook runs without startup code on 68000 systems.
 */

#ifndef ZZ9K_SOUND_MP3_H
#define ZZ9K_SOUND_MP3_H

#include <stdint.h>

#define ZZ9K_SOUND_ID3V2_MAX_BYTES (16UL * 1024UL * 1024UL)
#define ZZ9K_SOUND_SYNC_SCAN_BYTES (16UL * 1024UL)
/* Largest Layer III frame (MPEG-1 320 kbit/s at 32 kHz, or MPEG-2 160 kbit/s
 * at 8 kHz, both with padding) is 1441 bytes; two headers fit in this. */
#define ZZ9K_SOUND_MP3_PAIR_PROBE_BYTES 2896UL

typedef struct ZZ9KSoundMp3Envelope {
  uint32_t first_frame;
  uint32_t frame_bytes;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t samples_per_frame;
} ZZ9KSoundMp3Envelope;

static uint32_t zz9k_sound_udiv32(uint32_t dividend, uint32_t divisor)
{
  uint32_t quotient = 0U;
  uint32_t remainder = 0U;
  int bit;

  for (bit = 31; bit >= 0; --bit) {
    remainder = (remainder << 1) | ((dividend >> bit) & 1U);
    if (remainder >= divisor) {
      remainder -= divisor;
      quotient |= 1UL << bit;
    }
  }
  return quotient;
}

static uint32_t zz9k_sound_synchsafe32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 21) | ((uint32_t)p[1] << 14) |
         ((uint32_t)p[2] << 7) | (uint32_t)p[3];
}

/* Sets *start to the first byte after a valid ID3v2 tag, or 0 when the data
 * has no tag. Returns 0 for a malformed, truncated or oversized tag. */
static int zz9k_sound_audio_start(const uint8_t *bytes, uint32_t available,
                                  uint32_t *start)
{
  uint32_t body;
  uint32_t footer;

  *start = 0U;
  if (available < 3U || bytes[0] != 'I' || bytes[1] != 'D' ||
      bytes[2] != '3') {
    return 1;
  }
  if (available < 10U || bytes[3] == 0xffU || bytes[4] == 0xffU ||
      (bytes[6] | bytes[7] | bytes[8] | bytes[9]) >= 0x80U) {
    return 0;
  }
  body = zz9k_sound_synchsafe32(bytes + 6U);
  footer = (bytes[5] & 0x10U) != 0U ? 10U : 0U;
  if (body > ZZ9K_SOUND_ID3V2_MAX_BYTES) {
    return 0;
  }
  *start = 10U + body + footer;
  return 1;
}

static int zz9k_sound_mp3_header(const uint8_t *p, uint32_t available,
                                 ZZ9KSoundMp3Envelope *out)
{
  static const uint16_t rate_table[4][3] = {
    {11025U, 12000U, 8000U},
    {0U, 0U, 0U},
    {22050U, 24000U, 16000U},
    {44100U, 48000U, 32000U}
  };
  /* 144000 x MPEG-1 and 72000 x MPEG-2/2.5 Layer III kbit/s, precomputed so
   * the hook needs no 32-bit multiply helper on 68000. */
#define ZZ9K_V1(kbps) (144000UL * (kbps))
#define ZZ9K_V2(kbps) (72000UL * (kbps))
  static const uint32_t frame_numerator_v1[16] = {
    0UL, ZZ9K_V1(32), ZZ9K_V1(40), ZZ9K_V1(48), ZZ9K_V1(56), ZZ9K_V1(64),
    ZZ9K_V1(80), ZZ9K_V1(96), ZZ9K_V1(112), ZZ9K_V1(128), ZZ9K_V1(160),
    ZZ9K_V1(192), ZZ9K_V1(224), ZZ9K_V1(256), ZZ9K_V1(320), 0UL
  };
  static const uint32_t frame_numerator_v2[16] = {
    0UL, ZZ9K_V2(8), ZZ9K_V2(16), ZZ9K_V2(24), ZZ9K_V2(32), ZZ9K_V2(40),
    ZZ9K_V2(48), ZZ9K_V2(56), ZZ9K_V2(64), ZZ9K_V2(80), ZZ9K_V2(96),
    ZZ9K_V2(112), ZZ9K_V2(128), ZZ9K_V2(144), ZZ9K_V2(160), 0UL
  };
#undef ZZ9K_V1
#undef ZZ9K_V2
  uint32_t word;
  uint32_t version;
  uint32_t layer;
  uint32_t bitrate_index;
  uint32_t rate_index;
  uint32_t rate;
  uint32_t frame_bytes;

  if (!p || available < 4U) {
    return 0;
  }
  word = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
  if ((word & 0xffe00000UL) != 0xffe00000UL) {
    return 0;
  }
  version = (word >> 19) & 3U;
  layer = (word >> 17) & 3U;
  bitrate_index = (word >> 12) & 15U;
  rate_index = (word >> 10) & 3U;
  /* Layer bits 01 are Layer III. Version bits 01 are reserved. */
  if (version == 1U || layer != 1U || bitrate_index == 0U ||
      bitrate_index == 15U || rate_index == 3U) {
    return 0;
  }
  rate = rate_table[version][rate_index];
  frame_bytes = zz9k_sound_udiv32(version == 3U ?
                                      frame_numerator_v1[bitrate_index] :
                                      frame_numerator_v2[bitrate_index],
                                  rate);
  frame_bytes += (word >> 9) & 1U;
  if (out) {
    out->first_frame = 0U;
    out->frame_bytes = frame_bytes;
    out->sample_rate = rate;
    out->channels = ((word >> 6) & 3U) == 3U ? 1U : 2U;
    out->samples_per_frame = version == 3U ? 1152U : 576U;
  }
  return 1;
}

/* Two consecutive Layer III headers with matching rate and channel mode,
 * the first exactly at bytes[0]. */
static int zz9k_sound_mp3_pair_at(const uint8_t *bytes, uint32_t length,
                                  ZZ9KSoundMp3Envelope *envelope)
{
  ZZ9KSoundMp3Envelope first;
  ZZ9KSoundMp3Envelope second;

  if (!zz9k_sound_mp3_header(bytes, length, &first) ||
      first.frame_bytes > length ||
      length - first.frame_bytes < 4U ||
      !zz9k_sound_mp3_header(bytes + first.frame_bytes,
                             length - first.frame_bytes, &second) ||
      second.sample_rate != first.sample_rate ||
      second.channels != first.channels) {
    return 0;
  }
  if (envelope) {
    *envelope = first;
  }
  return 1;
}

/* Skips a bounded ID3v2 tag, then scans at most ZZ9K_SOUND_SYNC_SCAN_BYTES
 * for a positive two-frame Layer III envelope. */
static int zz9k_sound_recognize_mp3(const uint8_t *bytes, uint32_t length,
                                    ZZ9KSoundMp3Envelope *envelope)
{
  uint32_t start;
  uint32_t scan_end;
  uint32_t offset;

  if (!bytes || length < 8U || !zz9k_sound_audio_start(bytes, length, &start) ||
      start > length) {
    return 0;
  }
  scan_end = length;
  if (scan_end - start > ZZ9K_SOUND_SYNC_SCAN_BYTES) {
    scan_end = start + ZZ9K_SOUND_SYNC_SCAN_BYTES;
  }
  for (offset = start; offset + 8U <= scan_end; offset++) {
    if (zz9k_sound_mp3_pair_at(bytes + offset, length - offset, envelope)) {
      if (envelope) {
        envelope->first_frame = offset;
      }
      return 1;
    }
  }
  return 0;
}

#endif /* ZZ9K_SOUND_MP3_H */
