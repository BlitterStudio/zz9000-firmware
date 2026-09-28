/*
 * Offscreen SDK v2 service benchmark.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/host.h"
#include "zz9k/caps.h"
#include "zz9k/crypto.h"
#include "zz9k/image_geometry.h"
#include "zz9k/request.h"
#include "zz9k/surface.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
    defined(__VBCC__)
#define ZZ9K_BENCH_AMIGA 1
#include <devices/timer.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/timer.h>
#else
#include <time.h>
#endif

#define ZZ9K_BENCH_DEFAULT_ITERATIONS 8U
#define ZZ9K_BENCH_MAX_ITERATIONS 100U
#define ZZ9K_BENCH_BUFFER_BYTES (128UL * 1024UL)
#define ZZ9K_BENCH_SRC_WIDTH 160U
#define ZZ9K_BENCH_SRC_HEIGHT 100U
#define ZZ9K_BENCH_DST_WIDTH 320U
#define ZZ9K_BENCH_DST_HEIGHT 200U
#define ZZ9K_BENCH_LOCAL_SURFACE_WIDTH 1280UL
#define ZZ9K_BENCH_LOCAL_SURFACE_HEIGHT 720UL
#define ZZ9K_BENCH_LOCAL_SURFACE_BPP 4UL
#define ZZ9K_BENCH_LOCAL_SURFACE_BYTES \
  (ZZ9K_BENCH_LOCAL_SURFACE_WIDTH * ZZ9K_BENCH_LOCAL_SURFACE_HEIGHT * \
   ZZ9K_BENCH_LOCAL_SURFACE_BPP)
/* 68k CPU -> shared-buffer (Zorro) write bandwidth probe. Two block sizes so
 * the small case shows per-transfer overhead and the large case shows the
 * sustained Zorro write ceiling an offload broadcast actually pays. */
#define ZZ9K_BENCH_CPU_WRITE_SMALL_BYTES (16UL * 1024UL)
#define ZZ9K_BENCH_CPU_WRITE_LARGE_BYTES (256UL * 1024UL)
#define ZZ9K_BENCH_PING_PIPE_DEPTH 16U
/* Latency probe: tight ring polling bounded so a dead board cannot hang the
 * bench; the gapped variant controls for the probe's own Zorro read traffic
 * delaying the firmware's housekeeping (observer effect). */
#define ZZ9K_BENCH_PROBE_POLL_BACKSTOP 2000000UL
#define ZZ9K_BENCH_PROBE_GAP_SPINS 500UL
#define ZZ9K_BENCH_CRYPTO_PIPE_DEPTH 16U
#define ZZ9K_BENCH_TLS_RECORD_BYTES (16UL * 1024UL)
#define ZZ9K_BENCH_SHA256_DIGEST_BYTES 32U
#define ZZ9K_BENCH_POLY1305_TAG_BYTES 16U
#define ZZ9K_BENCH_CHACHA20_KEY_BYTES 32U
#define ZZ9K_BENCH_CHACHA20_NONCE_BYTES 12U
#define ZZ9K_BENCH_CHACHA20_BLOCK_BYTES 64U
#define ZZ9K_BENCH_CRYPTO_OUTPUT_BYTES \
  (ZZ9K_BENCH_CRYPTO_PIPE_DEPTH * \
   (ZZ9K_BENCH_TLS_RECORD_BYTES + ZZ9K_BENCH_POLY1305_TAG_BYTES))
#define ZZ9K_BENCH_PIPE_TIMEOUT_SECONDS 5U

typedef uint64_t ZZ9KBenchTick;

typedef struct ZZ9KBenchTimer {
  uint32_t ticks_per_second;
  int high_resolution;
#if ZZ9K_BENCH_AMIGA
  struct MsgPort *timer_port;
  struct timerequest *timer_request;
#endif
} ZZ9KBenchTimer;

#if ZZ9K_BENCH_AMIGA
struct Device *TimerBase;
#endif

static uint32_t zz9k_bench_parse_iterations(int argc, char **argv)
{
  unsigned long value;

  if (argc < 2) {
    return ZZ9K_BENCH_DEFAULT_ITERATIONS;
  }

  value = strtoul(argv[1], 0, 0);
  if (value < 1UL) {
    return 1U;
  }
  if (value > ZZ9K_BENCH_MAX_ITERATIONS) {
    return ZZ9K_BENCH_MAX_ITERATIONS;
  }

  return (uint32_t)value;
}

static ZZ9KBenchTick zz9k_bench_eclock_to_tick(uint32_t high, uint32_t low)
{
  return ((ZZ9KBenchTick)high << 32) | low;
}

static void zz9k_bench_timer_close(ZZ9KBenchTimer *timer)
{
#if ZZ9K_BENCH_AMIGA
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

static void zz9k_bench_timer_open(ZZ9KBenchTimer *timer)
{
  memset(timer, 0, sizeof(*timer));

#if ZZ9K_BENCH_AMIGA
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
    if (timer->ticks_per_second != 0) {
      timer->high_resolution = 1;
      return;
    }
    CloseDevice((struct IORequest *)timer->timer_request);
  }

  zz9k_bench_timer_close(timer);
  memset(timer, 0, sizeof(*timer));
  timer->ticks_per_second = 50U;
#else
  timer->ticks_per_second = (uint32_t)CLOCKS_PER_SEC;
  timer->high_resolution = 1;
#endif
}

static ZZ9KBenchTick zz9k_bench_timer_now(const ZZ9KBenchTimer *timer)
{
#if ZZ9K_BENCH_AMIGA
  if (timer && timer->high_resolution) {
    struct EClockVal value;

    ReadEClock(&value);
    return zz9k_bench_eclock_to_tick(value.ev_hi, value.ev_lo);
  } else {
    struct DateStamp stamp;
    uint32_t minutes;

    DateStamp(&stamp);
    minutes = ((uint32_t)stamp.ds_Days * 24U * 60U) +
              (uint32_t)stamp.ds_Minute;
    return ((ZZ9KBenchTick)minutes * 60ULL * 50ULL) +
           (uint32_t)stamp.ds_Tick;
  }
#else
  (void)timer;
  return (uint32_t)clock();
#endif
}

static ZZ9KBenchTick zz9k_bench_elapsed_ticks(ZZ9KBenchTick start_ticks,
                                              ZZ9KBenchTick end_ticks)
{
  return end_ticks - start_ticks;
}

static uint32_t zz9k_bench_ticks_to_ms(ZZ9KBenchTick ticks,
                                       uint32_t ticks_per_second)
{
  uint64_t ms;

  if (ticks_per_second == 0) {
    return 0;
  }

  ms = ((uint64_t)ticks * 1000ULL) / ticks_per_second;
  if (ms > UINT32_MAX) {
    return UINT32_MAX;
  }

  return (uint32_t)ms;
}

static uint32_t zz9k_bench_ticks_to_us(ZZ9KBenchTick ticks,
                                       uint32_t ticks_per_second)
{
  uint64_t us;

  if (ticks_per_second == 0) {
    return 0;
  }

  us = ((uint64_t)ticks * 1000000ULL) / ticks_per_second;
  if (us > UINT32_MAX) {
    return UINT32_MAX;
  }

  return (uint32_t)us;
}

/* Latency-probe accumulator: min/avg/max roundtrip ticks over a run. */
typedef struct ZZ9KBenchProbeStats {
  ZZ9KBenchTick total;
  ZZ9KBenchTick min;
  ZZ9KBenchTick max;
  uint32_t samples;
} ZZ9KBenchProbeStats;

static void zz9k_bench_probe_stats_init(ZZ9KBenchProbeStats *stats)
{
  memset(stats, 0, sizeof(*stats));
}

static void zz9k_bench_probe_stats_add(ZZ9KBenchProbeStats *stats,
                                       ZZ9KBenchTick elapsed)
{
  if (stats->samples == 0U || elapsed < stats->min) {
    stats->min = elapsed;
  }
  if (elapsed > stats->max) {
    stats->max = elapsed;
  }
  stats->total += elapsed;
  stats->samples++;
}

static ZZ9KBenchTick zz9k_bench_probe_stats_avg(
    const ZZ9KBenchProbeStats *stats)
{
  if (stats->samples == 0U) {
    return 0;
  }
  return stats->total / stats->samples;
}

static uint32_t zz9k_bench_kib_per_second(uint32_t bytes,
                                          ZZ9KBenchTick ticks,
                                          uint32_t ticks_per_second)
{
  uint64_t rate;

  if (ticks == 0 || ticks_per_second == 0) {
    return 0;
  }

  rate = ((uint64_t)bytes * ticks_per_second) / ((uint64_t)ticks * 1024ULL);
  if (rate > UINT32_MAX) {
    return UINT32_MAX;
  }

  return (uint32_t)rate;
}

static uint32_t zz9k_bench_kpix_per_second(uint32_t pixels,
                                           ZZ9KBenchTick ticks,
                                           uint32_t ticks_per_second)
{
  uint64_t rate;

  if (ticks == 0 || ticks_per_second == 0) {
    return 0;
  }

  rate = ((uint64_t)pixels * ticks_per_second) / ((uint64_t)ticks * 1000ULL);
  if (rate > UINT32_MAX) {
    return UINT32_MAX;
  }

  return (uint32_t)rate;
}

static uint32_t zz9k_bench_calls_per_second(uint32_t calls,
                                            ZZ9KBenchTick ticks,
                                            uint32_t ticks_per_second)
{
  uint64_t rate;

  if (ticks == 0 || ticks_per_second == 0) {
    return 0;
  }

  rate = ((uint64_t)calls * ticks_per_second) / ticks;
  if (rate > UINT32_MAX) {
    return UINT32_MAX;
  }

  return (uint32_t)rate;
}

static uint32_t zz9k_bench_u32_delta(uint32_t after, uint32_t before)
{
  if (after < before) {
    return 0U;
  }

  return after - before;
}

static ZZ9KBenchTick zz9k_bench_timeout_ticks(uint32_t ticks_per_second)
{
  return (ZZ9KBenchTick)ticks_per_second *
         ZZ9K_BENCH_PIPE_TIMEOUT_SECONDS;
}

static uint32_t zz9k_bench_choose_ping_batch(uint32_t remaining)
{
  if (remaining > ZZ9K_BENCH_PING_PIPE_DEPTH) {
    return ZZ9K_BENCH_PING_PIPE_DEPTH;
  }

  return remaining;
}

static void zz9k_bench_prepare_pipelined_ping(ZZ9KRequest *request,
                                              uint32_t sequence)
{
  uint8_t payload[4];

  payload[0] = 'p';
  payload[1] = 'q';
  payload[2] = (uint8_t)((sequence >> 8) & 0xffU);
  payload[3] = (uint8_t)(sequence & 0xffU);
  (void)zz9k_request_ping(request, payload, sizeof(payload));
  request->entry.user_cookie = sequence;
}

static int zz9k_bench_pipelined_ping_reply_ok(const ZZ9KMailboxEntry *reply)
{
  if (!reply || reply->opcode != ZZ9K_OP_PING ||
      reply->status != ZZ9K_STATUS_OK || reply->payload_len != 4U) {
    return 0;
  }

  return reply->payload.inline_data[0] == 'p' &&
         reply->payload.inline_data[1] == 'q' &&
         reply->payload.inline_data[2] ==
             (uint8_t)((reply->user_cookie >> 8) & 0xffU) &&
         reply->payload.inline_data[3] ==
             (uint8_t)(reply->user_cookie & 0xffU);
}

static int zz9k_bench_build_scale_desc(ZZ9KScaleImageDesc *desc,
                                       uint32_t src_surface,
                                       uint32_t dst_surface,
                                       uint32_t src_width,
                                       uint32_t src_height,
                                       uint32_t dst_width,
                                       uint32_t dst_height,
                                       uint32_t filter)
{
  ZZ9KRect dst_rect;

  dst_rect.x = 0U;
  dst_rect.y = 0U;
  dst_rect.w = dst_width;
  dst_rect.h = dst_height;
  return zz9k_image_build_surface_scale_desc(
      desc, src_surface, dst_surface, src_width, src_height,
      &dst_rect, filter);
}

static int zz9k_bench_build_surface_copy_desc(ZZ9KSurfaceCopyDesc *desc,
                                              uint32_t src_surface,
                                              uint32_t dst_surface,
                                              uint32_t width,
                                              uint32_t height)
{
  ZZ9KRect rect;

  rect.x = 0U;
  rect.y = 0U;
  rect.w = width;
  rect.h = height;
  return zz9k_surface_build_copy_desc(
      desc, src_surface, dst_surface, &rect, 0U, 0U, 0U);
}

static int zz9k_bench_build_surface_fill_desc(ZZ9KSurfaceFillDesc *desc,
                                              uint32_t surface,
                                              uint32_t width,
                                              uint32_t height,
                                              uint32_t color)
{
  ZZ9KRect rect;

  rect.x = 0U;
  rect.y = 0U;
  rect.w = width;
  rect.h = height;
  return zz9k_surface_build_fill_desc(desc, surface, &rect, color, 0U);
}

static uint32_t zz9k_bench_crypto_digest_length(uint32_t algorithm)
{
  return zz9k_crypto_digest_length(algorithm);
}

static void zz9k_bench_build_crypto_hash_desc(ZZ9KCryptoHashDesc *desc,
                                              uint32_t algorithm,
                                              uint32_t src_handle,
                                              uint32_t src_length,
                                              uint32_t dst_handle)
{
  (void)zz9k_crypto_build_hash_desc(desc, algorithm, src_handle, 0U,
                                    src_length, dst_handle, 0U);
}

static void zz9k_bench_build_crypto_hmac_desc(ZZ9KCryptoHashDesc *desc,
                                              uint32_t algorithm,
                                              uint32_t src_handle,
                                              uint32_t src_length,
                                              uint32_t dst_handle,
                                              uint32_t key_handle,
                                              uint32_t key_length)
{
  (void)zz9k_crypto_build_hmac_desc(desc, algorithm, src_handle, 0U,
                                    src_length, dst_handle, 0U,
                                    key_handle, 0U, key_length);
}

static void zz9k_bench_build_crypto_hmac_chunk_desc(
    ZZ9KCryptoHashDesc *desc,
    uint32_t algorithm,
    uint32_t src_handle,
    uint32_t src_offset,
    uint32_t src_length,
    uint32_t dst_handle,
    uint32_t dst_offset,
    uint32_t key_handle,
    uint32_t key_length)
{
  (void)zz9k_crypto_build_hmac_desc(desc, algorithm, src_handle,
                                    src_offset, src_length, dst_handle,
                                    dst_offset, key_handle, 0U, key_length);
}

static void zz9k_bench_build_crypto_poly1305_desc(
    ZZ9KCryptoHashDesc *desc,
    uint32_t src_handle,
    uint32_t src_offset,
    uint32_t src_length,
    uint32_t dst_handle,
    uint32_t dst_offset,
    uint32_t key_handle)
{
  (void)zz9k_crypto_build_poly1305_desc(desc, src_handle, src_offset,
                                        src_length, dst_handle, dst_offset,
                                        key_handle, 0U);
}

static void zz9k_bench_build_crypto_stream_desc(
    ZZ9KCryptoStreamDesc *desc,
    uint32_t src_handle,
    uint32_t src_offset,
    uint32_t src_length,
    uint32_t dst_handle,
    uint32_t dst_offset,
    uint32_t key_handle,
    uint32_t key_offset,
    uint32_t nonce_handle,
    uint32_t nonce_offset,
    uint32_t counter)
{
  (void)zz9k_crypto_build_chacha20_desc(desc, src_handle, src_offset,
                                        src_length, dst_handle, dst_offset,
                                        key_handle, key_offset,
                                        nonce_handle, nonce_offset,
                                        counter);
}

static void zz9k_bench_build_crypto_aead_desc(
    ZZ9KCryptoAeadDesc *desc,
    uint32_t src_handle,
    uint32_t src_offset,
    uint32_t src_length,
    uint32_t dst_handle,
    uint32_t dst_offset,
    uint32_t aad_handle,
    uint32_t aad_offset,
    uint32_t aad_length,
    uint32_t key_handle,
    uint32_t key_offset,
    uint32_t nonce_handle,
    uint32_t flags)
{
  (void)zz9k_crypto_build_chacha20_poly1305_desc(
      desc, src_handle, src_offset, src_length, dst_handle, dst_offset,
      aad_handle, aad_offset, aad_length, key_handle, key_offset,
      nonce_handle, flags);
}

static void zz9k_bench_fill_bgra_surface(const ZZ9KSurface *surface)
{
  uint32_t y;
  uint32_t x;

  for (y = 0; y < surface->height; y++) {
    volatile uint8_t *row =
      (volatile uint8_t *)surface->data + (y * surface->pitch);

    for (x = 0; x < surface->width; x++) {
      volatile uint8_t *pixel = row + (x * 4U);

      pixel[0] = (uint8_t)(x * 3U);
      pixel[1] = (uint8_t)(x + y);
      pixel[2] = (uint8_t)(y * 5U);
      pixel[3] = 0xffU;
    }
  }
}

static int zz9k_bench_check_surface_sample(const ZZ9KSurface *surface)
{
  volatile const uint8_t *data;

  if (!surface || !surface->data || surface->length < 4U) {
    return 0;
  }

  data = (volatile const uint8_t *)surface->data;
  return data[3] == 0xffU;
}

#ifndef ZZ9K_BENCH_NO_MAIN
static void zz9k_bench_usage(void)
{
  printf("usage: zz9k-bench [iterations]\n");
  printf("       iterations defaults to %u, capped at %u\n",
         (unsigned)ZZ9K_BENCH_DEFAULT_ITERATIONS,
         (unsigned)ZZ9K_BENCH_MAX_ITERATIONS);
}

static int zz9k_bench_require_cap(const ZZ9KCaps *caps, uint32_t bit)
{
  const char *name;

  if ((caps->capability_bits & bit) != 0U) {
    return 1;
  }

  name = zz9k_capability_name(bit);
  printf("zz9k-bench: missing required capability: %s\n",
         name ? name : "unknown");
  return 0;
}

static int zz9k_bench_check_buffer_sample(const ZZ9KSharedBuffer *buffer,
                                          uint8_t value)
{
  volatile const uint8_t *data;
  uint32_t middle;
  uint32_t last;

  if (!buffer || !buffer->data || buffer->length == 0) {
    return 0;
  }

  data = (volatile const uint8_t *)buffer->data;
  middle = buffer->length / 2U;
  last = buffer->length - 1U;
  return data[0] == value && data[middle] == value && data[last] == value;
}

static void zz9k_bench_fill_buffer_pattern(const ZZ9KSharedBuffer *buffer)
{
  volatile uint8_t *data;
  uint32_t i;

  data = (volatile uint8_t *)buffer->data;
  for (i = 0; i < buffer->length; i++) {
    data[i] = (uint8_t)((i * 37U + 11U) & 0xffU);
  }
}

static void zz9k_bench_fill_key_pattern(const ZZ9KSharedBuffer *buffer)
{
  volatile uint8_t *data;
  uint32_t i;

  data = (volatile uint8_t *)buffer->data;
  for (i = 0; i < buffer->length; i++) {
    data[i] = (uint8_t)(0xa0U ^ (i * 13U));
  }
}

static int zz9k_bench_image_service_supports_scale_filter(
    ZZ9KContext *ctx,
    const ZZ9KCaps *caps,
    uint32_t filter)
{
  ZZ9KServiceInfo service;
  int status;

  if (!ctx || !caps ||
      (caps->capability_bits & ZZ9K_CAP_SERVICE_DISCOVERY) == 0U) {
    return 0;
  }

  memset(&service, 0, sizeof(service));
  status = zz9k_query_service(ctx, ZZ9K_SERVICE_IMAGE, &service);
  if (status != ZZ9K_STATUS_OK) {
    return 0;
  }

  return zz9k_image_scale_filter_supported_by_service(service.flags, filter);
}

static void zz9k_bench_print_transfer(const char *label, uint32_t bytes,
                                      ZZ9KBenchTick ticks,
                                      uint32_t ticks_per_second)
{
  printf("%-18s %8lu KiB in %5lu ms  %8lu KiB/s\n",
         label,
         (unsigned long)(bytes / 1024U),
         (unsigned long)zz9k_bench_ticks_to_ms(ticks, ticks_per_second),
         (unsigned long)zz9k_bench_kib_per_second(bytes, ticks,
                                                  ticks_per_second));
}

static void zz9k_bench_print_pixels(const char *label, uint32_t pixels,
                                    ZZ9KBenchTick ticks,
                                    uint32_t ticks_per_second)
{
  printf("%-18s %8lu Kpix in %5lu ms  %8lu Kpix/s\n",
         label,
         (unsigned long)(pixels / 1000U),
         (unsigned long)zz9k_bench_ticks_to_ms(ticks, ticks_per_second),
         (unsigned long)zz9k_bench_kpix_per_second(pixels, ticks,
                                                   ticks_per_second));
}

static void zz9k_bench_print_calls(const char *label, uint32_t calls,
                                   ZZ9KBenchTick ticks,
                                   uint32_t ticks_per_second)
{
  printf("%-18s %8lu calls in %5lu ms  %8lu calls/s\n",
         label,
         (unsigned long)calls,
         (unsigned long)zz9k_bench_ticks_to_ms(ticks, ticks_per_second),
         (unsigned long)zz9k_bench_calls_per_second(calls, ticks,
                                                    ticks_per_second));
}

static int zz9k_bench_run_ping(ZZ9KContext *ctx,
                               const ZZ9KBenchTimer *timer,
                               uint32_t iterations,
                               uint32_t ticks_per_second)
{
  uint8_t payload[4];
  uint8_t reply[4];
  uint32_t reply_len;
  ZZ9KBenchTick start_ticks;
  ZZ9KBenchTick ping_ticks;
  uint32_t i;
  int status;

  payload[0] = 'z';
  payload[1] = 'z';
  payload[2] = '9';
  payload[3] = 'k';

  start_ticks = zz9k_bench_timer_now(timer);
  for (i = 0; i < iterations; i++) {
    memset(reply, 0, sizeof(reply));
    reply_len = sizeof(reply);
    status = zz9k_ping(ctx, payload, sizeof(payload), reply, &reply_len);
    if (status != ZZ9K_STATUS_OK) {
      printf("SDK ping: %s (%d)\n", zz9k_status_name(status), status);
      return 1;
    }
    if (reply_len != sizeof(payload) ||
        memcmp(reply, payload, sizeof(payload)) != 0) {
      printf("SDK ping: echo verification failed\n");
      return 1;
    }
  }
  ping_ticks = zz9k_bench_elapsed_ticks(start_ticks,
                                        zz9k_bench_timer_now(timer));

  zz9k_bench_print_calls("SDK ping", iterations, ping_ticks,
                         ticks_per_second);
  return 0;
}

static int zz9k_bench_run_pipelined_ping(ZZ9KContext *ctx,
                                         const ZZ9KBenchTimer *timer,
                                         uint32_t iterations,
                                         uint32_t ticks_per_second)
{
  uint32_t submitted;
  uint32_t completed;
  ZZ9KBenchTick start_ticks;
  ZZ9KBenchTick pipe_ticks;
  int status;

  submitted = 0;
  completed = 0;
  start_ticks = zz9k_bench_timer_now(timer);
  while (submitted < iterations) {
    ZZ9KRequest requests[ZZ9K_BENCH_PING_PIPE_DEPTH];
    ZZ9KMailboxEntry replies[ZZ9K_BENCH_PING_PIPE_DEPTH];
    uint32_t batch;
    uint32_t batch_submitted;
    uint32_t batch_done;
    ZZ9KBenchTick batch_wait_start;
    uint32_t i;

    batch = zz9k_bench_choose_ping_batch(iterations - submitted);
    for (i = 0; i < batch; i++) {
      zz9k_bench_prepare_pipelined_ping(&requests[i], submitted + i);
    }

    status = zz9k_submit_batch(ctx, requests, batch, 0, &batch_submitted);
    if (status != ZZ9K_STATUS_QUEUED || batch_submitted != batch) {
      printf("SDK ping pipe submit: %s (%d), submitted=%lu of %lu\n",
             zz9k_status_name(status), status,
             (unsigned long)batch_submitted, (unsigned long)batch);
      return 1;
    }
    submitted += batch;

    batch_done = 0;
    batch_wait_start = zz9k_bench_timer_now(timer);
    while (batch_done < batch) {
      uint32_t just_completed;

      memset(replies, 0, sizeof(replies));
      just_completed = 0;
      status = zz9k_poll_batch(ctx, replies, batch - batch_done,
                               &just_completed);
      if (status == ZZ9K_STATUS_BUSY) {
        ZZ9KBenchTick waited = zz9k_bench_elapsed_ticks(
            batch_wait_start, zz9k_bench_timer_now(timer));
        if (waited > zz9k_bench_timeout_ticks(ticks_per_second)) {
          printf("SDK ping pipe: timeout waiting for completions\n");
          return 1;
        }
        continue;
      }
      if (status != ZZ9K_STATUS_OK) {
        printf("SDK ping pipe poll: %s (%d)\n",
               zz9k_status_name(status), status);
        return 1;
      }

      for (i = 0; i < just_completed; i++) {
        if (!zz9k_bench_pipelined_ping_reply_ok(&replies[i])) {
          printf("SDK ping pipe: completion verification failed\n");
          return 1;
        }
      }

      batch_done += just_completed;
      completed += just_completed;
    }
  }
  pipe_ticks = zz9k_bench_elapsed_ticks(start_ticks,
                                        zz9k_bench_timer_now(timer));

  if (completed != iterations) {
    printf("SDK ping pipe: completed %lu of %lu\n",
           (unsigned long)completed, (unsigned long)iterations);
    return 1;
  }

  zz9k_bench_print_calls("SDK ping pipe", iterations, pipe_ticks,
                         ticks_per_second);
  return 0;
}

/* One submit -> poll-until-matching-completion roundtrip, bypassing
 * zz9k_call's wait loop entirely. gap_spins == 0 polls tight. */
static int zz9k_bench_probe_once(ZZ9KContext *ctx,
                                 const ZZ9KBenchTimer *timer,
                                 uint32_t gap_spins,
                                 ZZ9KBenchTick *elapsed)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  ZZ9KBenchTick start;
  uint8_t payload[4];
  uint32_t request_id;
  uint32_t polls;
  int status;

  payload[0] = 'p';
  payload[1] = 'r';
  payload[2] = 'b';
  payload[3] = gap_spins ? 'g' : 't';

  memset(&request, 0, sizeof(request));
  status = zz9k_request_ping(&request, payload, sizeof(payload));
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }

  request_id = 0;
  start = zz9k_bench_timer_now(timer);
  status = zz9k_submit(ctx, &request, &request_id);
  if (status != ZZ9K_STATUS_QUEUED) {
    return status;
  }

  for (polls = 0; polls < ZZ9K_BENCH_PROBE_POLL_BACKSTOP; polls++) {
    memset(&reply, 0, sizeof(reply));
    status = zz9k_poll(ctx, &reply);
    if (status == ZZ9K_STATUS_OK && reply.request_id == request_id) {
      *elapsed = zz9k_bench_elapsed_ticks(start,
                                          zz9k_bench_timer_now(timer));
      return reply.status;
    }
    if (status != ZZ9K_STATUS_OK && status != ZZ9K_STATUS_BUSY) {
      return status;
    }
    if (gap_spins) {
      volatile uint32_t spin;

      for (spin = 0; spin < gap_spins; spin++) {
      }
    }
  }

  return ZZ9K_STATUS_TIMEOUT;
}

static int zz9k_bench_run_ping_probe(ZZ9KContext *ctx,
                                     const ZZ9KBenchTimer *timer,
                                     uint32_t iterations,
                                     uint32_t ticks_per_second)
{
  static const struct {
    const char *label;
    uint32_t gap_spins;
  } variants[2] = {
    { "SDK ping probe tight", 0UL },
    { "SDK ping probe gapped", ZZ9K_BENCH_PROBE_GAP_SPINS },
  };
  uint32_t v;
  uint32_t i;

  for (v = 0; v < 2U; v++) {
    ZZ9KBenchProbeStats stats;
    ZZ9KBenchTick elapsed;
    int status;

    zz9k_bench_probe_stats_init(&stats);
    for (i = 0; i < iterations; i++) {
      elapsed = 0;
      status = zz9k_bench_probe_once(ctx, timer, variants[v].gap_spins,
                                     &elapsed);
      if (status != ZZ9K_STATUS_OK) {
        printf("%s: %s (%d)\n", variants[v].label,
               zz9k_status_name(status), status);
        return 1;
      }
      zz9k_bench_probe_stats_add(&stats, elapsed);
    }

    printf("%-22s min %8lu  avg %8lu  max %8lu us\n",
           variants[v].label,
           (unsigned long)zz9k_bench_ticks_to_us(stats.min,
                                                 ticks_per_second),
           (unsigned long)zz9k_bench_ticks_to_us(
               zz9k_bench_probe_stats_avg(&stats), ticks_per_second),
           (unsigned long)zz9k_bench_ticks_to_us(stats.max,
                                                 ticks_per_second));
  }

  return 0;
}

static int zz9k_bench_run_memory(ZZ9KContext *ctx,
                                 const ZZ9KBenchTimer *timer,
                                 uint32_t iterations,
                                 uint32_t ticks_per_second)
{
  ZZ9KSharedBuffer src;
  ZZ9KSharedBuffer dst;
  ZZ9KBenchTick start_ticks;
  ZZ9KBenchTick fill_ticks;
  ZZ9KBenchTick copy_ticks;
  uint32_t i;
  uint8_t value;
  int status;
  int rc = 1;

  memset(&src, 0, sizeof(src));
  memset(&dst, 0, sizeof(dst));

  status = zz9k_alloc_shared(ctx, ZZ9K_BENCH_BUFFER_BYTES, 64U, 0, &src);
  if (status != ZZ9K_STATUS_OK) {
    printf("mem src alloc: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }

  status = zz9k_alloc_shared(ctx, ZZ9K_BENCH_BUFFER_BYTES, 64U, 0, &dst);
  if (status != ZZ9K_STATUS_OK) {
    printf("mem dst alloc: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }

  value = 0x5aU;
  start_ticks = zz9k_bench_timer_now(timer);
  for (i = 0; i < iterations; i++) {
    value = (uint8_t)(0x40U + (i & 0x3fU));
    status = zz9k_mem_fill(ctx, src.handle, 0, src.length, value);
    if (status != ZZ9K_STATUS_OK) {
      printf("mem fill: %s (%d)\n", zz9k_status_name(status), status);
      goto out;
    }
  }
  fill_ticks = zz9k_bench_elapsed_ticks(start_ticks,
                                        zz9k_bench_timer_now(timer));

  if (!zz9k_bench_check_buffer_sample(&src, value)) {
    printf("mem fill: sample verification failed\n");
    goto out;
  }

  start_ticks = zz9k_bench_timer_now(timer);
  for (i = 0; i < iterations; i++) {
    status = zz9k_mem_copy(ctx, dst.handle, 0, src.handle, 0, src.length);
    if (status != ZZ9K_STATUS_OK) {
      printf("mem copy: %s (%d)\n", zz9k_status_name(status), status);
      goto out;
    }
  }
  copy_ticks = zz9k_bench_elapsed_ticks(start_ticks,
                                        zz9k_bench_timer_now(timer));

  if (!zz9k_bench_check_buffer_sample(&dst, value)) {
    printf("mem copy: sample verification failed\n");
    goto out;
  }

  zz9k_bench_print_transfer("ARM mem fill",
                            src.length * iterations,
                            fill_ticks, ticks_per_second);
  zz9k_bench_print_transfer("ARM mem copy",
                            dst.length * iterations,
                            copy_ticks, ticks_per_second);
  rc = 0;

out:
  if (dst.handle != 0 && dst.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, dst.handle);
  }
  if (src.handle != 0 && src.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, src.handle);
  }
  return rc;
}

/*
 * Time the 68k CPU doing a plain memcpy from local RAM into the shared
 * (Zorro-mapped) buffer. Unlike "ARM mem copy" (which runs on the card), this
 * is the cost the Amiga side pays to push bytes across the bus — the real per
 * frame budget for any 68k->ARM broadcast protocol. src[0] is stirred each
 * iteration so the copy cannot be hoisted, and the tail byte is checked after
 * the timed loop to prove the writes landed.
 */
static int zz9k_bench_run_cpu_write_block(const ZZ9KBenchTimer *timer,
                                          uint32_t iterations,
                                          uint32_t ticks_per_second,
                                          const ZZ9KSharedBuffer *dst,
                                          uint8_t *src,
                                          uint32_t block_bytes,
                                          const char *label)
{
  volatile uint8_t *data;
  ZZ9KBenchTick start_ticks;
  ZZ9KBenchTick write_ticks;
  uint32_t i;

  if (dst->length < block_bytes) {
    printf("%s: shared buffer too small\n", label);
    return 1;
  }

  data = (volatile uint8_t *)dst->data;
  start_ticks = zz9k_bench_timer_now(timer);
  for (i = 0; i < iterations; i++) {
    src[0] = (uint8_t)i;
    memcpy((void *)data, src, block_bytes);
  }
  write_ticks = zz9k_bench_elapsed_ticks(start_ticks,
                                         zz9k_bench_timer_now(timer));

  if (data[block_bytes - 1U] != src[block_bytes - 1U]) {
    printf("%s: write verification failed\n", label);
    return 1;
  }

  zz9k_bench_print_transfer(label, block_bytes * iterations,
                            write_ticks, ticks_per_second);
  return 0;
}

static int zz9k_bench_run_cpu_shared_write(ZZ9KContext *ctx,
                                           const ZZ9KBenchTimer *timer,
                                           uint32_t iterations,
                                           uint32_t ticks_per_second)
{
  static uint8_t local_src[ZZ9K_BENCH_CPU_WRITE_LARGE_BYTES];
  ZZ9KSharedBuffer dst;
  uint32_t i;
  int status;
  int rc = 1;

  memset(&dst, 0, sizeof(dst));

  status = zz9k_alloc_shared(ctx, ZZ9K_BENCH_CPU_WRITE_LARGE_BYTES, 64U, 0,
                             &dst);
  if (status != ZZ9K_STATUS_OK) {
    printf("cpu write alloc: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }
  if (!dst.data) {
    printf("cpu write: shared buffer is not CPU-visible\n");
    goto out;
  }

  for (i = 0; i < (uint32_t)sizeof(local_src); i++) {
    local_src[i] = (uint8_t)((i * 37U + 11U) & 0xffU);
  }

  printf("cpu write:        68k CPU memcpy local RAM -> shared buffer\n");

  if (zz9k_bench_run_cpu_write_block(
          timer, iterations, ticks_per_second, &dst, local_src,
          ZZ9K_BENCH_CPU_WRITE_SMALL_BYTES, "CPU shared write 16K") != 0) {
    goto out;
  }
  if (zz9k_bench_run_cpu_write_block(
          timer, iterations, ticks_per_second, &dst, local_src,
          ZZ9K_BENCH_CPU_WRITE_LARGE_BYTES, "CPU shared write 256K") != 0) {
    goto out;
  }

  rc = 0;

out:
  if (dst.handle != 0 && dst.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, dst.handle);
  }
  return rc;
}

static int zz9k_bench_run_scale_filter(ZZ9KContext *ctx,
                                       const ZZ9KBenchTimer *timer,
                                       uint32_t iterations,
                                       uint32_t ticks_per_second,
                                       const ZZ9KSurface *src,
                                       const ZZ9KSurface *dst,
                                       uint32_t filter)
{
  ZZ9KScaleImageDesc desc;
  ZZ9KBenchTick start_ticks;
  ZZ9KBenchTick scale_ticks;
  uint32_t pixels;
  uint32_t i;
  int status;

  if (!zz9k_bench_build_scale_desc(&desc, src->handle, dst->handle,
                                   src->width, src->height, dst->width,
                                   dst->height, filter)) {
    printf("scale image: could not build descriptor\n");
    return 1;
  }

  start_ticks = zz9k_bench_timer_now(timer);
  for (i = 0; i < iterations; i++) {
    status = zz9k_scale_image(ctx, &desc);
    if (status != ZZ9K_STATUS_OK) {
      printf("scale image: %s (%d)\n", zz9k_status_name(status), status);
      return 1;
    }
  }
  scale_ticks = zz9k_bench_elapsed_ticks(start_ticks,
                                         zz9k_bench_timer_now(timer));

  if (!zz9k_bench_check_surface_sample(dst)) {
    printf("scale image: sample verification failed\n");
    return 1;
  }

  pixels = dst->width * dst->height * iterations;
  zz9k_bench_print_pixels(filter == ZZ9K_SCALE_BILINEAR ?
                          "ARM scale bilinear" : "ARM scale nearest",
                          pixels, scale_ticks, ticks_per_second);
  return 0;
}

static int zz9k_bench_run_scale(ZZ9KContext *ctx,
                                const ZZ9KCaps *caps,
                                const ZZ9KBenchTimer *timer,
                                uint32_t iterations,
                                uint32_t ticks_per_second)
{
  ZZ9KSurface src;
  ZZ9KSurface dst;
  int status;
  int rc = 1;

  memset(&src, 0, sizeof(src));
  memset(&dst, 0, sizeof(dst));

  status = zz9k_alloc_surface_ex(ctx, ZZ9K_BENCH_SRC_WIDTH,
                                 ZZ9K_BENCH_SRC_HEIGHT,
                                 ZZ9K_SURFACE_FORMAT_BGRA8888, 0,
                                 ZZ9K_BENCH_SRC_WIDTH * 4U, &src);
  if (status != ZZ9K_STATUS_OK) {
    printf("scale src alloc: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }

  status = zz9k_alloc_surface_ex(ctx, ZZ9K_BENCH_DST_WIDTH,
                                 ZZ9K_BENCH_DST_HEIGHT,
                                 ZZ9K_SURFACE_FORMAT_BGRA8888, 0,
                                 ZZ9K_BENCH_DST_WIDTH * 4U, &dst);
  if (status != ZZ9K_STATUS_OK) {
    printf("scale dst alloc: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }

  if ((src.flags & ZZ9K_SURFACE_FLAG_CPU_VISIBLE) == 0 || !src.data) {
    printf("scale src surface is not CPU-visible\n");
    goto out;
  }

  zz9k_bench_fill_bgra_surface(&src);
  printf("scale geometry:    %lu x %lu -> %lu x %lu\n",
         (unsigned long)src.width, (unsigned long)src.height,
         (unsigned long)dst.width, (unsigned long)dst.height);

  if (zz9k_bench_run_scale_filter(ctx, timer, iterations, ticks_per_second,
                                  &src, &dst, ZZ9K_SCALE_NEAREST) != 0) {
    goto out;
  }

  if (zz9k_bench_image_service_supports_scale_filter(
        ctx, caps, ZZ9K_SCALE_BILINEAR)) {
    if (zz9k_bench_run_scale_filter(ctx, timer, iterations, ticks_per_second,
                                    &src, &dst,
                                    ZZ9K_SCALE_BILINEAR) != 0) {
      goto out;
    }
  } else {
    printf("%-18s skipped (not advertised)\n", "ARM scale bilinear");
  }

  rc = 0;

out:
  if (dst.handle != 0 && dst.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_surface(ctx, dst.handle);
  }
  if (src.handle != 0 && src.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_surface(ctx, src.handle);
  }
  return rc;
}

static int zz9k_bench_run_local_surface_copy(ZZ9KContext *ctx,
                                             const ZZ9KBenchTimer *timer,
                                             uint32_t iterations,
                                             uint32_t ticks_per_second)
{
  ZZ9KSurface src;
  ZZ9KSurface dst;
  ZZ9KSurfaceFillDesc fill;
  ZZ9KSurfaceCopyDesc copy;
  ZZ9KBenchTick start_ticks;
  ZZ9KBenchTick copy_ticks;
  uint32_t i;
  int status;
  int rc = 1;

  memset(&src, 0, sizeof(src));
  memset(&dst, 0, sizeof(dst));

  status = zz9k_alloc_surface_ex(
      ctx, ZZ9K_BENCH_LOCAL_SURFACE_WIDTH,
      ZZ9K_BENCH_LOCAL_SURFACE_HEIGHT, ZZ9K_SURFACE_FORMAT_BGRA8888,
      ZZ9K_SURFACE_FLAG_ARM_LOCAL,
      ZZ9K_BENCH_LOCAL_SURFACE_WIDTH * ZZ9K_BENCH_LOCAL_SURFACE_BPP,
      &src);
  if (status != ZZ9K_STATUS_OK) {
    printf("local copy src alloc: %s (%d)\n",
           zz9k_status_name(status), status);
    goto out;
  }

  status = zz9k_alloc_surface_ex(
      ctx, ZZ9K_BENCH_LOCAL_SURFACE_WIDTH,
      ZZ9K_BENCH_LOCAL_SURFACE_HEIGHT, ZZ9K_SURFACE_FORMAT_BGRA8888,
      ZZ9K_SURFACE_FLAG_ARM_LOCAL,
      ZZ9K_BENCH_LOCAL_SURFACE_WIDTH * ZZ9K_BENCH_LOCAL_SURFACE_BPP,
      &dst);
  if (status != ZZ9K_STATUS_OK) {
    printf("local copy dst alloc: %s (%d)\n",
           zz9k_status_name(status), status);
    goto out;
  }

  if ((src.flags & ZZ9K_SURFACE_FLAG_ARM_LOCAL) == 0U ||
      (dst.flags & ZZ9K_SURFACE_FLAG_ARM_LOCAL) == 0U) {
    printf("local copy: surfaces are not ARM-local\n");
    goto out;
  }
  if (src.length < ZZ9K_BENCH_LOCAL_SURFACE_BYTES ||
      dst.length < ZZ9K_BENCH_LOCAL_SURFACE_BYTES) {
    printf("local copy: surface allocation shorter than requested\n");
    goto out;
  }

  if (!zz9k_bench_build_surface_fill_desc(
          &fill, src.handle, ZZ9K_BENCH_LOCAL_SURFACE_WIDTH,
          ZZ9K_BENCH_LOCAL_SURFACE_HEIGHT,
          zz9k_surface_color_rgb(0x2aU, 0x7fU, 0xffU))) {
    printf("local copy fill: could not build descriptor\n");
    goto out;
  }
  status = zz9k_fill_surface(ctx, &fill);
  if (status != ZZ9K_STATUS_OK) {
    printf("local copy fill: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }

  if (!zz9k_bench_build_surface_copy_desc(
          &copy, src.handle, dst.handle, ZZ9K_BENCH_LOCAL_SURFACE_WIDTH,
          ZZ9K_BENCH_LOCAL_SURFACE_HEIGHT)) {
    printf("local copy: could not build descriptor\n");
    goto out;
  }

  printf("local geometry:   %lu x %lu BGRA8888 ARM-local\n",
         (unsigned long)ZZ9K_BENCH_LOCAL_SURFACE_WIDTH,
         (unsigned long)ZZ9K_BENCH_LOCAL_SURFACE_HEIGHT);

  start_ticks = zz9k_bench_timer_now(timer);
  for (i = 0; i < iterations; i++) {
    status = zz9k_copy_surface(ctx, &copy);
    if (status != ZZ9K_STATUS_OK) {
      printf("local copy: %s (%d)\n", zz9k_status_name(status), status);
      goto out;
    }
  }
  copy_ticks = zz9k_bench_elapsed_ticks(start_ticks,
                                        zz9k_bench_timer_now(timer));

  zz9k_bench_print_transfer("ARM local copy",
                            ZZ9K_BENCH_LOCAL_SURFACE_BYTES * iterations,
                            copy_ticks, ticks_per_second);
  rc = 0;

out:
  if (dst.handle != 0 && dst.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_surface(ctx, dst.handle);
  }
  if (src.handle != 0 && src.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_surface(ctx, src.handle);
  }
  return rc;
}

static int zz9k_bench_run_crypto_algorithm(ZZ9KContext *ctx,
                                           const ZZ9KBenchTimer *timer,
                                           uint32_t iterations,
                                           uint32_t ticks_per_second,
                                           const ZZ9KSharedBuffer *input,
                                           const ZZ9KSharedBuffer *output,
                                           const ZZ9KSharedBuffer *key,
                                           uint32_t algorithm,
                                           const char *label)
{
  ZZ9KCryptoHashDesc desc;
  ZZ9KCryptoResult result;
  ZZ9KBenchTick start_ticks;
  ZZ9KBenchTick hash_ticks;
  uint32_t digest_length;
  uint32_t i;
  int status;

  digest_length = zz9k_bench_crypto_digest_length(algorithm);
  if (digest_length == 0U) {
    return 0;
  }

  if (key) {
    zz9k_bench_build_crypto_hmac_desc(&desc, algorithm, input->handle,
                                      input->length, output->handle,
                                      key->handle, key->length);
  } else {
    zz9k_bench_build_crypto_hash_desc(&desc, algorithm, input->handle,
                                      input->length, output->handle);
  }
  start_ticks = zz9k_bench_timer_now(timer);
  for (i = 0; i < iterations; i++) {
    memset(&result, 0, sizeof(result));
    status = zz9k_crypto_hash(ctx, &desc, &result);
    if (status == ZZ9K_STATUS_UNSUPPORTED) {
      printf("%-18s skipped (unsupported)\n", label);
      return 0;
    }
    if (status != ZZ9K_STATUS_OK) {
      printf("%s: %s (%d)\n", label, zz9k_status_name(status), status);
      return 1;
    }
    if (result.bytes_written != digest_length ||
        result.algorithm != algorithm ||
        result.flags != (key ? ZZ9K_CRYPTO_HASH_FLAG_HMAC : 0U)) {
      printf("%s: result metadata verification failed\n", label);
      return 1;
    }
  }
  hash_ticks = zz9k_bench_elapsed_ticks(start_ticks,
                                        zz9k_bench_timer_now(timer));

  zz9k_bench_print_transfer(label, input->length * iterations,
                            hash_ticks, ticks_per_second);
  return 0;
}

static int zz9k_bench_run_hmac16_sync(ZZ9KContext *ctx,
                                      const ZZ9KBenchTimer *timer,
                                      uint32_t iterations,
                                      uint32_t ticks_per_second,
                                      const ZZ9KSharedBuffer *input,
                                      const ZZ9KSharedBuffer *output,
                                      const ZZ9KSharedBuffer *key)
{
  ZZ9KCryptoHashDesc desc;
  ZZ9KCryptoResult result;
  ZZ9KBenchTick start_ticks;
  ZZ9KBenchTick hmac_ticks;
  uint32_t input_records;
  uint32_t i;
  int status;

  input_records = input->length / ZZ9K_BENCH_TLS_RECORD_BYTES;
  if (input_records == 0U) {
    printf("ARM HMAC256 16K: input buffer too small\n");
    return 1;
  }

  start_ticks = zz9k_bench_timer_now(timer);
  for (i = 0; i < iterations; i++) {
    uint32_t src_offset;

    src_offset = (i % input_records) * ZZ9K_BENCH_TLS_RECORD_BYTES;
    zz9k_bench_build_crypto_hmac_chunk_desc(
        &desc, ZZ9K_CRYPTO_HASH_SHA256, input->handle, src_offset,
        ZZ9K_BENCH_TLS_RECORD_BYTES, output->handle, 0,
        key->handle, key->length);
    memset(&result, 0, sizeof(result));
    status = zz9k_crypto_hash(ctx, &desc, &result);
    if (status != ZZ9K_STATUS_OK) {
      printf("ARM HMAC256 16K: %s (%d)\n",
             zz9k_status_name(status), status);
      return 1;
    }
    if (result.bytes_written != ZZ9K_BENCH_SHA256_DIGEST_BYTES ||
        result.algorithm != ZZ9K_CRYPTO_HASH_SHA256 ||
        result.flags != ZZ9K_CRYPTO_HASH_FLAG_HMAC) {
      printf("ARM HMAC256 16K: result metadata verification failed\n");
      return 1;
    }
  }
  hmac_ticks = zz9k_bench_elapsed_ticks(start_ticks,
                                        zz9k_bench_timer_now(timer));

  zz9k_bench_print_transfer("ARM HMAC256 16K",
                            iterations * ZZ9K_BENCH_TLS_RECORD_BYTES,
                            hmac_ticks, ticks_per_second);
  return 0;
}

static int zz9k_bench_run_hmac16_pipe(ZZ9KContext *ctx,
                                      const ZZ9KBenchTimer *timer,
                                      uint32_t iterations,
                                      uint32_t ticks_per_second,
                                      const ZZ9KSharedBuffer *input,
                                      const ZZ9KSharedBuffer *output,
                                      const ZZ9KSharedBuffer *key)
{
  static ZZ9KCryptoHashDesc descs[ZZ9K_BENCH_MAX_ITERATIONS];
  static ZZ9KCryptoResult results[ZZ9K_BENCH_MAX_ITERATIONS];
  ZZ9KBenchTick start_ticks;
  ZZ9KBenchTick pipe_ticks;
  uint32_t input_records;
  uint32_t i;
  int status;

  input_records = input->length / ZZ9K_BENCH_TLS_RECORD_BYTES;
  if (input_records == 0U) {
    printf("ARM HMAC256 pipe: input buffer too small\n");
    return 1;
  }

  memset(descs, 0, sizeof(descs));
  memset(results, 0, sizeof(results));
  for (i = 0; i < iterations; i++) {
    uint32_t src_offset;
    uint32_t dst_offset;

    src_offset = (i % input_records) * ZZ9K_BENCH_TLS_RECORD_BYTES;
    dst_offset = (i % ZZ9K_BENCH_CRYPTO_PIPE_DEPTH) *
                 ZZ9K_BENCH_SHA256_DIGEST_BYTES;
    zz9k_bench_build_crypto_hmac_chunk_desc(
        &descs[i], ZZ9K_CRYPTO_HASH_SHA256, input->handle, src_offset,
        ZZ9K_BENCH_TLS_RECORD_BYTES, output->handle, dst_offset,
        key->handle, key->length);
  }

  start_ticks = zz9k_bench_timer_now(timer);
  status = zz9k_crypto_hash_batch(ctx, descs, results, iterations,
                                  ZZ9K_BENCH_CRYPTO_PIPE_DEPTH,
                                  ZZ9K_DEFAULT_TIMEOUT_TICKS);
  pipe_ticks = zz9k_bench_elapsed_ticks(start_ticks,
                                        zz9k_bench_timer_now(timer));
  if (status != ZZ9K_STATUS_OK) {
    printf("ARM HMAC256 pipe: %s (%d)\n", zz9k_status_name(status), status);
    return 1;
  }
  for (i = 0; i < iterations; i++) {
    if (results[i].bytes_written != ZZ9K_BENCH_SHA256_DIGEST_BYTES ||
        results[i].algorithm != ZZ9K_CRYPTO_HASH_SHA256 ||
        results[i].flags != ZZ9K_CRYPTO_HASH_FLAG_HMAC) {
      printf("ARM HMAC256 pipe: result verification failed\n");
      return 1;
    }
  }

  zz9k_bench_print_transfer("ARM HMAC256 pipe",
                            iterations * ZZ9K_BENCH_TLS_RECORD_BYTES,
                            pipe_ticks, ticks_per_second);
  return 0;
}

static int zz9k_bench_run_poly1305_sync(ZZ9KContext *ctx,
                                        const ZZ9KBenchTimer *timer,
                                        uint32_t iterations,
                                        uint32_t ticks_per_second,
                                        const ZZ9KSharedBuffer *input,
                                        const ZZ9KSharedBuffer *output,
                                        const ZZ9KSharedBuffer *key)
{
  ZZ9KCryptoHashDesc desc;
  ZZ9KCryptoResult result;
  ZZ9KBenchTick start_ticks;
  ZZ9KBenchTick mac_ticks;
  uint32_t i;
  int status;

  zz9k_bench_build_crypto_poly1305_desc(
      &desc, input->handle, 0, input->length, output->handle, 0,
      key->handle);

  start_ticks = zz9k_bench_timer_now(timer);
  for (i = 0; i < iterations; i++) {
    memset(&result, 0, sizeof(result));
    status = zz9k_crypto_hash(ctx, &desc, &result);
    if (status == ZZ9K_STATUS_UNSUPPORTED) {
      printf("%-18s skipped (unsupported)\n", "ARM Poly1305");
      return 0;
    }
    if (status != ZZ9K_STATUS_OK) {
      printf("ARM Poly1305: %s (%d)\n", zz9k_status_name(status), status);
      return 1;
    }
    if (result.bytes_written != ZZ9K_BENCH_POLY1305_TAG_BYTES ||
        result.algorithm != ZZ9K_CRYPTO_HASH_POLY1305 ||
        result.flags != 0U) {
      printf("ARM Poly1305: result metadata verification failed\n");
      return 1;
    }
  }
  mac_ticks = zz9k_bench_elapsed_ticks(start_ticks,
                                       zz9k_bench_timer_now(timer));

  zz9k_bench_print_transfer("ARM Poly1305",
                            input->length * iterations,
                            mac_ticks, ticks_per_second);
  return 0;
}

static int zz9k_bench_run_poly1305_pipe(ZZ9KContext *ctx,
                                        const ZZ9KBenchTimer *timer,
                                        uint32_t iterations,
                                        uint32_t ticks_per_second,
                                        const ZZ9KSharedBuffer *input,
                                        const ZZ9KSharedBuffer *output,
                                        const ZZ9KSharedBuffer *key)
{
  static ZZ9KCryptoHashDesc descs[ZZ9K_BENCH_MAX_ITERATIONS];
  static ZZ9KCryptoResult results[ZZ9K_BENCH_MAX_ITERATIONS];
  ZZ9KBenchTick start_ticks;
  ZZ9KBenchTick pipe_ticks;
  uint32_t input_records;
  uint32_t i;
  int status;

  input_records = input->length / ZZ9K_BENCH_TLS_RECORD_BYTES;
  if (input_records == 0U) {
    printf("ARM Poly1305 pipe: input buffer too small\n");
    return 1;
  }

  memset(descs, 0, sizeof(descs));
  memset(results, 0, sizeof(results));
  for (i = 0; i < iterations; i++) {
    uint32_t src_offset;
    uint32_t dst_offset;

    src_offset = (i % input_records) * ZZ9K_BENCH_TLS_RECORD_BYTES;
    dst_offset = (i % ZZ9K_BENCH_CRYPTO_PIPE_DEPTH) *
                 ZZ9K_BENCH_POLY1305_TAG_BYTES;
    zz9k_bench_build_crypto_poly1305_desc(
        &descs[i], input->handle, src_offset,
        ZZ9K_BENCH_TLS_RECORD_BYTES, output->handle, dst_offset,
        key->handle);
  }

  start_ticks = zz9k_bench_timer_now(timer);
  status = zz9k_crypto_hash_batch(ctx, descs, results, iterations,
                                  ZZ9K_BENCH_CRYPTO_PIPE_DEPTH,
                                  ZZ9K_DEFAULT_TIMEOUT_TICKS);
  pipe_ticks = zz9k_bench_elapsed_ticks(start_ticks,
                                        zz9k_bench_timer_now(timer));
  if (status == ZZ9K_STATUS_UNSUPPORTED) {
    printf("%-18s skipped (unsupported)\n", "ARM Poly1305 pipe");
    return 0;
  }
  if (status != ZZ9K_STATUS_OK) {
    printf("ARM Poly1305 pipe: %s (%d)\n",
           zz9k_status_name(status), status);
    return 1;
  }
  for (i = 0; i < iterations; i++) {
    if (results[i].bytes_written != ZZ9K_BENCH_POLY1305_TAG_BYTES ||
        results[i].algorithm != ZZ9K_CRYPTO_HASH_POLY1305 ||
        results[i].flags != 0U) {
      printf("ARM Poly1305 pipe: result verification failed\n");
      return 1;
    }
  }

  zz9k_bench_print_transfer("ARM Poly1305 pipe",
                            iterations * ZZ9K_BENCH_TLS_RECORD_BYTES,
                            pipe_ticks, ticks_per_second);
  return 0;
}

static int zz9k_bench_run_chacha20_sync(ZZ9KContext *ctx,
                                        const ZZ9KBenchTimer *timer,
                                        uint32_t iterations,
                                        uint32_t ticks_per_second,
                                        const ZZ9KSharedBuffer *input,
                                        const ZZ9KSharedBuffer *output,
                                        const ZZ9KSharedBuffer *key,
                                        const ZZ9KSharedBuffer *nonce)
{
  ZZ9KCryptoStreamDesc desc;
  ZZ9KCryptoResult result;
  ZZ9KBenchTick start_ticks;
  ZZ9KBenchTick stream_ticks;
  uint32_t i;
  int status;

  zz9k_bench_build_crypto_stream_desc(
      &desc, input->handle, 0, input->length, output->handle, 0,
      key->handle, 0, nonce->handle, 0, 1U);

  start_ticks = zz9k_bench_timer_now(timer);
  for (i = 0; i < iterations; i++) {
    memset(&result, 0, sizeof(result));
    status = zz9k_crypto_stream(ctx, &desc, &result);
    if (status == ZZ9K_STATUS_UNSUPPORTED) {
      printf("%-18s skipped (unsupported)\n", "ARM ChaCha20");
      return 0;
    }
    if (status != ZZ9K_STATUS_OK) {
      printf("ARM ChaCha20: %s (%d)\n", zz9k_status_name(status), status);
      return 1;
    }
    if (result.bytes_written != input->length ||
        result.algorithm != ZZ9K_CRYPTO_STREAM_CHACHA20 ||
        result.flags != 0U) {
      printf("ARM ChaCha20: result metadata verification failed\n");
      return 1;
    }
  }
  stream_ticks = zz9k_bench_elapsed_ticks(start_ticks,
                                          zz9k_bench_timer_now(timer));

  zz9k_bench_print_transfer("ARM ChaCha20",
                            input->length * iterations,
                            stream_ticks, ticks_per_second);
  return 0;
}

static int zz9k_bench_run_chacha20_pipe(ZZ9KContext *ctx,
                                        const ZZ9KBenchTimer *timer,
                                        uint32_t iterations,
                                        uint32_t ticks_per_second,
                                        const ZZ9KSharedBuffer *input,
                                        const ZZ9KSharedBuffer *output,
                                        const ZZ9KSharedBuffer *key,
                                        const ZZ9KSharedBuffer *nonce)
{
  static ZZ9KCryptoStreamDesc descs[ZZ9K_BENCH_MAX_ITERATIONS];
  static ZZ9KCryptoResult results[ZZ9K_BENCH_MAX_ITERATIONS];
  ZZ9KBenchTick start_ticks;
  ZZ9KBenchTick pipe_ticks;
  uint32_t input_records;
  uint32_t output_records;
  uint32_t i;
  int status;

  input_records = input->length / ZZ9K_BENCH_TLS_RECORD_BYTES;
  output_records = output->length / ZZ9K_BENCH_TLS_RECORD_BYTES;
  if (input_records == 0U || output_records == 0U) {
    printf("ARM ChaCha20 pipe: buffer too small\n");
    return 1;
  }

  memset(descs, 0, sizeof(descs));
  memset(results, 0, sizeof(results));
  for (i = 0; i < iterations; i++) {
    uint32_t src_offset;
    uint32_t dst_offset;
    uint32_t counter;

    src_offset = (i % input_records) * ZZ9K_BENCH_TLS_RECORD_BYTES;
    dst_offset = (i % output_records) * ZZ9K_BENCH_TLS_RECORD_BYTES;
    counter = 1U + (src_offset / ZZ9K_BENCH_CHACHA20_BLOCK_BYTES);
    zz9k_bench_build_crypto_stream_desc(
        &descs[i], input->handle, src_offset,
        ZZ9K_BENCH_TLS_RECORD_BYTES, output->handle, dst_offset,
        key->handle, 0, nonce->handle, 0, counter);
  }

  start_ticks = zz9k_bench_timer_now(timer);
  status = zz9k_crypto_stream_batch(ctx, descs, results, iterations,
                                    ZZ9K_BENCH_CRYPTO_PIPE_DEPTH,
                                    ZZ9K_DEFAULT_TIMEOUT_TICKS);
  pipe_ticks = zz9k_bench_elapsed_ticks(start_ticks,
                                        zz9k_bench_timer_now(timer));
  if (status == ZZ9K_STATUS_UNSUPPORTED) {
    printf("%-18s skipped (unsupported)\n", "ARM ChaCha20 pipe");
    return 0;
  }
  if (status != ZZ9K_STATUS_OK) {
    printf("ARM ChaCha20 pipe: %s (%d)\n",
           zz9k_status_name(status), status);
    return 1;
  }
  for (i = 0; i < iterations; i++) {
    if (results[i].bytes_written != ZZ9K_BENCH_TLS_RECORD_BYTES ||
        results[i].algorithm != ZZ9K_CRYPTO_STREAM_CHACHA20 ||
        results[i].flags != 0U) {
      printf("ARM ChaCha20 pipe: result verification failed\n");
      return 1;
    }
  }

  zz9k_bench_print_transfer("ARM ChaCha20 pipe",
                            iterations * ZZ9K_BENCH_TLS_RECORD_BYTES,
                            pipe_ticks, ticks_per_second);
  return 0;
}

static int zz9k_bench_run_aead_sync(ZZ9KContext *ctx,
                                    const ZZ9KBenchTimer *timer,
                                    uint32_t iterations,
                                    uint32_t ticks_per_second,
                                    const ZZ9KSharedBuffer *input,
                                    const ZZ9KSharedBuffer *output,
                                    const ZZ9KSharedBuffer *key,
                                    const ZZ9KSharedBuffer *nonce)
{
  ZZ9KCryptoAeadDesc desc;
  ZZ9KCryptoResult result;
  ZZ9KBenchTick start_ticks;
  ZZ9KBenchTick aead_ticks;
  uint32_t i;
  int status;

  if (input->length > (0xffffffffUL - ZZ9K_BENCH_POLY1305_TAG_BYTES) ||
      output->length < input->length + ZZ9K_BENCH_POLY1305_TAG_BYTES) {
    printf("ARM ChaCha20-Poly: output buffer too small\n");
    return 1;
  }

  zz9k_bench_build_crypto_aead_desc(
      &desc, input->handle, 0, input->length, output->handle, 0,
      key->handle, ZZ9K_BENCH_CHACHA20_KEY_BYTES, 12U,
      key->handle, 0, nonce->handle, 0U);

  start_ticks = zz9k_bench_timer_now(timer);
  for (i = 0; i < iterations; i++) {
    memset(&result, 0, sizeof(result));
    status = zz9k_crypto_aead(ctx, &desc, &result);
    if (status == ZZ9K_STATUS_UNSUPPORTED) {
      printf("%-18s skipped (unsupported)\n", "ARM ChaCha20-Poly");
      return 0;
    }
    if (status != ZZ9K_STATUS_OK) {
      printf("ARM ChaCha20-Poly: %s (%d)\n",
             zz9k_status_name(status), status);
      return 1;
    }
    if (result.bytes_written != input->length + ZZ9K_BENCH_POLY1305_TAG_BYTES ||
        result.algorithm != ZZ9K_CRYPTO_AEAD_CHACHA20_POLY1305 ||
        result.flags != 0U) {
      printf("ARM ChaCha20-Poly: result metadata verification failed\n");
      return 1;
    }
  }
  aead_ticks = zz9k_bench_elapsed_ticks(start_ticks,
                                        zz9k_bench_timer_now(timer));

  zz9k_bench_print_transfer("ARM ChaCha20-Poly",
                            input->length * iterations,
                            aead_ticks, ticks_per_second);
  return 0;
}

static int zz9k_bench_run_aead_pipe(ZZ9KContext *ctx,
                                    const ZZ9KBenchTimer *timer,
                                    uint32_t iterations,
                                    uint32_t ticks_per_second,
                                    const ZZ9KSharedBuffer *input,
                                    const ZZ9KSharedBuffer *output,
                                    const ZZ9KSharedBuffer *key,
                                    const ZZ9KSharedBuffer *nonce)
{
  static ZZ9KCryptoAeadDesc descs[ZZ9K_BENCH_MAX_ITERATIONS];
  static ZZ9KCryptoResult results[ZZ9K_BENCH_MAX_ITERATIONS];
  ZZ9KBenchTick start_ticks;
  ZZ9KBenchTick pipe_ticks;
  uint32_t input_records;
  uint32_t output_record_bytes;
  uint32_t output_records;
  uint32_t i;
  int status;

  output_record_bytes = ZZ9K_BENCH_TLS_RECORD_BYTES +
                        ZZ9K_BENCH_POLY1305_TAG_BYTES;
  input_records = input->length / ZZ9K_BENCH_TLS_RECORD_BYTES;
  output_records = output->length / output_record_bytes;
  if (input_records == 0U || output_records == 0U) {
    printf("ARM ChaCha20-Poly pipe: buffer too small\n");
    return 1;
  }

  memset(descs, 0, sizeof(descs));
  memset(results, 0, sizeof(results));
  for (i = 0; i < iterations; i++) {
    uint32_t src_offset;
    uint32_t dst_offset;

    src_offset = (i % input_records) * ZZ9K_BENCH_TLS_RECORD_BYTES;
    dst_offset = (i % output_records) * output_record_bytes;
    zz9k_bench_build_crypto_aead_desc(
        &descs[i], input->handle, src_offset,
        ZZ9K_BENCH_TLS_RECORD_BYTES, output->handle, dst_offset,
        key->handle, ZZ9K_BENCH_CHACHA20_KEY_BYTES, 12U,
        key->handle, 0, nonce->handle, 0U);
  }

  start_ticks = zz9k_bench_timer_now(timer);
  status = zz9k_crypto_aead_batch(ctx, descs, results, iterations,
                                  ZZ9K_BENCH_CRYPTO_PIPE_DEPTH,
                                  ZZ9K_DEFAULT_TIMEOUT_TICKS);
  pipe_ticks = zz9k_bench_elapsed_ticks(start_ticks,
                                        zz9k_bench_timer_now(timer));
  if (status == ZZ9K_STATUS_UNSUPPORTED) {
    printf("%-18s skipped (unsupported)\n", "ARM ChaCha20-Poly pipe");
    return 0;
  }
  if (status != ZZ9K_STATUS_OK) {
    printf("ARM ChaCha20-Poly pipe: %s (%d)\n",
           zz9k_status_name(status), status);
    return 1;
  }
  for (i = 0; i < iterations; i++) {
    if (results[i].bytes_written != output_record_bytes ||
        results[i].algorithm != ZZ9K_CRYPTO_AEAD_CHACHA20_POLY1305 ||
        results[i].flags != 0U) {
      printf("ARM ChaCha20-Poly pipe: result verification failed\n");
      return 1;
    }
  }

  zz9k_bench_print_transfer("ARM ChaCha20-Poly pipe",
                            iterations * ZZ9K_BENCH_TLS_RECORD_BYTES,
                            pipe_ticks, ticks_per_second);
  return 0;
}

static int zz9k_bench_run_crypto(ZZ9KContext *ctx,
                                 const ZZ9KBenchTimer *timer,
                                 uint32_t iterations,
                                 uint32_t ticks_per_second)
{
  ZZ9KSharedBuffer input;
  ZZ9KSharedBuffer output;
  ZZ9KSharedBuffer key;
  ZZ9KSharedBuffer nonce;
  int status;
  int rc = 1;

  memset(&input, 0, sizeof(input));
  memset(&output, 0, sizeof(output));
  memset(&key, 0, sizeof(key));
  memset(&nonce, 0, sizeof(nonce));

  status = zz9k_alloc_shared(ctx, ZZ9K_BENCH_BUFFER_BYTES, 64U, 0, &input);
  if (status != ZZ9K_STATUS_OK) {
    printf("crypto input alloc: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }

  status = zz9k_alloc_shared(ctx, ZZ9K_BENCH_CRYPTO_OUTPUT_BYTES,
                             64U, 0, &output);
  if (status != ZZ9K_STATUS_OK) {
    printf("crypto output alloc: %s (%d)\n",
           zz9k_status_name(status), status);
    goto out;
  }

  status = zz9k_alloc_shared(ctx, 64U, 16U, 0, &key);
  if (status != ZZ9K_STATUS_OK) {
    printf("crypto key alloc: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }

  status = zz9k_alloc_shared(ctx, ZZ9K_BENCH_CHACHA20_NONCE_BYTES,
                             16U, 0, &nonce);
  if (status != ZZ9K_STATUS_OK) {
    printf("crypto nonce alloc: %s (%d)\n", zz9k_status_name(status), status);
    goto out;
  }

  zz9k_bench_fill_buffer_pattern(&input);
  zz9k_bench_fill_key_pattern(&key);
  zz9k_bench_fill_key_pattern(&nonce);

  if (zz9k_bench_run_crypto_algorithm(ctx, timer, iterations,
                                      ticks_per_second, &input, &output,
                                      0,
                                      ZZ9K_CRYPTO_HASH_SHA1,
                                      "ARM SHA1 hash") != 0) {
    goto out;
  }
  if (zz9k_bench_run_crypto_algorithm(ctx, timer, iterations,
                                      ticks_per_second, &input, &output,
                                      0,
                                      ZZ9K_CRYPTO_HASH_SHA256,
                                      "ARM SHA256 hash") != 0) {
    goto out;
  }
  if (zz9k_bench_run_crypto_algorithm(ctx, timer, iterations,
                                      ticks_per_second, &input, &output,
                                      &key,
                                      ZZ9K_CRYPTO_HASH_SHA1,
                                      "ARM HMAC-SHA1") != 0) {
    goto out;
  }
  if (zz9k_bench_run_crypto_algorithm(ctx, timer, iterations,
                                      ticks_per_second, &input, &output,
                                      &key,
                                      ZZ9K_CRYPTO_HASH_SHA256,
                                      "ARM HMAC-SHA256") != 0) {
    goto out;
  }
  printf("crypto record:    %lu byte HMAC-SHA256 records\n",
         (unsigned long)ZZ9K_BENCH_TLS_RECORD_BYTES);
  if (zz9k_bench_run_hmac16_sync(ctx, timer, iterations,
                                 ticks_per_second, &input, &output,
                                 &key) != 0) {
    goto out;
  }
  if (zz9k_bench_run_hmac16_pipe(ctx, timer, iterations,
                                 ticks_per_second, &input, &output,
                                 &key) != 0) {
    goto out;
  }
  if (zz9k_bench_run_poly1305_sync(ctx, timer, iterations,
                                   ticks_per_second, &input, &output,
                                   &key) != 0) {
    goto out;
  }
  printf("crypto mac:       %lu byte Poly1305 chunks\n",
         (unsigned long)ZZ9K_BENCH_TLS_RECORD_BYTES);
  if (zz9k_bench_run_poly1305_pipe(ctx, timer, iterations,
                                   ticks_per_second, &input, &output,
                                   &key) != 0) {
    goto out;
  }
  if (zz9k_bench_run_chacha20_sync(ctx, timer, iterations,
                                   ticks_per_second, &input, &output,
                                   &key, &nonce) != 0) {
    goto out;
  }
  printf("crypto stream:    %lu byte ChaCha20 chunks\n",
         (unsigned long)ZZ9K_BENCH_TLS_RECORD_BYTES);
  if (zz9k_bench_run_chacha20_pipe(ctx, timer, iterations,
                                   ticks_per_second, &input, &output,
                                   &key, &nonce) != 0) {
    goto out;
  }
  if (zz9k_bench_run_aead_sync(ctx, timer, iterations,
                               ticks_per_second, &input, &output,
                               &key, &nonce) != 0) {
    goto out;
  }
  printf("crypto aead:      %lu byte ChaCha20-Poly1305 records\n",
         (unsigned long)ZZ9K_BENCH_TLS_RECORD_BYTES);
  if (zz9k_bench_run_aead_pipe(ctx, timer, iterations,
                               ticks_per_second, &input, &output,
                               &key, &nonce) != 0) {
    goto out;
  }

  rc = 0;

out:
  if (nonce.handle != 0 && nonce.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, nonce.handle);
  }
  if (key.handle != 0 && key.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, key.handle);
  }
  if (output.handle != 0 && output.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, output.handle);
  }
  if (input.handle != 0 && input.handle != ZZ9K_INVALID_HANDLE) {
    zz9k_free_shared(ctx, input.handle);
  }
  return rc;
}

static void zz9k_bench_print_diag(ZZ9KContext *ctx,
                                  const ZZ9KDiagInfo *start_diag)
{
  ZZ9KDiagInfo diag;
  uint32_t completed_delta;
  uint32_t failed_delta;

  if (zz9k_read_diag(ctx, &diag) != ZZ9K_STATUS_OK) {
    return;
  }

  completed_delta = start_diag ?
      zz9k_bench_u32_delta(diag.requests_completed,
                           start_diag->requests_completed) :
      0U;
  failed_delta = start_diag ?
      zz9k_bench_u32_delta(diag.requests_failed,
                           start_diag->requests_failed) :
      0U;

  printf("diag: completed=%lu failed=%lu completed_delta=%lu "
         "failed_delta=%lu last_status=%s (%lu) pending=%lu "
         "shared=%lu surfaces=%lu invalid_slots=%lu heap_free=%lu "
         "largest=%lu\n",
         (unsigned long)diag.requests_completed,
         (unsigned long)diag.requests_failed,
         (unsigned long)completed_delta,
         (unsigned long)failed_delta,
         zz9k_status_name((int)diag.last_status),
         (unsigned long)diag.last_status,
         (unsigned long)diag.pending_requests,
         (unsigned long)diag.shared_buffers_used,
         (unsigned long)diag.surfaces_used,
         (unsigned long)diag.allocator_invalid_slots,
         (unsigned long)diag.shared_heap_free,
         (unsigned long)diag.shared_heap_largest_free);
}

int main(int argc, char **argv)
{
  ZZ9KContext *ctx = 0;
  ZZ9KCaps caps;
  ZZ9KDiagInfo start_diag;
  ZZ9KBenchTimer timer;
  uint32_t iterations;
  uint32_t ticks_per_second;
  int status;
  int rc = 1;
  int have_start_diag = 0;

  if (argc > 2) {
    zz9k_bench_usage();
    return 1;
  }

  iterations = zz9k_bench_parse_iterations(argc, argv);
  zz9k_bench_timer_open(&timer);
  ticks_per_second = timer.ticks_per_second;

  printf("zz9k-bench: opening SDK mailbox\n");
  fflush(stdout);
  status = zz9k_open(&ctx);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-bench: open failed: %s (%d)\n",
           zz9k_status_name(status), status);
    zz9k_bench_timer_close(&timer);
    return 1;
  }

  status = zz9k_query_caps(ctx, &caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-bench: query caps failed: %s (%d)\n",
           zz9k_status_name(status), status);
    goto out;
  }

  printf("SDK ABI %u.%u caps=0x%08lx iterations=%lu timer=%luHz%s\n",
         (unsigned)caps.abi_major, (unsigned)caps.abi_minor,
         (unsigned long)caps.capability_bits,
         (unsigned long)iterations,
         (unsigned long)ticks_per_second,
         timer.high_resolution ? "" : " fallback");

  memset(&start_diag, 0, sizeof(start_diag));
  if (zz9k_read_diag(ctx, &start_diag) == ZZ9K_STATUS_OK) {
    have_start_diag = 1;
  }

  if (!zz9k_bench_require_cap(&caps, ZZ9K_CAP_SHARED_ALLOC) ||
      !zz9k_bench_require_cap(&caps, ZZ9K_CAP_MEMORY_OPS) ||
      !zz9k_bench_require_cap(&caps, ZZ9K_CAP_SURFACES) ||
      !zz9k_bench_require_cap(&caps, ZZ9K_CAP_SURFACE_OPS) ||
      !zz9k_bench_require_cap(&caps, ZZ9K_CAP_IMAGE_SCALE)) {
    goto out;
  }

  if (zz9k_bench_run_ping(ctx, &timer, iterations, ticks_per_second) != 0) {
    goto out;
  }
  if (zz9k_bench_run_pipelined_ping(ctx, &timer, iterations,
                                    ticks_per_second) != 0) {
    goto out;
  }
  if (zz9k_bench_run_ping_probe(ctx, &timer, iterations,
                                ticks_per_second) != 0) {
    goto out;
  }
  if (zz9k_bench_run_memory(ctx, &timer, iterations, ticks_per_second) != 0) {
    goto out;
  }
  if (zz9k_bench_run_cpu_shared_write(ctx, &timer, iterations,
                                      ticks_per_second) != 0) {
    goto out;
  }
  if (zz9k_bench_run_local_surface_copy(ctx, &timer, iterations,
                                        ticks_per_second) != 0) {
    goto out;
  }
  if (zz9k_bench_run_scale(ctx, &caps, &timer, iterations,
                           ticks_per_second) != 0) {
    goto out;
  }
  if ((caps.capability_bits & ZZ9K_CAP_CRYPTO) != 0) {
    if (zz9k_bench_run_crypto(ctx, &timer, iterations,
                              ticks_per_second) != 0) {
      goto out;
    }
  } else {
    printf("%-18s skipped (capability not advertised)\n", "ARM crypto hash");
  }

  zz9k_bench_print_diag(ctx, have_start_diag ? &start_diag : 0);
  rc = 0;

out:
  zz9k_close(ctx);
  zz9k_bench_timer_close(&timer);
  return rc;
}
#endif
