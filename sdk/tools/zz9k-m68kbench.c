/*
 * Native 68k workload benchmark for the offload comparison model.
 *
 * Measures simple checksum and copy workloads locally, then uses optional
 * supplied offload timings to feed the comparison model. This is a data
 * collector for deciding whether the experimental 68k runner is worth taking
 * beyond contract tests.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-m68k-offload-model.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
    defined(__VBCC__)
#define ZZ9K_M68KBENCH_AMIGA 1
#include <devices/timer.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/timer.h>
#else
#include <time.h>
#endif

#define ZZ9K_M68KBENCH_DEFAULT_ITERATIONS 128U
#define ZZ9K_M68KBENCH_MAX_ITERATIONS 4096U
#define ZZ9K_M68KBENCH_DEFAULT_BYTES 4096U
#define ZZ9K_M68KBENCH_MAX_BYTES 65536U

typedef uint64_t ZZ9KM68KBenchTick;

typedef struct ZZ9KM68KBenchOptions {
  uint32_t iterations;
  uint32_t bytes;
  uint32_t mailbox_micros;
  uint32_t transfer_micros;
  uint32_t runner_micros;
  uint32_t service_micros;
} ZZ9KM68KBenchOptions;

typedef struct ZZ9KM68KBenchTimer {
  uint32_t ticks_per_second;
  int high_resolution;
#if ZZ9K_M68KBENCH_AMIGA
  struct MsgPort *timer_port;
  struct timerequest *timer_request;
#endif
} ZZ9KM68KBenchTimer;

#if ZZ9K_M68KBENCH_AMIGA
struct Device *TimerBase;
#endif

static volatile uint32_t zz9k_m68kbench_sink;

static uint32_t zz9k_m68kbench_clamp_ulong(unsigned long value,
                                           uint32_t min_value,
                                           uint32_t max_value)
{
  if (value < (unsigned long)min_value) {
    return min_value;
  }
  if (value > (unsigned long)max_value) {
    return max_value;
  }
  return (uint32_t)value;
}

static uint32_t zz9k_m68kbench_parse_u32(const char *text,
                                         uint32_t default_value,
                                         uint32_t min_value,
                                         uint32_t max_value)
{
  unsigned long value;

  if (!text) {
    return default_value;
  }
  value = strtoul(text, 0, 0);
  return zz9k_m68kbench_clamp_ulong(value, min_value, max_value);
}

static int zz9k_m68kbench_parse_options(int argc,
                                        char **argv,
                                        ZZ9KM68KBenchOptions *options)
{
  if (!options || argc > 7) {
    return 0;
  }

  memset(options, 0, sizeof(*options));
  options->iterations = ZZ9K_M68KBENCH_DEFAULT_ITERATIONS;
  options->bytes = ZZ9K_M68KBENCH_DEFAULT_BYTES;

  if (argc > 1) {
    options->iterations = zz9k_m68kbench_parse_u32(
        argv[1], ZZ9K_M68KBENCH_DEFAULT_ITERATIONS, 1U,
        ZZ9K_M68KBENCH_MAX_ITERATIONS);
  }
  if (argc > 2) {
    options->bytes = zz9k_m68kbench_parse_u32(
        argv[2], ZZ9K_M68KBENCH_DEFAULT_BYTES, 1U,
        ZZ9K_M68KBENCH_MAX_BYTES);
  }
  if (argc > 3) {
    options->mailbox_micros = zz9k_m68kbench_parse_u32(
        argv[3], 0U, 0U, 0xffffffffUL);
  }
  if (argc > 4) {
    options->transfer_micros = zz9k_m68kbench_parse_u32(
        argv[4], 0U, 0U, 0xffffffffUL);
  }
  if (argc > 5) {
    options->runner_micros = zz9k_m68kbench_parse_u32(
        argv[5], 0U, 0U, 0xffffffffUL);
  }
  if (argc > 6) {
    options->service_micros = zz9k_m68kbench_parse_u32(
        argv[6], 0U, 0U, 0xffffffffUL);
  }

  return 1;
}

static void zz9k_m68kbench_fill_pattern(uint8_t *data, uint32_t length)
{
  uint32_t i;

  if (!data) {
    return;
  }
  for (i = 0; i < length; i++) {
    data[i] = (uint8_t)((i * 37U + 11U) & 0xffU);
  }
}

static uint32_t zz9k_m68kbench_checksum_bytes(const uint8_t *data,
                                              uint32_t length)
{
  uint32_t i;
  uint32_t sum = 0U;

  if (!data) {
    return 0U;
  }
  for (i = 0; i < length; i++) {
    sum += data[i];
  }
  return sum;
}

static uint32_t zz9k_m68kbench_copy_bytes(const uint8_t *src,
                                          uint8_t *dst,
                                          uint32_t length)
{
  uint32_t i;
  uint32_t sum = 0U;

  if (!src || !dst) {
    return 0U;
  }
  for (i = 0; i < length; i++) {
    uint8_t value = src[i];

    dst[i] = value;
    sum += value;
  }
  return sum;
}

static uint32_t zz9k_m68kbench_ticks_to_micros(ZZ9KM68KBenchTick ticks,
                                               uint32_t ticks_per_second)
{
  uint64_t micros;

  if (ticks == 0U || ticks_per_second == 0U) {
    return 0U;
  }
  micros = ((uint64_t)ticks * 1000000ULL +
            (uint64_t)ticks_per_second - 1ULL) /
           (uint64_t)ticks_per_second;
  if (micros > 0xffffffffULL) {
    return 0xffffffffUL;
  }
  return (uint32_t)micros;
}

static uint32_t zz9k_m68kbench_div_round_up(uint32_t value,
                                            uint32_t divisor)
{
  uint64_t rounded;

  if (value == 0U || divisor == 0U) {
    return 0U;
  }
  rounded = ((uint64_t)value + (uint64_t)divisor - 1ULL) /
            (uint64_t)divisor;
  if (rounded > 0xffffffffULL) {
    return 0xffffffffUL;
  }
  return (uint32_t)rounded;
}

static void zz9k_m68kbench_build_estimate(
    const ZZ9KM68KBenchOptions *options,
    uint32_t native_micros,
    ZZ9KM68KOffloadEstimate *estimate)
{
  memset(estimate, 0, sizeof(*estimate));
  if (!options) {
    return;
  }
  estimate->bytes = options->bytes;
  estimate->native_m68k_micros = native_micros;
  estimate->mailbox_micros = options->mailbox_micros;
  estimate->transfer_micros = options->transfer_micros;
  estimate->arm_runner_micros = options->runner_micros;
  estimate->arm_service_micros = options->service_micros;
}

#ifndef ZZ9K_M68KBENCH_NO_MAIN

static uint32_t zz9k_m68kbench_run_checksum_iterations(
    const uint8_t *src,
    uint32_t bytes,
    uint32_t iterations)
{
  uint32_t i;
  uint32_t guard = 0U;

  for (i = 0; i < iterations; i++) {
    guard += zz9k_m68kbench_checksum_bytes(src, bytes) ^ i;
  }
  zz9k_m68kbench_sink ^= guard;
  return guard;
}

static uint32_t zz9k_m68kbench_run_copy_iterations(const uint8_t *src,
                                                   uint8_t *dst,
                                                   uint32_t bytes,
                                                   uint32_t iterations)
{
  uint32_t i;
  uint32_t guard = 0U;

  for (i = 0; i < iterations; i++) {
    guard += zz9k_m68kbench_copy_bytes(src, dst, bytes) ^ i;
  }
  zz9k_m68kbench_sink ^= guard;
  return guard;
}

static void zz9k_m68kbench_timer_close(ZZ9KM68KBenchTimer *timer)
{
#if ZZ9K_M68KBENCH_AMIGA
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

static void zz9k_m68kbench_timer_open(ZZ9KM68KBenchTimer *timer)
{
  memset(timer, 0, sizeof(*timer));
#if ZZ9K_M68KBENCH_AMIGA
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
  zz9k_m68kbench_timer_close(timer);
  memset(timer, 0, sizeof(*timer));
  timer->ticks_per_second = 50U;
#else
  timer->ticks_per_second = (uint32_t)CLOCKS_PER_SEC;
  timer->high_resolution = 1;
#endif
}

static ZZ9KM68KBenchTick zz9k_m68kbench_timer_now(
    const ZZ9KM68KBenchTimer *timer)
{
#if ZZ9K_M68KBENCH_AMIGA
  if (timer && timer->high_resolution) {
    struct EClockVal value;

    ReadEClock(&value);
    return ((ZZ9KM68KBenchTick)value.ev_hi << 32) | value.ev_lo;
  } else {
    struct DateStamp stamp;
    uint32_t minutes;

    DateStamp(&stamp);
    minutes = ((uint32_t)stamp.ds_Days * 24U * 60U) +
              (uint32_t)stamp.ds_Minute;
    return ((ZZ9KM68KBenchTick)minutes * 60ULL * 50ULL) +
           (uint32_t)stamp.ds_Tick;
  }
#else
  (void)timer;
  return (ZZ9KM68KBenchTick)clock();
#endif
}

static const char *zz9k_m68kbench_choice_name(
    ZZ9KM68KOffloadChoice choice)
{
  switch (choice) {
    case ZZ9K_M68K_OFFLOAD_CHOICE_NATIVE:
      return "native";
    case ZZ9K_M68K_OFFLOAD_CHOICE_RUNNER:
      return "runner";
    case ZZ9K_M68K_OFFLOAD_CHOICE_ARM_SERVICE:
      return "service";
    default:
      break;
  }
  return "unmeasured";
}

static void zz9k_m68kbench_print_usage(void)
{
  printf("usage: zz9k-m68kbench [iterations] [bytes] "
         "[mailbox_us] [transfer_us] [runner_us] [service_us]\n");
}

static void zz9k_m68kbench_print_model(
    const char *label,
    const ZZ9KM68KOffloadEstimate *estimate)
{
  ZZ9KM68KOffloadChoice choice;

  choice = zz9k_m68k_offload_best_choice(estimate);
  printf("%s model: native=%lu us runner=%lu us service=%lu us "
         "choice=%s speedup_runner=%lu.%02lux speedup_service=%lu.%02lux\n",
         label,
         (unsigned long)estimate->native_m68k_micros,
         (unsigned long)zz9k_m68k_offload_runner_micros(estimate),
         (unsigned long)zz9k_m68k_offload_service_micros(estimate),
         zz9k_m68kbench_choice_name(choice),
         (unsigned long)(zz9k_m68k_offload_speedup_x100(
             estimate, ZZ9K_M68K_OFFLOAD_CHOICE_RUNNER) / 100U),
         (unsigned long)(zz9k_m68k_offload_speedup_x100(
             estimate, ZZ9K_M68K_OFFLOAD_CHOICE_RUNNER) % 100U),
         (unsigned long)(zz9k_m68k_offload_speedup_x100(
             estimate, ZZ9K_M68K_OFFLOAD_CHOICE_ARM_SERVICE) / 100U),
         (unsigned long)(zz9k_m68k_offload_speedup_x100(
             estimate, ZZ9K_M68K_OFFLOAD_CHOICE_ARM_SERVICE) % 100U));
}

static void zz9k_m68kbench_print_native(const char *label,
                                        uint32_t bytes,
                                        uint32_t iterations,
                                        uint32_t micros_per_op,
                                        uint32_t guard)
{
  printf("%s native: bytes=%lu iterations=%lu us/op=%lu KiB/s=%lu "
         "guard=%08lx\n",
         label,
         (unsigned long)bytes,
         (unsigned long)iterations,
         (unsigned long)micros_per_op,
         (unsigned long)zz9k_m68k_offload_kib_per_second(
             bytes, micros_per_op),
         (unsigned long)guard);
}

int main(int argc, char **argv)
{
  static uint8_t src[ZZ9K_M68KBENCH_MAX_BYTES];
  static uint8_t dst[ZZ9K_M68KBENCH_MAX_BYTES];
  ZZ9KM68KBenchOptions options;
  ZZ9KM68KBenchTimer timer;
  ZZ9KM68KBenchTick start;
  ZZ9KM68KBenchTick elapsed;
  ZZ9KM68KOffloadEstimate estimate;
  uint32_t checksum_total_micros;
  uint32_t checksum_micros;
  uint32_t checksum_guard;
  uint32_t copy_total_micros;
  uint32_t copy_micros;
  uint32_t copy_guard;

  if (!zz9k_m68kbench_parse_options(argc, argv, &options)) {
    zz9k_m68kbench_print_usage();
    return 2;
  }

  zz9k_m68kbench_fill_pattern(src, options.bytes);
  memset(dst, 0, options.bytes);
  zz9k_m68kbench_timer_open(&timer);

  printf("zz9k-m68kbench: iterations=%lu bytes=%lu ticks_per_second=%lu\n",
         (unsigned long)options.iterations,
         (unsigned long)options.bytes,
         (unsigned long)timer.ticks_per_second);
  printf("offload inputs: mailbox=%lu us transfer=%lu us runner=%lu us "
         "service=%lu us\n",
         (unsigned long)options.mailbox_micros,
         (unsigned long)options.transfer_micros,
         (unsigned long)options.runner_micros,
         (unsigned long)options.service_micros);

  start = zz9k_m68kbench_timer_now(&timer);
  checksum_guard = zz9k_m68kbench_run_checksum_iterations(
      src, options.bytes, options.iterations);
  elapsed = zz9k_m68kbench_timer_now(&timer) - start;
  checksum_total_micros = zz9k_m68kbench_ticks_to_micros(
      elapsed, timer.ticks_per_second);
  checksum_micros = zz9k_m68kbench_div_round_up(
      checksum_total_micros, options.iterations);

  start = zz9k_m68kbench_timer_now(&timer);
  copy_guard = zz9k_m68kbench_run_copy_iterations(
      src, dst, options.bytes, options.iterations);
  elapsed = zz9k_m68kbench_timer_now(&timer) - start;
  copy_total_micros = zz9k_m68kbench_ticks_to_micros(
      elapsed, timer.ticks_per_second);
  copy_micros = zz9k_m68kbench_div_round_up(
      copy_total_micros, options.iterations);

  zz9k_m68kbench_print_native("checksum", options.bytes, options.iterations,
                              checksum_micros, checksum_guard);
  zz9k_m68kbench_build_estimate(&options, checksum_micros, &estimate);
  zz9k_m68kbench_print_model("checksum", &estimate);

  zz9k_m68kbench_print_native("copy", options.bytes, options.iterations,
                              copy_micros, copy_guard);
  zz9k_m68kbench_build_estimate(&options, copy_micros, &estimate);
  zz9k_m68kbench_print_model("copy", &estimate);

  zz9k_m68kbench_timer_close(&timer);
  return 0;
}

#endif /* ZZ9K_M68KBENCH_NO_MAIN */
