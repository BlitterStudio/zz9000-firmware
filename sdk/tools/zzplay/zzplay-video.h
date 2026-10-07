/* Video-sink contract and service-result helpers for zzplay.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_VIDEO_H
#define ZZPLAY_VIDEO_H

#include "zz9k/host.h"

#include <stdint.h>

#define ZZPLAY_REQUIRED_VIDEO_FLAGS                                      \
  (ZZ9K_SERVICE_FLAG_VIDEO_MPEG1 | ZZ9K_SERVICE_FLAG_VIDEO_MPEG_PS |    \
   ZZ9K_SERVICE_FLAG_VIDEO_DIRECT_OVERLAY |                              \
   ZZ9K_SERVICE_FLAG_VIDEO_STREAMING_INPUT)

typedef enum ZZPlayVideoSinkStatus {
  ZZPLAY_VIDEO_SINK_READY = 0,
  ZZPLAY_VIDEO_SINK_UNSUPPORTED_BOARD,
  ZZPLAY_VIDEO_SINK_P96_UNAVAILABLE,
  ZZPLAY_VIDEO_SINK_PIP_UNAVAILABLE
} ZZPlayVideoSinkStatus;

typedef enum ZZPlayVideoResultAction {
  ZZPLAY_VIDEO_RESULT_INVALID = 0,
  ZZPLAY_VIDEO_RESULT_CONTINUE,
  ZZPLAY_VIDEO_RESULT_NEED_INPUT,
  ZZPLAY_VIDEO_RESULT_DONE
} ZZPlayVideoResultAction;

typedef struct ZZPlayVideoSinkOps {
  int (*open)(void *user, uint32_t width, uint32_t height);
  void (*close)(void *user);
} ZZPlayVideoSinkOps;

ZZPlayVideoSinkStatus zzplay_video_sink_check(
    int board_found, uint32_t zorro_version,
    int p96_available, int pip_available);
int zzplay_video_z2_aperture_ready(const ZZ9KApertureLayout *layout,
                                   uint32_t width, uint32_t height);
int zzplay_video_backend_available(uint32_t flags);
int zzplay_video_result_has_frame(uint32_t flags);
ZZPlayVideoResultAction zzplay_video_result_action(uint32_t flags);

#endif /* ZZPLAY_VIDEO_H */
