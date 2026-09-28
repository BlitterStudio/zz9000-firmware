/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "../tools/zzplay-media.h"

#include <stdint.h>
#include <string.h>

static int check_linear_and_alignment(void)
{
  uint8_t source[16];
  uint8_t destination[16];
  ZZ9KSharedBuffer shared;
  ZZPlayPCMRing ring;
  unsigned i;

  for (i = 0U; i < sizeof(source); i++) {
    source[i] = (uint8_t)i;
  }
  memset(destination, 0xff, sizeof(destination));
  memset(&shared, 0, sizeof(shared));
  shared.handle = 1U;
  shared.length = sizeof(source);
  shared.data = source;
  zzplay_pcm_ring_init(&ring, &shared);
  if (zzplay_pcm_ring_copy(
          &ring, 10U, destination, 9U, 4U) != 8U ||
      memcmp(source, destination, 8U) != 0 ||
      !zzplay_pcm_ring_acknowledge(&ring, 10U, 8U) ||
      ring.acknowledged != 8U ||
      zzplay_pcm_ring_available(&ring, 10U) != 2U) {
    return 0;
  }
  return 1;
}

static int check_wrap_and_invalid_progress(void)
{
  uint8_t source[8] = { 8U, 9U, 10U, 11U, 4U, 5U, 6U, 7U };
  uint8_t destination[8];
  static const uint8_t expected[8] =
      { 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U };
  ZZ9KSharedBuffer shared;
  ZZPlayPCMRing ring;

  memset(&shared, 0, sizeof(shared));
  shared.handle = 2U;
  shared.length = sizeof(source);
  shared.data = source;
  zzplay_pcm_ring_init(&ring, &shared);
  ring.acknowledged = 4U;
  if (zzplay_pcm_ring_copy(
          &ring, 12U, destination, sizeof(destination), 4U) !=
          sizeof(destination) ||
      memcmp(destination, expected, sizeof(expected)) != 0 ||
      zzplay_pcm_ring_acknowledge(&ring, 11U, 8U) ||
      zzplay_pcm_ring_available(&ring, 13U) != 0U) {
    return 0;
  }
  return 1;
}

int main(void)
{
  if (!check_linear_and_alignment()) {
    return 1;
  }
  if (!check_wrap_and_invalid_progress()) {
    return 2;
  }
  if (zzplay_media_result_action(
          ZZ9K_MEDIA_SESSION_RESULT_FRAME_HELD) !=
          ZZPLAY_MEDIA_FRAME_HELD ||
      zzplay_media_result_action(
          ZZ9K_MEDIA_SESSION_RESULT_DONE |
          ZZ9K_MEDIA_SESSION_RESULT_AUDIO_READY) !=
          ZZPLAY_MEDIA_DONE ||
      zzplay_media_result_action(
          ZZ9K_MEDIA_SESSION_RESULT_NEED_INPUT) !=
          ZZPLAY_MEDIA_NEED_INPUT ||
      zzplay_media_result_action(0U) != ZZPLAY_MEDIA_CONTINUE) {
    return 3;
  }
  return 0;
}
