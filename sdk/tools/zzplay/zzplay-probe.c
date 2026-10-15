/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-probe.h"

#include "zz9k/image.h"
/* Shared with the sound DataType so both claim exactly the same files. */
#include "../../amiga/datatypes/zz9k_sound_flac.h"
#include "../../amiga/datatypes/zz9k_sound_vorbis.h"
#include "zzplay-sys.h"
#include <string.h>

#define ZZPLAY_PROBE_BYTES (256U * 1024U)
#define ZZPLAY_PROBE_CHUNK 4096U
/* Bytes kept from one window to the next: the longest structure the probe
 * reads after a start code, so one that straddles a chunk edge is re-read
 * whole. That is a PES packet's header (9 bytes plus up to 255 header data
 * bytes) and the first 6 bytes of its payload (a DVD LPCM substream header
 * up to its format byte); a sequence header with both quantiser matrices
 * and its extension run (12 + 2 * 64 + 64 bytes) is shorter. */
#define ZZPLAY_PROBE_CARRY (9U + 255U + 6U)
typedef char zzplay_probe_carry_check[
    (ZZPLAY_PROBE_CARRY >= 12U + 2U * 64U + 64U) ? 1 : -1];
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
    *out_w = width;
    *out_h = height;
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
                                         uint32_t *ext_h,
                                         int *has_display_extension)
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
      *has_display_extension = 1;
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

  if (!data || !info || length < 12U) {
    return 0;
  }
  for (i = 0U; i + 12U <= length; i++) {
    uint32_t width;
    uint32_t height;
    uint32_t rate;
    size_t extensions_at;
    uint8_t load_intra;
    uint8_t load_non_intra = 0U;
    int matrices_visible = 1;
    int is_mpeg2 = 0;
    int has_display_extension = 0;
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
    /* A sequence extension after the full header means MPEG-2. The intra
     * matrix begins in byte 11's low bit, so its trailing byte carries the
     * following non-intra flag. Do not scan a partial matrix as extensions. */
    load_intra = (uint8_t)(data[i + 11U] & 0x02U);
    extensions_at = i + 12U;
    if (load_intra != 0U) {
      if (length < extensions_at + 64U) {
        matrices_visible = 0;
      } else {
        load_non_intra = (uint8_t)(data[i + 75U] & 0x01U);
        extensions_at += 64U;
      }
    } else {
      load_non_intra = (uint8_t)(data[i + 11U] & 0x01U);
    }
    if (matrices_visible && load_non_intra != 0U) {
      if (length < extensions_at + 64U) {
        matrices_visible = 0;
      } else {
        extensions_at += 64U;
      }
    }
    if (matrices_visible) {
      zzplay_probe_mpeg_extensions(data, length, extensions_at, &is_mpeg2,
                                    &ext_w, &ext_h,
                                    &has_display_extension);
    }
    info->width = width;
    info->height = height;
    info->frame_rate_milli = rate;
    is_mpeg2 = is_mpeg2 || info->is_mpeg2;
    info->is_mpeg2 = is_mpeg2;
    info->has_sequence_display_extension = has_display_extension;
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
  if (avail >= 7U && (p[6] & 0xc0U) == 0x80U) {
    /* An MPEG-2 header's length byte places the payload; a header the
     * window cuts is left to the next window, not read as MPEG-1. */
    if (avail < 9U) {
      return 0U;
    }
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

/* DVD LPCM format byte (sixth byte of the substream header): quantisation
 * in bits 7-6, sample rate in bits 5-4, channels minus one in bits 2-0.
 * The firmware's sdk_dvd_lpcm_header() takes 16-bit 48 kHz mono or stereo
 * only. */
static int zzplay_lpcm_format_supported(uint8_t format)
{
  return (format & 0xf0U) == 0U && (format & 0x07U) <= 1U;
}

int zzplay_mpeg_needs_mpeg2_backend(const ZZPlayVideoInfo *info,
                                    int audio_decoded)
{
  /* The MPEG-2 backend decodes MPEG-1 video too; it is needed for MPEG-2
   * video and for decoded DVD audio, since the MPEG-1 backend takes MP2
   * only. MPEG-1 video with MP2, or played without audio, stays on the
   * release MPEG-1 service whatever the pack syntax. */
  return info->is_mpeg2 ||
         (audio_decoded &&
          (info->audio_codec == ZZPLAY_MEDIA_AUDIO_AC3 ||
           info->audio_codec == ZZPLAY_MEDIA_AUDIO_LPCM));
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
      /* The pack header's MPEG-2 syntax says nothing about the video
       * codec: DVDs and VCD remuxes carry MPEG-1 video in MPEG-2 packs.
       * Only a sequence extension marks MPEG-2 video. */
      info->is_program_stream = 1;
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
      size_t avail = length - i;
      size_t payload = zzplay_pes_payload_at(data + i, avail);
      size_t total;
      uint8_t sub;

      if (payload != 0U) {
        /* A non-zero payload offset proves the 6-byte PES header is
         * visible. */
        total = 6U + (((size_t)data[i + 4U] << 8) | data[i + 5U]);
        sub = data[i + payload];
        if (sub >= 0x80U && sub <= 0x87U) {
          info->has_audio_pes = 1;
          info->audio_codec = ZZPLAY_MEDIA_AUDIO_AC3;
        } else if (sub >= 0xa0U && sub <= 0xa7U &&
                   info->audio_codec == ZZPLAY_MEDIA_AUDIO_NONE &&
                   payload + 5U < avail && payload + 5U < total &&
                   zzplay_lpcm_format_supported(data[i + payload + 5U])) {
          /* The firmware drops every LPCM packet whose format byte is not
           * 48 kHz/16-bit mono or stereo, so other LPCM is no audio. */
          info->has_audio_pes = 1;
          info->audio_codec = ZZPLAY_MEDIA_AUDIO_LPCM;
        }
      }
    }
  }
}

/* Sequence-header scan over a byte window whose last ZZPLAY_PROBE_CARRY
 * bytes are kept for the next window. */
typedef struct ZZPlaySeqScan {
  uint8_t *data;
  size_t length;
  int found;
} ZZPlaySeqScan;

static void zzplay_seq_carry(ZZPlaySeqScan *scan)
{
  if (scan->length > ZZPLAY_PROBE_CARRY) {
    memmove(scan->data, scan->data + scan->length - ZZPLAY_PROBE_CARRY,
            ZZPLAY_PROBE_CARRY);
    scan->length = ZZPLAY_PROBE_CARRY;
  }
}

static void zzplay_seq_scan(ZZPlaySeqScan *scan, ZZPlayVideoInfo *info)
{
  if (!scan->found) {
    scan->found = zzplay_probe_mpeg_sequence(scan->data, scan->length, info);
  } else if (!info->is_mpeg2 || !info->has_sequence_display_extension) {
    /* Re-read repeat headers so a header whose extensions straddled a
     * window edge still yields the MPEG-2 mark and its display extension.
     * A no-find changes nothing. */
    (void)zzplay_probe_mpeg_sequence(scan->data, scan->length, info);
  }
  zzplay_seq_carry(scan);
}

static void zzplay_seq_put(ZZPlaySeqScan *scan, ZZPlayVideoInfo *info,
                           const uint8_t *src, size_t length)
{
  while (length != 0U) {
    size_t room = ZZPLAY_PROBE_CHUNK + ZZPLAY_PROBE_CARRY - scan->length;
    size_t take = length < room ? length : room;

    memcpy(scan->data + scan->length, src, take);
    scan->length += take;
    src += take;
    length -= take;
    if (scan->length == ZZPLAY_PROBE_CHUNK + ZZPLAY_PROBE_CARRY) {
      zzplay_seq_scan(scan, info);
    }
  }
}

/* Program Stream demux reduced to what the probe needs: the payload of the
 * first video stream's PES packets, so a sequence header and its
 * extensions are read contiguously even when a muxer split them across
 * packets and put other streams' packets in between. Like the firmware
 * demux, it follows the first video stream ID only. */
typedef struct ZZPlayPSDemux {
  uint32_t shift;    /* last four bytes outside packets, for start codes */
  uint32_t skip;     /* bytes of the current packet to drop */
  uint32_t copy;     /* video payload bytes of the current packet left */
  uint32_t total;    /* current PES packet length, start code included */
  uint32_t hdr_len;  /* header bytes collected, start code included */
  uint32_t hdr_need; /* collect until hdr_len reaches it; 0 when idle */
  uint8_t video_id;  /* latched video stream ID; 0 until the first one */
  uint8_t hdr[48];   /* MPEG-1 PES headers end within 6 + 34 bytes */
} ZZPlayPSDemux;

static void zzplay_ps_header(ZZPlayPSDemux *d, ZZPlaySeqScan *es,
                             ZZPlayVideoInfo *info)
{
  uint8_t id = d->hdr[3];
  size_t payload;

  d->hdr_need = 0U;
  if (id == 0xbaU) {
    /* MPEG-2 pack: 14 bytes plus stuffing; MPEG-1 pack: 12 bytes. */
    if (d->hdr_len == 5U) {
      d->hdr_need = (d->hdr[4] & 0xc0U) == 0x40U ? 14U : 12U;
    } else if (d->hdr_len == 14U) {
      d->skip = d->hdr[13] & 0x07U;
    }
    return;
  }
  if (d->hdr_len == 6U) {
    d->total = 6U + (((uint32_t)d->hdr[4] << 8) | d->hdr[5]);
    if (id >= 0xe0U && id <= 0xefU && d->video_id == 0U) {
      d->video_id = id;
    }
    if (id != d->video_id) {
      d->skip = d->total - 6U; /* system header, audio, other video, ... */
    } else if (d->total > 6U) {
      d->hdr_need = d->total < 9U ? d->total : 9U;
    }
    return;
  }
  if (d->hdr_len >= 9U && (d->hdr[6] & 0xc0U) == 0x80U) {
    payload = 9U + (size_t)d->hdr[8];
    if (payload >= d->total) {
      d->skip = d->total - 9U;
    } else {
      d->skip = (uint32_t)(payload - 9U);
      d->copy = d->total - (uint32_t)payload;
    }
    return;
  }
  if (d->hdr_len < d->total && d->hdr_len < 40U) {
    d->hdr_need = d->total < 40U ? d->total : 40U;
    return;
  }
  payload = zzplay_pes_payload_at(d->hdr, d->hdr_len);
  if (payload != 0U) {
    zzplay_seq_put(es, info, d->hdr + payload, d->hdr_len - payload);
    d->copy = d->total - d->hdr_len;
  } else {
    d->skip = d->total - d->hdr_len;
  }
}

static void zzplay_ps_demux(ZZPlayPSDemux *d, const uint8_t *data,
                            size_t length, ZZPlaySeqScan *es,
                            ZZPlayVideoInfo *info)
{
  size_t i = 0U;

  while (i < length) {
    size_t take;

    if (d->skip != 0U) {
      take = length - i < d->skip ? length - i : d->skip;
      d->skip -= (uint32_t)take;
      i += take;
    } else if (d->copy != 0U) {
      take = length - i < d->copy ? length - i : d->copy;
      zzplay_seq_put(es, info, data + i, take);
      d->copy -= (uint32_t)take;
      i += take;
    } else if (d->hdr_need != 0U) {
      d->hdr[d->hdr_len++] = data[i++];
      if (d->hdr_len == d->hdr_need) {
        zzplay_ps_header(d, es, info);
      }
    } else {
      d->shift = (d->shift << 8) | data[i++];
      if ((d->shift & 0xffffff00U) == 0x00000100U &&
          (d->shift & 0xffU) > 0xb9U) {
        /* A pack or PES start code; the program end code (b9) has no
         * body. */
        d->hdr[0] = 0x00U;
        d->hdr[1] = 0x00U;
        d->hdr[2] = 0x01U;
        d->hdr[3] = (uint8_t)(d->shift & 0xffU);
        d->hdr_len = 4U;
        d->hdr_need = d->hdr[3] == 0xbaU ? 5U : 6U;
        d->shift = 0xffffffffU;
      }
    }
  }
}

int zzplay_probe_file(ZZPlayFile *file, ZZPlayVideoInfo *info)
{
  uint8_t *buffer;
  uint8_t *es_buffer;
  ZZPlaySeqScan raw;
  ZZPlaySeqScan es;
  ZZPlayPSDemux demux;
  size_t total = 0U;

  if (!file || !info ||
      !zzplay_file_seek(file, 0L, ZZPLAY_FILE_BEGIN)) {
    return 0;
  }
  memset(info, 0, sizeof(*info));
  buffer = (uint8_t *)zzplay_alloc(ZZPLAY_PROBE_CHUNK + ZZPLAY_PROBE_CARRY);
  es_buffer = (uint8_t *)zzplay_alloc(ZZPLAY_PROBE_CHUNK + ZZPLAY_PROBE_CARRY);
  if (!buffer || !es_buffer) {
    zzplay_free(es_buffer);
    zzplay_free(buffer);
    return 0;
  }
  memset(&raw, 0, sizeof(raw));
  memset(&es, 0, sizeof(es));
  memset(&demux, 0, sizeof(demux));
  raw.data = buffer;
  es.data = es_buffer;
  demux.shift = 0xffffffffU;
  while (total < ZZPLAY_PROBE_BYTES) {
    size_t want = ZZPLAY_PROBE_CHUNK;
    size_t got;

    if (want > ZZPLAY_PROBE_BYTES - total) {
      want = ZZPLAY_PROBE_BYTES - total;
    }
    got = zzplay_file_read(file, buffer + raw.length, want);
    raw.length += got;
    zzplay_probe_mpeg_program(buffer, raw.length, info);
    if (info->is_program_stream) {
      /* Packs start a Program Stream, so the demux sees it from its
       * first pack. */
      zzplay_ps_demux(&demux, buffer + raw.length - got, got, &es, info);
      zzplay_seq_scan(&es, info);
      zzplay_seq_carry(&raw);
    } else {
      zzplay_seq_scan(&raw, info);
    }
    total += got;
    if (got == 0U) {
      break;
    }
  }
  zzplay_free(es_buffer);
  zzplay_free(buffer);
  if (!zzplay_file_seek(file, 0L, ZZPLAY_FILE_BEGIN)) {
    zzplay_file_clear_error(file);
    return 0;
  }
  zzplay_file_clear_error(file);
  return raw.found || es.found;
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

static int zzplay_frame_size_supported(uint32_t width, uint32_t height)
{
  uint32_t side;

  if (width == 0U || height == 0U) {
    return 0;
  }
  side = width > height ? width : height;
  return side <= ZZPLAY_MAX_SIDE &&
         width <= ZZPLAY_MAX_PIXELS / height;
}

int zzplay_webp_info_supported(const ZZPlayWebPInfo *info)
{
  return info && info->is_animated &&
         info->width >= 16U && info->height >= 16U &&
         info->width <= ZZPLAY_WEBP_MAX_WIDTH &&
         info->height <= ZZPLAY_WEBP_MAX_HEIGHT;
}

int zzplay_probe_media_file(ZZPlayFile *file, ZZPlayProbeInfo *info)
{
  static uint8_t buffer[ZZPLAY_MP3_PROBE_BYTES];
  uint8_t id3[10];
  long audio_offset = 0L;
  size_t got;

  if (!file || !info) {
    return 0;
  }
  memset(info, 0, sizeof(*info));

  if (!zzplay_file_seek(file, 0L, ZZPLAY_FILE_BEGIN)) {
    zzplay_file_clear_error(file);
    return 0;
  }
  got = zzplay_file_read(file, buffer, sizeof(buffer));
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

  zzplay_file_clear_error(file);
  if (!zzplay_file_seek(file, 0L, ZZPLAY_FILE_BEGIN)) {
    zzplay_file_clear_error(file);
    return 0;
  }

  if (zzplay_probe_file(file, &info->video) &&
      info->video.is_program_stream &&
      info->video.has_video_pes) {
    info->kind = ZZPLAY_MEDIA_KIND_MPEG_PS;
    return 1;
  }
  if (zzplay_file_read(file, id3, sizeof(id3)) == sizeof(id3) &&
      memcmp(id3, "ID3", 3U) == 0) {
    uint32_t tag_bytes = zzplay_synchsafe_u28(id3 + 6U);

    if (tag_bytes == UINT32_MAX) {
      goto done;
    }
    audio_offset = (long)(10U + tag_bytes +
                          ((id3[5] & 0x10U) != 0U ? 10U : 0U));
  }
  zzplay_file_clear_error(file);
  if (!zzplay_file_seek(file, audio_offset, ZZPLAY_FILE_BEGIN)) {
    goto done;
  }
  got = zzplay_file_read(file, buffer, sizeof(buffer));
  if (!zzplay_file_error(file) && zzplay_probe_mp3(buffer, got, &info->mp3)) {
    info->kind = ZZPLAY_MEDIA_KIND_MP3;
  }

done:
  zzplay_file_clear_error(file);
  if (!zzplay_file_seek(file, 0L, ZZPLAY_FILE_BEGIN)) {
    zzplay_file_clear_error(file);
    return 0;
  }
  return info->kind != ZZPLAY_MEDIA_KIND_UNSUPPORTED;
}

int zzplay_video_info_supported(const ZZPlayVideoInfo *info)
{
  return info && info->width >= 16U && info->height >= 16U &&
         zzplay_frame_size_supported(info->width, info->height) &&
         info->frame_rate_milli != 0U;
}

int zzplay_ogg_last_granule(ZZPlayFile *file, uint32_t serial,
                            uint64_t *granule)
{
  static uint8_t tail[64U * 1024U];
  long size;
  long start;
  size_t got;
  size_t i;

  if (!file || !granule ||
      !zzplay_file_seek(file, 0L, ZZPLAY_FILE_END) ||
      (size = zzplay_file_tell(file)) < 27L) {
    zzplay_file_clear_error(file);
    return 0;
  }
  start = size > (long)sizeof(tail) ? size - (long)sizeof(tail) : 0L;
  if (!zzplay_file_seek(file, start, ZZPLAY_FILE_BEGIN)) {
    zzplay_file_clear_error(file);
    return 0;
  }
  got = zzplay_file_read(file, tail, (size_t)(size - start));
  zzplay_file_clear_error(file);
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
