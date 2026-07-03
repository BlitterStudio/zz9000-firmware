/*
 * Host unit tests for the ZZ9000 dual-core task scheduler (portable core).
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "scheduler.h"
#include <stdio.h>

static int failures;

static void expect_u32(const char *n, uint32_t a, uint32_t e)
{
  if (a != e) { printf("%s: got %u expected %u\n", n, a, e); failures++; }
}

static void expect_int(const char *n, int a, int e)
{
  if (a != e) { printf("%s: got %d expected %d\n", n, a, e); failures++; }
}

static void test_stress_fill_is_deterministic(void)
{
  expect_u32("fill_same", taskq_stress_fill(0x1234u, 7u),
             taskq_stress_fill(0x1234u, 7u));
}

static void test_stress_fill_varies_by_index(void)
{
  if (taskq_stress_fill(0x1234u, 7u) == taskq_stress_fill(0x1234u, 8u)) {
    printf("fill_index_collision\n");
    failures++;
  }
}

static void test_stress_check_matches_fill(void)
{
  uint32_t v = taskq_stress_fill(0xABCDu, 42u);
  expect_int("check_ok",  taskq_stress_check(0xABCDu, 42u, v), 1);
  expect_int("check_bad", taskq_stress_check(0xABCDu, 42u, v ^ 1u), 0);
}

int main(void)
{
  test_stress_fill_is_deterministic();
  test_stress_fill_varies_by_index();
  test_stress_check_matches_fill();

  if (failures) {
    printf("scheduler_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("scheduler_test: all checks passed\n");
  return 0;
}
