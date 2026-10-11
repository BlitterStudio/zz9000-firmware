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

static int zzplay_audio_is_program(ZZPlayMediaAudio media)
{
  return media == ZZPLAY_MEDIA_AUDIO_MP2 ||
         media == ZZPLAY_MEDIA_AUDIO_OPUS ||
         media == ZZPLAY_MEDIA_AUDIO_VORBIS;
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
    return media == ZZPLAY_MEDIA_AUDIO_MP3 ||
           zzplay_audio_is_program(media);
  }
  if (backend == ZZPLAY_AUDIO_AX) {
    return zzplay_audio_is_program(media);
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
  if (zzplay_audio_is_program(media)) {
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
        zzplay_audio_is_program(media);
    return decision;
  }
  if (zzplay_audio_is_program(media)) {
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

static const char zzplay_card_mhi_refusal[] =
    "%s plays on the card or through AHI, not MHI";
static const char zzplay_card_unsupported[] =
    "on-card %s playback refused: unsupported stream";
static const char zzplay_card_busy[] =
    "on-card %s playback refused: ZZ9000AX is busy";
static const char zzplay_card_failed[] = "on-card %s playback failed";

static ZZPlayCardDecision zzplay_card_decision(ZZPlayCardPath path,
                                               int fell_back,
                                               const char *message)
{
  ZZPlayCardDecision decision;

  decision.path = path;
  decision.fell_back = fell_back;
  decision.message = message;
  return decision;
}

static int zzplay_card_output_refused(ZZPlayCardAnswer answer)
{
  return answer == ZZPLAY_CARD_UNSUPPORTED || answer == ZZPLAY_CARD_BUSY;
}

static const char *zzplay_card_refusal_message(ZZPlayCardAnswer answer)
{
  if (answer == ZZPLAY_CARD_UNSUPPORTED) {
    return zzplay_card_unsupported;
  }
  if (answer == ZZPLAY_CARD_BUSY) {
    return zzplay_card_busy;
  }
  return zzplay_card_failed;
}

ZZPlayCardDecision zzplay_card_stream_decide(ZZPlayAudioBackend requested,
                                             int strict,
                                             ZZPlayCardAnswer answer)
{
  int try_card;

  if (requested == ZZPLAY_AUDIO_NONE) {
    return zzplay_card_decision(ZZPLAY_CARD_PATH_NONE, 0, 0);
  }
  if (requested == ZZPLAY_AUDIO_AHI) {
    return zzplay_card_decision(ZZPLAY_CARD_PATH_AHI, 0, 0);
  }
  if (requested == ZZPLAY_AUDIO_MHI && strict) {
    return zzplay_card_decision(ZZPLAY_CARD_PATH_REFUSED, 0,
                                zzplay_card_mhi_refusal);
  }
  /* AUTO, strict or saved AX, and a non-strict MHI preference (MHI cannot
   * play these files, so it is not a reason to refuse them). */
  try_card = requested == ZZPLAY_AUDIO_AUTO ||
             requested == ZZPLAY_AUDIO_AX ||
             requested == ZZPLAY_AUDIO_MHI;
  if (!try_card) {
    return zzplay_card_decision(ZZPLAY_CARD_PATH_REFUSED, 0,
                                zzplay_card_failed);
  }
  if (answer == ZZPLAY_CARD_NOT_ASKED || answer == ZZPLAY_CARD_OK) {
    return zzplay_card_decision(ZZPLAY_CARD_PATH_AX, 0, 0);
  }
  if (zzplay_card_output_refused(answer) &&
      !(strict && requested == ZZPLAY_AUDIO_AX)) {
    return zzplay_card_decision(ZZPLAY_CARD_PATH_AHI, 1, 0);
  }
  return zzplay_card_decision(ZZPLAY_CARD_PATH_REFUSED, 0,
                              zzplay_card_refusal_message(answer));
}

uint32_t zzplay_card_stream_gain(uint32_t percent)
{
  if (percent > 100U) {
    percent = 100U;
  }
  return (percent * 128U) / 100U;
}
