/* MPEG sequence-header and input-file probing for zzplay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_PROBE_H
#define ZZPLAY_PROBE_H

#include "zzplay-webm.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define ZZPLAY_MAX_WIDTH 1920U
#define ZZPLAY_MAX_HEIGHT 1080U

typedef struct ZZPlayVideoInfo {
  uint32_t width;
  uint32_t height;
  uint32_t frame_rate_milli;
  int is_program_stream;
  int has_video_pes;
  int has_audio_pes;
} ZZPlayVideoInfo;

typedef struct ZZPlayMP3Info {
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t bitrate_kbps;
  uint32_t frame_bytes;
  uint8_t mpeg_version;
} ZZPlayMP3Info;

typedef struct ZZPlayFLACInfo {
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t bits_per_sample;
  uint64_t total_samples; /* 0 when STREAMINFO leaves it unknown */
  uint32_t max_block_size; /* samples per channel in the largest block */
  uint32_t max_frame_bytes; /* largest compressed frame (bound if unknown) */
} ZZPlayFLACInfo;

typedef struct ZZPlayVorbisInfo {
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t serial;          /* logical stream of the identification page */
  uint32_t nominal_bitrate; /* bits per second; 0 when unset */
  uint32_t max_block_samples; /* long-window size */
} ZZPlayVorbisInfo;

typedef struct ZZPlayWebPInfo {
  uint32_t width;
  uint32_t height;
  uint32_t format;
  int is_animated;
  int has_alpha;
} ZZPlayWebPInfo;

typedef enum ZZPlayMediaKind {
  ZZPLAY_MEDIA_KIND_UNSUPPORTED = 0,
  ZZPLAY_MEDIA_KIND_MPEG_PS,
  ZZPLAY_MEDIA_KIND_MP3,
  ZZPLAY_MEDIA_KIND_WEBP,
  ZZPLAY_MEDIA_KIND_FLAC,
  ZZPLAY_MEDIA_KIND_VORBIS,
  ZZPLAY_MEDIA_KIND_WEBM
} ZZPlayMediaKind;

typedef struct ZZPlayProbeInfo {
  ZZPlayMediaKind kind;
  ZZPlayVideoInfo video;
  ZZPlayMP3Info mp3;
  ZZPlayWebPInfo webp;
  ZZPlayFLACInfo flac;
  ZZPlayVorbisInfo vorbis;
  ZZPlayWebMInfo webm;
} ZZPlayProbeInfo;

uint32_t zzplay_mpeg_frame_rate_milli(uint8_t code);
int zzplay_probe_mpeg_sequence(const uint8_t *data,
                               size_t length,
                               ZZPlayVideoInfo *info);
void zzplay_probe_mpeg_program(const uint8_t *data,
                               size_t length,
                               ZZPlayVideoInfo *info);
int zzplay_probe_file(FILE *file, ZZPlayVideoInfo *info);
int zzplay_probe_mp3_frame(const uint8_t *data,
                           size_t length,
                           ZZPlayMP3Info *info);
int zzplay_probe_mp3(const uint8_t *data,
                     size_t length,
                     ZZPlayMP3Info *info);
int zzplay_probe_media_file(FILE *file, ZZPlayProbeInfo *info);
int zzplay_video_info_supported(const ZZPlayVideoInfo *info);
int zzplay_probe_webp(const uint8_t *data,
                      size_t length,
                      ZZPlayWebPInfo *info);
int zzplay_webp_info_supported(const ZZPlayWebPInfo *info);
/* The granule position of the last Ogg page of logical stream `serial`
 * found in the final 64 KiB of `file` (for Vorbis, the stream's sample
 * count). Returns 0 when no such page is there, as with a chained file
 * whose last link has another serial. The file position is not restored. */
int zzplay_ogg_last_granule(FILE *file, uint32_t serial, uint64_t *granule);

#endif /* ZZPLAY_PROBE_H */
