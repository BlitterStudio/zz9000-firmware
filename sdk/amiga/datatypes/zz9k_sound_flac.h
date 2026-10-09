/*
 * Allocation-free native FLAC recognition shared by the ZZ9000 sound
 * DataType and ZZPlay.
 *
 * Copyright (C) 2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_SOUND_FLAC_H
#define ZZ9K_SOUND_FLAC_H

#include <stdint.h>

#define ZZ9K_SOUND_FLAC_STREAMINFO_BYTES 34U
#define ZZ9K_SOUND_FLAC_HEADER_BYTES (4U + 4U + ZZ9K_SOUND_FLAC_STREAMINFO_BYTES) /* 42 bytes */

typedef struct ZZ9KSoundFlacEnvelope {
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t bits_per_sample;
  uint64_t total_samples;
  uint32_t min_block_size;
  uint32_t max_block_size;
  uint32_t min_frame_size;
  uint32_t max_frame_size;
} ZZ9KSoundFlacEnvelope;

/*
 * Recognizes a native FLAC stream: the "fLaC" marker followed by a
 * 34-byte STREAMINFO block. Ogg-FLAC ("OggS"), a different first block,
 * more than two channels and sample widths outside 4..24 bits are rejected.
 */
static int zz9k_sound_recognize_flac(const uint8_t *bytes, uint32_t length,
                                    ZZ9KSoundFlacEnvelope *envelope)
{
  uint32_t min_block, max_block, min_frame, max_frame;
  uint32_t rate, channels, bits;
  uint64_t total;
  uint32_t block_len;

  if (!bytes || length < ZZ9K_SOUND_FLAC_HEADER_BYTES) {
    return 0;
  }
  /* Native FLAC 4-byte marker "fLaC" */
  if (bytes[0] != 'f' || bytes[1] != 'L' || bytes[2] != 'a' || bytes[3] != 'C') {
    return 0;
  }
  /* First metadata block header must be STREAMINFO (type 0) */
  if ((bytes[4] & 0x7fU) != 0U) {
    return 0;
  }
  /* STREAMINFO block length must be exactly 34 bytes */
  block_len = ((uint32_t)bytes[5] << 16) | ((uint32_t)bytes[6] << 8) | (uint32_t)bytes[7];
  if (block_len != ZZ9K_SOUND_FLAC_STREAMINFO_BYTES) {
    return 0;
  }
  /* STREAMINFO payload starts at byte 8 */
  min_block = ((uint32_t)bytes[8] << 8) | (uint32_t)bytes[9];
  max_block = ((uint32_t)bytes[10] << 8) | (uint32_t)bytes[11];
  if (min_block < 16U || max_block < min_block) {
    return 0;
  }
  min_frame = ((uint32_t)bytes[12] << 16) | ((uint32_t)bytes[13] << 8) | (uint32_t)bytes[14];
  max_frame = ((uint32_t)bytes[15] << 16) | ((uint32_t)bytes[16] << 8) | (uint32_t)bytes[17];
  if (min_frame != 0U && max_frame != 0U && max_frame < min_frame) {
    return 0;
  }
  /* Sample rate: 20 bits; the firmware decoder accepts 8..192 kHz. */
  rate = ((uint32_t)bytes[18] << 12) | ((uint32_t)bytes[19] << 4) | ((uint32_t)bytes[20] >> 4);
  if (rate < 8000U || rate > 192000U) {
    return 0;
  }
  /* Channels: 3 bits (0 = 1 ch, 1 = 2 ch). Reject > 2 channels in U7 */
  channels = (((uint32_t)bytes[20] >> 1) & 0x07U) + 1U;
  if (channels != 1U && channels != 2U) {
    return 0;
  }
  /* Bits per sample: 5 bits (0 = 1 bit .. 31 = 32 bits). Valid audio: 4..24 bits */
  bits = ((((uint32_t)bytes[20] & 0x01U) << 4) | ((uint32_t)bytes[21] >> 4)) + 1U;
  if (bits < 4U || bits > 24U) {
    return 0;
  }
  /* Total samples: 36 bits */
  total = (((uint64_t)(bytes[21] & 0x0fU)) << 32) |
          (((uint64_t)bytes[22]) << 24) |
          (((uint64_t)bytes[23]) << 16) |
          (((uint64_t)bytes[24]) << 8) |
          ((uint64_t)bytes[25]);

  if (envelope) {
    envelope->sample_rate = rate;
    envelope->channels = channels;
    envelope->bits_per_sample = bits;
    envelope->total_samples = total;
    envelope->min_block_size = min_block;
    envelope->max_block_size = max_block;
    envelope->min_frame_size = min_frame;
    envelope->max_frame_size = max_frame;
  }
  return 1;
}

#endif /* ZZ9K_SOUND_FLAC_H */
