/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-audio.h"

static ZZPlayBackendDecision zzplay_audio_decision(
    ZZPlayBackendStatus status,
    ZZPlayAudioBackend selected,
    int fell_back)
{
  ZZPlayBackendDecision decision;

  decision.status = status;
  decision.selected = selected;
  decision.fell_back = fell_back;
  return decision;
}

static ZZPlayBackendAvailability zzplay_audio_availability(
    ZZPlayAudioBackend backend,
    const ZZPlayAudioAvailability *availability)
{
  if (!availability) {
    return ZZPLAY_BACKEND_MISSING;
  }
  if (backend == ZZPLAY_AUDIO_AHI) {
    return availability->ahi;
  }
  if (backend == ZZPLAY_AUDIO_MHI) {
    return availability->mhi;
  }
  if (backend == ZZPLAY_AUDIO_AX) {
    return availability->ax;
  }
  return ZZPLAY_BACKEND_FREE;
}

static int zzplay_audio_supports(ZZPlayMediaAudio media,
                                 ZZPlayAudioBackend backend)
{
  if (backend == ZZPLAY_AUDIO_NONE) {
    return 1;
  }
  if (backend == ZZPLAY_AUDIO_MHI) {
    return media == ZZPLAY_MEDIA_AUDIO_MP3;
  }
  if (backend == ZZPLAY_AUDIO_AHI) {
    return media == ZZPLAY_MEDIA_AUDIO_MP2 ||
           media == ZZPLAY_MEDIA_AUDIO_MP3;
  }
  if (backend == ZZPLAY_AUDIO_AX) {
    return media == ZZPLAY_MEDIA_AUDIO_MP2;
  }
  return 0;
}

static ZZPlayBackendDecision zzplay_audio_select_explicit(
    ZZPlayMediaAudio media,
    ZZPlayAudioBackend backend,
    const ZZPlayAudioAvailability *availability)
{
  ZZPlayBackendAvailability state;

  if (!zzplay_audio_supports(media, backend)) {
    return zzplay_audio_decision(ZZPLAY_BACKEND_UNSUPPORTED,
                                 ZZPLAY_AUDIO_NONE, 0);
  }
  if (backend == ZZPLAY_AUDIO_NONE) {
    return zzplay_audio_decision(ZZPLAY_BACKEND_OK, backend, 0);
  }
  state = zzplay_audio_availability(backend, availability);
  if (state == ZZPLAY_BACKEND_BUSY) {
    return zzplay_audio_decision(ZZPLAY_BACKEND_BUSY_RESULT,
                                 ZZPLAY_AUDIO_NONE, 0);
  }
  if (state == ZZPLAY_BACKEND_MISSING) {
    return zzplay_audio_decision(ZZPLAY_BACKEND_MISSING_RESULT,
                                 ZZPLAY_AUDIO_NONE, 0);
  }
  return zzplay_audio_decision(ZZPLAY_BACKEND_OK, backend, 0);
}

ZZPlayBackendDecision zzplay_audio_select(
    ZZPlayMediaAudio media,
    ZZPlayAudioBackend requested,
    const ZZPlayAudioAvailability *availability)
{
  ZZPlayBackendDecision decision;

  if (requested != ZZPLAY_AUDIO_AUTO) {
    return zzplay_audio_select_explicit(media, requested, availability);
  }
  if (media == ZZPLAY_MEDIA_AUDIO_NONE) {
    return zzplay_audio_decision(ZZPLAY_BACKEND_OK, ZZPLAY_AUDIO_NONE, 0);
  }
  if (media == ZZPLAY_MEDIA_AUDIO_MP3) {
    decision = zzplay_audio_select_explicit(media, ZZPLAY_AUDIO_MHI,
                                            availability);
    if (decision.status == ZZPLAY_BACKEND_OK) {
      return decision;
    }
  }
  if (media == ZZPLAY_MEDIA_AUDIO_MP2) {
    decision = zzplay_audio_select_explicit(media, ZZPLAY_AUDIO_AX,
                                            availability);
    if (decision.status == ZZPLAY_BACKEND_OK) {
      return decision;
    }
  }
  decision = zzplay_audio_select_explicit(media, ZZPLAY_AUDIO_AHI,
                                          availability);
  if (decision.status == ZZPLAY_BACKEND_OK) {
    decision.fell_back =
        media == ZZPLAY_MEDIA_AUDIO_MP3 ||
        media == ZZPLAY_MEDIA_AUDIO_MP2;
    return decision;
  }
  if (media == ZZPLAY_MEDIA_AUDIO_MP2) {
    return decision;
  }
  return decision;
}

int zzplay_audio_start_ready(ZZPlayAudioBackend backend,
                             uint64_t queued_frames,
                             uint64_t prebuffer_target_frames)
{
  if (queued_frames == 0U) {
    return 0;
  }
  if (backend != ZZPLAY_AUDIO_AHI) {
    return 1;
  }
  return prebuffer_target_frames != 0U &&
         queued_frames >= prebuffer_target_frames;
}
