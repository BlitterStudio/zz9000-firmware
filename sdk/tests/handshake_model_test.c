/*
 * Unit checks for the handshake offload economic model.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-handshake-model.h"

#include <stdint.h>
#include <stdio.h>

static int test_offload_micros_sums_arm_and_mailbox(void)
{
  ZZ9KHandshakeOp op;

  op.m68k_micros = 500000U;
  op.arm_micros = 3000U;
  op.mailbox_micros = 120U;
  if (zz9k_handshake_offload_micros(&op) != 3120U) {
    return 1;
  }
  if (zz9k_handshake_offload_micros(0) != 0U) {
    return 2;
  }
  return 0;
}

/* A heavy asymmetric op (X25519-like): offload should win by a wide margin. */
static int test_heavy_op_offload_wins_big(void)
{
  ZZ9KHandshakeOp op;

  op.m68k_micros = 500000U; /* 500 ms software */
  op.arm_micros = 3000U;    /* 3 ms ARM compute */
  op.mailbox_micros = 120U; /* negligible round trip */
  /* 500000 / 3120 = 160.25x -> 16025 hundredths. */
  if (zz9k_handshake_speedup_x100(&op) != 16025U) {
    return 1;
  }
  if (zz9k_handshake_saved_micros(&op) != 496880U) {
    return 2;
  }
  return 0;
}

/* A cheap op dominated by the round trip: offload must not claim a win. */
static int test_cheap_op_offload_does_not_pay(void)
{
  ZZ9KHandshakeOp op;

  op.m68k_micros = 100U;    /* already cheap in software */
  op.arm_micros = 20U;
  op.mailbox_micros = 120U; /* round trip alone exceeds software cost */
  if (zz9k_handshake_speedup_x100(&op) != 0U) {
    return 1;
  }
  if (zz9k_handshake_saved_micros(&op) != 0U) {
    return 2;
  }
  return 0;
}

/* Exactly break-even (software == offload) is not a win. */
static int test_breakeven_is_not_a_win(void)
{
  ZZ9KHandshakeOp op;

  op.m68k_micros = 3120U;
  op.arm_micros = 3000U;
  op.mailbox_micros = 120U;
  if (zz9k_handshake_speedup_x100(&op) != 0U) {
    return 1;
  }
  return 0;
}

int main(void)
{
  int rc;

  rc = test_offload_micros_sums_arm_and_mailbox();
  if (rc) {
    printf("test_offload_micros_sums_arm_and_mailbox failed: %d\n", rc);
    return 10 + rc;
  }
  rc = test_heavy_op_offload_wins_big();
  if (rc) {
    printf("test_heavy_op_offload_wins_big failed: %d\n", rc);
    return 20 + rc;
  }
  rc = test_cheap_op_offload_does_not_pay();
  if (rc) {
    printf("test_cheap_op_offload_does_not_pay failed: %d\n", rc);
    return 30 + rc;
  }
  rc = test_breakeven_is_not_a_win();
  if (rc) {
    printf("test_breakeven_is_not_a_win failed: %d\n", rc);
    return 40 + rc;
  }

  printf("handshake_model_test: all checks passed\n");
  return 0;
}
