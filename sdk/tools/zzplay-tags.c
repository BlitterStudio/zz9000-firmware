/* ID3 metadata parser for zzplay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-tags.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t zzplay_syncsafe32(const uint8_t *b)
{
  return ((uint32_t)(b[0] & 0x7FU) << 21) |
         ((uint32_t)(b[1] & 0x7FU) << 14) |
         ((uint32_t)(b[2] & 0x7FU) << 7) |
         (uint32_t)(b[3] & 0x7FU);
}

static void zzplay_trim_trailing(char *s)
{
  size_t len;

  if (!s) {
    return;
  }
  len = strlen(s);
  while (len > 0U && ((unsigned char)s[len - 1U] <= ' ' || s[len - 1U] == '\0')) {
    s[--len] = '\0';
  }
}

static void zzplay_decode_id3_text(char *out, size_t out_cap, uint8_t encoding,
                                   const uint8_t *data, size_t len)
{
  size_t out_len = 0U;

  if (!out || out_cap == 0U) {
    return;
  }
  out[0] = '\0';
  if (!data || len == 0U) {
    return;
  }

  if (encoding == 0U) {
    /* ISO-8859-1 (Latin-1) */
    while (len > 0U && *data != '\0' && out_len + 1U < out_cap) {
      out[out_len++] = (char)*data++;
      len--;
    }
  } else if (encoding == 1U || encoding == 2U) {
    /* UTF-16 */
    int be = 1;
    if (encoding == 1U) {
      if (len < 2U) {
        return;
      }
      if (data[0] == 0xFEU && data[1] == 0xFFU) {
        be = 1;
        data += 2;
        len -= 2;
      } else if (data[0] == 0xFFU && data[1] == 0xFEU) {
        be = 0;
        data += 2;
        len -= 2;
      }
    }
    while (len >= 2U && out_len + 1U < out_cap) {
      uint16_t u = be ? (((uint16_t)data[0] << 8) | (uint16_t)data[1])
                      : (((uint16_t)data[1] << 8) | (uint16_t)data[0]);
      data += 2;
      len -= 2;
      if (u == 0U) {
        break;
      }
      if (u >= 0xD800U && u <= 0xDBFFU) {
        /* Surrogate pair */
        if (len >= 2U) {
          uint16_t low = be ? (((uint16_t)data[0] << 8) | (uint16_t)data[1])
                            : (((uint16_t)data[1] << 8) | (uint16_t)data[0]);
          if (low >= 0xDC00U && low <= 0xDFFFU) {
            data += 2;
            len -= 2;
          }
        }
        out[out_len++] = '?';
      } else if (u >= 0xDC00U && u <= 0xDFFFU) {
        out[out_len++] = '?';
      } else if (u <= 0xFFU) {
        out[out_len++] = (char)(uint8_t)u;
      } else {
        out[out_len++] = '?';
      }
    }
  } else if (encoding == 3U) {
    /* UTF-8 */
    while (len > 0U && out_len + 1U < out_cap) {
      uint8_t c = *data++;
      len--;
      if (c == '\0') {
        break;
      }
      if (c < 0x80U) {
        out[out_len++] = (char)c;
      } else if ((c & 0xE0U) == 0xC0U) {
        if (len < 1U) {
          out[out_len++] = '?';
          break;
        }
        {
          uint8_t c2 = *data++;
          uint32_t cp = ((uint32_t)(c & 0x1FU) << 6) | (uint32_t)(c2 & 0x3FU);
          len--;
          out[out_len++] = (cp <= 0xFFU) ? (char)(uint8_t)cp : '?';
        }
      } else if ((c & 0xF0U) == 0xE0U) {
        if (len < 2U) {
          out[out_len++] = '?';
          break;
        }
        data += 2;
        len -= 2;
        out[out_len++] = '?';
      } else if ((c & 0xF8U) == 0xF0U) {
        if (len < 3U) {
          out[out_len++] = '?';
          break;
        }
        data += 3;
        len -= 3;
        out[out_len++] = '?';
      } else {
        out[out_len++] = '?';
      }
    }
  }

  out[out_len] = '\0';
  zzplay_trim_trailing(out);
}

uint32_t zzplay_tags_id3v2_size(const uint8_t *header, size_t length)
{
  uint8_t ver;
  uint8_t flags;
  uint32_t tag_body;
  uint32_t total;

  if (!header || length < 10U) {
    return 0U;
  }
  if (header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
    return 0U;
  }
  ver = header[3];
  if (ver < 2U || ver > 4U) {
    return 0U;
  }
  flags = header[5];
  if ((header[6] & 0x80U) || (header[7] & 0x80U) ||
      (header[8] & 0x80U) || (header[9] & 0x80U)) {
    return 0U;
  }
  tag_body = zzplay_syncsafe32(&header[6]);
  total = 10U + tag_body;
  if (ver == 4U && (flags & 0x10U)) {
    /* v2.4 footer present */
    total += 10U;
  }
  return total;
}

int zzplay_tags_parse_id3v2(const uint8_t *data, size_t length,
                            ZZPlayTags *tags)
{
  uint8_t ver;
  uint8_t flags;
  uint32_t total_size;
  size_t limit;
  size_t offset = 10U;

  if (!data || !tags || length < 10U) {
    return 0;
  }
  total_size = zzplay_tags_id3v2_size(data, length);
  if (total_size == 0U) {
    return 0;
  }
  ver = data[3];
  flags = data[5];
  limit = (length < total_size) ? length : (size_t)total_size;

  /* Extended header skip */
  if (flags & 0x40U) {
    if (ver == 3U) {
      if (offset + 4U > limit) {
        return 1;
      }
      {
        uint32_t ext_sz = ((uint32_t)data[offset] << 24) |
                          ((uint32_t)data[offset + 1U] << 16) |
                          ((uint32_t)data[offset + 2U] << 8) |
                          (uint32_t)data[offset + 3U];
        offset += 4U + ext_sz;
      }
    } else if (ver == 4U) {
      if (offset + 4U > limit) {
        return 1;
      }
      {
        uint32_t ext_sz = zzplay_syncsafe32(&data[offset]);
        offset += ext_sz;
      }
    }
  }

  while (offset < limit) {
    char id[5];
    uint32_t frame_sz = 0U;
    size_t hdr_sz = 0U;
    int skip = 0;

    if (data[offset] == 0U) {
      /* Padding reached */
      break;
    }

    if (ver == 2U) {
      if (offset + 6U > limit) {
        break;
      }
      id[0] = (char)data[offset];
      id[1] = (char)data[offset + 1U];
      id[2] = (char)data[offset + 2U];
      id[3] = '\0';
      frame_sz = ((uint32_t)data[offset + 3U] << 16) |
                 ((uint32_t)data[offset + 4U] << 8) |
                 (uint32_t)data[offset + 5U];
      hdr_sz = 6U;
    } else {
      if (offset + 10U > limit) {
        break;
      }
      id[0] = (char)data[offset];
      id[1] = (char)data[offset + 1U];
      id[2] = (char)data[offset + 2U];
      id[3] = (char)data[offset + 3U];
      id[4] = '\0';
      if (ver == 3U) {
        frame_sz = ((uint32_t)data[offset + 4U] << 24) |
                   ((uint32_t)data[offset + 5U] << 16) |
                   ((uint32_t)data[offset + 6U] << 8) |
                   (uint32_t)data[offset + 7U];
        /* Skip compression (bit 7) or encryption (bit 6) */
        if (data[offset + 9U] & 0xC0U) {
          skip = 1;
        }
      } else {
        /* v2.4 syncsafe size */
        if ((data[offset + 4U] & 0x80U) || (data[offset + 5U] & 0x80U) ||
            (data[offset + 6U] & 0x80U) || (data[offset + 7U] & 0x80U)) {
          break;
        }
        frame_sz = zzplay_syncsafe32(&data[offset + 4U]);
        /* Skip compression (bit 3), encryption (bit 2), or unsync (bit 1) */
        if (data[offset + 9U] & 0x0EU) {
          skip = 1;
        }
      }
      hdr_sz = 10U;
    }

    /* Subtract, never add: frame_sz is file-controlled and m68k size_t is
     * 32 bits, so offset + hdr_sz + frame_sz can wrap. The header checks
     * above guarantee offset + hdr_sz <= limit. */
    if (frame_sz > limit - offset - hdr_sz) {
      break;
    }

    if (!skip && frame_sz > 1U) {
      char *target = NULL;
      if (ver == 2U) {
        if (strcmp(id, "TT2") == 0) target = tags->title;
        else if (strcmp(id, "TP1") == 0) target = tags->artist;
        else if (strcmp(id, "TAL") == 0) target = tags->album;
      } else {
        if (strcmp(id, "TIT2") == 0) target = tags->title;
        else if (strcmp(id, "TPE1") == 0) target = tags->artist;
        else if (strcmp(id, "TALB") == 0) target = tags->album;
      }

      if (target && target[0] == '\0') {
        const uint8_t *frame_body = data + offset + hdr_sz;
        uint8_t encoding = frame_body[0];
        zzplay_decode_id3_text(target, ZZPLAY_TAG_TEXT_MAX, encoding,
                               frame_body + 1U, frame_sz - 1U);
      }
    }

    offset += hdr_sz + frame_sz;
  }

  return 1;
}

static void zzplay_id3v1_field(char *out, size_t out_cap, const uint8_t *src,
                               size_t src_len)
{
  size_t copy_len;

  if (!out || out_cap == 0U || out[0] != '\0' || !src) {
    return;
  }
  copy_len = (src_len < out_cap - 1U) ? src_len : (out_cap - 1U);
  memcpy(out, src, copy_len);
  out[copy_len] = '\0';
  zzplay_trim_trailing(out);
}

int zzplay_tags_parse_id3v1(const uint8_t *trailer, size_t length,
                            ZZPlayTags *tags)
{
  const uint8_t *p;

  if (!trailer || !tags || length < 128U) {
    return 0;
  }
  p = trailer + length - 128U;
  if (p[0] != 'T' || p[1] != 'A' || p[2] != 'G') {
    return 0;
  }
  zzplay_id3v1_field(tags->title, sizeof(tags->title), p + 3U, 30U);
  zzplay_id3v1_field(tags->artist, sizeof(tags->artist), p + 33U, 30U);
  zzplay_id3v1_field(tags->album, sizeof(tags->album), p + 63U, 30U);
  return 1;
}

int zzplay_tags_read(FILE *file, ZZPlayTags *tags)
{
  long orig_pos;
  long file_len;
  uint8_t header[10];
  uint32_t v2_size = 0U;

  if (!file || !tags) {
    return 0;
  }
  memset(tags, 0, sizeof(*tags));

  orig_pos = ftell(file);
  if (orig_pos < 0) {
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    return 0;
  }
  file_len = ftell(file);
  if (file_len < 0) {
    fseek(file, orig_pos, SEEK_SET);
    return 0;
  }

  if (fseek(file, 0, SEEK_SET) == 0 && file_len >= 10) {
    if (fread(header, 1U, 10U, file) == 10U) {
      v2_size = zzplay_tags_id3v2_size(header, 10U);
      if (v2_size > 0U) {
        size_t read_bytes = (v2_size > 65536U) ? 65536U : (size_t)v2_size;
        uint8_t *v2_buf;
        if (read_bytes > (size_t)file_len) {
          read_bytes = (size_t)file_len;
        }
        v2_buf = (uint8_t *)malloc(read_bytes);
        if (v2_buf) {
          fseek(file, 0, SEEK_SET);
          if (fread(v2_buf, 1U, read_bytes, file) == read_bytes) {
            zzplay_tags_parse_id3v2(v2_buf, read_bytes, tags);
          }
          free(v2_buf);
        }
        tags->audio_start = v2_size;
      }
    }
  }

  if (file_len >= 128) {
    uint8_t v1_buf[128];
    if (fseek(file, file_len - 128, SEEK_SET) == 0) {
      if (fread(v1_buf, 1U, 128U, file) == 128U) {
        if (zzplay_tags_parse_id3v1(v1_buf, 128U, tags)) {
          tags->trailer_bytes = 128U;
        }
      }
    }
  }

  fseek(file, orig_pos, SEEK_SET);

  return (tags->title[0] != '\0' || tags->artist[0] != '\0' ||
          tags->album[0] != '\0')
             ? 1
             : 0;
}

void zzplay_tags_display(const ZZPlayTags *tags, char *out, size_t capacity)
{
  if (!out || capacity == 0U) {
    return;
  }
  if (!tags) {
    out[0] = '\0';
    return;
  }
  if (tags->artist[0] != '\0' && tags->title[0] != '\0') {
    snprintf(out, capacity, "%s - %s", tags->artist, tags->title);
  } else if (tags->title[0] != '\0') {
    snprintf(out, capacity, "%s", tags->title);
  } else {
    out[0] = '\0';
  }
}
