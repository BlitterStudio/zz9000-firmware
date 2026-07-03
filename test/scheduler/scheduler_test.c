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
                    0x100u, 32u, 0x200u, 64u, 0xAAu, 0xBBu, NULL);
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
    (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, i, i, NULL);
  }
  s = taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 99, 99, NULL);
  expect_int("enq_full", s, -1);
}

static void test_enqueue_copies_op_params(void)
{
  taskq_t q;
  uint8_t params[TASKQ_OP_PARAM_BYTES];
  uint32_t i;
  int s;
  for (i = 0; i < TASKQ_OP_PARAM_BYTES; i++) params[i] = (uint8_t)(i + 1);
  taskq_init(&q);
  s = taskq_enqueue(&q, TASKQ_OP_CRYPTO_AEAD, TASK_LONG, 0, 0, 0, 0, 1, 1, params);
  expect_int("enq_params_slot", s, 0);
  for (i = 0; i < TASKQ_OP_PARAM_BYTES; i++) {
    expect_u32("enq_params_byte", q.descs[0].op_params[i], (uint8_t)(i + 1));
  }
}

static void test_enqueue_null_params_zeroes(void)
{
  taskq_t q;
  uint32_t i;
  taskq_init(&q);
  /* pre-dirty the slot's params, then enqueue with NULL: must be zeroed */
  for (i = 0; i < TASKQ_OP_PARAM_BYTES; i++) q.descs[0].op_params[i] = 0xEE;
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 1, 1, NULL);
  for (i = 0; i < TASKQ_OP_PARAM_BYTES; i++) {
    expect_u32("enq_null_params", q.descs[0].op_params[i], 0u);
  }
}

static void test_claim_any_returns_queued_and_marks_claimed(void)
{
  taskq_t q;
  int s;
  taskq_init(&q);
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 1, 1, NULL);
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
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 1, 1, NULL);
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 2, 2, NULL);
  a = taskq_claim_any(&q);
  b = taskq_claim_any(&q);
  if (a == b) { printf("claim_collision %d\n", a); failures++; }
}

static void test_claim_advances_past_lost_cas(void)
{
  taskq_t q;
  int s;
  taskq_init(&q);
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 1, 1, NULL); /* slot0 */
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 2, 2, NULL); /* slot1 */
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
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_AEAD, TASK_LONG, 0, 8192, 0, 0, 1, 1, NULL); /* slot0 LONG */
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 2, 2, NULL);     /* slot1 SHORT */
  s = taskq_claim_short(&q);
  expect_int("claim_short_slot1", s, 1);
  expect_u32("claim_short_left_long", q.descs[0].state, TASK_QUEUED);
}

static void test_complete_sets_done_and_payload(void)
{
  taskq_t q;
  uint8_t pay[3] = {7, 8, 9};
  taskq_init(&q);
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 1, 1, NULL);
  (void)taskq_claim_any(&q);
  taskq_complete(&q, 0, 0 /*OK*/, pay, 3);
  expect_u32("done_state", q.descs[0].state, TASK_DONE);
  expect_u32("done_len", q.descs[0].result_len, 3u);
  expect_u32("done_pay1", q.descs[0].result_payload[1], 8u);
}

static void test_complete_clamps_payload(void)
{
  taskq_t q;
  uint8_t big[64];
  uint32_t i;
  taskq_init(&q);
  for (i = 0; i < 64; i++) big[i] = (uint8_t)i;
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 1, 1, NULL);
  (void)taskq_claim_any(&q);
  taskq_complete(&q, 0, 0, big, 64);
  expect_u32("clamp_len", q.descs[0].result_len, TASKQ_RESULT_PAYLOAD);
}

static void test_fail_sets_failed(void)
{
  taskq_t q;
  taskq_init(&q);
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 1, 1, NULL);
  (void)taskq_claim_any(&q);
  taskq_fail(&q, 0, 5 /*err*/);
  expect_u32("fail_state", q.descs[0].state, TASK_FAILED);
  expect_u32("fail_status", q.descs[0].result_status, 5u);
}

static void test_harvest_finds_done_then_release(void)
{
  taskq_t q;
  int h;
  taskq_init(&q);
  (void)taskq_enqueue(&q, TASKQ_OP_CRYPTO_KX, TASK_SHORT, 0, 0, 0, 0, 1, 1, NULL);
  expect_int("harvest_none", taskq_harvest(&q), -1);
  (void)taskq_claim_any(&q);
  taskq_complete(&q, 0, 0, 0, 0);
  h = taskq_harvest(&q);
  expect_int("harvest_slot0", h, 0);
  taskq_release(&q, 0);
  expect_u32("release_free", q.descs[0].state, TASK_FREE);
  expect_int("harvest_after_release", taskq_harvest(&q), -1);
}

static void test_class_kx_verify_always_short(void)
{
  expect_int("kx_short", taskq_class_for_opcode(TASKQ_OP_CRYPTO_KX, 999999u), TASK_SHORT);
  expect_int("verify_short", taskq_class_for_opcode(TASKQ_OP_CRYPTO_VERIFY, 999999u), TASK_SHORT);
}

static void test_class_data_ops_size_threshold(void)
{
  expect_int("aead_small", taskq_class_for_opcode(TASKQ_OP_CRYPTO_AEAD, 4096u), TASK_SHORT);
  expect_int("aead_big",   taskq_class_for_opcode(TASKQ_OP_CRYPTO_AEAD, 4097u), TASK_LONG);
  expect_int("hash_big",   taskq_class_for_opcode(TASKQ_OP_CRYPTO_HASH, 65536u), TASK_LONG);
  expect_int("stream_small", taskq_class_for_opcode(TASKQ_OP_CRYPTO_STREAM, 16u), TASK_SHORT);
}

static void test_class_unknown_is_long(void)
{
  expect_int("unknown_long", taskq_class_for_opcode(0x9999u, 0u), TASK_LONG);
}

static void test_should_drain_gates(void)
{
  expect_int("drain_idle",     taskq_should_drain(0, 0, 1), 1);
  expect_int("drain_zorro",    taskq_should_drain(1, 0, 1), 0);
  expect_int("drain_display",  taskq_should_drain(0, 1, 1), 0);
  expect_int("drain_no_core1", taskq_should_drain(0, 0, 0), 0);
}

static void test_watchdog_trips_at_threshold(void)
{
  taskq_watchdog_t w;
  taskq_watchdog_init(&w, 3);
  expect_int("wd_enabled0", taskq_watchdog_core1_enabled(&w), 1);
  taskq_watchdog_on_fault(&w);
  taskq_watchdog_on_fault(&w);
  expect_int("wd_enabled2", taskq_watchdog_core1_enabled(&w), 1);
  taskq_watchdog_on_fault(&w);
  expect_int("wd_disabled3", taskq_watchdog_core1_enabled(&w), 0);
}

static void test_watchdog_success_resets_counter(void)
{
  taskq_watchdog_t w;
  taskq_watchdog_init(&w, 3);
  taskq_watchdog_on_fault(&w);
  taskq_watchdog_on_fault(&w);
  taskq_watchdog_on_success(&w);
  taskq_watchdog_on_fault(&w);
  taskq_watchdog_on_fault(&w);
  expect_int("wd_still_enabled", taskq_watchdog_core1_enabled(&w), 1);
}

int main(void)
{
  test_stress_fill_is_deterministic();
  test_stress_fill_varies_by_index();
  test_stress_check_matches_fill();
  test_init_marks_all_free();
  test_enqueue_fills_and_queues();
  test_enqueue_full_returns_minus1();
  test_enqueue_copies_op_params();
  test_enqueue_null_params_zeroes();
  test_claim_any_returns_queued_and_marks_claimed();
  test_two_claims_never_collide();
  test_claim_advances_past_lost_cas();
  test_claim_short_skips_long();
  test_complete_sets_done_and_payload();
  test_complete_clamps_payload();
  test_fail_sets_failed();
  test_harvest_finds_done_then_release();
  test_class_kx_verify_always_short();
  test_class_data_ops_size_threshold();
  test_class_unknown_is_long();
  test_should_drain_gates();
  test_watchdog_trips_at_threshold();
  test_watchdog_success_resets_counter();

  if (failures) {
    printf("scheduler_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("scheduler_test: all checks passed\n");
  return 0;
}
