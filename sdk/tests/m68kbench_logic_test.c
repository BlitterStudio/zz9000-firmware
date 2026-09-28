/*
 * Unit checks for zz9k-m68kbench helper logic.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define ZZ9K_M68KBENCH_NO_MAIN
#include "../tools/zz9k-m68kbench.c"

#include <stdint.h>
#include <stdio.h>

static int test_option_parser_defaults(void)
{
  char *argv[] = {"zz9k-m68kbench"};
  ZZ9KM68KBenchOptions options;

  if (!zz9k_m68kbench_parse_options(1, argv, &options)) return 1;
  if (options.iterations != ZZ9K_M68KBENCH_DEFAULT_ITERATIONS) return 2;
  if (options.bytes != ZZ9K_M68KBENCH_DEFAULT_BYTES) return 3;
  if (options.mailbox_micros != 0U || options.transfer_micros != 0U ||
      options.runner_micros != 0U || options.service_micros != 0U) {
    return 4;
  }
  return 0;
}

static int test_option_parser_clamps_iterations_and_bytes(void)
{
  char *argv[] = {"zz9k-m68kbench", "0", "999999"};
  ZZ9KM68KBenchOptions options;

  if (!zz9k_m68kbench_parse_options(3, argv, &options)) return 1;
  if (options.iterations != 1U) return 2;
  if (options.bytes != ZZ9K_M68KBENCH_MAX_BYTES) return 3;
  return 0;
}

static int test_option_parser_accepts_model_costs(void)
{
  char *argv[] = {
      "zz9k-m68kbench", "32", "2048", "6000", "120", "900", "300"};
  ZZ9KM68KBenchOptions options;

  if (!zz9k_m68kbench_parse_options(7, argv, &options)) return 1;
  if (options.iterations != 32U) return 2;
  if (options.bytes != 2048U) return 3;
  if (options.mailbox_micros != 6000U) return 4;
  if (options.transfer_micros != 120U) return 5;
  if (options.runner_micros != 900U) return 6;
  if (options.service_micros != 300U) return 7;
  return 0;
}

static int test_option_parser_rejects_too_many_args(void)
{
  char *argv[] = {
      "zz9k-m68kbench", "1", "2", "3", "4", "5", "6", "7"};
  ZZ9KM68KBenchOptions options;

  if (zz9k_m68kbench_parse_options(8, argv, &options)) return 1;
  return 0;
}

static int test_pattern_checksum_and_copy_workloads(void)
{
  uint8_t src[16];
  uint8_t dst[16];
  uint32_t i;

  zz9k_m68kbench_fill_pattern(src, 16U);
  for (i = 0; i < 16U; i++) {
    dst[i] = 0U;
  }
  if (src[0] != 11U || src[1] != 48U || src[2] != 85U) return 1;
  if (zz9k_m68kbench_checksum_bytes(src, 4U) != 266U) return 2;
  if (zz9k_m68kbench_copy_bytes(src, dst, 4U) != 266U) return 3;
  if (dst[0] != 11U || dst[1] != 48U || dst[2] != 85U ||
      dst[3] != 122U) {
    return 4;
  }
  return 0;
}

static int test_ticks_to_micros_rounds_up(void)
{
  if (zz9k_m68kbench_ticks_to_micros(2U, 1000U) != 2000U) return 1;
  if (zz9k_m68kbench_ticks_to_micros(1U, 3U) != 333334U) return 2;
  if (zz9k_m68kbench_ticks_to_micros(0U, 1000U) != 0U) return 3;
  if (zz9k_m68kbench_ticks_to_micros(1U, 0U) != 0U) return 4;
  if (zz9k_m68kbench_div_round_up(0xffffffffUL, 2U) != 0x80000000UL) {
    return 5;
  }
  return 0;
}

static int test_model_estimate_uses_native_and_supplied_offload_costs(void)
{
  ZZ9KM68KBenchOptions options;
  ZZ9KM68KOffloadEstimate estimate;

  options.iterations = 16U;
  options.bytes = 4096U;
  options.mailbox_micros = 6000U;
  options.transfer_micros = 120U;
  options.runner_micros = 900U;
  options.service_micros = 300U;

  zz9k_m68kbench_build_estimate(&options, 10000U, &estimate);
  if (estimate.bytes != 4096U) return 1;
  if (estimate.native_m68k_micros != 10000U) return 2;
  if (estimate.mailbox_micros != 6000U) return 3;
  if (estimate.transfer_micros != 120U) return 4;
  if (estimate.arm_runner_micros != 900U) return 5;
  if (estimate.arm_service_micros != 300U) return 6;
  if (zz9k_m68k_offload_best_choice(&estimate) !=
      ZZ9K_M68K_OFFLOAD_CHOICE_ARM_SERVICE) {
    return 7;
  }
  return 0;
}

int main(void)
{
  int rc;

  rc = test_option_parser_defaults();
  if (rc) {
    printf("test_option_parser_defaults failed: %d\n", rc);
    return 10 + rc;
  }
  rc = test_option_parser_clamps_iterations_and_bytes();
  if (rc) {
    printf("test_option_parser_clamps_iterations_and_bytes failed: %d\n", rc);
    return 20 + rc;
  }
  rc = test_option_parser_accepts_model_costs();
  if (rc) {
    printf("test_option_parser_accepts_model_costs failed: %d\n", rc);
    return 30 + rc;
  }
  rc = test_option_parser_rejects_too_many_args();
  if (rc) {
    printf("test_option_parser_rejects_too_many_args failed: %d\n", rc);
    return 40 + rc;
  }
  rc = test_pattern_checksum_and_copy_workloads();
  if (rc) {
    printf("test_pattern_checksum_and_copy_workloads failed: %d\n", rc);
    return 50 + rc;
  }
  rc = test_ticks_to_micros_rounds_up();
  if (rc) {
    printf("test_ticks_to_micros_rounds_up failed: %d\n", rc);
    return 60 + rc;
  }
  rc = test_model_estimate_uses_native_and_supplied_offload_costs();
  if (rc) {
    printf("test_model_estimate_uses_native_and_supplied_offload_costs failed: %d\n",
           rc);
    return 70 + rc;
  }

  printf("m68kbench_logic_test: all checks passed\n");
  return 0;
}
