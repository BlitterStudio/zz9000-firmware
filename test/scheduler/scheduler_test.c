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

static void test_init_marks_all_free(void)
{
  taskq_t q;
  uint32_t i;
  taskq_init(&q);
  for (i = 0; i < TASKQ_CAPACITY; i++) {
    expect_u32("init_free", q.descs[i].state, TASK_FREE);
  }
  expect_u32("init_cursor", q.enqueue_cursor, 0u);
}

static void test_enqueue_fills_and_queues(void)
{
  taskq_t q;
  int s;
  taskq_init(&q);
  s = taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT,
                    0x100u, 32u, 0x200u, 64u, 0xAAu, 0xBBu);
  expect_int("enq_slot0", s, 0);
  expect_u32("enq_state", q.descs[0].state, TASK_QUEUED);
  expect_u32("enq_opcode", q.descs[0].opcode, TASKQ_OP_CRYPTO_KX);
  expect_u32("enq_in_addr", q.descs[0].in_addr, 0x100u);
  expect_u32("enq_cookie", q.descs[0].user_cookie, 0xBBu);
  expect_u32("enq_cursor", q.enqueue_cursor, 1u);
}

static void test_enqueue_full_returns_minus1(void)
{
  taskq_t q;
  uint32_t i;
  int s;
  taskq_init(&q);
  for (i = 0; i < TASKQ_CAPACITY; i++) {
    (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, i, i);
  }
  s = taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 99, 99);
  expect_int("enq_full", s, -1);
}

static void test_claim_any_returns_queued_and_marks_claimed(void)
{
  taskq_t q;
  int s;
  taskq_init(&q);
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 1, 1);
  s = taskq_claim_any(&q);
  expect_int("claim_slot0", s, 0);
  expect_u32("claim_state", q.descs[0].state, TASK_CLAIMED);
  expect_int("claim_again_empty", taskq_claim_any(&q), -1);
}

static void test_two_claims_never_collide(void)
{
  taskq_t q;
  int a, b;
  taskq_init(&q);
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 1, 1);
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 2, 2);
  a = taskq_claim_any(&q);
  b = taskq_claim_any(&q);
  if (a == b) { printf("claim_collision %d\n", a); failures++; }
}

static void test_claim_advances_past_lost_cas(void)
{
  taskq_t q;
  int s;
  taskq_init(&q);
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 1, 1); /* slot0 */
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 2, 2); /* slot1 */
  taskq_test_force_cas_fail = 1;    /* lose the race on the first CAS (slot0) */
  s = taskq_claim_any(&q);
  expect_int("claim_after_lost", s, 1);
  taskq_test_force_cas_fail = 0;
}

static void test_claim_short_skips_long(void)
{
  taskq_t q;
  int s;
  taskq_init(&q);
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_AEAD, TASK_LONG, 0, 8192, 0, 0, 1, 1); /* slot0 LONG */
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 2, 2);     /* slot1 SHORT */
  s = taskq_claim_short(&q);
  expect_int("claim_short_slot1", s, 1);
  expect_u32("claim_short_left_long", q.descs[0].state, TASK_QUEUED);
}

int main(void)
{
  test_stress_fill_is_deterministic();
  test_stress_fill_varies_by_index();
  test_stress_check_matches_fill();
  test_init_marks_all_free();
  test_enqueue_fills_and_queues();
  test_enqueue_full_returns_minus1();
  test_claim_any_returns_queued_and_marks_claimed();
  test_two_claims_never_collide();
  test_claim_advances_past_lost_cas();
  test_claim_short_skips_long();

  if (failures) {
    printf("scheduler_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("scheduler_test: all checks passed\n");
  return 0;
}
