/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "../tools/zzplay/zzplay-audio.h"
#include "../tools/zzplay/zzplay-video.h"

#include <string.h>

static int check_video_preflight(void)
{
  if (zzplay_video_sink_check(1, 3U, 1, 1) !=
          ZZPLAY_VIDEO_SINK_READY ||
      zzplay_video_sink_check(0, 0U, 0, 0) !=
          ZZPLAY_VIDEO_SINK_UNSUPPORTED_BOARD ||
      zzplay_video_sink_check(1, 2U, 1, 1) !=
          ZZPLAY_VIDEO_SINK_READY ||
      zzplay_video_sink_check(1, 3U, 0, 0) !=
          ZZPLAY_VIDEO_SINK_P96_UNAVAILABLE ||
      zzplay_video_sink_check(1, 3U, 1, 0) !=
          ZZPLAY_VIDEO_SINK_PIP_UNAVAILABLE) {
    return 0;
  }
  return 1;
}

static int check_z2_aperture(void)
{
  ZZ9KApertureLayout layout;

  memset(&layout, 0, sizeof(layout));
  layout.profile = ZZ9K_APERTURE_PROFILE(
      ZZ9K_APERTURE_LAYOUT_GENERATION_1,
      ZZ9K_APERTURE_FLAG_VALID | ZZ9K_APERTURE_FLAG_ACKED |
          ZZ9K_APERTURE_FLAG_HOST_WINDOW | ZZ9K_APERTURE_FLAG_PIP);
  layout.pip_size = 0x38000U;

  if (!zzplay_video_z2_aperture_ready(&layout, 320U, 240U) ||
      !zzplay_video_z2_aperture_ready(&layout, 368U, 300U) ||
      !zzplay_video_z2_aperture_ready(&layout, 1024U, 112U) ||
      zzplay_video_z2_aperture_ready(&layout, 1024U, 113U) ||
      zzplay_video_z2_aperture_ready(&layout, 640U, 480U) ||
      zzplay_video_z2_aperture_ready(&layout, 367U, 300U) ||
      zzplay_video_z2_aperture_ready(&layout, 14U, 300U) ||
      zzplay_video_z2_aperture_ready(&layout, 4098U, 16U))
    return 0;
  layout.profile &= ~ZZ9K_APERTURE_FLAG_ACKED;
  if (zzplay_video_z2_aperture_ready(&layout, 320U, 240U))
    return 0;
  layout.profile |= ZZ9K_APERTURE_FLAG_ACKED;
  layout.pip_size = 0U;
  return !zzplay_video_z2_aperture_ready(&layout, 320U, 240U) &&
         !zzplay_video_z2_aperture_ready(NULL, 320U, 240U);
}

static int check_selection(ZZPlayMediaAudio media,
                           ZZPlayAudioBackend requested,
                           ZZPlayBackendAvailability ahi,
                           ZZPlayBackendAvailability mhi,
                           ZZPlayBackendAvailability ax,
                           ZZPlayBackendStatus expected_status,
                           ZZPlayAudioBackend expected_backend,
                           int expected_fallback)
{
  ZZPlayAudioAvailability availability;
  ZZPlayBackendDecision decision;

  memset(&availability, 0, sizeof(availability));
  availability.ahi = ahi;
  availability.mhi = mhi;
  availability.ax = ax;
  decision = zzplay_audio_select(media, requested, &availability);
  return decision.status == expected_status &&
         decision.selected == expected_backend &&
         decision.fell_back == expected_fallback;
}

static int check_audio_policy(void)
{
  if (!check_selection(ZZPLAY_MEDIA_AUDIO_NONE, ZZPLAY_AUDIO_AUTO,
                       ZZPLAY_BACKEND_MISSING, ZZPLAY_BACKEND_MISSING,
                       ZZPLAY_BACKEND_MISSING, ZZPLAY_BACKEND_OK,
                       ZZPLAY_AUDIO_NONE, 0) ||
      !check_selection(ZZPLAY_MEDIA_AUDIO_MP3, ZZPLAY_AUDIO_AUTO,
                       ZZPLAY_BACKEND_FREE, ZZPLAY_BACKEND_FREE,
                       ZZPLAY_BACKEND_MISSING, ZZPLAY_BACKEND_OK,
                       ZZPLAY_AUDIO_MHI, 0) ||
      !check_selection(ZZPLAY_MEDIA_AUDIO_MP3, ZZPLAY_AUDIO_AUTO,
                       ZZPLAY_BACKEND_FREE, ZZPLAY_BACKEND_BUSY,
                       ZZPLAY_BACKEND_MISSING, ZZPLAY_BACKEND_OK,
                       ZZPLAY_AUDIO_AHI, 1) ||
      !check_selection(ZZPLAY_MEDIA_AUDIO_MP2, ZZPLAY_AUDIO_AUTO,
                       ZZPLAY_BACKEND_FREE, ZZPLAY_BACKEND_FREE,
                       ZZPLAY_BACKEND_FREE, ZZPLAY_BACKEND_OK,
                       ZZPLAY_AUDIO_AX, 0) ||
      !check_selection(ZZPLAY_MEDIA_AUDIO_MP2, ZZPLAY_AUDIO_MHI,
                       ZZPLAY_BACKEND_FREE, ZZPLAY_BACKEND_FREE,
                       ZZPLAY_BACKEND_FREE, ZZPLAY_BACKEND_UNSUPPORTED,
                       ZZPLAY_AUDIO_NONE, 0) ||
      !check_selection(ZZPLAY_MEDIA_AUDIO_MP2, ZZPLAY_AUDIO_AX,
                       ZZPLAY_BACKEND_FREE, ZZPLAY_BACKEND_MISSING,
                       ZZPLAY_BACKEND_FREE, ZZPLAY_BACKEND_OK,
                       ZZPLAY_AUDIO_AX, 0) ||
      !check_selection(ZZPLAY_MEDIA_AUDIO_MP2, ZZPLAY_AUDIO_AUTO,
                       ZZPLAY_BACKEND_FREE, ZZPLAY_BACKEND_FREE,
                       ZZPLAY_BACKEND_MISSING, ZZPLAY_BACKEND_OK,
                       ZZPLAY_AUDIO_AHI, 1) ||
      !check_selection(ZZPLAY_MEDIA_AUDIO_MP2, ZZPLAY_AUDIO_AUTO,
                       ZZPLAY_BACKEND_BUSY, ZZPLAY_BACKEND_FREE,
                       ZZPLAY_BACKEND_BUSY, ZZPLAY_BACKEND_BUSY_RESULT,
                       ZZPLAY_AUDIO_NONE, 0) ||
      !check_selection(ZZPLAY_MEDIA_AUDIO_MP3, ZZPLAY_AUDIO_MHI,
                       ZZPLAY_BACKEND_FREE, ZZPLAY_BACKEND_BUSY,
                       ZZPLAY_BACKEND_MISSING, ZZPLAY_BACKEND_BUSY_RESULT,
                       ZZPLAY_AUDIO_NONE, 0) ||
      !check_selection(ZZPLAY_MEDIA_AUDIO_MP3, ZZPLAY_AUDIO_MHI,
                       ZZPLAY_BACKEND_FREE, ZZPLAY_BACKEND_MISSING,
                       ZZPLAY_BACKEND_MISSING, ZZPLAY_BACKEND_MISSING_RESULT,
                       ZZPLAY_AUDIO_NONE, 0) ||
      !check_selection(ZZPLAY_MEDIA_AUDIO_MP3, ZZPLAY_AUDIO_AHI,
                       ZZPLAY_BACKEND_BUSY, ZZPLAY_BACKEND_FREE,
                       ZZPLAY_BACKEND_FREE, ZZPLAY_BACKEND_BUSY_RESULT,
                       ZZPLAY_AUDIO_NONE, 0) ||
      !check_selection(ZZPLAY_MEDIA_AUDIO_MP3, ZZPLAY_AUDIO_AHI,
                       ZZPLAY_BACKEND_MISSING, ZZPLAY_BACKEND_FREE,
                       ZZPLAY_BACKEND_FREE, ZZPLAY_BACKEND_MISSING_RESULT,
                       ZZPLAY_AUDIO_NONE, 0) ||
      !check_selection(ZZPLAY_MEDIA_AUDIO_MP3, ZZPLAY_AUDIO_AX,
                       ZZPLAY_BACKEND_FREE, ZZPLAY_BACKEND_FREE,
                       ZZPLAY_BACKEND_FREE, ZZPLAY_BACKEND_UNSUPPORTED,
                       ZZPLAY_AUDIO_NONE, 0) ||
      !check_selection(ZZPLAY_MEDIA_AUDIO_MP3, ZZPLAY_AUDIO_AUTO,
                       ZZPLAY_BACKEND_MISSING, ZZPLAY_BACKEND_BUSY,
                       ZZPLAY_BACKEND_FREE, ZZPLAY_BACKEND_MISSING_RESULT,
                       ZZPLAY_AUDIO_NONE, 0) ||
      !check_selection(ZZPLAY_MEDIA_AUDIO_MP3, ZZPLAY_AUDIO_NONE,
                       ZZPLAY_BACKEND_FREE, ZZPLAY_BACKEND_FREE,
                       ZZPLAY_BACKEND_FREE, ZZPLAY_BACKEND_OK,
                       ZZPLAY_AUDIO_NONE, 0)) {
    return 0;
  }
  return 1;
}

static int check_audio_start_prebuffer(void)
{
  if (zzplay_audio_start_ready(
          ZZPLAY_AUDIO_AHI, 0U, 9600U) ||
      zzplay_audio_start_ready(
          ZZPLAY_AUDIO_AHI, 4800U, 9600U) ||
      zzplay_audio_start_ready(
          ZZPLAY_AUDIO_AHI, 9600U, 0U) ||
      !zzplay_audio_start_ready(
          ZZPLAY_AUDIO_AHI, 9600U, 9600U) ||
      !zzplay_audio_start_ready(
          ZZPLAY_AUDIO_AHI, 12000U, 9600U) ||
      !zzplay_audio_start_ready(
          ZZPLAY_AUDIO_AX, 1U, 0U)) {
    return 0;
  }
  return 1;
}

static int check_card_case(ZZPlayAudioBackend requested, int strict,
                           ZZPlayCardAnswer answer, ZZPlayCardPath path,
                           int fell_back, const char *message)
{
  ZZPlayCardDecision decision =
      zzplay_card_stream_decide(requested, strict, answer);

  if (decision.path != path || decision.fell_back != fell_back) {
    return 0;
  }
  if (!message) {
    return decision.message == 0;
  }
  return decision.message && strcmp(decision.message, message) == 0;
}

/* The strict-MHI refusal must name both outputs FLAC/Vorbis can use, not
 * claim AHI is the only one: they play on the card first. */
static int check_card_mhi_refusal(void)
{
  ZZPlayCardDecision decision =
      zzplay_card_stream_decide(ZZPLAY_AUDIO_MHI, 1, ZZPLAY_CARD_NOT_ASKED);

  return decision.path == ZZPLAY_CARD_PATH_REFUSED && decision.message &&
         !strstr(decision.message, "AHI only") &&
         strstr(decision.message, "card") &&
         strstr(decision.message, "AHI") && strstr(decision.message, "MHI");
}

static int check_card_policy(void)
{
  const char *mhi = "%s plays on the card or through AHI, not MHI";
  const char *unsupported =
      "on-card %s playback refused: unsupported stream";
  const char *busy = "on-card %s playback refused: ZZ9000AX is busy";
  const char *failed = "on-card %s playback failed";

  if (!check_card_mhi_refusal()) {
    return 0;
  }
  if (!check_card_case(ZZPLAY_AUDIO_AUTO, 0, ZZPLAY_CARD_NOT_ASKED,
                       ZZPLAY_CARD_PATH_AX, 0, 0) ||
      !check_card_case(ZZPLAY_AUDIO_AUTO, 0, ZZPLAY_CARD_OK,
                       ZZPLAY_CARD_PATH_AX, 0, 0) ||
      !check_card_case(ZZPLAY_AUDIO_AUTO, 0, ZZPLAY_CARD_UNSUPPORTED,
                       ZZPLAY_CARD_PATH_AHI, 1, 0) ||
      !check_card_case(ZZPLAY_AUDIO_AUTO, 0, ZZPLAY_CARD_BUSY,
                       ZZPLAY_CARD_PATH_AHI, 1, 0) ||
      !check_card_case(ZZPLAY_AUDIO_AUTO, 0, ZZPLAY_CARD_FAILED,
                       ZZPLAY_CARD_PATH_REFUSED, 0, failed) ||
      !check_card_case(ZZPLAY_AUDIO_AX, 1, ZZPLAY_CARD_OK,
                       ZZPLAY_CARD_PATH_AX, 0, 0) ||
      !check_card_case(ZZPLAY_AUDIO_AX, 1, ZZPLAY_CARD_UNSUPPORTED,
                       ZZPLAY_CARD_PATH_REFUSED, 0, unsupported) ||
      !check_card_case(ZZPLAY_AUDIO_AX, 1, ZZPLAY_CARD_BUSY,
                       ZZPLAY_CARD_PATH_REFUSED, 0, busy) ||
      !check_card_case(ZZPLAY_AUDIO_AX, 0, ZZPLAY_CARD_UNSUPPORTED,
                       ZZPLAY_CARD_PATH_AHI, 1, 0) ||
      !check_card_case(ZZPLAY_AUDIO_AX, 0, ZZPLAY_CARD_BUSY,
                       ZZPLAY_CARD_PATH_AHI, 1, 0) ||
      !check_card_case(ZZPLAY_AUDIO_AHI, 1, ZZPLAY_CARD_UNSUPPORTED,
                       ZZPLAY_CARD_PATH_AHI, 0, 0) ||
      !check_card_case(ZZPLAY_AUDIO_AHI, 0, ZZPLAY_CARD_BUSY,
                       ZZPLAY_CARD_PATH_AHI, 0, 0) ||
      !check_card_case(ZZPLAY_AUDIO_MHI, 1, ZZPLAY_CARD_OK,
                       ZZPLAY_CARD_PATH_REFUSED, 0, mhi) ||
      !check_card_case(ZZPLAY_AUDIO_MHI, 0, ZZPLAY_CARD_UNSUPPORTED,
                       ZZPLAY_CARD_PATH_AHI, 1, 0) ||
      !check_card_case(ZZPLAY_AUDIO_NONE, 1, ZZPLAY_CARD_OK,
                       ZZPLAY_CARD_PATH_NONE, 0, 0)) {
    return 0;
  }
  return 1;
}

static int check_card_gain(void)
{
  return zzplay_card_stream_gain(0U) == 0U &&
         zzplay_card_stream_gain(50U) == 64U &&
         zzplay_card_stream_gain(100U) == 128U;
}

int main(void)
{
  if (!check_video_preflight()) {
    return 1;
  }
  if (!check_audio_policy()) {
    return 2;
  }
  if (!check_z2_aperture()) {
    return 3;
  }
  if (!check_audio_start_prebuffer()) {
    return 4;
  }
  if (!check_card_policy()) {
    return 5;
  }
  if (!check_card_gain()) {
    return 6;
  }
  return 0;
}
