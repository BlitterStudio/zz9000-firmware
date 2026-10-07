/*
 * Host-visible media-session PCM ring helpers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZPLAY_MEDIA_H
#define ZZPLAY_MEDIA_H

#include "zz9k/abi.h"
#include "zz9k/shared.h"

#include <stddef.h>
#include <stdint.h>

typedef struct ZZPlayPCMRing {
  ZZ9KSharedBuffer shared;
  uint32_t capacity;
  uint64_t acknowledged;
} ZZPlayPCMRing;

typedef enum ZZPlayMediaAction {
  ZZPLAY_MEDIA_INVALID = 0,
  ZZPLAY_MEDIA_CONTINUE,
  ZZPLAY_MEDIA_NEED_INPUT,
  ZZPLAY_MEDIA_FRAME_HELD,
  ZZPLAY_MEDIA_DONE
} ZZPlayMediaAction;

void zzplay_pcm_ring_init(ZZPlayPCMRing *ring,
                          const ZZ9KSharedBuffer *shared);
uint64_t zzplay_pcm_ring_available(const ZZPlayPCMRing *ring,
                                   uint64_t produced);
size_t zzplay_pcm_ring_copy(const ZZPlayPCMRing *ring,
                            uint64_t produced,
                            void *destination,
                            size_t destination_bytes,
                            uint32_t frame_bytes);
int zzplay_pcm_ring_acknowledge(ZZPlayPCMRing *ring,
                                uint64_t produced,
                                size_t bytes);
ZZPlayMediaAction zzplay_media_result_action(uint32_t flags);

#endif /* ZZPLAY_MEDIA_H */
