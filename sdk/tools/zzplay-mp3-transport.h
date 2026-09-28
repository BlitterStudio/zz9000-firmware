/* Shared accelerated MP3 streaming/backpressure calculations.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_MP3_TRANSPORT_H
#define ZZPLAY_MP3_TRANSPORT_H

#include <stdint.h>

#define ZZPLAY_MP3_INPUT_CAPACITY (128UL * 1024UL)
#define ZZPLAY_MP3_FEED_MAX_BYTES (64UL * 1024UL)
#define ZZPLAY_MP3_FEED_MIN_BYTES (4UL * 1024UL)
#define ZZPLAY_MP3_DEFAULT_PCM_CAPACITY (256UL * 1024UL)

uint32_t zzplay_mp3_pcm_ack_batch_bytes(uint32_t pcm_capacity);
int zzplay_mp3_pcm_ack_due(uint32_t pending_ack,
                           uint32_t pcm_capacity,
                           int force);
int zzplay_mp3_pcm_read_due(uint32_t pending_ack,
                            uint32_t pcm_capacity,
                            int force);
uint32_t zzplay_mp3_ahi_period_frames(uint32_t sample_rate,
                                      uint32_t buffer_count);
uint32_t zzplay_mp3_decode_quantum_bytes(uint32_t requested,
                                         uint32_t pcm_capacity);
uint32_t zzplay_mp3_feed_chunk_bytes(uint32_t requested,
                                     uint32_t decode_quantum);
uint32_t zzplay_mp3_ring_advance(uint32_t offset,
                                 uint32_t bytes,
                                 uint32_t capacity);
uint32_t zzplay_mp3_input_buffered(uint32_t bytes_fed,
                                   uint32_t bytes_consumed);
int zzplay_mp3_input_room_low(uint32_t bytes_fed,
                              uint32_t bytes_consumed,
                              uint32_t capacity,
                              uint32_t next_feed_bytes);

#endif /* ZZPLAY_MP3_TRANSPORT_H */
