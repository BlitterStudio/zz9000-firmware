/* Bounded WebM header probe and playback policy for zzplay.
 *
 * The card demuxes. This only reads the EBML header, Segment Info and
 * Tracks, skipping everything else by size, so a hostile file cannot
 * make the player scan the whole stream. DisplayWidth/DisplayHeight are
 * a presentation aspect, not the coded size sent at session begin.
 *
 * Host-testable. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZPLAY_WEBM_H
#define ZZPLAY_WEBM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Longest side and pixel count the card will accept, either orientation.
 * Realtime is a smaller budget: above it audio stays continuous and video
 * may skip to the next keyframe. */
#define ZZPLAY_WEBM_MAX_SIDE 1920U
#define ZZPLAY_WEBM_MAX_PIXELS (1920U * 1088U)
#define ZZPLAY_WEBM_VP8_REALTIME_PIXELS (1280U * 720U)
#define ZZPLAY_WEBM_VP9_REALTIME_PIXELS (854U * 480U)

/* More than this many 90 kHz ticks behind the audio master means skip to
 * the next keyframe. Phone clips can place keyframes several seconds
 * apart, so a smaller threshold would jump the picture on ordinary lag. */
#define ZZPLAY_WEBM_KEYFRAME_LAG_PTS (500U * 90U)

typedef enum ZZPlayWebMVideo {
  ZZPLAY_WEBM_VIDEO_NONE = 0,
  ZZPLAY_WEBM_VIDEO_VP8,
  ZZPLAY_WEBM_VIDEO_VP9
} ZZPlayWebMVideo;

typedef enum ZZPlayWebMAudio {
  ZZPLAY_WEBM_AUDIO_NONE = 0,
  ZZPLAY_WEBM_AUDIO_OPUS,
  ZZPLAY_WEBM_AUDIO_VORBIS
} ZZPlayWebMAudio;

typedef enum ZZPlayWebMRefusal {
  ZZPLAY_WEBM_OK = 0,
  ZZPLAY_WEBM_NOT_CONTAINER,
  ZZPLAY_WEBM_MATROSKA,
  ZZPLAY_WEBM_TRUNCATED,
  ZZPLAY_WEBM_TWO_VIDEO,
  ZZPLAY_WEBM_TWO_AUDIO,
  ZZPLAY_WEBM_UNKNOWN_CODEC,
  ZZPLAY_WEBM_OVERSIZE,
  ZZPLAY_WEBM_NO_VIDEO,
  ZZPLAY_WEBM_BAD_AUDIO,
  ZZPLAY_WEBM_HOSTILE
} ZZPlayWebMRefusal;

typedef struct ZZPlayWebMInfo {
  ZZPlayWebMVideo video;
  ZZPlayWebMAudio audio;
  ZZPlayWebMRefusal refusal;
  uint32_t width;
  uint32_t height;
  uint32_t display_width;  /* 0: use the coded size for aspect */
  uint32_t display_height;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t timestamp_scale; /* nanoseconds; 1000000 when the file omits it */
  uint32_t duration_ms;     /* 0 when Segment Info has no Duration */
  uint32_t frame_rate_milli; /* from DefaultDuration; 0 when absent */
  int32_t pose_roll_milli;  /* degrees * 1000; only if pose_roll_present */
  uint8_t pose_roll_present;
  uint8_t has_duration;
} ZZPlayWebMInfo;

/* 1 when `data` starts with an EBML header we classified (see refusal).
 * 0 when it is not EBML, so another probe may try. `capped` is set when
 * `data` is a prefix of a longer file rather than the whole file. */
int zzplay_probe_webm(const uint8_t *data, size_t length, int capped,
                      ZZPlayWebMInfo *info);
int zzplay_probe_webm_file(FILE *file, ZZPlayWebMInfo *info);
void zzplay_webm_presentation_size(const ZZPlayWebMInfo *info,
                                   uint32_t *width, uint32_t *height);

/* Window aspect at roughly the coded-frame scale. A 9:16 DisplayWidth/
 * DisplayHeight must not open a 9x16 window. */
void zzplay_webm_window_source(const ZZPlayWebMInfo *info,
                               uint32_t *width, uint32_t *height);

int zzplay_webm_within_cap(uint32_t width, uint32_t height);
int zzplay_webm_realtime(ZZPlayWebMVideo video, uint32_t width,
                         uint32_t height);



/* "WebM VP9 640x360, Opus 48 kHz stereo". Returns 0 if it does not fit. */
int zzplay_webm_format_line(const ZZPlayWebMInfo *info, char *out,
                            size_t capacity);

/* Service-flag mask that must be advertised, not including the audio
 * flag when the file has no audio. 0 for a codec we will not begin. */
uint32_t zzplay_webm_required_flags(ZZPlayWebMVideo video,
                                   ZZPlayWebMAudio audio);
int zzplay_webm_service_ready(uint32_t flags, ZZPlayWebMVideo video,
                              ZZPlayWebMAudio audio);

uint32_t zzplay_webm_video_codec_id(ZZPlayWebMVideo video);
uint32_t zzplay_webm_audio_codec_id(ZZPlayWebMAudio audio);

const char *zzplay_webm_video_name(ZZPlayWebMVideo video);
const char *zzplay_webm_audio_name(ZZPlayWebMAudio audio);

#endif /* ZZPLAY_WEBM_H */
