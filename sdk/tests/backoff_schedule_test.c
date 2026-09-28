/*
 * Regression pin for the unarmed poll backoff schedule: fine-grained head so
 * short ops are discovered within ~1 ms, pre-existing coarse tail so long
 * ops poll the Zorro completion ring no more often than before.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/host.h"
#include <stdint.h>
#include <stdio.h>

static int failures;

static void check(int cond, const char *msg)
{
  if (cond) {
    printf("ok   %s\n", msg);
  } else {
    printf("FAIL %s\n", msg);
    failures++;
  }
}

static void test_head_is_fine_grained(void)
{
  uint32_t ticks;

  for (ticks = 0; ticks < 16U; ticks++) {
    check(zz9k_idle_backoff_limit(ticks) == 500U,
          "head ticks 0-15 spin 500");
  }
}

static void test_escalation_doubles(void)
{
  check(zz9k_idle_backoff_limit(16U) == 1000U, "tick 16 -> 1000");
  check(zz9k_idle_backoff_limit(17U) == 2000U, "tick 17 -> 2000");
  check(zz9k_idle_backoff_limit(18U) == 4000U, "tick 18 -> 4000");
  check(zz9k_idle_backoff_limit(19U) == 8000U, "tick 19 -> 8000");
  check(zz9k_idle_backoff_limit(20U) == 16000U, "tick 20 -> 16000");
  check(zz9k_idle_backoff_limit(21U) == 32000U, "tick 21 -> 32000");
}

static void test_tail_is_capped(void)
{
  check(zz9k_idle_backoff_limit(22U) == 50000U, "tick 22 -> 50000 cap");
  check(zz9k_idle_backoff_limit(23U) == 50000U, "tick 23 -> 50000 cap");
  check(zz9k_idle_backoff_limit(1000U) == 50000U, "tick 1000 -> 50000 cap");
  check(zz9k_idle_backoff_limit(0xffffffffU) == 50000U,
        "tick UINT32_MAX -> 50000 cap (no shift overflow)");
}

static void test_schedule_is_monotonic(void)
{
  uint32_t ticks;

  for (ticks = 0; ticks < 30U; ticks++) {
    check(zz9k_idle_backoff_limit(ticks + 1U) >=
              zz9k_idle_backoff_limit(ticks),
          "schedule never decreases");
  }
}

int main(void)
{
  test_head_is_fine_grained();
  test_escalation_doubles();
  test_tail_is_capped();
  test_schedule_is_monotonic();

  if (failures) {
    printf("backoff_schedule_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("backoff_schedule_test: all passed\n");
  return 0;
}
