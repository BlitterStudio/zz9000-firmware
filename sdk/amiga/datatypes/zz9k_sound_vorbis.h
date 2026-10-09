/*
 * Allocation-free Ogg Vorbis recognition shared by the ZZ9000 sound
 * DataType and ZZPlay.
 *
 * Copyright (C) 2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_SOUND_VORBIS_H
#define ZZ9K_SOUND_VORBIS_H

#include <stdint.h>

/* Vorbis I requires the first Ogg page to carry only the 30-byte
 * identification packet: a 27-byte page header, one lacing value (30),
 * then the packet. */
#define ZZ9K_SOUND_VORBIS_PAGE_HEADER_BYTES 27U
#define ZZ9K_SOUND_VORBIS_ID_PACKET_BYTES 30U
#define ZZ9K_SOUND_VORBIS_HEADER_BYTES \
  (ZZ9K_SOUND_VORBIS_PAGE_HEADER_BYTES + 1U + ZZ9K_SOUND_VORBIS_ID_PACKET_BYTES)

typedef struct ZZ9KSoundVorbisEnvelope {
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t serial;
  uint32_t nominal_bitrate; /* 0 when unset */
  uint32_t max_block_samples; /* long-window size, 64..8192 */
} ZZ9KSoundVorbisEnvelope;

static uint32_t zz9k_sound_vorbis_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/* Ogg page CRC: CRC-32, polynomial 0x04c11db7, no reflection, initial and
 * final value 0, computed with the CRC field (bytes 22..25) taken as zero. */
static uint32_t zz9k_sound_ogg_crc(const uint8_t *page, uint32_t length)
{
  uint32_t crc = 0U;
  uint32_t i;

  for (i = 0U; i < length; i++) {
    uint32_t byte = (i >= 22U && i < 26U) ? 0U : page[i];
    int bit;

    crc ^= byte << 24;
    for (bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80000000UL) ? (crc << 1) ^ 0x04c11db7UL : crc << 1;
    }
  }
  return crc;
}

/*
 * Recognizes an Ogg Vorbis stream from its first page: a version-0 BOS
 * page at granule 0 and sequence 0 with one 30-byte segment holding a
 * Vorbis I identification packet, and a matching page CRC. The stream must
 * be mono or stereo at 8..192 kHz with valid block sizes. Opus, Ogg-FLAC,
 * other Ogg codecs and anything wider are not claimed. Chained or
 * multiplexed streams are rejected later, by the decoder.
 */
static int zz9k_sound_recognize_vorbis(const uint8_t *bytes, uint32_t length,
                                       ZZ9KSoundVorbisEnvelope *envelope)
{
  const uint8_t *packet;
  uint32_t i;
  uint32_t channels;
  uint32_t rate;
  uint32_t small_block;
  uint32_t large_block;

  if (!bytes || length < ZZ9K_SOUND_VORBIS_HEADER_BYTES ||
      bytes[0] != 'O' || bytes[1] != 'g' || bytes[2] != 'g' ||
      bytes[3] != 'S' || bytes[4] != 0U || bytes[5] != 0x02U ||
      bytes[26] != 1U || bytes[27] != ZZ9K_SOUND_VORBIS_ID_PACKET_BYTES) {
    return 0;
  }
  for (i = 6U; i < 14U; i++) {
    if (bytes[i] != 0U) {
      return 0; /* granule position of a header page is 0 */
    }
  }
  if (zz9k_sound_vorbis_le32(bytes + 18U) != 0U ||
      zz9k_sound_vorbis_le32(bytes + 22U) !=
          zz9k_sound_ogg_crc(bytes, ZZ9K_SOUND_VORBIS_HEADER_BYTES)) {
    return 0;
  }
  packet = bytes + ZZ9K_SOUND_VORBIS_PAGE_HEADER_BYTES + 1U;
  if (packet[0] != 0x01U || packet[1] != 'v' || packet[2] != 'o' ||
      packet[3] != 'r' || packet[4] != 'b' || packet[5] != 'i' ||
      packet[6] != 's' || zz9k_sound_vorbis_le32(packet + 7U) != 0U) {
    return 0;
  }
  channels = packet[11];
  rate = zz9k_sound_vorbis_le32(packet + 12U);
  small_block = packet[28] & 0x0fU;
  large_block = packet[28] >> 4;
  if ((channels != 1U && channels != 2U) || rate < 8000U || rate > 192000U ||
      small_block < 6U || large_block > 13U || small_block > large_block ||
      (packet[29] & 0x01U) == 0U) {
    return 0;
  }
  if (envelope) {
    envelope->sample_rate = rate;
    envelope->channels = channels;
    envelope->serial = zz9k_sound_vorbis_le32(bytes + 14U);
    envelope->nominal_bitrate = zz9k_sound_vorbis_le32(packet + 20U);
    envelope->max_block_samples = 1UL << large_block;
  }
  return 1;
}

#endif /* ZZ9K_SOUND_VORBIS_H */
