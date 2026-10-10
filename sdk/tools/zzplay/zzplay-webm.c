/* Bounded WebM header probe. See zzplay-webm.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-webm.h"

#include "zz9k/abi.h"

#include <stdlib.h>
#include <string.h>

#define ZZPLAY_WEBM_PROBE_MAX (256U * 1024U)
#define ZZPLAY_WEBM_MAX_ELEMENTS 4096U
#define ZZPLAY_WEBM_MAX_DEPTH 8U
#define ZZPLAY_WEBM_VORBIS_MIN_RATE 8000U
#define ZZPLAY_WEBM_VORBIS_MAX_RATE 96000U

enum {
  ID_EBML = 0x1A45DFA3,
  ID_EBML_MAX_ID = 0x42F2,
  ID_EBML_MAX_SIZE = 0x42F3,
  ID_DOCTYPE = 0x4282,
  ID_SEGMENT = 0x18538067,
  ID_INFO = 0x1549A966,
  ID_SCALE = 0x2AD7B1,
  ID_DURATION = 0x4489,
  ID_TRACKS = 0x1654AE6B,
  ID_TRACK = 0xAE,
  ID_FLAG_ENABLED = 0xB9,
  ID_CODEC = 0x86,
  ID_TYPE = 0x83,
  ID_DEFAULT_DUR = 0x23E383,
  ID_VIDEO = 0xE0,
  ID_AUDIO = 0xE1,
  ID_WIDTH = 0xB0,
  ID_HEIGHT = 0xBA,
  ID_DISPLAY_W = 0x54B0,
  ID_DISPLAY_H = 0x54BA,
  ID_CHANNELS = 0x9F,
  ID_RATE = 0xB5,
  ID_PROJECTION = 0x7670,
  ID_POSE_ROLL = 0x7675,
  ID_CLUSTER = 0x1F43B675
};

enum {
  WALK_TOP = 0,
  WALK_EBML,
  WALK_SEGMENT,
  WALK_INFO,
  WALK_TRACKS,
  WALK_TRACK,
  WALK_VIDEO,
  WALK_AUDIO,
  WALK_PROJECTION
};

typedef struct ZZPlayWebMCur {
  const uint8_t *data;
  size_t length;
  size_t pos;
  uint32_t elements;
  int capped;
  ZZPlayWebMInfo *info;
  int video_tracks;
  int audio_tracks;
  int saw_tracks;
  int stop;
  int header_done;
  uint32_t track_type;
  int track_enabled;
  uint32_t track_default_ns;
  char codec[16];
  uint32_t pixel_w;
  uint32_t pixel_h;
  uint32_t disp_w;
  uint32_t disp_h;
  uint32_t channels;
  uint32_t rate;
  int saw_rate;
  int32_t pose_roll_milli;
  int pose_roll_present;
  uint64_t duration_ticks;
  int saw_duration;
  int saw_doctype;
} ZZPlayWebMCur;
static void zzplay_webm_fail(ZZPlayWebMCur *cur, ZZPlayWebMRefusal refusal)
{
  if (cur->info->refusal == ZZPLAY_WEBM_OK ||
      cur->info->refusal == ZZPLAY_WEBM_NOT_CONTAINER) {
    cur->info->refusal = refusal;
  }
  cur->stop = 1;
}

static uint64_t zzplay_webm_be(const uint8_t *p, uint32_t n)
{
  uint64_t v = 0U;
  uint32_t i;

  for (i = 0U; i < n; i++) {
    v = (v << 8) | p[i];
  }
  return v;
}

/* Truncating conversion of a big-endian IEEE float. Negatives, NaNs and
 * infinities become 0; values above 2^64-1 saturate. No libm. */
static uint64_t zzplay_webm_float_u64(const uint8_t *p, uint32_t n)
{
  uint64_t bits;
  uint64_t frac;
  uint64_t sig;
  int exp;
  int bias;
  int mant_bits;
  int shift;

  if (n == 4U) {
    bits = zzplay_webm_be(p, 4U);
    bias = 127;
    mant_bits = 23;
  } else if (n == 8U) {
    bits = zzplay_webm_be(p, 8U);
    bias = 1023;
    mant_bits = 52;
  } else {
    return 0U;
  }
  if ((bits >> ((n * 8U) - 1U)) != 0U) {
    return 0U;
  }
  exp = (int)((bits >> mant_bits) &
              ((n == 4U) ? 0xffU : 0x7ffU)) -
        bias;
  frac = bits & ((1ULL << mant_bits) - 1ULL);
  if (exp >= ((n == 4U) ? 128 : 1024) || exp < 0) {
    return 0U;
  }
  sig = (1ULL << mant_bits) | frac;
  if (exp >= mant_bits) {
    shift = exp - mant_bits;
    if (shift >= 64 || sig > (UINT64_MAX >> shift)) {
      return UINT64_MAX;
    }
    return sig << shift;
  }
  return sig >> (mant_bits - exp);
}

static int32_t zzplay_webm_float_milli(const uint8_t *p, uint32_t n)
{
  uint64_t bits;
  uint64_t frac;
  uint64_t sig;
  int exp;
  int bias;
  int mant_bits;
  int negative;
  uint64_t milli;
  int shift;

  if (n != 4U && n != 8U) {
    return 0;
  }
  bits = zzplay_webm_be(p, n);
  negative = (bits >> ((n * 8U) - 1U)) != 0U;
  bias = n == 4U ? 127 : 1023;
  mant_bits = n == 4U ? 23 : 52;
  exp = (int)((bits >> mant_bits) &
              ((n == 4U) ? 0xffU : 0x7ffU)) -
        bias;
  frac = bits & ((1ULL << mant_bits) - 1ULL);
  if (exp >= ((n == 4U) ? 128 : 1024)) {
    return negative ? INT32_MIN : INT32_MAX;
  }
  if (exp < -10) {
    return 0;
  }
  sig = (1ULL << mant_bits) | frac;
  /* milli = sig * 1000 * 2^(exp - mant_bits) */
  if (sig > UINT64_MAX / 1000ULL) {
    return negative ? INT32_MIN : INT32_MAX;
  }
  milli = sig * 1000ULL;
  shift = exp - mant_bits;
  if (shift >= 0) {
    if (shift >= 32 || milli > ((uint64_t)INT32_MAX >> shift)) {
      return negative ? INT32_MIN : INT32_MAX;
    }
    milli <<= shift;
  } else if (-shift >= 64) {
    return 0;
  } else {
    milli >>= -shift;
  }
  if (milli > (uint64_t)INT32_MAX) {
    return negative ? INT32_MIN : INT32_MAX;
  }
  return negative ? -(int32_t)milli : (int32_t)milli;
}

static uint32_t zzplay_webm_duration_ms(uint64_t ticks, uint32_t scale_ns)
{
  uint64_t ms;

  if (scale_ns == 0U) {
    scale_ns = 1000000U;
  }
  if (ticks > UINT64_MAX / (uint64_t)scale_ns) {
    ms = (ticks / 1000000ULL) * (uint64_t)scale_ns;
  } else {
    ms = ticks * (uint64_t)scale_ns / 1000000ULL;
  }
  return ms > 0xffffffffULL ? 0xffffffffU : (uint32_t)ms;
}

static int zzplay_webm_read(ZZPlayWebMCur *cur, void *dst, uint32_t n)
{
  if (n > cur->length - cur->pos) {
    zzplay_webm_fail(cur, cur->capped ? ZZPLAY_WEBM_HOSTILE
                                      : ZZPLAY_WEBM_TRUNCATED);
    return 0;
  }
  if (dst) {
    memcpy(dst, cur->data + cur->pos, n);
  }
  cur->pos += n;
  return 1;
}

static int zzplay_webm_vint(ZZPlayWebMCur *cur, uint64_t *val, uint32_t *len,
                            int mask_len, int *unknown)
{
  uint8_t b[8];
  uint32_t n = 1U;
  uint32_t i;
  uint8_t mark = 0x80U;
  uint64_t v;

  if (!zzplay_webm_read(cur, b, 1U)) {
    return 0;
  }
  while (n < 8U && (b[0] & mark) == 0U) {
    mark >>= 1;
    n++;
  }
  if ((b[0] & mark) == 0U) {
    zzplay_webm_fail(cur, ZZPLAY_WEBM_HOSTILE);
    return 0;
  }
  if (n > 1U && !zzplay_webm_read(cur, b + 1, n - 1U)) {
    return 0;
  }
  v = mask_len ? (uint64_t)(b[0] & (uint8_t)(mark - 1U)) : b[0];
  for (i = 1U; i < n; i++) {
    v = (v << 8) | b[i];
  }
  if (unknown) {
    *unknown = mask_len && v == ((1ULL << (7U * n)) - 1ULL);
  }
  *val = v;
  *len = n;
  return 1;
}

static int zzplay_webm_id_size(ZZPlayWebMCur *cur, uint32_t *id,
                               uint64_t *size, int *unknown)
{
  uint64_t raw;
  uint32_t len;

  if (!zzplay_webm_vint(cur, &raw, &len, 0, 0)) {
    return 0;
  }
  if (len > 4U || raw > 0xffffffffULL) {
    zzplay_webm_fail(cur, ZZPLAY_WEBM_HOSTILE);
    return 0;
  }
  *id = (uint32_t)raw;
  return zzplay_webm_vint(cur, size, &len, 1, unknown);
}

static int zzplay_webm_payload(ZZPlayWebMCur *cur, uint64_t size,
                               const uint8_t **out, uint32_t *n)
{
  if (size > 0xffffffffULL ||
      (size_t)size > cur->length - cur->pos) {
    zzplay_webm_fail(cur, cur->capped && size > (uint64_t)(cur->length - cur->pos)
                             ? ZZPLAY_WEBM_HOSTILE
                             : ZZPLAY_WEBM_TRUNCATED);
    return 0;
  }
  *out = cur->data + cur->pos;
  *n = (uint32_t)size;
  cur->pos += (size_t)size;
  return 1;
}

static int zzplay_webm_codec_is(const ZZPlayWebMCur *cur, const char *name)
{
  return strcmp(cur->codec, name) == 0;
}

static void zzplay_webm_finish_track(ZZPlayWebMCur *cur)
{
  ZZPlayWebMInfo *info = cur->info;

  if (!cur->track_enabled || cur->codec[0] == '\0') {
    return;
  }
  if (cur->track_type == 1U) {
    cur->video_tracks++;
    if (cur->video_tracks > 1) {
      zzplay_webm_fail(cur, ZZPLAY_WEBM_TWO_VIDEO);
      return;
    }
    if (zzplay_webm_codec_is(cur, "V_VP8")) {
      info->video = ZZPLAY_WEBM_VIDEO_VP8;
    } else if (zzplay_webm_codec_is(cur, "V_VP9")) {
      info->video = ZZPLAY_WEBM_VIDEO_VP9;
    } else {
      zzplay_webm_fail(cur, ZZPLAY_WEBM_UNKNOWN_CODEC);
      return;
    }
    if (cur->pixel_w == 0U || cur->pixel_h == 0U) {
      zzplay_webm_fail(cur, ZZPLAY_WEBM_HOSTILE);
      return;
    }
    info->width = cur->pixel_w;
    info->height = cur->pixel_h;
    info->display_width = cur->disp_w;
    info->display_height = cur->disp_h;
    info->pose_roll_present = (uint8_t)cur->pose_roll_present;
    info->pose_roll_milli = cur->pose_roll_milli;
    if (cur->track_default_ns != 0U &&
        cur->track_default_ns <= 1000000000U) {
      uint64_t milli = 1000000000000ULL / cur->track_default_ns;

      if (milli > 0U && milli <= 240000U) {
        info->frame_rate_milli = (uint32_t)milli;
      }
    }
    return;
  }
  if (cur->track_type == 2U) {
    cur->audio_tracks++;
    if (cur->audio_tracks > 1) {
      zzplay_webm_fail(cur, ZZPLAY_WEBM_TWO_AUDIO);
      return;
    }
    if (zzplay_webm_codec_is(cur, "A_OPUS")) {
      info->audio = ZZPLAY_WEBM_AUDIO_OPUS;
      info->sample_rate = 48000U;
    } else if (zzplay_webm_codec_is(cur, "A_VORBIS")) {
      info->audio = ZZPLAY_WEBM_AUDIO_VORBIS;
      info->sample_rate = cur->saw_rate ? cur->rate : 8000U;
    } else {
      zzplay_webm_fail(cur, ZZPLAY_WEBM_UNKNOWN_CODEC);
      return;
    }
    info->channels = cur->channels != 0U ? cur->channels : 1U;
  }
}

static int zzplay_webm_walk(ZZPlayWebMCur *cur, size_t end, int depth,
                            int kind);

static int zzplay_webm_take_master(ZZPlayWebMCur *cur, uint64_t size,
                                   int unknown, int depth, int kind,
                                   size_t end)
{
  size_t child_end;

  if (unknown || size > (uint64_t)(cur->length - cur->pos)) {
    zzplay_webm_fail(cur, unknown || cur->capped ? ZZPLAY_WEBM_HOSTILE
                                                 : ZZPLAY_WEBM_TRUNCATED);
    return 0;
  }
  child_end = cur->pos + (size_t)size;
  if (child_end > end) {
    zzplay_webm_fail(cur, ZZPLAY_WEBM_HOSTILE);
    return 0;
  }
  return zzplay_webm_walk(cur, child_end, depth + 1, kind);
}

static int zzplay_webm_walk(ZZPlayWebMCur *cur, size_t end, int depth,
                            int kind)
{
  if (depth > (int)ZZPLAY_WEBM_MAX_DEPTH) {
    zzplay_webm_fail(cur, ZZPLAY_WEBM_HOSTILE);
    return 0;
  }
  while (!cur->stop && cur->pos + 2U <= end) {
    uint32_t id;
    uint64_t size;
    int unknown = 0;
    const uint8_t *payload;
    uint32_t n;
    size_t at;

    if (cur->elements >= ZZPLAY_WEBM_MAX_ELEMENTS) {
      zzplay_webm_fail(cur, ZZPLAY_WEBM_HOSTILE);
      return 0;
    }
    cur->elements++;
    at = cur->pos;
    if (!zzplay_webm_id_size(cur, &id, &size, &unknown)) {
      return 0;
    }
    if (kind == WALK_SEGMENT && id == ID_CLUSTER) {
      cur->header_done = 1;
      cur->pos = at;
      return 1;
    }
    if (id == ID_EBML && kind == WALK_TOP) {
      if (!zzplay_webm_take_master(cur, size, unknown, depth, WALK_EBML, end)) {
        return 0;
      }
      continue;
    }
    if (id == ID_SEGMENT && kind == WALK_TOP) {
      size_t seg_end;

      if (unknown) {
        seg_end = cur->length;
      } else if (size > (uint64_t)(cur->length - cur->pos)) {
        /* The segment is the rest of the file. Walk the header that
         * fits; clusters past the probe budget are not an error. */
        seg_end = cur->length;
      } else {
        seg_end = cur->pos + (size_t)size;
      }
      if (!zzplay_webm_walk(cur, seg_end, depth + 1, WALK_SEGMENT)) {
        return 0;
      }
      break;
    }
    if (unknown) {
      zzplay_webm_fail(cur, ZZPLAY_WEBM_HOSTILE);
      return 0;
    }
    if (kind == WALK_EBML && id == ID_DOCTYPE) {
      if (!zzplay_webm_payload(cur, size, &payload, &n)) {
        return 0;
      }
      cur->saw_doctype = 1;
      if (n == 4U && memcmp(payload, "webm", 4U) == 0) {
        cur->info->refusal = ZZPLAY_WEBM_OK;
      } else if (n == 8U && memcmp(payload, "matroska", 8U) == 0) {
        zzplay_webm_fail(cur, ZZPLAY_WEBM_MATROSKA);
      } else {
        zzplay_webm_fail(cur, ZZPLAY_WEBM_NOT_CONTAINER);
      }
      continue;
    }
    if (kind == WALK_EBML &&
        (id == ID_EBML_MAX_ID || id == ID_EBML_MAX_SIZE)) {
      if (!zzplay_webm_payload(cur, size, &payload, &n) || n == 0U ||
          n > 4U) {
        zzplay_webm_fail(cur, ZZPLAY_WEBM_HOSTILE);
        return 0;
      }
      if (zzplay_webm_be(payload, n) > 4U && id == ID_EBML_MAX_ID) {
        zzplay_webm_fail(cur, ZZPLAY_WEBM_HOSTILE);
        return 0;
      }
      if (zzplay_webm_be(payload, n) > 8U && id == ID_EBML_MAX_SIZE) {
        zzplay_webm_fail(cur, ZZPLAY_WEBM_HOSTILE);
        return 0;
      }
      continue;
    }
    if (kind == WALK_INFO && id == ID_SCALE) {
      if (!zzplay_webm_payload(cur, size, &payload, &n) || n == 0U ||
          n > 8U) {
        zzplay_webm_fail(cur, ZZPLAY_WEBM_HOSTILE);
        return 0;
      }
      {
        uint64_t scale = zzplay_webm_be(payload, n);

        if (scale == 0U || scale > 0xffffffffULL) {
          zzplay_webm_fail(cur, ZZPLAY_WEBM_HOSTILE);
          return 0;
        }
        cur->info->timestamp_scale = (uint32_t)scale;
      }
      continue;
    }
    if (kind == WALK_INFO && id == ID_DURATION &&
        (size == 4U || size == 8U)) {
      if (!zzplay_webm_payload(cur, size, &payload, &n)) {
        return 0;
      }
      cur->duration_ticks = zzplay_webm_float_u64(payload, n);
      cur->saw_duration = 1;
      continue;
    }
    if (kind == WALK_TRACK && id == ID_TYPE) {
      if (!zzplay_webm_payload(cur, size, &payload, &n) || n == 0U ||
          n > 4U) {
        zzplay_webm_fail(cur, ZZPLAY_WEBM_HOSTILE);
        return 0;
      }
      cur->track_type = (uint32_t)zzplay_webm_be(payload, n);
      continue;
    }
    if (kind == WALK_TRACK && id == ID_FLAG_ENABLED) {
      if (!zzplay_webm_payload(cur, size, &payload, &n) || n != 1U) {
        zzplay_webm_fail(cur, ZZPLAY_WEBM_HOSTILE);
        return 0;
      }
      cur->track_enabled = payload[0] != 0U;
      continue;
    }
    if (kind == WALK_TRACK && id == ID_CODEC) {
      if (!zzplay_webm_payload(cur, size, &payload, &n) || n == 0U ||
          n >= sizeof(cur->codec)) {
        zzplay_webm_fail(cur, ZZPLAY_WEBM_UNKNOWN_CODEC);
        return 0;
      }
      memcpy(cur->codec, payload, n);
      cur->codec[n] = '\0';
      continue;
    }
    if (kind == WALK_TRACK && id == ID_DEFAULT_DUR) {
      if (!zzplay_webm_payload(cur, size, &payload, &n) || n == 0U ||
          n > 4U) {
        zzplay_webm_fail(cur, ZZPLAY_WEBM_HOSTILE);
        return 0;
      }
      cur->track_default_ns = (uint32_t)zzplay_webm_be(payload, n);
      continue;
    }
    if ((kind == WALK_VIDEO &&
         (id == ID_WIDTH || id == ID_HEIGHT || id == ID_DISPLAY_W ||
          id == ID_DISPLAY_H)) ||
        (kind == WALK_AUDIO && id == ID_CHANNELS)) {
      uint64_t v;

      if (!zzplay_webm_payload(cur, size, &payload, &n) || n == 0U ||
          n > 4U) {
        zzplay_webm_fail(cur, ZZPLAY_WEBM_HOSTILE);
        return 0;
      }
      v = zzplay_webm_be(payload, n);
      if (v > 0xffffffffULL) {
        zzplay_webm_fail(cur, ZZPLAY_WEBM_HOSTILE);
        return 0;
      }
      if (id == ID_WIDTH) {
        cur->pixel_w = (uint32_t)v;
      } else if (id == ID_HEIGHT) {
        cur->pixel_h = (uint32_t)v;
      } else if (id == ID_DISPLAY_W) {
        cur->disp_w = (uint32_t)v;
      } else if (id == ID_DISPLAY_H) {
        cur->disp_h = (uint32_t)v;
      } else {
        cur->channels = (uint32_t)v;
      }
      continue;
    }
    if (kind == WALK_AUDIO && id == ID_RATE &&
        (size == 4U || size == 8U)) {
      if (!zzplay_webm_payload(cur, size, &payload, &n)) {
        return 0;
      }
      cur->rate = (uint32_t)zzplay_webm_float_u64(payload, n);
      cur->saw_rate = 1;
      continue;
    }
    if (kind == WALK_PROJECTION && id == ID_POSE_ROLL &&
        (size == 4U || size == 8U)) {
      if (!zzplay_webm_payload(cur, size, &payload, &n)) {
        return 0;
      }
      cur->pose_roll_milli = zzplay_webm_float_milli(payload, n);
      cur->pose_roll_present = 1;
      continue;
    }
    if (id == ID_INFO && kind == WALK_SEGMENT) {
      if (!zzplay_webm_take_master(cur, size, 0, depth, WALK_INFO, end)) {
        return 0;
      }
      continue;
    }
    if (id == ID_TRACKS && kind == WALK_SEGMENT) {
      cur->saw_tracks = 1;
      if (!zzplay_webm_take_master(cur, size, 0, depth, WALK_TRACKS, end)) {
        return 0;
      }
      continue;
    }
    if (id == ID_TRACK && kind == WALK_TRACKS) {
      cur->track_type = 0U;
      cur->track_enabled = 1;
      cur->track_default_ns = 0U;
      cur->codec[0] = '\0';
      cur->pixel_w = 0U;
      cur->pixel_h = 0U;
      cur->disp_w = 0U;
      cur->disp_h = 0U;
      cur->channels = 0U;
      cur->rate = 0U;
      cur->saw_rate = 0;
      cur->pose_roll_milli = 0;
      cur->pose_roll_present = 0;
      if (!zzplay_webm_take_master(cur, size, 0, depth, WALK_TRACK, end)) {
        return 0;
      }
      if (!cur->stop) {
        zzplay_webm_finish_track(cur);
      }
      continue;
    }
    if (id == ID_VIDEO && kind == WALK_TRACK) {
      if (!zzplay_webm_take_master(cur, size, 0, depth, WALK_VIDEO, end)) {
        return 0;
      }
      continue;
    }
    if (id == ID_AUDIO && kind == WALK_TRACK) {
      if (!zzplay_webm_take_master(cur, size, 0, depth, WALK_AUDIO, end)) {
        return 0;
      }
      continue;
    }
    if (id == ID_PROJECTION && kind == WALK_VIDEO) {
      if (!zzplay_webm_take_master(cur, size, 0, depth, WALK_PROJECTION, end)) {
        return 0;
      }
      continue;
    }
    if (!zzplay_webm_payload(cur, size, &payload, &n)) {
      return 0;
    }
    (void)payload;
  }
  return !cur->stop;
}

int zzplay_webm_within_cap(uint32_t width, uint32_t height)
{
  uint64_t pixels;
  uint32_t longest;

  if (width == 0U || height == 0U) {
    return 0;
  }
  longest = width > height ? width : height;
  if (longest > ZZPLAY_WEBM_MAX_SIDE) {
    return 0;
  }
  pixels = (uint64_t)width * (uint64_t)height;
  return pixels <= (uint64_t)ZZPLAY_WEBM_MAX_PIXELS;
}

int zzplay_webm_realtime(ZZPlayWebMVideo video, uint32_t width,
                         uint32_t height)
{
  uint64_t pixels;
  uint64_t budget;

  if (width == 0U || height == 0U) {
    return 0;
  }
  pixels = (uint64_t)width * (uint64_t)height;
  if (video == ZZPLAY_WEBM_VIDEO_VP8) {
    budget = ZZPLAY_WEBM_VP8_REALTIME_PIXELS;
  } else if (video == ZZPLAY_WEBM_VIDEO_VP9) {
    budget = ZZPLAY_WEBM_VP9_REALTIME_PIXELS;
  } else {
    return 0;
  }
  return pixels <= budget;
}

void zzplay_webm_presentation_size(const ZZPlayWebMInfo *info,
                                   uint32_t *width, uint32_t *height)
{
  uint32_t w;
  uint32_t h;

  if (!info) {
    if (width) {
      *width = 0U;
    }
    if (height) {
      *height = 0U;
    }
    return;
  }
  w = info->width;
  h = info->height;
  if (info->display_width != 0U && info->display_height != 0U) {
    w = info->display_width;
    h = info->display_height;
  }
  if (width) {
    *width = w;
  }
  if (height) {
    *height = h;
  }
}

void zzplay_webm_window_source(const ZZPlayWebMInfo *info,
                               uint32_t *width, uint32_t *height)
{
  uint32_t dw;
  uint32_t dh;
  uint32_t long_src;
  uint32_t long_disp;
  uint32_t w;
  uint32_t h;

  zzplay_webm_presentation_size(info, &dw, &dh);
  if (!info || dw == 0U || dh == 0U) {
    if (width) {
      *width = 0U;
    }
    if (height) {
      *height = 0U;
    }
    return;
  }
  /* A ratio smaller than the coded frame is an aspect, not a window. */
  if (dw < info->width || dh < info->height) {
    long_src = info->width > info->height ? info->width : info->height;
    long_disp = dw > dh ? dw : dh;
    w = (uint32_t)(((uint64_t)dw * long_src) / long_disp);
    h = (uint32_t)(((uint64_t)dh * long_src) / long_disp);
    if (w == 0U) {
      w = 1U;
    }
    if (h == 0U) {
      h = 1U;
    }
  } else {
    w = dw;
    h = dh;
  }
  if (width) {
    *width = w;
  }
  if (height) {
    *height = h;
  }
}

static int zzplay_webm_append(char *out, size_t capacity, size_t *used,
                              const char *text)
{
  size_t n = strlen(text);

  if (*used + n + 1U > capacity) {
    return 0;
  }
  memcpy(out + *used, text, n + 1U);
  *used += n;
  return 1;
}

static int zzplay_webm_append_u(char *out, size_t capacity, size_t *used,
                                uint32_t value)
{
  char tmp[16];
  char rev[16];
  uint32_t n = 0U;
  uint32_t i;

  if (value == 0U) {
    return zzplay_webm_append(out, capacity, used, "0");
  }
  while (value != 0U && n < sizeof(rev)) {
    rev[n++] = (char)('0' + (value % 10U));
    value /= 10U;
  }
  for (i = 0U; i < n; i++) {
    tmp[i] = rev[n - 1U - i];
  }
  tmp[n] = '\0';
  return zzplay_webm_append(out, capacity, used, tmp);
}

int zzplay_webm_format_line(const ZZPlayWebMInfo *info, char *out,
                            size_t capacity)
{
  size_t used = 0U;

  if (!info || !out || capacity == 0U ||
      info->video == ZZPLAY_WEBM_VIDEO_NONE) {
    return 0;
  }
  out[0] = '\0';
  if (!zzplay_webm_append(out, capacity, &used, "WebM ") ||
      !zzplay_webm_append(out, capacity, &used,
                          zzplay_webm_video_name(info->video)) ||
      !zzplay_webm_append(out, capacity, &used, " ") ||
      !zzplay_webm_append_u(out, capacity, &used, info->width) ||
      !zzplay_webm_append(out, capacity, &used, "x") ||
      !zzplay_webm_append_u(out, capacity, &used, info->height)) {
    return 0;
  }
  if (info->audio == ZZPLAY_WEBM_AUDIO_NONE) {
    return zzplay_webm_append(out, capacity, &used, ", no audio");
  }
  if (!zzplay_webm_append(out, capacity, &used, ", ") ||
      !zzplay_webm_append(out, capacity, &used,
                          zzplay_webm_audio_name(info->audio)) ||
      !zzplay_webm_append(out, capacity, &used, " ")) {
    return 0;
  }
  if (info->sample_rate % 1000U == 0U) {
    if (!zzplay_webm_append_u(out, capacity, &used,
                              info->sample_rate / 1000U) ||
        !zzplay_webm_append(out, capacity, &used, " kHz")) {
      return 0;
    }
  } else if (info->sample_rate % 100U == 0U) {
    if (!zzplay_webm_append_u(out, capacity, &used,
                              info->sample_rate / 1000U) ||
        !zzplay_webm_append(out, capacity, &used, ".") ||
        !zzplay_webm_append_u(out, capacity, &used,
                              (info->sample_rate % 1000U) / 100U) ||
        !zzplay_webm_append(out, capacity, &used, " kHz")) {
      return 0;
    }
  } else if (!zzplay_webm_append_u(out, capacity, &used,
                                   info->sample_rate) ||
             !zzplay_webm_append(out, capacity, &used, " Hz")) {
    return 0;
  }
  if (info->channels == 1U) {
    return zzplay_webm_append(out, capacity, &used, " mono");
  }
  if (info->channels == 2U) {
    return zzplay_webm_append(out, capacity, &used, " stereo");
  }
  if (!zzplay_webm_append(out, capacity, &used, " ") ||
      !zzplay_webm_append_u(out, capacity, &used, info->channels) ||
      !zzplay_webm_append(out, capacity, &used, " ch")) {
    return 0;
  }
  return 1;
}

uint32_t zzplay_webm_required_flags(ZZPlayWebMVideo video,
                                   ZZPlayWebMAudio audio)
{
  uint32_t flags = ZZ9K_SERVICE_FLAG_VIDEO_DIRECT_OVERLAY |
                   ZZ9K_SERVICE_FLAG_VIDEO_STREAMING_INPUT |
                   ZZ9K_SERVICE_FLAG_VIDEO_MEDIA_SESSION;

  if (video == ZZPLAY_WEBM_VIDEO_VP8) {
    flags |= ZZ9K_SERVICE_FLAG_VIDEO_WEBM_VP8;
  } else if (video == ZZPLAY_WEBM_VIDEO_VP9) {
    flags |= ZZ9K_SERVICE_FLAG_VIDEO_WEBM_VP9;
  } else {
    return 0U;
  }
  if (audio == ZZPLAY_WEBM_AUDIO_OPUS) {
    flags |= ZZ9K_SERVICE_FLAG_VIDEO_MEDIA_OPUS;
  } else if (audio == ZZPLAY_WEBM_AUDIO_VORBIS) {
    flags |= ZZ9K_SERVICE_FLAG_VIDEO_MEDIA_VORBIS;
  }
  return flags;
}

int zzplay_webm_service_ready(uint32_t flags, ZZPlayWebMVideo video,
                              ZZPlayWebMAudio audio)
{
  uint32_t need = zzplay_webm_required_flags(video, audio);

  return need != 0U && (flags & need) == need;
}

uint32_t zzplay_webm_video_codec_id(ZZPlayWebMVideo video)
{
  if (video == ZZPLAY_WEBM_VIDEO_VP8) {
    return ZZ9K_VIDEO_CODEC_VP8;
  }
  if (video == ZZPLAY_WEBM_VIDEO_VP9) {
    return ZZ9K_VIDEO_CODEC_VP9;
  }
  return 0U;
}

uint32_t zzplay_webm_audio_codec_id(ZZPlayWebMAudio audio)
{
  if (audio == ZZPLAY_WEBM_AUDIO_OPUS) {
    return ZZ9K_MEDIA_AUDIO_OPUS;
  }
  if (audio == ZZPLAY_WEBM_AUDIO_VORBIS) {
    return ZZ9K_MEDIA_AUDIO_VORBIS;
  }
  return ZZ9K_MEDIA_AUDIO_NONE;
}

const char *zzplay_webm_video_name(ZZPlayWebMVideo video)
{
  if (video == ZZPLAY_WEBM_VIDEO_VP8) {
    return "VP8";
  }
  if (video == ZZPLAY_WEBM_VIDEO_VP9) {
    return "VP9";
  }
  return "video";
}

const char *zzplay_webm_audio_name(ZZPlayWebMAudio audio)
{
  if (audio == ZZPLAY_WEBM_AUDIO_OPUS) {
    return "Opus";
  }
  if (audio == ZZPLAY_WEBM_AUDIO_VORBIS) {
    return "Vorbis";
  }
  return "audio";
}

int zzplay_probe_webm(const uint8_t *data, size_t length, int capped,
                      ZZPlayWebMInfo *info)
{
  ZZPlayWebMCur cur;

  if (!info) {
    return 0;
  }
  memset(info, 0, sizeof(*info));
  info->timestamp_scale = 1000000U;
  info->refusal = ZZPLAY_WEBM_NOT_CONTAINER;
  if (!data || length < 4U || data[0] != 0x1AU || data[1] != 0x45U ||
      data[2] != 0xDFU || data[3] != 0xA3U) {
    return 0;
  }
  memset(&cur, 0, sizeof(cur));
  cur.data = data;
  cur.length = length;
  cur.capped = capped;
  cur.info = info;
  if (!zzplay_webm_walk(&cur, length, 0, WALK_TOP)) {
    if (info->refusal == ZZPLAY_WEBM_OK ||
        (info->refusal == ZZPLAY_WEBM_NOT_CONTAINER &&
         !cur.saw_doctype)) {
      info->refusal = capped ? ZZPLAY_WEBM_HOSTILE : ZZPLAY_WEBM_TRUNCATED;
    }
    return 1;
  }
  if (info->refusal == ZZPLAY_WEBM_MATROSKA ||
      info->refusal == ZZPLAY_WEBM_NOT_CONTAINER) {
    return 1;
  }
  if (info->refusal != ZZPLAY_WEBM_OK) {
    return 1;
  }
  if (!cur.saw_tracks || info->video == ZZPLAY_WEBM_VIDEO_NONE) {
    info->refusal = cur.saw_tracks ? ZZPLAY_WEBM_NO_VIDEO
                                   : (capped ? ZZPLAY_WEBM_HOSTILE
                                             : ZZPLAY_WEBM_TRUNCATED);
    return 1;
  }
  if (!zzplay_webm_within_cap(info->width, info->height)) {
    info->refusal = ZZPLAY_WEBM_OVERSIZE;
    return 1;
  }
  if (info->audio != ZZPLAY_WEBM_AUDIO_NONE &&
      (info->channels != 1U && info->channels != 2U)) {
    info->refusal = ZZPLAY_WEBM_BAD_AUDIO;
    return 1;
  }
  if (info->audio == ZZPLAY_WEBM_AUDIO_VORBIS &&
      (info->sample_rate < ZZPLAY_WEBM_VORBIS_MIN_RATE ||
       info->sample_rate > ZZPLAY_WEBM_VORBIS_MAX_RATE)) {
    info->refusal = ZZPLAY_WEBM_BAD_AUDIO;
    return 1;
  }
  if (cur.saw_duration) {
    info->duration_ms = zzplay_webm_duration_ms(
        cur.duration_ticks, info->timestamp_scale);
    info->has_duration = info->duration_ms != 0U;
  }
  return 1;
}

int zzplay_probe_webm_file(FILE *file, ZZPlayWebMInfo *info)
{
  uint8_t *buf;
  size_t got;
  int capped;
  int recognized;
  long pos;

  if (!file || !info) {
    return 0;
  }
  pos = ftell(file);
  if (fseek(file, 0L, SEEK_SET) != 0) {
    clearerr(file);
    return 0;
  }
  buf = (uint8_t *)malloc(ZZPLAY_WEBM_PROBE_MAX);
  if (!buf) {
    if (pos >= 0L) {
      (void)fseek(file, pos, SEEK_SET);
    }
    return 0;
  }
  got = fread(buf, 1U, ZZPLAY_WEBM_PROBE_MAX, file);
  capped = got == ZZPLAY_WEBM_PROBE_MAX && !feof(file);
  clearerr(file);
  recognized = zzplay_probe_webm(buf, got, capped, info);
  free(buf);
  if (fseek(file, 0L, SEEK_SET) != 0) {
    clearerr(file);
  }
  return recognized;
}
