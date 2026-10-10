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
/* Covers a sequence header plus its extension run, so a header straddling
 * a chunk edge is re-read whole with the next chunk. */
#define ZZPLAY_PROBE_CARRY 48U
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

/* MPEG-2 aspect_ratio_information: the display aspect ratio of the coded
 * frame (1 is square samples). MPEG-1 keeps its pixel-aspect meaning and
 * is not mapped here. */
static const uint16_t zzplay_mpeg2_dar_w[5] = {0U, 1U, 4U, 16U, 221U};
static const uint16_t zzplay_mpeg2_dar_h[5] = {0U, 1U, 3U, 9U, 100U};

/* Square-pixel display size of a coded w x h frame: the display aspect
 * applies to the sequence display extension's area when present, else to
 * the whole coded frame. Height is kept, width carries the ratio. */
static void zzplay_mpeg2_display_size(uint32_t width, uint32_t height,
                                      uint8_t aspect, uint32_t ext_w,
                                      uint32_t ext_h,
                                      uint32_t *out_w, uint32_t *out_h)
{
  uint64_t num;
  uint64_t den;

  if (aspect > 4U || height == 0U) {
    *out_w = 0U;
    *out_h = 0U;
    return;
  }
  if (aspect == 1U) {
    /* Square samples: the display extension's area is the display size. */
    *out_w = ext_w != 0U ? ext_w : width;
    *out_h = ext_h != 0U ? ext_h : height;
    return;
  }
  num = (uint64_t)width * zzplay_mpeg2_dar_w[aspect] *
        (ext_h != 0U ? ext_h : height);
  den = (uint64_t)zzplay_mpeg2_dar_h[aspect] *
        (ext_w != 0U ? ext_w : width);
  *out_w = den != 0U ? (uint32_t)(num / den) : width;
  if (*out_w == 0U) {
    *out_w = 1U;
  }
  *out_h = height;
}

/* Walks the extension run that follows a sequence header: the sequence
 * extension (id 1) marks MPEG-2, the sequence display extension (id 2)
 * carries the area the display aspect applies to. Best effort: a run cut
 * short by the buffer end leaves what was seen so far, and the caller
 * re-reads the header with the next chunk's carried bytes. */
static void zzplay_probe_mpeg_extensions(const uint8_t *data,
                                         size_t length,
                                         size_t from,
                                         int *is_mpeg2,
                                         uint32_t *ext_w,
                                         uint32_t *ext_h)
{
  size_t limit = from + 64U; /* a header's extension run is far shorter */
  size_t i;

  for (i = from; i + 4U <= length && i < limit; i++) {
    uint8_t ext_id;
    uint8_t color;
    size_t base;

    if (data[i] != 0x00U || data[i + 1U] != 0x00U ||
        data[i + 2U] != 0x01U || data[i + 3U] != 0xb5U) {
      continue; /* scan forward for the next extension start code */
    }
    if (i + 5U > length) {
      return;
    }
    ext_id = (uint8_t)(data[i + 4U] >> 4);
    color = (uint8_t)(data[i + 4U] & 0x01U);
    if (ext_id == 1U && i + 10U <= length) {
      *is_mpeg2 = 1;
      i += 6U; /* sequence extension body: 6 bytes we do not read */
    } else if (ext_id == 2U) {
      base = i + 5U + (color != 0U ? 3U : 0U);
      if (base + 4U > length) {
        return;
      }
      if ((data[base + 1U] & 0x02U) != 0U) {
        *ext_w = ((uint32_t)data[base] << 6) |
                 (uint32_t)(data[base + 1U] >> 2);
        *ext_h = ((uint32_t)(data[base + 1U] & 0x01U) << 13) |
                 ((uint32_t)data[base + 2U] << 5) |
                 (uint32_t)(data[base + 3U] >> 3);
      }
      i = base + 3U; /* display sizes end mid-byte; the for-loop adds 1 */
    } else {
      /* Not an extension we read: its length is unknown, so the run ends
       * here rather than risking a skip into the next element. */
      return;
    }
  }
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
    int is_mpeg2 = 0;
    uint32_t ext_w = 0U;
    uint32_t ext_h = 0U;

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
    /* A sequence extension after the header means MPEG-2; its display
     * extension (when present) supplies the area the display aspect
     * applies to. Extensions cut short by the buffer end are picked up
     * when the probe re-reads this header with the next chunk's carry. */
    zzplay_probe_mpeg_extensions(data, length, i + 8U, &is_mpeg2,
                                  &ext_w, &ext_h);
    info->width = width;
    info->height = height;
    info->frame_rate_milli = rate;
    is_mpeg2 = is_mpeg2 || info->is_mpeg2;
    info->is_mpeg2 = is_mpeg2;
    info->display_width = 0U;
    info->display_height = 0U;
    if (is_mpeg2) {
      zzplay_mpeg2_display_size(width, height,
                                (uint8_t)(data[i + 7U] >> 4),
                                ext_w, ext_h,
                                &info->display_width,
                                &info->display_height);
    }
    return 1;
  }
  return 0;
}

/* First payload byte offset of a PES packet at `p`, the way the firmware
 * demux computes it: MPEG-2 optional header, else MPEG-1 stuffing and
 * timestamp skipping. Returns 0 when no payload byte is visible. */
static size_t zzplay_pes_payload_at(const uint8_t *p, size_t avail)
{
  size_t payload_at = 6U;
  size_t total;

  if (avail < 6U) {
    return 0U;
  }
  total = 6U + (((size_t)p[4] << 8) | (size_t)p[5]);
  if (avail >= 9U && (p[6] & 0xc0U) == 0x80U) {
    payload_at = 9U + (size_t)p[8];
  } else {
    while (payload_at < total && payload_at < avail &&
           p[payload_at] == 0xffU) {
      payload_at++;
    }
    if (payload_at + 1U < total && payload_at + 1U < avail &&
        (p[payload_at] & 0xc0U) == 0x40U) {
      payload_at += 2U;
    }
    if (payload_at < total && payload_at < avail) {
      if ((p[payload_at] & 0xf0U) == 0x20U) {
        payload_at += 5U;
      } else if ((p[payload_at] & 0xf0U) == 0x30U) {
        payload_at += 10U;
      } else if (p[payload_at] == 0x0fU) {
        payload_at++;
      }
    }
  }
  return payload_at < avail && payload_at < total ? payload_at : 0U;
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
      /* MPEG-2 pack headers start with marker bits '01'; MPEG-1 packs
       * start with '0010'. Only a full 5th byte proves MPEG-2. */
      if (i + 5U <= length && (data[i + 4U] & 0xc0U) == 0x40U) {
        info->is_mpeg2 = 1;
      }
    } else if (stream_id >= 0xe0U && stream_id <= 0xefU) {
      info->has_video_pes = 1;
    } else if (stream_id >= 0xc0U && stream_id <= 0xdfU) {
      /* The firmware demux latches the first MPEG audio stream; AC-3
       * still outranks MP2 when both are present. */
      info->has_audio_pes = 1;
      if (info->audio_codec != ZZPLAY_MEDIA_AUDIO_AC3) {
        info->audio_codec = ZZPLAY_MEDIA_AUDIO_MP2;
      }
    } else if (stream_id == 0xbdU) {
      /* Private stream 1: AC-3 substreams 0x80-0x87, DVD LPCM 0xA0-0xA7.
       * AC-3 wins over MP2, MP2 over LPCM, matching the demux's choice. */
      size_t payload = zzplay_pes_payload_at(data + i, length - i);
      uint8_t sub;

      if (payload != 0U) {
        sub = data[i + payload];
        if (sub >= 0x80U && sub <= 0x87U) {
          info->has_audio_pes = 1;
          info->audio_codec = ZZPLAY_MEDIA_AUDIO_AC3;
        } else if (sub >= 0xa0U && sub <= 0xa7U &&
                   info->audio_codec == ZZPLAY_MEDIA_AUDIO_NONE) {
          info->has_audio_pes = 1;
          info->audio_codec = ZZPLAY_MEDIA_AUDIO_LPCM;
        }
      }
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
    if (!found) {
      found = zzplay_probe_mpeg_sequence(buffer, available, info);
    } else if (!info->is_mpeg2 || info->display_width == 0U) {
      /* Re-read repeat headers so a header whose extensions straddled a
       * chunk edge (or followed an MPEG-2 pack header) still yields the
       * MPEG-2 mark and its display aspect. A no-find changes nothing. */
      (void)zzplay_probe_mpeg_sequence(buffer, available, info);
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

void zzplay_video_window_source(const ZZPlayVideoInfo *info,
                                uint32_t *width, uint32_t *height)
{
  uint32_t long_src;
  uint32_t long_disp;
  uint32_t w;
  uint32_t h;

  if (!info || info->display_width == 0U || info->display_height == 0U) {
    /* No display aspect named (MPEG-1): the coded frame is the window. */
    if (width) {
      *width = info ? info->width : 0U;
    }
    if (height) {
      *height = info ? info->height : 0U;
    }
    return;
  }
  w = info->display_width;
  h = info->display_height;
  if (w < info->width || h < info->height) {
    /* A display size below the coded frame is an aspect (the MPEG-2
     * square-pixel size of an anamorphic frame), not a window: scale it
     * to the coded long side, like the WebM DisplayWidth/Height path. */
    long_src = info->width > info->height ? info->width : info->height;
    long_disp = w > h ? w : h;
    w = (uint32_t)(((uint64_t)w * long_src) / long_disp);
    h = (uint32_t)(((uint64_t)h * long_src) / long_disp);
    if (w == 0U) {
      w = 1U;
    }
    if (h == 0U) {
      h = 1U;
    }
  }
  if (width) {
    *width = w;
  }
  if (height) {
    *height = h;
  }
}
