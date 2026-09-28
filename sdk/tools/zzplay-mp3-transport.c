/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-mp3-transport.h"

uint32_t zzplay_mp3_pcm_ack_batch_bytes(uint32_t pcm_capacity)
{
  if (pcm_capacity < 2U) {
    return 0U;
  }
  if (pcm_capacity < ZZPLAY_MP3_DEFAULT_PCM_CAPACITY) {
    return (pcm_capacity / 2U) & ~1UL;
  }
  return 192UL * 1024UL;
}

int zzplay_mp3_pcm_ack_due(uint32_t pending_ack,
                           uint32_t pcm_capacity,
                           int force)
{
  if (pending_ack == 0U) {
    return 0;
  }
  return force || pending_ack >=
                      zzplay_mp3_pcm_ack_batch_bytes(pcm_capacity);
}

/* Batching decides when PCM credit is worth returning, but a forced read is
 * also the client's only way to make a backpressured firmware consume
 * compressed input: it skips decoding on the rejected feed, so nothing moves
 * until a read arrives. Send that read even with no credit pending. */
int zzplay_mp3_pcm_read_due(uint32_t pending_ack,
                            uint32_t pcm_capacity,
                            int force)
{
  return force || zzplay_mp3_pcm_ack_due(pending_ack, pcm_capacity, 0);
}

/* Standalone MP3 hands PCM to AHI from the same thread that reads the next
 * file chunk, stages it across the bus and waits out a synchronous decode
 * call. None of that services AHI, so whatever is already queued is the only
 * thing covering those stalls, and the queue has to outlast the longest of
 * them rather than the average. 400 ms across the buffers is double what the
 * hardware-qualified MPEG/AHI path queues, which is the right side to err on:
 * the only cost is start-up latency and a little MEMF_PUBLIC, and standalone
 * MP3 has no interactive control surface to make that latency visible. */
#define ZZPLAY_MP3_AHI_QUEUE_MS 400U

uint32_t zzplay_mp3_ahi_period_frames(uint32_t sample_rate,
                                      uint32_t buffer_count)
{
  uint32_t frames;

  if (sample_rate == 0U || buffer_count == 0U) {
    return 0U;
  }
  frames = (uint32_t)(((uint64_t)sample_rate * ZZPLAY_MP3_AHI_QUEUE_MS) /
                      (1000U * (uint64_t)buffer_count));
  return frames != 0U ? frames : 1U;
}

uint32_t zzplay_mp3_decode_quantum_bytes(uint32_t requested,
                                         uint32_t pcm_capacity)
{
  uint32_t quantum;

  if (pcm_capacity < 2U || requested == 0U) {
    return 0U;
  }
  quantum = requested & ~1UL;
  return quantum != 0U && quantum < pcm_capacity ? quantum : 0U;
}

uint32_t zzplay_mp3_feed_chunk_bytes(uint32_t requested,
                                     uint32_t decode_quantum)
{
  uint32_t chunk;

  if (requested != 0U) {
    chunk = requested & ~1UL;
  } else if (decode_quantum == 0U) {
    chunk = ZZPLAY_MP3_FEED_MAX_BYTES;
  } else {
    chunk = (decode_quantum / 4U) & ~1UL;
    if (chunk < ZZPLAY_MP3_FEED_MIN_BYTES) {
      chunk = ZZPLAY_MP3_FEED_MIN_BYTES;
    }
  }
  return chunk != 0U && chunk <= ZZPLAY_MP3_FEED_MAX_BYTES
             ? chunk
             : 0U;
}

uint32_t zzplay_mp3_ring_advance(uint32_t offset,
                                 uint32_t bytes,
                                 uint32_t capacity)
{
  if (capacity == 0U) {
    return offset;
  }
  bytes %= capacity;
  if (bytes >= capacity - offset) {
    return bytes - (capacity - offset);
  }
  return offset + bytes;
}

uint32_t zzplay_mp3_input_buffered(uint32_t bytes_fed,
                                   uint32_t bytes_consumed)
{
  return bytes_consumed >= bytes_fed
             ? 0U
             : bytes_fed - bytes_consumed;
}

int zzplay_mp3_input_room_low(uint32_t bytes_fed,
                              uint32_t bytes_consumed,
                              uint32_t capacity,
                              uint32_t next_feed_bytes)
{
  uint32_t buffered;

  if (capacity == 0U || next_feed_bytes == 0U ||
      next_feed_bytes > capacity) {
    return 0;
  }
  buffered = zzplay_mp3_input_buffered(bytes_fed, bytes_consumed);
  return buffered > capacity - next_feed_bytes;
}
