/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-video.h"

#define ZZPLAY_PIP_MIN_DIM 16U
#define ZZPLAY_PIP_MAX_DIM 4096U

ZZPlayVideoSinkStatus zzplay_video_sink_check(
    int board_found, uint32_t zorro_version,
    int p96_available, int pip_available)
{
  if (!board_found || (zorro_version != 2U && zorro_version != 3U)) {
    return ZZPLAY_VIDEO_SINK_UNSUPPORTED_BOARD;
  }
  if (!p96_available) {
    return ZZPLAY_VIDEO_SINK_P96_UNAVAILABLE;
  }
  if (!pip_available) {
    return ZZPLAY_VIDEO_SINK_PIP_UNAVAILABLE;
  }
  return ZZPLAY_VIDEO_SINK_READY;
}

int zzplay_video_z2_aperture_ready(const ZZ9KApertureLayout *layout,
                                   uint32_t width, uint32_t height)
{
  uint32_t flags;
  uint32_t pitch;
  uint32_t bytes;
  uint32_t required = ZZ9K_APERTURE_FLAG_VALID |
                      ZZ9K_APERTURE_FLAG_ACKED |
                      ZZ9K_APERTURE_FLAG_PIP;

  if (!layout) {
    return 0;
  }
  flags = layout->profile & ZZ9K_APERTURE_LAYOUT_FLAGS_MASK;
  if (((layout->profile & ZZ9K_APERTURE_LAYOUT_GENERATION_MASK) >>
       ZZ9K_APERTURE_LAYOUT_GENERATION_SHIFT) !=
          ZZ9K_APERTURE_LAYOUT_GENERATION_1 ||
      (flags & required) != required || layout->pip_size == 0U ||
      width < ZZPLAY_PIP_MIN_DIM || height < ZZPLAY_PIP_MIN_DIM ||
      width > ZZPLAY_PIP_MAX_DIM || height > ZZPLAY_PIP_MAX_DIM ||
      (width & 1U) != 0U) {
    return 0;
  }
  pitch = width * 2U;
  if (height > 0xffffffffUL / pitch) {
    return 0;
  }
  bytes = pitch * height;
  return bytes <= layout->pip_size;
}

int zzplay_video_backend_available(uint32_t flags)
{
  return (flags & ZZPLAY_REQUIRED_VIDEO_FLAGS) ==
         ZZPLAY_REQUIRED_VIDEO_FLAGS;
}

int zzplay_video_result_has_frame(uint32_t flags)
{
  return (flags & ZZ9K_VIDEO_SESSION_RESULT_FRAME_READY) != 0U;
}

ZZPlayVideoResultAction zzplay_video_result_action(uint32_t flags)
{
  if ((flags & ZZ9K_VIDEO_SESSION_RESULT_DONE) != 0U) {
    return ZZPLAY_VIDEO_RESULT_DONE;
  }
  if ((flags & ZZ9K_VIDEO_SESSION_RESULT_NEED_INPUT) != 0U) {
    return ZZPLAY_VIDEO_RESULT_NEED_INPUT;
  }
  if ((flags & (ZZ9K_VIDEO_SESSION_RESULT_FRAME_READY |
                ZZ9K_VIDEO_SESSION_RESULT_HEADER_READY)) != 0U) {
    return ZZPLAY_VIDEO_RESULT_CONTINUE;
  }
  return ZZPLAY_VIDEO_RESULT_INVALID;
}
