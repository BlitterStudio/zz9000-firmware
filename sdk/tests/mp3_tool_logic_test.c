/*
 * Logic checks for zz9k-mp3.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define ZZ9K_MP3_NO_MAIN 1
#include "../tools/zz9k-mp3.c"

#include <stdint.h>
#include <stdio.h>

static int test_stream_pcm_ack_batch_threshold(void)
{
  if (mp3_stream_pcm_ack_batch_bytes(256UL * 1024UL) !=
      192UL * 1024UL) {
    return 1;
  }
  if (mp3_stream_pcm_ack_batch_bytes(32UL * 1024UL) != 16UL * 1024UL) {
    return 2;
  }
  if (mp3_stream_pcm_ack_due(0U, 256UL * 1024UL, 1)) {
    return 3;
  }
  if (mp3_stream_pcm_ack_due(64UL * 1024UL, 256UL * 1024UL, 0)) {
    return 4;
  }
  if (!mp3_stream_pcm_ack_due(192UL * 1024UL, 256UL * 1024UL, 0)) {
    return 5;
  }
  if (!mp3_stream_pcm_ack_due(64UL * 1024UL, 256UL * 1024UL, 1)) {
    return 6;
  }
  if (!mp3_stream_pcm_ack_due(16UL * 1024UL, 32UL * 1024UL, 0)) {
    return 7;
  }
  return 0;
}

/* While a feed is backpressured the firmware skips decoding entirely, so
 * AUDIO_STREAM_READ is the only call that lets it consume compressed input
 * and clear the condition. A forced read therefore has to go out even with
 * no PCM credit to return -- otherwise a client that has just emptied its
 * pending count retries the same rejected feed until its guard trips. */
static int test_stream_forced_read_always_due(void)
{
  if (!zzplay_mp3_pcm_read_due(0U, 256UL * 1024UL, 1)) {
    return 1;
  }
  if (zzplay_mp3_pcm_read_due(0U, 256UL * 1024UL, 0)) {
    return 2;
  }
  if (zzplay_mp3_pcm_read_due(64UL * 1024UL, 256UL * 1024UL, 0)) {
    return 3;
  }
  if (!zzplay_mp3_pcm_read_due(64UL * 1024UL, 256UL * 1024UL, 1)) {
    return 4;
  }
  if (!zzplay_mp3_pcm_read_due(192UL * 1024UL, 256UL * 1024UL, 0)) {
    return 5;
  }
  return 0;
}

/* Total milliseconds of audio the AHI queue holds across all its buffers. */
static uint32_t ahi_queue_ms(uint32_t sample_rate, uint32_t buffer_count)
{
  uint32_t frames =
      zzplay_mp3_ahi_period_frames(sample_rate, buffer_count);

  return (uint32_t)(((uint64_t)frames * buffer_count * 1000U) /
                    sample_rate);
}

/* Nothing services AHI while the producer reads the next file chunk, stages
 * it across the bus and waits out a synchronous decode call, so the queue has
 * to be deep enough to cover the longest of those stalls. The MPEG/AHI path
 * that passed hardware qualification queues 200 ms; standalone MP3 blocks for
 * longer than that path does, so it must not queue less. */
static int test_ahi_queue_covers_producer_stalls(void)
{
  if (ahi_queue_ms(44100U, 2U) < 300U) {
    return 1;
  }
  if (ahi_queue_ms(22050U, 2U) < 300U) {
    return 2;
  }
  if (zzplay_mp3_ahi_period_frames(0U, 2U) != 0U) {
    return 3;
  }
  if (zzplay_mp3_ahi_period_frames(44100U, 0U) != 0U) {
    return 4;
  }
  /* Every buffer must hold at least one frame, however odd the rate. */
  if (zzplay_mp3_ahi_period_frames(1U, 2U) == 0U) {
    return 5;
  }
  return 0;
}

static int test_stream_decode_quantum(void)
{
  if (mp3_stream_decode_quantum_bytes(0U, 256UL * 1024UL) != 0U) {
    return 1;
  }
  if (mp3_stream_decode_quantum_bytes(32UL * 1024UL,
                                      256UL * 1024UL) !=
      32UL * 1024UL) {
    return 2;
  }
  if (mp3_stream_decode_quantum_bytes(0U, 32UL * 1024UL) != 0U) {
    return 3;
  }
  if (mp3_stream_decode_quantum_bytes(256UL * 1024UL,
                                      256UL * 1024UL) != 0U) {
    return 4;
  }
  return 0;
}

static int test_stream_feed_chunk(void)
{
  if (mp3_stream_feed_chunk_bytes(0U, 0U) != STREAM_CHUNK_BYTES) {
    return 1;
  }
  if (mp3_stream_feed_chunk_bytes(0U, 64UL * 1024UL) !=
      16UL * 1024UL) {
    return 2;
  }
  if (mp3_stream_feed_chunk_bytes(0U, 32UL * 1024UL) !=
      8UL * 1024UL) {
    return 3;
  }
  if (mp3_stream_feed_chunk_bytes(12UL * 1024UL,
                                  32UL * 1024UL) !=
      12UL * 1024UL) {
    return 4;
  }
  if (mp3_stream_feed_chunk_bytes(STREAM_CHUNK_BYTES + 2U,
                                  32UL * 1024UL) != 0U) {
    return 5;
  }
  return 0;
}

static int test_stream_ring_advance_wraps(void)
{
  if (mp3_stream_ring_advance(0U, 64U, 256U) != 64U) {
    return 1;
  }
  if (mp3_stream_ring_advance(200U, 100U, 256U) != 44U) {
    return 2;
  }
  if (mp3_stream_ring_advance(7U, 0U, 256U) != 7U) {
    return 3;
  }
  return 0;
}

static int test_stream_input_room_checks(void)
{
  if (mp3_stream_input_buffered(96U, 32U) != 64U) {
    return 1;
  }
  if (mp3_stream_input_buffered(32U, 96U) != 0U) {
    return 2;
  }
  if (mp3_stream_input_room_low(96U, 32U, 128U, 64U)) {
    return 3;
  }
  if (!mp3_stream_input_room_low(97U, 32U, 128U, 64U)) {
    return 4;
  }
  if (mp3_stream_input_room_low(97U, 32U, 0U, 64U)) {
    return 5;
  }
  return 0;
}

int main(void)
{
  int result;

  result = test_stream_pcm_ack_batch_threshold();
  if (result != 0) {
    printf("test_stream_pcm_ack_batch_threshold failed: %d\n", result);
    return result;
  }
  result = test_stream_forced_read_always_due();
  if (result != 0) {
    printf("test_stream_forced_read_always_due failed: %d\n", result);
    return result;
  }
  result = test_ahi_queue_covers_producer_stalls();
  if (result != 0) {
    printf("test_ahi_queue_covers_producer_stalls failed: %d\n", result);
    return result;
  }
  result = test_stream_decode_quantum();
  if (result != 0) {
    printf("test_stream_decode_quantum failed: %d\n", result);
    return result;
  }
  result = test_stream_feed_chunk();
  if (result != 0) {
    printf("test_stream_feed_chunk failed: %d\n", result);
    return result;
  }
  result = test_stream_ring_advance_wraps();
  if (result != 0) {
    printf("test_stream_ring_advance_wraps failed: %d\n", result);
    return result;
  }
  result = test_stream_input_room_checks();
  if (result != 0) {
    printf("test_stream_input_room_checks failed: %d\n", result);
    return result;
  }

  return 0;
}
