/*
 * 68k offload comparison model.
 *
 * A small break-even helper for deciding whether a workload should stay on
 * native 68k, run through the experimental 68k runner on the ARM side, or be
 * implemented as a purpose-built ARM service.
 *
 * Costs are supplied by benchmark tools or hardware notes. Offload compute
 * costs set to zero are treated as unmeasured/unavailable, not free.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_M68K_OFFLOAD_MODEL_H
#define ZZ9K_M68K_OFFLOAD_MODEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ZZ9KM68KOffloadChoice {
  ZZ9K_M68K_OFFLOAD_CHOICE_UNMEASURED = 0,
  ZZ9K_M68K_OFFLOAD_CHOICE_NATIVE = 1,
  ZZ9K_M68K_OFFLOAD_CHOICE_RUNNER = 2,
  ZZ9K_M68K_OFFLOAD_CHOICE_ARM_SERVICE = 3
} ZZ9KM68KOffloadChoice;

typedef struct ZZ9KM68KOffloadEstimate {
  uint32_t bytes;
  uint32_t native_m68k_micros;
  uint32_t mailbox_micros;
  uint32_t transfer_micros;
  uint32_t arm_runner_micros;
  uint32_t arm_service_micros;
} ZZ9KM68KOffloadEstimate;

static uint32_t zz9k_m68k_offload_clamp_u64(uint64_t value)
{
  if (value > 0xffffffffULL) {
    return 0xffffffffUL;
  }
  return (uint32_t)value;
}

static uint32_t zz9k_m68k_offload_runner_micros(
    const ZZ9KM68KOffloadEstimate *estimate)
{
  if (!estimate || estimate->arm_runner_micros == 0U) {
    return 0U;
  }
  return zz9k_m68k_offload_clamp_u64(
      (uint64_t)estimate->mailbox_micros +
      (uint64_t)estimate->transfer_micros +
      (uint64_t)estimate->arm_runner_micros);
}

static uint32_t zz9k_m68k_offload_service_micros(
    const ZZ9KM68KOffloadEstimate *estimate)
{
  if (!estimate || estimate->arm_service_micros == 0U) {
    return 0U;
  }
  return zz9k_m68k_offload_clamp_u64(
      (uint64_t)estimate->mailbox_micros +
      (uint64_t)estimate->transfer_micros +
      (uint64_t)estimate->arm_service_micros);
}

static uint32_t zz9k_m68k_offload_choice_micros(
    const ZZ9KM68KOffloadEstimate *estimate,
    ZZ9KM68KOffloadChoice choice)
{
  if (!estimate) {
    return 0U;
  }
  switch (choice) {
    case ZZ9K_M68K_OFFLOAD_CHOICE_NATIVE:
      return estimate->native_m68k_micros;
    case ZZ9K_M68K_OFFLOAD_CHOICE_RUNNER:
      return zz9k_m68k_offload_runner_micros(estimate);
    case ZZ9K_M68K_OFFLOAD_CHOICE_ARM_SERVICE:
      return zz9k_m68k_offload_service_micros(estimate);
    default:
      break;
  }
  return 0U;
}

static ZZ9KM68KOffloadChoice zz9k_m68k_offload_best_choice(
    const ZZ9KM68KOffloadEstimate *estimate)
{
  uint32_t best_micros;
  uint32_t service_micros;
  uint32_t runner_micros;
  ZZ9KM68KOffloadChoice best;

  if (!estimate || estimate->native_m68k_micros == 0U) {
    return ZZ9K_M68K_OFFLOAD_CHOICE_UNMEASURED;
  }

  best = ZZ9K_M68K_OFFLOAD_CHOICE_NATIVE;
  best_micros = estimate->native_m68k_micros;

  service_micros = zz9k_m68k_offload_service_micros(estimate);
  if (service_micros != 0U && service_micros < best_micros) {
    best = ZZ9K_M68K_OFFLOAD_CHOICE_ARM_SERVICE;
    best_micros = service_micros;
  }

  runner_micros = zz9k_m68k_offload_runner_micros(estimate);
  if (runner_micros != 0U && runner_micros < best_micros) {
    best = ZZ9K_M68K_OFFLOAD_CHOICE_RUNNER;
  }

  return best;
}

static uint32_t zz9k_m68k_offload_speedup_x100(
    const ZZ9KM68KOffloadEstimate *estimate,
    ZZ9KM68KOffloadChoice choice)
{
  uint32_t choice_micros;

  if (!estimate || estimate->native_m68k_micros == 0U) {
    return 0U;
  }
  choice_micros = zz9k_m68k_offload_choice_micros(estimate, choice);
  if (choice_micros == 0U ||
      estimate->native_m68k_micros <= choice_micros) {
    return 0U;
  }
  return zz9k_m68k_offload_clamp_u64(
      ((uint64_t)estimate->native_m68k_micros * 100ULL) /
      (uint64_t)choice_micros);
}

static uint32_t zz9k_m68k_offload_kib_per_second(uint32_t bytes,
                                                 uint32_t micros)
{
  if (bytes == 0U || micros == 0U) {
    return 0U;
  }
  return zz9k_m68k_offload_clamp_u64(
      ((uint64_t)bytes * 1000000ULL) /
      (1024ULL * (uint64_t)micros));
}

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_M68K_OFFLOAD_MODEL_H */
