/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-media.h"

#include <string.h>

void zzplay_pcm_ring_init(ZZPlayPCMRing *ring,
                          const ZZ9KSharedBuffer *shared)
{
  if (!ring) {
    return;
  }
  if (shared) {
    ring->shared = *shared;
    ring->capacity = shared->length;
  } else {
    memset(&ring->shared, 0, sizeof(ring->shared));
    ring->capacity = 0U;
  }
  ring->acknowledged = 0U;
}

uint64_t zzplay_pcm_ring_available(const ZZPlayPCMRing *ring,
                                   uint64_t produced)
{
  uint64_t available;

  if (!ring || !ring->shared.data || ring->capacity == 0U ||
      produced < ring->acknowledged) {
    return 0U;
  }
  available = produced - ring->acknowledged;
  return available <= ring->capacity ? available : 0U;
}

size_t zzplay_pcm_ring_copy(const ZZPlayPCMRing *ring,
                            uint64_t produced,
                            void *destination,
                            size_t destination_bytes,
                            uint32_t frame_bytes)
{
  uint64_t available;
  size_t bytes;
  size_t first;
  uint32_t offset;

  if (!ring || !destination || frame_bytes == 0U) {
    return 0U;
  }
  available = zzplay_pcm_ring_available(ring, produced);
  bytes = available < destination_bytes
              ? (size_t)available
              : destination_bytes;
  bytes -= bytes % frame_bytes;
  if (bytes == 0U) {
    return 0U;
  }
  offset = (uint32_t)(ring->acknowledged % ring->capacity);
  first = bytes;
  if (first > ring->capacity - offset) {
    first = ring->capacity - offset;
  }
  if (!zz9k_shared_copy_from(
          destination, &ring->shared, offset, (uint32_t)first)) {
    return 0U;
  }
  if (first < bytes &&
      !zz9k_shared_copy_from(
          (uint8_t *)destination + first, &ring->shared, 0U,
          (uint32_t)(bytes - first))) {
    return 0U;
  }
  return bytes;
}

int zzplay_pcm_ring_acknowledge(ZZPlayPCMRing *ring,
                                uint64_t produced,
                                size_t bytes)
{
  uint64_t available;

  if (!ring) {
    return 0;
  }
  available = zzplay_pcm_ring_available(ring, produced);
  if ((uint64_t)bytes > available) {
    return 0;
  }
  ring->acknowledged += bytes;
  return 1;
}

ZZPlayMediaAction zzplay_media_result_action(uint32_t flags)
{
  if ((flags & ZZ9K_MEDIA_SESSION_RESULT_DONE) != 0U) {
    return ZZPLAY_MEDIA_DONE;
  }
  if ((flags & ZZ9K_MEDIA_SESSION_RESULT_FRAME_HELD) != 0U) {
    return ZZPLAY_MEDIA_FRAME_HELD;
  }
  if ((flags & ZZ9K_MEDIA_SESSION_RESULT_NEED_INPUT) != 0U) {
    return ZZPLAY_MEDIA_NEED_INPUT;
  }
  if ((flags & (ZZ9K_MEDIA_SESSION_RESULT_HEADER_READY |
                ZZ9K_MEDIA_SESSION_RESULT_AUDIO_READY |
                ZZ9K_MEDIA_SESSION_RESULT_BACKPRESSURE |
                ZZ9K_MEDIA_SESSION_RESULT_PRESENTED |
                ZZ9K_MEDIA_SESSION_RESULT_DISCARDED)) != 0U) {
    return ZZPLAY_MEDIA_CONTINUE;
  }
  return ZZPLAY_MEDIA_CONTINUE;
}
