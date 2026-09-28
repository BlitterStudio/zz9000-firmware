/* Audio-backend policy and the sink contract for zzplay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_AUDIO_H
#define ZZPLAY_AUDIO_H

#include <stddef.h>
#include <stdint.h>

typedef enum ZZPlayAudioBackend {
  ZZPLAY_AUDIO_AUTO = 0,
  ZZPLAY_AUDIO_AHI,
  ZZPLAY_AUDIO_MHI,
  ZZPLAY_AUDIO_AX,
  ZZPLAY_AUDIO_NONE
} ZZPlayAudioBackend;

typedef enum ZZPlayMediaAudio {
  ZZPLAY_MEDIA_AUDIO_NONE = 0,
  ZZPLAY_MEDIA_AUDIO_MP2,
  ZZPLAY_MEDIA_AUDIO_MP3
} ZZPlayMediaAudio;

typedef enum ZZPlayBackendAvailability {
  ZZPLAY_BACKEND_MISSING = 0,
  ZZPLAY_BACKEND_FREE,
  ZZPLAY_BACKEND_BUSY
} ZZPlayBackendAvailability;

typedef enum ZZPlayBackendStatus {
  ZZPLAY_BACKEND_OK = 0,
  ZZPLAY_BACKEND_UNSUPPORTED,
  ZZPLAY_BACKEND_MISSING_RESULT,
  ZZPLAY_BACKEND_BUSY_RESULT
} ZZPlayBackendStatus;

typedef struct ZZPlayAudioAvailability {
  ZZPlayBackendAvailability ahi;
  ZZPlayBackendAvailability mhi;
  ZZPlayBackendAvailability ax;
} ZZPlayAudioAvailability;

typedef struct ZZPlayBackendDecision {
  ZZPlayBackendStatus status;
  ZZPlayAudioBackend selected;
  int fell_back;
} ZZPlayBackendDecision;

typedef struct ZZPlayAudioSinkOps {
  int (*prepare)(void *user, uint32_t sample_rate, uint32_t channels);
  void *(*acquire_buffer)(void *user, size_t *capacity_bytes);
  int (*submit_buffer)(void *user, size_t bytes);
  int (*play)(void *user);
  int (*poll)(void *user);
  int (*pause)(void *user);
  int (*resume)(void *user);
  int (*drain)(void *user);
  void (*stop)(void *user);
  void (*close)(void *user);
  uint64_t (*played_frames)(const void *user);
  uint64_t (*queued_frames)(const void *user);
} ZZPlayAudioSinkOps;

ZZPlayBackendDecision zzplay_audio_select(
    ZZPlayMediaAudio media,
    ZZPlayAudioBackend requested,
    const ZZPlayAudioAvailability *availability);
int zzplay_audio_start_ready(ZZPlayAudioBackend backend,
                             uint64_t queued_frames,
                             uint64_t prebuffer_target_frames);

#endif /* ZZPLAY_AUDIO_H */
