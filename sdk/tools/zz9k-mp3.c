/*
 * ZZ9000 SDK MP3-to-PCM decode smoke tool.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/audio.h"
#include "zz9k/caps.h"
#include "zz9k/host.h"
#include "zz9k/shared.h"
#include "zzplay-mp3-transport.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
    defined(__VBCC__)
#define ZZ9K_MP3_AMIGA 1
#include <devices/timer.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/timer.h>
#else
#include <time.h>
#endif

#define DEFAULT_OUTPUT_CAPACITY (256UL * 1024UL)
#define DEFAULT_STREAM_PCM_CAPACITY DEFAULT_OUTPUT_CAPACITY
#define STREAM_INPUT_CAPACITY (128UL * 1024UL)
#define STREAM_CHUNK_BYTES (64UL * 1024UL)
#define STREAM_DECODE_QUANTUM_BYTES STREAM_CHUNK_BYTES
#define STREAM_MIN_FEED_CHUNK_BYTES (4UL * 1024UL)
#define STREAM_OUTPUT_CHUNK_BYTES (32UL * 1024UL)
#define STREAM_PCM_ACK_BATCH_BYTES (192UL * 1024UL)

#ifndef ZZ9K_MP3_NO_MAIN
#define ZZ9K_MP3_NO_MAIN 0
#endif

typedef uint64_t ZZ9KMP3Tick;

typedef struct ZZ9KMP3Timer {
  uint32_t ticks_per_second;
  int high_resolution;
#if ZZ9K_MP3_AMIGA
  struct MsgPort *timer_port;
  struct timerequest *timer_request;
#endif
} ZZ9KMP3Timer;

typedef struct ZZ9KMP3StreamStats {
  ZZ9KMP3Timer timer;
  ZZ9KMP3Tick total_ticks;
  ZZ9KMP3Tick file_read_ticks;
  ZZ9KMP3Tick staging_copy_ticks;
  ZZ9KMP3Tick feed_ticks;
  ZZ9KMP3Tick pcm_copy_ticks;
  ZZ9KMP3Tick output_write_ticks;
  ZZ9KMP3Tick read_ack_ticks;
  uint32_t feed_calls;
  uint32_t read_calls;
  uint32_t backpressure_hits;
  uint32_t max_pcm_delta;
} ZZ9KMP3StreamStats;

#if ZZ9K_MP3_AMIGA
struct Device *TimerBase;
#endif

static void usage(const char *name)
{
  printf("usage: %s [--capacity <bytes>] [--decode-quantum <bytes>] "
         "[--feed-chunk <bytes>] [--s16be] [--wav] [--oneshot] "
         "[--stats] [--out <file>] <input.mp3>\n", name);
}

static ZZ9KMP3Tick mp3_eclock_to_tick(uint32_t high, uint32_t low)
{
  return ((ZZ9KMP3Tick)high << 32) | low;
}

static void mp3_timer_close(ZZ9KMP3Timer *timer)
{
#if ZZ9K_MP3_AMIGA
  if (!timer) {
    return;
  }
  if (timer->timer_request) {
    if (timer->high_resolution) {
      CloseDevice((struct IORequest *)timer->timer_request);
    }
    DeleteIORequest(timer->timer_request);
  }
  if (timer->timer_port) {
    DeleteMsgPort(timer->timer_port);
  }
  if (TimerBase && timer->high_resolution) {
    TimerBase = 0;
  }
#else
  (void)timer;
#endif
}

static void mp3_timer_open(ZZ9KMP3Timer *timer)
{
  memset(timer, 0, sizeof(*timer));

#if ZZ9K_MP3_AMIGA
  timer->timer_port = CreateMsgPort();
  if (timer->timer_port) {
    timer->timer_request = (struct timerequest *)CreateIORequest(
        timer->timer_port, sizeof(*timer->timer_request));
  }
  if (timer->timer_request &&
      OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                 (struct IORequest *)timer->timer_request, 0) == 0) {
    struct EClockVal value;

    TimerBase = (struct Device *)timer->timer_request->tr_node.io_Device;
    timer->ticks_per_second = ReadEClock(&value);
    if (timer->ticks_per_second != 0U) {
      timer->high_resolution = 1;
      return;
    }
    CloseDevice((struct IORequest *)timer->timer_request);
  }

  mp3_timer_close(timer);
  memset(timer, 0, sizeof(*timer));
  timer->ticks_per_second = 50U;
#else
  timer->ticks_per_second = (uint32_t)CLOCKS_PER_SEC;
  timer->high_resolution = 1;
#endif
}

static ZZ9KMP3Tick mp3_timer_now(const ZZ9KMP3Timer *timer)
{
#if ZZ9K_MP3_AMIGA
  if (timer && timer->high_resolution) {
    struct EClockVal value;

    ReadEClock(&value);
    return mp3_eclock_to_tick(value.ev_hi, value.ev_lo);
  } else {
    struct DateStamp stamp;
    uint32_t minutes;

    DateStamp(&stamp);
    minutes = ((uint32_t)stamp.ds_Days * 24U * 60U) +
              (uint32_t)stamp.ds_Minute;
    return ((ZZ9KMP3Tick)minutes * 60ULL * 50ULL) +
           (uint32_t)stamp.ds_Tick;
  }
#else
  (void)timer;
  return (ZZ9KMP3Tick)clock();
#endif
}

static uint32_t mp3_ticks_to_ms(ZZ9KMP3Tick ticks, uint32_t ticks_per_second)
{
  uint64_t ms;

  if (ticks_per_second == 0U) {
    return 0U;
  }
  ms = (ticks * 1000ULL) / ticks_per_second;
  return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static uint32_t mp3_kib_per_second(uint32_t bytes, ZZ9KMP3Tick ticks,
                                   uint32_t ticks_per_second)
{
  uint64_t rate;

  if (ticks == 0U || ticks_per_second == 0U) {
    return 0U;
  }
  rate = ((uint64_t)bytes * (uint64_t)ticks_per_second) /
         (ticks * 1024ULL);
  return rate > UINT32_MAX ? UINT32_MAX : (uint32_t)rate;
}

static uint32_t mp3_pcm_duration_ms(uint32_t bytes, uint32_t sample_rate,
                                    uint32_t channels)
{
  uint64_t denominator;
  uint64_t ms;

  if (sample_rate == 0U || channels == 0U) {
    return 0U;
  }

  denominator = (uint64_t)sample_rate * (uint64_t)channels * 2ULL;
  if (denominator == 0U) {
    return 0U;
  }

  ms = ((uint64_t)bytes * 1000ULL) / denominator;
  return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static uint32_t mp3_realtime_x100(uint32_t audio_ms, uint32_t elapsed_ms)
{
  uint64_t ratio;

  if (elapsed_ms == 0U) {
    return 0U;
  }

  ratio = ((uint64_t)audio_ms * 100ULL) / (uint64_t)elapsed_ms;
  return ratio > UINT32_MAX ? UINT32_MAX : (uint32_t)ratio;
}

static void mp3_stats_add(ZZ9KMP3Tick *dst, ZZ9KMP3Tick start,
                          ZZ9KMP3Tick end)
{
  if (dst) {
    *dst += end - start;
  }
}

static uint32_t mp3_stream_pcm_ack_batch_bytes(uint32_t pcm_capacity)
{
  return zzplay_mp3_pcm_ack_batch_bytes(pcm_capacity);
}

static int mp3_stream_pcm_ack_due(uint32_t pending_ack,
                                  uint32_t pcm_capacity,
                                  int force)
{
  return zzplay_mp3_pcm_ack_due(pending_ack, pcm_capacity, force);
}

static uint32_t mp3_stream_decode_quantum_bytes(uint32_t requested,
                                                uint32_t pcm_capacity)
{
  return zzplay_mp3_decode_quantum_bytes(requested, pcm_capacity);
}

static uint32_t mp3_stream_feed_chunk_bytes(uint32_t requested,
                                            uint32_t decode_quantum)
{
  return zzplay_mp3_feed_chunk_bytes(requested, decode_quantum);
}

static uint32_t mp3_stream_ring_advance(uint32_t offset, uint32_t bytes,
                                        uint32_t capacity)
{
  return zzplay_mp3_ring_advance(offset, bytes, capacity);
}

static uint32_t mp3_stream_input_buffered(uint32_t bytes_fed,
                                          uint32_t bytes_consumed)
{
  return zzplay_mp3_input_buffered(bytes_fed, bytes_consumed);
}

static int mp3_stream_input_room_low(uint32_t bytes_fed,
                                     uint32_t bytes_consumed,
                                     uint32_t input_capacity,
                                     uint32_t next_feed_bytes)
{
  return zzplay_mp3_input_room_low(bytes_fed, bytes_consumed,
                                   input_capacity, next_feed_bytes);
}

static int parse_u32(const char *text, uint32_t *value)
{
  char *end;
  unsigned long parsed;

  parsed = strtoul(text, &end, 0);
  if (!text[0] || *end != '\0' || parsed == 0UL || parsed > 0xffffffffUL) {
    return 0;
  }

  *value = (uint32_t)parsed;
  return 1;
}

static int read_file(const char *path, uint8_t **data, uint32_t *length)
{
  FILE *file;
  long size;
  uint8_t *bytes;

  file = fopen(path, "rb");
  if (!file) {
    printf("open input failed: %s\n", path);
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return 0;
  }
  size = ftell(file);
  if (size <= 0 || size > 0x7fffffffL) {
    fclose(file);
    printf("unsupported input size\n");
    return 0;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }

  bytes = (uint8_t *)malloc((size_t)size);
  if (!bytes) {
    fclose(file);
    printf("host input allocation failed\n");
    return 0;
  }
  if (fread(bytes, 1U, (size_t)size, file) != (size_t)size) {
    free(bytes);
    fclose(file);
    printf("input read failed\n");
    return 0;
  }

  fclose(file);
  *data = bytes;
  *length = (uint32_t)size;
  return 1;
}

static void put_le16(uint8_t *dst, uint32_t value)
{
  dst[0] = (uint8_t)(value & 0xffU);
  dst[1] = (uint8_t)((value >> 8) & 0xffU);
}

static void put_le32(uint8_t *dst, uint32_t value)
{
  dst[0] = (uint8_t)(value & 0xffU);
  dst[1] = (uint8_t)((value >> 8) & 0xffU);
  dst[2] = (uint8_t)((value >> 16) & 0xffU);
  dst[3] = (uint8_t)((value >> 24) & 0xffU);
}

static int write_wav_header(FILE *file, uint32_t data_bytes,
                            uint32_t sample_rate, uint32_t channels)
{
  uint8_t header[44];
  uint32_t byte_rate;
  uint32_t block_align;

  if (!file || sample_rate == 0U || channels == 0U || channels > 2U ||
      data_bytes > 0xffffffffUL - 36U) {
    return 0;
  }

  byte_rate = sample_rate * channels * 2U;
  block_align = channels * 2U;
  memset(header, 0, sizeof(header));
  memcpy(&header[0], "RIFF", 4U);
  put_le32(&header[4], 36U + data_bytes);
  memcpy(&header[8], "WAVEfmt ", 8U);
  put_le32(&header[16], 16U);
  put_le16(&header[20], 1U);
  put_le16(&header[22], channels);
  put_le32(&header[24], sample_rate);
  put_le32(&header[28], byte_rate);
  put_le16(&header[32], block_align);
  put_le16(&header[34], 16U);
  memcpy(&header[36], "data", 4U);
  put_le32(&header[40], data_bytes);

  return fwrite(header, 1U, sizeof(header), file) == sizeof(header);
}

static int patch_wav_header(FILE *file, uint32_t data_bytes,
                            uint32_t sample_rate, uint32_t channels)
{
  if (!file || fseek(file, 0L, SEEK_SET) != 0 ||
      !write_wav_header(file, data_bytes, sample_rate, channels)) {
    return 0;
  }
  return fseek(file, 0L, SEEK_END) == 0;
}

static int write_shared_file(const char *path, const ZZ9KSharedBuffer *buffer,
                             uint32_t length, int wav_output,
                             uint32_t sample_rate, uint32_t channels)
{
  FILE *file;
  static uint8_t chunk[STREAM_OUTPUT_CHUNK_BYTES];
  uint32_t offset;

  file = fopen(path, "wb");
  if (!file) {
    printf("open output failed: %s\n", path);
    return 0;
  }
  if (wav_output && !write_wav_header(file, length, sample_rate, channels)) {
    fclose(file);
    printf("WAV header write failed\n");
    return 0;
  }

  offset = 0U;
  while (offset < length) {
    uint32_t part = length - offset;
    if (part > STREAM_OUTPUT_CHUNK_BYTES) {
      part = STREAM_OUTPUT_CHUNK_BYTES;
    }
    if (!zz9k_shared_copy_from(chunk, buffer, offset, part) ||
        fwrite(chunk, 1U, part, file) != part) {
      fclose(file);
      printf("output write failed\n");
      return 0;
    }
    offset += part;
  }

  fclose(file);
  return 1;
}

static int write_shared_ring(FILE *file, const ZZ9KSharedBuffer *buffer,
                             uint32_t offset, uint32_t length,
                             uint32_t capacity, ZZ9KMP3StreamStats *stats)
{
  static uint8_t chunk[STREAM_OUTPUT_CHUNK_BYTES];
  uint32_t remaining = length;
  uint32_t cursor = offset;

  if (!file) {
    return 1;
  }

  while (remaining != 0U) {
    uint32_t part = remaining;
    uint32_t linear = capacity - cursor;
    ZZ9KMP3Tick start;
    ZZ9KMP3Tick end;

    if (part > linear) {
      part = linear;
    }
    if (part > STREAM_OUTPUT_CHUNK_BYTES) {
      part = STREAM_OUTPUT_CHUNK_BYTES;
    }
    start = stats ? mp3_timer_now(&stats->timer) : 0U;
    if (!zz9k_shared_copy_from(chunk, buffer, cursor, part)) {
      printf("PCM copy failed\n");
      return 0;
    }
    end = stats ? mp3_timer_now(&stats->timer) : 0U;
    if (stats) {
      mp3_stats_add(&stats->pcm_copy_ticks, start, end);
    }

    start = stats ? mp3_timer_now(&stats->timer) : 0U;
    if (fwrite(chunk, 1U, part, file) != part) {
      printf("PCM output write failed\n");
      return 0;
    }
    end = stats ? mp3_timer_now(&stats->timer) : 0U;
    if (stats) {
      mp3_stats_add(&stats->output_write_ticks, start, end);
    }
    cursor = (cursor + part) % capacity;
    remaining -= part;
  }

  return 1;
}

static int require_audio_service(ZZ9KContext *ctx, ZZ9KServiceInfo *service)
{
  ZZ9KCaps caps;
  int status;

  status = zz9k_query_caps(ctx, &caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("query caps failed: %s (%d)\n", zz9k_status_name(status), status);
    return 0;
  }
  if ((caps.capability_bits & ZZ9K_CAP_AUDIO_DECODE) == 0U) {
    printf("%s capability not advertised\n",
           zz9k_capability_name(ZZ9K_CAP_AUDIO_DECODE));
    return 0;
  }

  status = zz9k_query_service(ctx, ZZ9K_SERVICE_AUDIO, service);
  if (status != ZZ9K_STATUS_OK) {
    printf("query audio service failed: %s (%d)\n",
           zz9k_status_name(status), status);
    return 0;
  }
  if ((service->flags & ZZ9K_SERVICE_FLAG_AUDIO_MP3_DECODE) == 0U) {
    printf("audio service does not advertise %s\n",
           zz9k_service_flag_name(ZZ9K_SERVICE_AUDIO,
                                  ZZ9K_SERVICE_FLAG_AUDIO_MP3_DECODE));
    return 0;
  }

  return 1;
}

static int ack_stream_pcm(ZZ9KContext *ctx, uint32_t session,
                          uint32_t pcm_capacity,
                          ZZ9KAudioStreamResult *result,
                          uint32_t *pending_ack,
                          uint32_t *read_offset,
                          int force,
                          ZZ9KMP3StreamStats *stats)
{
  ZZ9KMP3Tick start;
  int status;

  if (!ctx || !result || !pending_ack || !read_offset) {
    return 0;
  }
  if (!mp3_stream_pcm_ack_due(*pending_ack, pcm_capacity, force)) {
    return 1;
  }

  start = stats ? mp3_timer_now(&stats->timer) : 0U;
  status = zz9k_audio_stream_read(ctx, session, *pending_ack, 0U, result);
  if (stats) {
    mp3_stats_add(&stats->read_ack_ticks, start,
                  mp3_timer_now(&stats->timer));
    stats->read_calls++;
  }
  if (status != ZZ9K_STATUS_OK) {
    printf("stream read failed: %s (%d)\n",
           zz9k_status_name(status), status);
    return 0;
  }

  *pending_ack = 0U;
  *read_offset = result->pcm_read;
  return 1;
}

static int drain_stream_pcm(ZZ9KContext *ctx, uint32_t session,
                            const ZZ9KSharedBuffer *pcm_ring,
                            uint32_t pcm_capacity, FILE *output_file,
                            ZZ9KAudioStreamResult *result,
                            uint32_t *produced_seen,
                            uint32_t *read_offset,
                            uint32_t *pending_ack,
                            uint32_t *total_written,
                            ZZ9KMP3StreamStats *stats)
{
  uint32_t guard = 0U;

  while (guard++ < 64U) {
    uint32_t produced_delta;

    if (result->bytes_produced < *produced_seen) {
      printf("stream result moved backwards\n");
      return 0;
    }
    produced_delta = result->bytes_produced - *produced_seen;
    if (produced_delta == 0U) {
      return 1;
    }
    if (produced_delta > pcm_capacity) {
      printf("stream produced more PCM than the ring can hold\n");
      return 0;
    }
    if (stats && produced_delta > stats->max_pcm_delta) {
      stats->max_pcm_delta = produced_delta;
    }
    if (!write_shared_ring(output_file, pcm_ring, *read_offset,
                           produced_delta, pcm_capacity, stats)) {
      return 0;
    }
    *read_offset = mp3_stream_ring_advance(*read_offset, produced_delta,
                                           pcm_capacity);
    *total_written += produced_delta;
    *produced_seen += produced_delta;
    if (produced_delta > UINT32_MAX - *pending_ack) {
      printf("stream pending PCM ack overflow\n");
      return 0;
    }
    *pending_ack += produced_delta;
    if (!ack_stream_pcm(ctx, session, pcm_capacity, result, pending_ack,
                        read_offset, 0, stats)) {
      return 0;
    }
  }

  printf("stream drain made too many read iterations\n");
  return 0;
}

static int flush_stream_pcm(ZZ9KContext *ctx, uint32_t session,
                            const ZZ9KSharedBuffer *pcm_ring,
                            uint32_t pcm_capacity, FILE *output_file,
                            ZZ9KAudioStreamResult *result,
                            uint32_t *produced_seen,
                            uint32_t *read_offset,
                            uint32_t *pending_ack,
                            uint32_t *total_written,
                            ZZ9KMP3StreamStats *stats)
{
  uint32_t guard = 0U;

  while (guard++ < 64U) {
    if (!drain_stream_pcm(ctx, session, pcm_ring, pcm_capacity,
                          output_file, result, produced_seen, read_offset,
                          pending_ack, total_written, stats)) {
      return 0;
    }
    if (*pending_ack == 0U) {
      return 1;
    }
    if (!ack_stream_pcm(ctx, session, pcm_capacity, result, pending_ack,
                        read_offset, 1, stats)) {
      return 0;
    }
  }

  printf("stream flush made too many read iterations\n");
  return 0;
}

static int make_stream_input_room(ZZ9KContext *ctx, uint32_t session,
                                  const ZZ9KSharedBuffer *pcm_ring,
                                  uint32_t pcm_capacity,
                                  uint32_t mp3_capacity,
                                  FILE *output_file,
                                  ZZ9KAudioStreamResult *result,
                                  uint32_t total_read,
                                  uint32_t next_feed_bytes,
                                  uint32_t *produced_seen,
                                  uint32_t *read_offset,
                                  uint32_t *pending_ack,
                                  uint32_t *total_written,
                                  ZZ9KMP3StreamStats *stats)
{
  uint32_t guard = 0U;

  while (mp3_stream_input_room_low(total_read, result->bytes_consumed,
                                   mp3_capacity, next_feed_bytes)) {
    if (*pending_ack == 0U) {
      return 1;
    }
    if (++guard > 64U) {
      printf("stream input room made too many read iterations\n");
      return 0;
    }
    if (!ack_stream_pcm(ctx, session, pcm_capacity, result, pending_ack,
                        read_offset, 1, stats)) {
      return 0;
    }
    if (!drain_stream_pcm(ctx, session, pcm_ring, pcm_capacity, output_file,
                          result, produced_seen, read_offset, pending_ack,
                          total_written, stats)) {
      return 0;
    }
  }
  return 1;
}

static int stream_decode_file(ZZ9KContext *ctx, const char *input_path,
                              const char *output_path,
                              uint32_t output_format,
                              uint32_t pcm_capacity,
                              uint32_t decode_quantum,
                              uint32_t feed_chunk,
                              int wav_output,
                              int show_stats)
{
  FILE *input_file = 0;
  FILE *output_file = 0;
  ZZ9KSharedBuffer mp3_ring;
  ZZ9KSharedBuffer pcm_ring;
  ZZ9KSharedBuffer staging;
  ZZ9KAudioStreamBeginDesc begin;
  ZZ9KAudioStreamFeedDesc feed;
  ZZ9KAudioStreamResult result;
  static uint8_t chunk[STREAM_CHUNK_BYTES];
  uint32_t produced_seen = 0U;
  uint32_t read_offset = 0U;
  uint32_t pending_ack = 0U;
  uint32_t total_written = 0U;
  uint32_t total_read = 0U;
  ZZ9KMP3StreamStats stats;
  ZZ9KMP3Tick total_start = 0U;
  int mp3_allocated = 0;
  int pcm_allocated = 0;
  int staging_allocated = 0;
  int session_open = 0;
  int status;
  int ok = 0;

  memset(&mp3_ring, 0, sizeof(mp3_ring));
  memset(&pcm_ring, 0, sizeof(pcm_ring));
  memset(&staging, 0, sizeof(staging));
  memset(&stats, 0, sizeof(stats));
  if (show_stats) {
    mp3_timer_open(&stats.timer);
    total_start = mp3_timer_now(&stats.timer);
  }

  if (pcm_capacity < 16384U) {
    pcm_capacity = DEFAULT_OUTPUT_CAPACITY;
  }
  pcm_capacity &= ~1UL;
  if (decode_quantum != 0U) {
    decode_quantum =
        mp3_stream_decode_quantum_bytes(decode_quantum, pcm_capacity);
    if (decode_quantum == 0U) {
      printf("invalid MP3 decode quantum for PCM ring capacity %lu bytes\n",
             (unsigned long)pcm_capacity);
      goto cleanup;
    }
  }
  feed_chunk = mp3_stream_feed_chunk_bytes(feed_chunk, decode_quantum);
  if (feed_chunk == 0U) {
    printf("invalid MP3 feed chunk\n");
    goto cleanup;
  }

  input_file = fopen(input_path, "rb");
  if (!input_file) {
    printf("open input failed: %s\n", input_path);
    goto cleanup;
  }
  if (output_path) {
    output_file = fopen(output_path, "wb");
    if (!output_file) {
      printf("open output failed: %s\n", output_path);
      goto cleanup;
    }
    if (wav_output && !write_wav_header(output_file, 0U, 44100U, 2U)) {
      printf("WAV header write failed\n");
      goto cleanup;
    }
  }

  status = zz9k_alloc_shared(ctx, STREAM_INPUT_CAPACITY, 16U, 0U, &mp3_ring);
  if (status != ZZ9K_STATUS_OK) {
    printf("stream MP3 ring alloc failed: %s (%d)\n",
           zz9k_status_name(status), status);
    goto cleanup;
  }
  mp3_allocated = 1;
  status = zz9k_alloc_shared(ctx, pcm_capacity, 16U, 0U, &pcm_ring);
  if (status != ZZ9K_STATUS_OK) {
    printf("stream PCM ring alloc failed: %s (%d), requested=%lu bytes\n",
           zz9k_status_name(status), status, (unsigned long)pcm_capacity);
    goto cleanup;
  }
  pcm_allocated = 1;
  status = zz9k_alloc_shared(ctx, STREAM_CHUNK_BYTES, 16U, 0U, &staging);
  if (status != ZZ9K_STATUS_OK) {
    printf("stream staging alloc failed: %s (%d)\n",
           zz9k_status_name(status), status);
    goto cleanup;
  }
  staging_allocated = 1;

  if (!zz9k_audio_build_stream_begin_desc(
          &begin, mp3_ring.handle, mp3_ring.length, pcm_ring.handle,
          pcm_ring.length, 0U, 0U, output_format, 0U,
          decode_quantum, 0U)) {
    printf("could not build MP3 stream begin descriptor\n");
    goto cleanup;
  }
  status = zz9k_audio_stream_begin(ctx, &begin, &result);
  if (status != ZZ9K_STATUS_OK) {
    printf("stream begin failed: %s (%d)\n", zz9k_status_name(status),
           status);
    goto cleanup;
  }
  session_open = 1;
  read_offset = result.pcm_read;

  while (1) {
    size_t got;
    uint32_t flags = 0U;
    uint32_t feed_attempts = 0U;
    ZZ9KMP3Tick start;

    start = show_stats ? mp3_timer_now(&stats.timer) : 0U;
    got = fread(chunk, 1U, feed_chunk, input_file);
    if (show_stats) {
      mp3_stats_add(&stats.file_read_ticks, start,
                    mp3_timer_now(&stats.timer));
    }
    if (got == 0U) {
      if (ferror(input_file)) {
        printf("input read failed\n");
        goto cleanup;
      }
      flags = ZZ9K_AUDIO_STREAM_FEED_EOF;
    } else {
      if (!make_stream_input_room(
              ctx, result.session, &pcm_ring, pcm_ring.length,
              mp3_ring.length, output_file, &result, total_read,
              (uint32_t)got, &produced_seen, &read_offset, &pending_ack,
              &total_written, show_stats ? &stats : 0)) {
        goto cleanup;
      }
      start = show_stats ? mp3_timer_now(&stats.timer) : 0U;
      if (!zz9k_shared_copy_to(&staging, 0U, chunk, (uint32_t)got)) {
        printf("stream input copy failed\n");
        goto cleanup;
      }
      if (show_stats) {
        mp3_stats_add(&stats.staging_copy_ticks, start,
                      mp3_timer_now(&stats.timer));
      }
    }

    do {
      if (++feed_attempts > 64U) {
        printf("stream feed stayed backpressured\n");
        goto cleanup;
      }
      if (!zz9k_audio_build_stream_feed_desc(&feed, result.session,
                                             staging.handle, 0U,
                                             (uint32_t)got, flags)) {
        printf("could not build MP3 stream feed descriptor\n");
        goto cleanup;
      }
      start = show_stats ? mp3_timer_now(&stats.timer) : 0U;
      status = zz9k_audio_stream_feed(ctx, &feed, &result);
      if (show_stats) {
        mp3_stats_add(&stats.feed_ticks, start,
                      mp3_timer_now(&stats.timer));
        stats.feed_calls++;
      }
      if (status != ZZ9K_STATUS_OK) {
        printf("stream feed failed: %s (%d)\n",
               zz9k_status_name(status), status);
        goto cleanup;
      }
      if (!drain_stream_pcm(ctx, result.session, &pcm_ring, pcm_ring.length,
                            output_file, &result, &produced_seen,
                            &read_offset, &pending_ack, &total_written,
                            show_stats ? &stats : 0)) {
        goto cleanup;
      }
      if ((result.flags & ZZ9K_AUDIO_STREAM_RESULT_BACKPRESSURE) != 0U &&
          !ack_stream_pcm(ctx, result.session, pcm_ring.length, &result,
                          &pending_ack, &read_offset, 1,
                          show_stats ? &stats : 0)) {
        goto cleanup;
      }
      if (show_stats &&
          (result.flags & ZZ9K_AUDIO_STREAM_RESULT_BACKPRESSURE) != 0U) {
        stats.backpressure_hits++;
      }
    } while ((result.flags & ZZ9K_AUDIO_STREAM_RESULT_BACKPRESSURE) != 0U);
    total_read += (uint32_t)got;
    if ((flags & ZZ9K_AUDIO_STREAM_FEED_EOF) != 0U) {
      break;
    }
  }
  if (!flush_stream_pcm(ctx, result.session, &pcm_ring, pcm_ring.length,
                        output_file, &result, &produced_seen, &read_offset,
                        &pending_ack, &total_written,
                        show_stats ? &stats : 0)) {
    goto cleanup;
  }

  printf("mp3 stream ok: input=%lu bytes consumed=%lu bytes written=%lu bytes rate=%lu "
         "channels=%lu format=%s frames=%lu flags=0x%08lx\n",
         (unsigned long)total_read,
         (unsigned long)result.bytes_consumed,
         (unsigned long)total_written,
         (unsigned long)result.sample_rate,
         (unsigned long)result.channels,
         output_format == ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE ? "s16be" : "s16le",
         (unsigned long)result.frames_decoded,
         (unsigned long)result.flags);
  if (output_path) {
    if (wav_output) {
      if (!patch_wav_header(output_file, total_written,
                            result.sample_rate, result.channels)) {
        printf("WAV header patch failed\n");
        goto cleanup;
      }
      printf("wrote WAV to %s\n", output_path);
    } else {
      printf("wrote raw PCM to %s\n", output_path);
    }
  }
  if (result.bytes_consumed < total_read) {
    printf("note: stream ended before all input bytes were consumed\n");
  }
  if (show_stats) {
    uint32_t ticks_per_second = stats.timer.ticks_per_second;
    ZZ9KMP3Tick total_end = mp3_timer_now(&stats.timer);
    uint32_t elapsed_ms;
    uint32_t audio_ms;
    uint32_t realtime_x100;

    stats.total_ticks = total_end - total_start;
    elapsed_ms = mp3_ticks_to_ms(stats.total_ticks, ticks_per_second);
    audio_ms = mp3_pcm_duration_ms(total_written, result.sample_rate,
                                   result.channels);
    realtime_x100 = mp3_realtime_x100(audio_ms, elapsed_ms);
    printf("stream stats: elapsed=%lu ms input_rate=%lu KiB/s "
           "pcm_rate=%lu KiB/s audio=%lu.%03lus realtime=%lu.%02lux "
           "feeds=%lu reads=%lu backpressure=%lu max_pcm_delta=%lu "
           "decode_quantum=%lu feed_chunk=%lu\n",
           (unsigned long)elapsed_ms,
           (unsigned long)mp3_kib_per_second(total_read, stats.total_ticks,
                                             ticks_per_second),
           (unsigned long)mp3_kib_per_second(total_written,
                                             stats.total_ticks,
                                             ticks_per_second),
           (unsigned long)(audio_ms / 1000U),
           (unsigned long)(audio_ms % 1000U),
           (unsigned long)(realtime_x100 / 100U),
           (unsigned long)(realtime_x100 % 100U),
            (unsigned long)stats.feed_calls,
            (unsigned long)stats.read_calls,
            (unsigned long)stats.backpressure_hits,
            (unsigned long)stats.max_pcm_delta,
            (unsigned long)decode_quantum,
            (unsigned long)feed_chunk);
    printf("stream stats detail: file_read=%lu ms staging_copy=%lu ms "
           "feed_call=%lu ms pcm_copy=%lu ms output_write=%lu ms "
           "read_ack=%lu ms\n",
           (unsigned long)mp3_ticks_to_ms(stats.file_read_ticks,
                                          ticks_per_second),
           (unsigned long)mp3_ticks_to_ms(stats.staging_copy_ticks,
                                          ticks_per_second),
           (unsigned long)mp3_ticks_to_ms(stats.feed_ticks,
                                          ticks_per_second),
           (unsigned long)mp3_ticks_to_ms(stats.pcm_copy_ticks,
                                          ticks_per_second),
           (unsigned long)mp3_ticks_to_ms(stats.output_write_ticks,
                                          ticks_per_second),
           (unsigned long)mp3_ticks_to_ms(stats.read_ack_ticks,
                                          ticks_per_second));
  }
  ok = 1;

cleanup:
  if (show_stats) {
    mp3_timer_close(&stats.timer);
  }
  if (session_open) {
    ZZ9KAudioStreamResult close_result;
    (void)zz9k_audio_stream_close(ctx, result.session, 0U, &close_result);
  }
  if (staging_allocated) {
    zz9k_free_shared(ctx, staging.handle);
  }
  if (pcm_allocated) {
    zz9k_free_shared(ctx, pcm_ring.handle);
  }
  if (mp3_allocated) {
    zz9k_free_shared(ctx, mp3_ring.handle);
  }
  if (output_file) {
    fclose(output_file);
  }
  if (input_file) {
    fclose(input_file);
  }
  return ok;
}

static int one_shot_capacity_ok(ZZ9KContext *ctx,
                                uint32_t input_length,
                                uint32_t output_capacity,
                                int stream_available)
{
  ZZ9KDiagInfo diag;
  uint32_t needed;
  int status;

  status = zz9k_read_diag(ctx, &diag);
  if (status != ZZ9K_STATUS_OK) {
    return 1;
  }

  needed = input_length + output_capacity;
  if (input_length > diag.shared_heap_largest_free ||
      output_capacity > diag.shared_heap_largest_free ||
      needed > diag.shared_heap_free) {
    printf("one-shot MP3 decode needs shared memory for the whole input and "
           "PCM output\n");
    printf("input=%lu bytes output_capacity=%lu bytes needed=%lu bytes\n",
           (unsigned long)input_length, (unsigned long)output_capacity,
           (unsigned long)needed);
    printf("shared heap free=%lu bytes largest_block=%lu bytes\n",
           (unsigned long)diag.shared_heap_free,
           (unsigned long)diag.shared_heap_largest_free);
    if (stream_available) {
      printf("falling back to streaming MP3 decode\n");
    } else {
      printf("streaming MP3 decode is not advertised by this firmware\n");
    }
    return 0;
  }

  return 1;
}

#if !ZZ9K_MP3_NO_MAIN
int main(int argc, char **argv)
{
  ZZ9KContext *ctx;
  ZZ9KServiceInfo service;
  ZZ9KSharedBuffer input;
  ZZ9KSharedBuffer output;
  ZZ9KAudioDecodeDesc desc;
  ZZ9KAudioDecodeResult result;
  uint8_t *input_bytes;
  uint32_t input_length;
  uint32_t output_capacity;
  uint32_t decode_quantum;
  uint32_t feed_chunk;
  uint32_t output_format;
  const char *input_path;
  const char *output_path;
  int input_allocated;
  int output_allocated;
  int capacity_set;
  int one_shot_requested;
  int show_stats;
  int wav_output;
  int status;
  int arg;
  int rc;

  ctx = 0;
  input_bytes = 0;
  input_length = 0U;
  output_capacity = DEFAULT_OUTPUT_CAPACITY;
  decode_quantum = 0U;
  feed_chunk = 0U;
  output_format = ZZ9K_AUDIO_SAMPLE_FORMAT_S16LE;
  input_path = 0;
  output_path = 0;
  input_allocated = 0;
  output_allocated = 0;
  capacity_set = 0;
  one_shot_requested = 0;
  show_stats = 0;
  wav_output = 0;
  rc = 1;
  memset(&input, 0, sizeof(input));
  memset(&output, 0, sizeof(output));

  arg = 1;
  while (arg < argc) {
    if (strcmp(argv[arg], "--capacity") == 0) {
      arg++;
      if (arg >= argc || !parse_u32(argv[arg], &output_capacity)) {
        usage(argv[0]);
        return 2;
      }
      capacity_set = 1;
      arg++;
    } else if (strcmp(argv[arg], "--decode-quantum") == 0) {
      arg++;
      if (arg >= argc || !parse_u32(argv[arg], &decode_quantum) ||
          decode_quantum == 0U) {
        usage(argv[0]);
        return 2;
      }
      arg++;
    } else if (strcmp(argv[arg], "--feed-chunk") == 0) {
      arg++;
      if (arg >= argc || !parse_u32(argv[arg], &feed_chunk) ||
          feed_chunk == 0U) {
        usage(argv[0]);
        return 2;
      }
      arg++;
    } else if (strcmp(argv[arg], "--s16be") == 0) {
      output_format = ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE;
      arg++;
    } else if (strcmp(argv[arg], "--wav") == 0) {
      wav_output = 1;
      arg++;
    } else if (strcmp(argv[arg], "--oneshot") == 0) {
      one_shot_requested = 1;
      arg++;
    } else if (strcmp(argv[arg], "--stats") == 0) {
      show_stats = 1;
      arg++;
    } else if (strcmp(argv[arg], "--out") == 0) {
      arg++;
      if (arg >= argc) {
        usage(argv[0]);
        return 2;
      }
      output_path = argv[arg++];
    } else {
      if (input_path) {
        usage(argv[0]);
        return 2;
      }
      input_path = argv[arg++];
    }
  }

  if (!input_path || output_capacity < 2U) {
    usage(argv[0]);
    return 2;
  }
  if (wav_output && output_format != ZZ9K_AUDIO_SAMPLE_FORMAT_S16LE) {
    printf("--wav requires little-endian S16 output\n");
    return 2;
  }
  output_capacity &= ~1UL;

  status = zz9k_open(&ctx);
  if (status != ZZ9K_STATUS_OK) {
    printf("open failed: %s (%d)\n", zz9k_status_name(status), status);
    goto cleanup;
  }
  if (!require_audio_service(ctx, &service)) {
    goto cleanup;
  }

  if (!one_shot_requested &&
      (service.flags & ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM) != 0U) {
    rc = stream_decode_file(ctx, input_path, output_path, output_format,
                            capacity_set ? output_capacity :
                                           DEFAULT_STREAM_PCM_CAPACITY,
                            decode_quantum, feed_chunk,
                            wav_output, show_stats) ? 0 : 1;
    goto cleanup;
  }

  if (!read_file(input_path, &input_bytes, &input_length)) {
    rc = 3;
    goto cleanup;
  }

  if (!one_shot_capacity_ok(
          ctx, input_length, output_capacity,
          !one_shot_requested &&
              (service.flags & ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM) != 0U)) {
    if (one_shot_requested) {
      goto cleanup;
    }
    if ((service.flags & ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM) == 0U) {
      printf("audio service does not advertise %s\n",
             zz9k_service_flag_name(ZZ9K_SERVICE_AUDIO,
                                    ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM));
      goto cleanup;
    }
    free(input_bytes);
    input_bytes = 0;
    rc = stream_decode_file(ctx, input_path, output_path, output_format,
                            capacity_set ? output_capacity :
                                           DEFAULT_STREAM_PCM_CAPACITY,
                            decode_quantum, feed_chunk,
                            wav_output,
                            show_stats) ? 0 : 1;
    goto cleanup;
  }

  status = zz9k_alloc_shared(ctx, input_length, 16U, 0U, &input);
  if (status != ZZ9K_STATUS_OK) {
    printf("input alloc failed: %s (%d), requested=%lu bytes\n",
           zz9k_status_name(status), status, (unsigned long)input_length);
    goto cleanup;
  }
  input_allocated = 1;

  status = zz9k_alloc_shared(ctx, output_capacity, 16U, 0U, &output);
  if (status != ZZ9K_STATUS_OK) {
    printf("output alloc failed: %s (%d), requested=%lu bytes\n",
           zz9k_status_name(status), status, (unsigned long)output_capacity);
    goto cleanup;
  }
  output_allocated = 1;

  if (!zz9k_shared_copy_to(&input, 0U, input_bytes, input_length)) {
    printf("input copy failed\n");
    goto cleanup;
  }
  if (!zz9k_audio_build_decode_desc(&desc, input.handle, 0U, input_length,
                                    output.handle, 0U, output.length, 0U, 0U,
                                    output_format,
                                    ZZ9K_AUDIO_DECODE_FLAG_EXPECT_END)) {
    printf("could not build MP3 decode descriptor\n");
    goto cleanup;
  }

  memset(&result, 0, sizeof(result));
  status = zz9k_decode_mp3(ctx, &desc, &result);
  if (status != ZZ9K_STATUS_OK) {
    printf("mp3 decode failed: %s (%d)\n", zz9k_status_name(status), status);
    goto cleanup;
  }

  printf("mp3 decode ok: consumed=%lu/%lu bytes written=%lu bytes "
         "rate=%lu channels=%lu format=%s frames=%lu flags=0x%08lx\n",
         (unsigned long)result.bytes_consumed,
         (unsigned long)input_length,
         (unsigned long)result.bytes_written,
         (unsigned long)result.sample_rate,
         (unsigned long)result.channels,
         result.sample_format == ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE ?
           "s16be" : "s16le",
         (unsigned long)result.frames_written,
         (unsigned long)result.flags);

  if (output_path && !write_shared_file(output_path, &output,
                                        result.bytes_written,
                                        wav_output, result.sample_rate,
                                        result.channels)) {
    goto cleanup;
  }
  if (output_path) {
    printf("wrote %s to %s\n", wav_output ? "WAV" : "raw PCM",
           output_path);
  }
  if (result.bytes_consumed < input_length) {
    printf("note: output buffer filled before the whole stream was consumed; "
           "increase --capacity or use a smaller MP3\n");
  }

  rc = 0;

cleanup:
  if (output_allocated) {
    zz9k_free_shared(ctx, output.handle);
  }
  if (input_allocated) {
    zz9k_free_shared(ctx, input.handle);
  }
  if (ctx) {
    zz9k_close(ctx);
  }
  free(input_bytes);
  return rc;
}
#endif
