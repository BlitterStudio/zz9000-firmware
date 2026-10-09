/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-probe.h"

#include "zz9k/image.h"
/* Shared with the sound DataType so both claim exactly the same files. */
#include "../../amiga/datatypes/zz9k_sound_flac.h"
#include "../../amiga/datatypes/zz9k_sound_vorbis.h"
#include <stdlib.h>
#include <string.h>

#define ZZPLAY_PROBE_BYTES (256U * 1024U)
#define ZZPLAY_PROBE_CHUNK 4096U
#define ZZPLAY_PROBE_CARRY 7U
#define ZZPLAY_MP3_PROBE_BYTES 4096U

static uint32_t zzplay_synchsafe_u28(const uint8_t *data)
{
  if ((data[0] | data[1] | data[2] | data[3]) & 0x80U) {
    return UINT32_MAX;
  }
  return ((uint32_t)data[0] << 21) |
         ((uint32_t)data[1] << 14) |
         ((uint32_t)data[2] << 7) |
         (uint32_t)data[3];
}

int zzplay_probe_mp3_frame(const uint8_t *data,
                           size_t length,
                           ZZPlayMP3Info *info)
{
  static const uint16_t mpeg1_bitrates[16] = {
    0U, 32U, 40U, 48U, 56U, 64U, 80U, 96U,
    112U, 128U, 160U, 192U, 224U, 256U, 320U, 0U
  };
  static const uint16_t mpeg2_bitrates[16] = {
    0U, 8U, 16U, 24U, 32U, 40U, 48U, 56U,
    64U, 80U, 96U, 112U, 128U, 144U, 160U, 0U
  };
  static const uint32_t sample_rates[3] = {44100U, 48000U, 32000U};
  uint32_t version_bits;
  uint32_t bitrate_index;
  uint32_t sample_index;
  uint32_t bitrate;
  uint32_t sample_rate;
  uint32_t coefficient;

  if (!data || !info || length < 4U || data[0] != 0xffU ||
      (data[1] & 0xe0U) != 0xe0U) {
    return 0;
  }
  version_bits = (data[1] >> 3) & 3U;
  if (version_bits == 1U || ((data[1] >> 1) & 3U) != 1U) {
    return 0;
  }
  bitrate_index = (data[2] >> 4) & 0x0fU;
  sample_index = (data[2] >> 2) & 3U;
  if (sample_index == 3U) {
    return 0;
  }
  bitrate = version_bits == 3U
                ? mpeg1_bitrates[bitrate_index]
                : mpeg2_bitrates[bitrate_index];
  if (bitrate == 0U) {
    return 0;
  }
  sample_rate = sample_rates[sample_index];
  if (version_bits == 2U) {
    sample_rate /= 2U;
  } else if (version_bits == 0U) {
    sample_rate /= 4U;
  }
  coefficient = version_bits == 3U ? 144000U : 72000U;
  memset(info, 0, sizeof(*info));
  info->sample_rate = sample_rate;
  info->channels = (data[3] & 0xc0U) == 0xc0U ? 1U : 2U;
  info->bitrate_kbps = bitrate;
  info->frame_bytes =
      coefficient * bitrate / sample_rate + ((data[2] >> 1) & 1U);
  info->mpeg_version = version_bits == 3U ? 1U
                       : version_bits == 2U ? 2U : 25U;
  return info->frame_bytes >= 4U;
}

int zzplay_probe_mp3(const uint8_t *data,
                     size_t length,
                     ZZPlayMP3Info *info)
{
  size_t offset = 0U;

  if (!data || !info) {
    return 0;
  }
  if (length >= 10U && memcmp(data, "ID3", 3U) == 0) {
    uint32_t tag_bytes = zzplay_synchsafe_u28(data + 6U);
    if (tag_bytes == UINT32_MAX || tag_bytes > length - 10U) {
      return 0;
    }
    offset = 10U + tag_bytes;
    if ((data[5] & 0x10U) != 0U) {
      if (offset > length - 10U) {
        return 0;
      }
      offset += 10U;
    }
  }
  for (; offset + 4U <= length; offset++) {
    ZZPlayMP3Info candidate;

    if (!zzplay_probe_mp3_frame(data + offset, length - offset,
                                &candidate)) {
      continue;
    }
    /* If another complete frame header is in the probe, require it to be
     * Layer III too. This rejects most sync-like byte sequences in junk. */
    if (candidate.frame_bytes <= length - offset - 4U) {
      ZZPlayMP3Info next;
      if (!zzplay_probe_mp3_frame(data + offset + candidate.frame_bytes,
                                  length - offset - candidate.frame_bytes,
                                  &next)) {
        continue;
      }
    }
    *info = candidate;
    return 1;
  }
  return 0;
}

uint32_t zzplay_mpeg_frame_rate_milli(uint8_t code)
{
  static const uint32_t rates[16] = {
    0U, 23976U, 24000U, 25000U, 29970U, 30000U, 50000U, 59940U,
    60000U, 0U, 0U, 0U, 0U, 0U, 0U, 0U
  };

  return rates[code & 0x0fU];
}

int zzplay_probe_mpeg_sequence(const uint8_t *data,
                               size_t length,
                               ZZPlayVideoInfo *info)
{
  size_t i;

  if (!data || !info || length < 8U) {
    return 0;
  }
  for (i = 0U; i + 8U <= length; i++) {
    uint32_t width;
    uint32_t height;
    uint32_t rate;

    if (data[i] != 0x00U || data[i + 1U] != 0x00U ||
        data[i + 2U] != 0x01U || data[i + 3U] != 0xb3U) {
      continue;
    }
    width = ((uint32_t)data[i + 4U] << 4) |
            (uint32_t)(data[i + 5U] >> 4);
    height = ((uint32_t)(data[i + 5U] & 0x0fU) << 8) |
             (uint32_t)data[i + 6U];
    rate = zzplay_mpeg_frame_rate_milli(data[i + 7U]);
    if (width == 0U || height == 0U || rate == 0U) {
      continue;
    }
    info->width = width;
    info->height = height;
    info->frame_rate_milli = rate;
    return 1;
  }
  return 0;
}

void zzplay_probe_mpeg_program(const uint8_t *data,
                               size_t length,
                               ZZPlayVideoInfo *info)
{
  size_t i;

  if (!data || !info || length < 4U) {
    return;
  }
  for (i = 0U; i + 4U <= length; i++) {
    uint8_t stream_id;

    if (data[i] != 0x00U || data[i + 1U] != 0x00U ||
        data[i + 2U] != 0x01U) {
      continue;
    }
    stream_id = data[i + 3U];
    if (stream_id == 0xbaU) {
      info->is_program_stream = 1;
    } else if (stream_id >= 0xe0U && stream_id <= 0xefU) {
      info->has_video_pes = 1;
    } else if (stream_id >= 0xc0U && stream_id <= 0xdfU) {
      info->has_audio_pes = 1;
    }
  }
}

int zzplay_probe_file(FILE *file, ZZPlayVideoInfo *info)
{
  uint8_t *buffer;
  size_t carry = 0U;
  size_t total = 0U;
  int found = 0;

  if (!file || !info || fseek(file, 0L, SEEK_SET) != 0) {
    return 0;
  }
  memset(info, 0, sizeof(*info));
  buffer = (uint8_t *)malloc(ZZPLAY_PROBE_CHUNK + ZZPLAY_PROBE_CARRY);
  if (!buffer) {
    return 0;
  }
  while (total < ZZPLAY_PROBE_BYTES) {
    size_t want = ZZPLAY_PROBE_CHUNK;
    size_t got;
    size_t available;

    if (want > ZZPLAY_PROBE_BYTES - total) {
      want = ZZPLAY_PROBE_BYTES - total;
    }
    got = fread(buffer + carry, 1U, want, file);
    available = carry + got;
    zzplay_probe_mpeg_program(buffer, available, info);
    if (!found &&
        zzplay_probe_mpeg_sequence(buffer, available, info)) {
      found = 1;
    }
    total += got;
    if (got == 0U) {
      break;
    }
    carry = available < ZZPLAY_PROBE_CARRY
                ? available
                : ZZPLAY_PROBE_CARRY;
    memmove(buffer, buffer + available - carry, carry);
  }
  free(buffer);
  if (fseek(file, 0L, SEEK_SET) != 0) {
    clearerr(file);
    return 0;
  }
  clearerr(file);
  return found;
}

int zzplay_probe_webp(const uint8_t *data,
                      size_t length,
                      ZZPlayWebPInfo *info)
{
  ZZ9KWebPHeader header;
  ZZ9KWebPParseStatus status;

  if (info) {
    memset(info, 0, sizeof(*info));
  }
  if (!data || length < ZZ9K_WEBP_HEADER_MIN_BYTES) {
    return 0;
  }
  status = zz9k_webp_parse_header(data, (uint32_t)length, &header);
  if (status != ZZ9K_WEBP_PARSE_READY) {
    return 0;
  }
  if (info) {
    info->width = header.width;
    info->height = header.height;
    info->format = header.format;
    info->is_animated = header.is_animated ? 1 : 0;
    info->has_alpha = header.has_alpha ? 1 : 0;
  }
  return 1;
}

int zzplay_webp_info_supported(const ZZPlayWebPInfo *info)
{
  return info && info->is_animated &&
         info->width >= 16U && info->height >= 16U &&
         info->width <= ZZPLAY_MAX_WIDTH &&
         info->height <= ZZPLAY_MAX_HEIGHT;
}

int zzplay_probe_media_file(FILE *file, ZZPlayProbeInfo *info)
{
  static uint8_t buffer[ZZPLAY_MP3_PROBE_BYTES];
  uint8_t id3[10];
  long audio_offset = 0L;
  size_t got;

  if (!file || !info) {
    return 0;
  }
  memset(info, 0, sizeof(*info));

  if (fseek(file, 0L, SEEK_SET) != 0) {
    clearerr(file);
    return 0;
  }
  got = fread(buffer, 1U, sizeof(buffer), file);
  if (got >= 4U && buffer[0] == 0x1AU && buffer[1] == 0x45U &&
      buffer[2] == 0xDFU && buffer[3] == 0xA3U &&
      zzplay_probe_webm_file(file, &info->webm)) {
    info->kind = ZZPLAY_MEDIA_KIND_WEBM;
    goto done;
  }
  if (got >= ZZ9K_WEBP_HEADER_MIN_BYTES &&
      zzplay_probe_webp(buffer, got, &info->webp)) {
    if (info->webp.is_animated) {
      info->kind = ZZPLAY_MEDIA_KIND_WEBP;
    } else {
      info->kind = ZZPLAY_MEDIA_KIND_UNSUPPORTED;
    }
    goto done;
  }
  {
    ZZ9KSoundFlacEnvelope flac;

    if (zz9k_sound_recognize_flac(buffer, (uint32_t)got, &flac)) {
      info->flac.sample_rate = flac.sample_rate;
      info->flac.channels = flac.channels;
      info->flac.bits_per_sample = flac.bits_per_sample;
      info->flac.total_samples = flac.total_samples;
      info->flac.max_block_size = flac.max_block_size;
      info->flac.max_frame_bytes = zz9k_sound_flac_max_frame_bytes(&flac);
      info->kind = ZZPLAY_MEDIA_KIND_FLAC;
      goto done;
    }
  }
  {
    ZZ9KSoundVorbisEnvelope vorbis;

    if (zz9k_sound_recognize_vorbis(buffer, (uint32_t)got, &vorbis)) {
      info->vorbis.sample_rate = vorbis.sample_rate;
      info->vorbis.channels = vorbis.channels;
      info->vorbis.serial = vorbis.serial;
      info->vorbis.nominal_bitrate = vorbis.nominal_bitrate;
      info->vorbis.max_block_samples = vorbis.max_block_samples;
      info->kind = ZZPLAY_MEDIA_KIND_VORBIS;
      goto done;
    }
  }

  clearerr(file);
  if (fseek(file, 0L, SEEK_SET) != 0) {
    clearerr(file);
    return 0;
  }

  if (zzplay_probe_file(file, &info->video) &&
      info->video.is_program_stream &&
      info->video.has_video_pes) {
    info->kind = ZZPLAY_MEDIA_KIND_MPEG_PS;
    return 1;
  }
  if (fread(id3, 1U, sizeof(id3), file) == sizeof(id3) &&
      memcmp(id3, "ID3", 3U) == 0) {
    uint32_t tag_bytes = zzplay_synchsafe_u28(id3 + 6U);

    if (tag_bytes == UINT32_MAX) {
      goto done;
    }
    audio_offset = (long)(10U + tag_bytes +
                          ((id3[5] & 0x10U) != 0U ? 10U : 0U));
  }
  clearerr(file);
  if (fseek(file, audio_offset, SEEK_SET) != 0) {
    goto done;
  }
  got = fread(buffer, 1U, sizeof(buffer), file);
  if (!ferror(file) && zzplay_probe_mp3(buffer, got, &info->mp3)) {
    info->kind = ZZPLAY_MEDIA_KIND_MP3;
  }

done:
  clearerr(file);
  if (fseek(file, 0L, SEEK_SET) != 0) {
    clearerr(file);
    return 0;
  }
  return info->kind != ZZPLAY_MEDIA_KIND_UNSUPPORTED;
}

int zzplay_video_info_supported(const ZZPlayVideoInfo *info)
{
  return info && info->width >= 16U && info->height >= 16U &&
         info->width <= ZZPLAY_MAX_WIDTH &&
         info->height <= ZZPLAY_MAX_HEIGHT &&
         info->frame_rate_milli != 0U;
}

int zzplay_ogg_last_granule(FILE *file, uint32_t serial, uint64_t *granule)
{
  static uint8_t tail[64U * 1024U];
  long size;
  long start;
  size_t got;
  size_t i;

  if (!file || !granule || fseek(file, 0L, SEEK_END) != 0 ||
      (size = ftell(file)) < 27L) {
    clearerr(file);
    return 0;
  }
  start = size > (long)sizeof(tail) ? size - (long)sizeof(tail) : 0L;
  if (fseek(file, start, SEEK_SET) != 0) {
    clearerr(file);
    return 0;
  }
  got = fread(tail, 1U, (size_t)(size - start), file);
  clearerr(file);
  for (i = got >= 27U ? got - 27U + 1U : 0U; i-- > 0U;) {
    const uint8_t *page = tail + i;
    uint64_t position = 0U;
    int b;

    if (page[0] != 'O' || page[1] != 'g' || page[2] != 'g' ||
        page[3] != 'S' || page[4] != 0U ||
        zz9k_sound_vorbis_le32(page + 14U) != serial ||
        i + 27U + page[26] > got) {
      continue;
    }
    for (b = 7; b >= 0; b--) {
      position = (position << 8) | page[6 + b];
    }
    /* All ones marks a page on which no packet ends. */
    if (position != UINT64_MAX) {
      *granule = position;
      return 1;
    }
  }
  return 0;
}
