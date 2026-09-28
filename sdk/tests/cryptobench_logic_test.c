/*
 * Unit checks for the crypto break-even benchmark helper logic.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define ZZ9K_CRYPTOBENCH_NO_MAIN
#include "../tools/zz9k-cryptobench.c"

#include <stdint.h>
#include <stdio.h>

static int test_sweep_sizes_double_from_64(void)
{
  if (zz9k_cryptobench_sweep_size(0U) != 64U) return 1;
  if (zz9k_cryptobench_sweep_size(1U) != 128U) return 2;
  if (zz9k_cryptobench_sweep_size(4U) != 1024U) return 3;
  if (zz9k_cryptobench_sweep_size(ZZ9K_CRYPTOBENCH_SWEEP_COUNT - 1U) !=
      16384U) {
    return 4;
  }
  if (zz9k_cryptobench_sweep_size(ZZ9K_CRYPTOBENCH_SWEEP_COUNT) != 0U) {
    return 5;
  }
  return 0;
}

static int test_iteration_parser_defaults_and_clamps(void)
{
  char *default_argv[] = {"zz9k-cryptobench"};
  char *low_argv[] = {"zz9k-cryptobench", "0"};
  char *high_argv[] = {"zz9k-cryptobench", "99999"};
  char *ok_argv[] = {"zz9k-cryptobench", "32"};

  if (zz9k_cryptobench_parse_iterations(1, default_argv) !=
      ZZ9K_CRYPTOBENCH_DEFAULT_ITERATIONS) {
    return 1;
  }
  if (zz9k_cryptobench_parse_iterations(2, low_argv) != 1U) return 2;
  if (zz9k_cryptobench_parse_iterations(2, high_argv) !=
      ZZ9K_CRYPTOBENCH_MAX_ITERATIONS) {
    return 3;
  }
  if (zz9k_cryptobench_parse_iterations(2, ok_argv) != 32U) return 4;
  return 0;
}

static int test_kib_per_second_matches_rate(void)
{
  if (zz9k_cryptobench_kib_per_second(4096U, 50U, 50U) != 4U) return 1;
  if (zz9k_cryptobench_kib_per_second(4096U, 0U, 50U) != 0U) return 2;
  if (zz9k_cryptobench_kib_per_second(4096U, 50U, 0U) != 0U) return 3;
  return 0;
}

static int test_breakeven_finds_first_crossover(void)
{
  uint32_t sizes[4] = {256U, 512U, 1024U, 2048U};
  uint32_t software[4] = {800U, 700U, 600U, 500U};
  /* Offload starts slower (mailbox overhead), overtakes at 1024. */
  uint32_t offload[4] = {200U, 500U, 900U, 1200U};

  if (zz9k_cryptobench_breakeven_size(sizes, software, offload, 4U) !=
      1024U) {
    return 1;
  }
  return 0;
}

static int test_breakeven_zero_when_offload_never_wins(void)
{
  uint32_t sizes[3] = {256U, 512U, 1024U};
  uint32_t software[3] = {800U, 800U, 800U};
  uint32_t offload[3] = {100U, 200U, 300U};

  if (zz9k_cryptobench_breakeven_size(sizes, software, offload, 3U) != 0U) {
    return 1;
  }
  return 0;
}

static int test_breakeven_skips_unmeasured_offload(void)
{
  uint32_t sizes[3] = {256U, 512U, 1024U};
  uint32_t software[3] = {800U, 800U, 800U};
  /* Zero means "not measured" and must never count as a win. */
  uint32_t offload[3] = {0U, 0U, 900U};

  if (zz9k_cryptobench_breakeven_size(sizes, software, offload, 3U) !=
      1024U) {
    return 1;
  }
  return 0;
}

int main(void)
{
  int rc;

  rc = test_sweep_sizes_double_from_64();
  if (rc) {
    printf("test_sweep_sizes_double_from_64 failed: %d\n", rc);
    return 10 + rc;
  }
  rc = test_iteration_parser_defaults_and_clamps();
  if (rc) {
    printf("test_iteration_parser_defaults_and_clamps failed: %d\n", rc);
    return 20 + rc;
  }
  rc = test_kib_per_second_matches_rate();
  if (rc) {
    printf("test_kib_per_second_matches_rate failed: %d\n", rc);
    return 30 + rc;
  }
  rc = test_breakeven_finds_first_crossover();
  if (rc) {
    printf("test_breakeven_finds_first_crossover failed: %d\n", rc);
    return 40 + rc;
  }
  rc = test_breakeven_zero_when_offload_never_wins();
  if (rc) {
    printf("test_breakeven_zero_when_offload_never_wins failed: %d\n", rc);
    return 50 + rc;
  }
  rc = test_breakeven_skips_unmeasured_offload();
  if (rc) {
    printf("test_breakeven_skips_unmeasured_offload failed: %d\n", rc);
    return 60 + rc;
  }

  printf("cryptobench_logic_test: all checks passed\n");
  return 0;
}
