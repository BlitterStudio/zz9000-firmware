/*
 * Unit checks for the 68k offload comparison model.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-m68k-offload-model.h"

#include <stdint.h>
#include <stdio.h>

static int test_costs_include_mailbox_and_transfer(void)
{
  ZZ9KM68KOffloadEstimate estimate;

  estimate.bytes = 4096U;
  estimate.native_m68k_micros = 10000U;
  estimate.mailbox_micros = 6000U;
  estimate.transfer_micros = 200U;
  estimate.arm_runner_micros = 1000U;
  estimate.arm_service_micros = 300U;

  if (zz9k_m68k_offload_runner_micros(&estimate) != 7200U) return 1;
  if (zz9k_m68k_offload_service_micros(&estimate) != 6500U) return 2;
  return 0;
}

static int test_native_wins_when_mailbox_dominates(void)
{
  ZZ9KM68KOffloadEstimate estimate;

  estimate.bytes = 64U;
  estimate.native_m68k_micros = 500U;
  estimate.mailbox_micros = 6000U;
  estimate.transfer_micros = 0U;
  estimate.arm_runner_micros = 100U;
  estimate.arm_service_micros = 20U;

  if (zz9k_m68k_offload_best_choice(&estimate) !=
      ZZ9K_M68K_OFFLOAD_CHOICE_NATIVE) {
    return 1;
  }
  if (zz9k_m68k_offload_speedup_x100(
          &estimate, ZZ9K_M68K_OFFLOAD_CHOICE_RUNNER) != 0U) {
    return 2;
  }
  if (zz9k_m68k_offload_speedup_x100(
          &estimate, ZZ9K_M68K_OFFLOAD_CHOICE_ARM_SERVICE) != 0U) {
    return 3;
  }
  return 0;
}

static int test_arm_service_wins_when_it_is_fastest(void)
{
  ZZ9KM68KOffloadEstimate estimate;

  estimate.bytes = 4096U;
  estimate.native_m68k_micros = 10000U;
  estimate.mailbox_micros = 6000U;
  estimate.transfer_micros = 200U;
  estimate.arm_runner_micros = 1000U;
  estimate.arm_service_micros = 300U;

  if (zz9k_m68k_offload_best_choice(&estimate) !=
      ZZ9K_M68K_OFFLOAD_CHOICE_ARM_SERVICE) {
    return 1;
  }
  if (zz9k_m68k_offload_speedup_x100(
          &estimate, ZZ9K_M68K_OFFLOAD_CHOICE_ARM_SERVICE) != 153U) {
    return 2;
  }
  return 0;
}

static int test_arm_service_wins_ties_against_runner(void)
{
  ZZ9KM68KOffloadEstimate estimate;

  estimate.bytes = 4096U;
  estimate.native_m68k_micros = 10000U;
  estimate.mailbox_micros = 100U;
  estimate.transfer_micros = 0U;
  estimate.arm_runner_micros = 1000U;
  estimate.arm_service_micros = 1000U;

  if (zz9k_m68k_offload_best_choice(&estimate) !=
      ZZ9K_M68K_OFFLOAD_CHOICE_ARM_SERVICE) {
    return 1;
  }
  return 0;
}

static int test_unmeasured_offload_path_cannot_win(void)
{
  ZZ9KM68KOffloadEstimate estimate;

  estimate.bytes = 4096U;
  estimate.native_m68k_micros = 10000U;
  estimate.mailbox_micros = 100U;
  estimate.transfer_micros = 0U;
  estimate.arm_runner_micros = 4000U;
  estimate.arm_service_micros = 0U;

  if (zz9k_m68k_offload_service_micros(&estimate) != 0U) return 1;
  if (zz9k_m68k_offload_best_choice(&estimate) !=
      ZZ9K_M68K_OFFLOAD_CHOICE_RUNNER) {
    return 2;
  }
  return 0;
}

static int test_missing_native_baseline_is_unmeasured(void)
{
  ZZ9KM68KOffloadEstimate estimate;

  estimate.bytes = 4096U;
  estimate.native_m68k_micros = 0U;
  estimate.mailbox_micros = 100U;
  estimate.transfer_micros = 0U;
  estimate.arm_runner_micros = 4000U;
  estimate.arm_service_micros = 300U;

  if (zz9k_m68k_offload_best_choice(&estimate) !=
      ZZ9K_M68K_OFFLOAD_CHOICE_UNMEASURED) {
    return 1;
  }
  return 0;
}

static int test_rate_uses_bytes_and_microseconds(void)
{
  if (zz9k_m68k_offload_kib_per_second(4096U, 2000U) != 2000U) return 1;
  if (zz9k_m68k_offload_kib_per_second(0U, 2000U) != 0U) return 2;
  if (zz9k_m68k_offload_kib_per_second(4096U, 0U) != 0U) return 3;
  return 0;
}

int main(void)
{
  int rc;

  rc = test_costs_include_mailbox_and_transfer();
  if (rc) {
    printf("test_costs_include_mailbox_and_transfer failed: %d\n", rc);
    return 10 + rc;
  }
  rc = test_native_wins_when_mailbox_dominates();
  if (rc) {
    printf("test_native_wins_when_mailbox_dominates failed: %d\n", rc);
    return 20 + rc;
  }
  rc = test_arm_service_wins_when_it_is_fastest();
  if (rc) {
    printf("test_arm_service_wins_when_it_is_fastest failed: %d\n", rc);
    return 30 + rc;
  }
  rc = test_arm_service_wins_ties_against_runner();
  if (rc) {
    printf("test_arm_service_wins_ties_against_runner failed: %d\n", rc);
    return 40 + rc;
  }
  rc = test_unmeasured_offload_path_cannot_win();
  if (rc) {
    printf("test_unmeasured_offload_path_cannot_win failed: %d\n", rc);
    return 50 + rc;
  }
  rc = test_missing_native_baseline_is_unmeasured();
  if (rc) {
    printf("test_missing_native_baseline_is_unmeasured failed: %d\n", rc);
    return 60 + rc;
  }
  rc = test_rate_uses_bytes_and_microseconds();
  if (rc) {
    printf("test_rate_uses_bytes_and_microseconds failed: %d\n", rc);
    return 70 + rc;
  }

  printf("m68k_offload_model_test: all checks passed\n");
  return 0;
}
