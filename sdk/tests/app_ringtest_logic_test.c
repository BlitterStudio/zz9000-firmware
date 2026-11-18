/*
 * Unit checks for the zz9k-app-ringtest message accounting and verdict.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "../tools/zz9k-app-ringtest.h"

#include <stdint.h>
#include <string.h>

#define IDLE ZZ9K_RT_PHASE_IDLE
#define FLOOD ZZ9K_RT_PHASE_FLOOD

static void make_info(uint8_t m[32], uint32_t offset, uint32_t app_us)
{
  memset(m, 0, 32);
  zz9k_rt_put(m, 0, ZZ9K_RT_KIND_INFO);
  zz9k_rt_put(m, 1, ZZ9K_RT_VERSION);
  zz9k_rt_put(m, 2, offset);
  zz9k_rt_put(m, 3, 4096U);
  zz9k_rt_put(m, 4, app_us);
}

static void make_data(uint8_t m[32], uint32_t seq, uint32_t app_us,
                      uint32_t counter)
{
  unsigned i;

  memset(m, 0, 32);
  zz9k_rt_put(m, 0, ZZ9K_RT_KIND_DATA);
  zz9k_rt_put(m, 1, seq);
  zz9k_rt_put(m, 2, app_us);
  zz9k_rt_put(m, 3, counter);
  for (i = 0; i < 4U; i++) zz9k_rt_put(m, 4U + i, zz9k_rt_pattern(seq, i));
}

static void make_done(uint8_t m[32], uint32_t total, uint32_t counter,
                      uint32_t pongs)
{
  memset(m, 0, 32);
  zz9k_rt_put(m, 0, ZZ9K_RT_KIND_DONE);
  zz9k_rt_put(m, 1, total);
  zz9k_rt_put(m, 3, counter);
  zz9k_rt_put(m, 5, pongs);
}

static void make_pong(uint8_t m[32], uint32_t id, uint64_t stamp)
{
  memset(m, 0, 32);
  zz9k_rt_put(m, 0, ZZ9K_RT_KIND_PONG);
  zz9k_rt_put(m, 1, id);
  zz9k_rt_put(m, 2, (uint32_t)stamp);
  zz9k_rt_put(m, 3, (uint32_t)(stamp >> 32));
}

static void make_ack(uint8_t m[32], uint32_t id, uint32_t mode,
                     uint32_t count)
{
  memset(m, 0, 32);
  zz9k_rt_put(m, 0, ZZ9K_RT_KIND_ACK);
  zz9k_rt_put(m, 1, id);
  zz9k_rt_put(m, 2, mode);
  zz9k_rt_put(m, 3, count);
}

/* INFO only. */
static void begin_run(ZZ9KRingTest *t)
{
  uint8_t m[32];

  zz9k_rt_init(t);
  make_info(m, 0x00123000U, 5000U);
  zz9k_rt_on_message(t, m, 100000U);
}

/* Messages first..last at 100 us spacing, each followed by a counter read
 * equal to what it carried. */
static void add_data(ZZ9KRingTest *t, uint32_t first, uint32_t last)
{
  uint8_t m[32];
  uint32_t seq;

  for (seq = first; seq <= last; seq++) {
    make_data(m, seq, 5000U + seq * 100U, seq * 3U);
    zz9k_rt_on_message(t, m, 100000U + seq * 100U);
    zz9k_rt_on_counter(t, seq * 3U);
  }
}

/* INFO, then messages 1..count. */
static void clean_run(ZZ9KRingTest *t, uint32_t count)
{
  begin_run(t);
  add_data(t, 1U, count);
}

/* Sends ping ids first..first+n-1 in a phase and answers each after rtt. */
static void ping_round(ZZ9KRingTest *t, uint32_t phase, uint32_t first,
                       uint32_t n, uint64_t rtt)
{
  uint8_t m[32];
  uint32_t id;

  t->phase = phase;
  for (id = first; id < first + n; id++) {
    uint64_t stamp = 1000000ULL + id * 20000ULL;

    zz9k_rt_note_ping(t, id, stamp);
    make_pong(m, id, stamp);
    zz9k_rt_on_message(t, m, stamp + rtt);
  }
}

/* A CTRL and its ACK carrying the DATA count the host has seen. */
static void ctrl_round(ZZ9KRingTest *t, uint32_t id, uint32_t mode,
                       uint32_t count)
{
  uint8_t m[32];

  zz9k_rt_note_ctrl(t, id, mode);
  make_ack(m, id, mode, count);
  zz9k_rt_on_message(t, m, 500000U);
}

/* The whole flow the tool runs: idle pings, flood, flood pings, back to
 * quiet, idle pings, DONE. done_total and done_pongs override what DONE
 * reports; the rest is consistent. */
static void full_run_ex(ZZ9KRingTest *t, uint32_t count, uint32_t done_total,
                        uint32_t done_pongs)
{
  uint8_t m[32];

  begin_run(t);
  ping_round(t, IDLE, 1U, 3U, 300U);
  ctrl_round(t, 1U, ZZ9K_RT_MODE_FLOOD, 0U);
  add_data(t, 1U, count);
  ping_round(t, FLOOD, 4U, 3U, 12000U);
  ctrl_round(t, 2U, ZZ9K_RT_MODE_QUIET, count);
  ping_round(t, IDLE, 7U, 3U, 400U);
  make_done(m, done_total, count * 3U, done_pongs);
  zz9k_rt_on_message(t, m, 900000U);
}

static void full_run(ZZ9KRingTest *t, uint32_t count)
{
  full_run_ex(t, count, count, 9U);
}

/* The same four vectors sdk/zzapp/host/tests/check_runner.py checks against
 * the ARM app's output. */
static int test_pattern_known_answers(void)
{
  static const struct {
    uint32_t seq;
    uint32_t w[4];
  } vectors[] = {
    { 1U, { 0x249cb285U, 0xcc0d53cdU, 0x5ceb4d08U, 0x18c9aec4U } },
    { 2U, { 0x4939650bU, 0xc27c2913U, 0xe9250490U, 0x93a0fe04U } },
    { 1000U, { 0x1f0a4224U, 0x9f34547fU, 0xb764bc10U, 0x4f4ffc5fU } },
    { 0xfffffffeU, { 0xf94c9009U, 0x0cc81f63U, 0xc24094c8U, 0x937f414eU } },
  };
  unsigned v;
  unsigned i;

  for (v = 0; v < sizeof(vectors) / sizeof(vectors[0]); v++) {
    for (i = 0; i < 4U; i++) {
      if (zz9k_rt_pattern(vectors[v].seq, i) != vectors[v].w[i]) return 1;
    }
  }
  return 0;
}

static int test_word_layout_is_little_endian(void)
{
  uint8_t m[32];

  memset(m, 0, sizeof(m));
  zz9k_rt_put(m, 1, 0x11223344U);
  if (m[4] != 0x44U || m[5] != 0x33U || m[6] != 0x22U || m[7] != 0x11U) {
    return 1;
  }
  if (zz9k_rt_get(m, 1) != 0x11223344U) return 2;
  zz9k_rt_build_ping(m, 9U, 0x0000000200000001ULL);
  if (zz9k_rt_get(m, 0) != ZZ9K_RT_KIND_PING || zz9k_rt_get(m, 1) != 9U ||
      zz9k_rt_get(m, 2) != 1U || zz9k_rt_get(m, 3) != 2U) {
    return 3;
  }
  zz9k_rt_build_ctrl(m, 5U, ZZ9K_RT_MODE_FLOOD);
  if (zz9k_rt_get(m, 0) != ZZ9K_RT_KIND_CTRL || zz9k_rt_get(m, 1) != 5U ||
      zz9k_rt_get(m, 2) != ZZ9K_RT_MODE_FLOOD || zz9k_rt_get(m, 3) != 0U) {
    return 4;
  }
  return 0;
}

static int test_full_run_passes(void)
{
  ZZ9KRingTest t;

  full_run(&t, 100U);
  if (zz9k_rt_verdict(&t, 0U) != 0) return 1;
  if (t.messages != 100U || zz9k_rt_failures(&t) != 0U) return 2;
  if (t.block_offset != 0x00123000U || t.block_bytes != 4096U) return 3;
  if (t.counter_samples != 100U || !t.have_done) return 4;
  if (t.rtt[IDLE].n != 6U || t.rtt[FLOOD].n != 3U) return 5;
  if (t.pongs_received != 9U || t.ctrls_acked != 2U) return 6;
  return 0;
}

static int test_gap_is_counted_and_fails(void)
{
  ZZ9KRingTest t;
  uint8_t m[32];

  zz9k_rt_init(&t);
  make_info(m, 0, 0);
  zz9k_rt_on_message(&t, m, 0);
  make_data(m, 1, 0, 0);
  zz9k_rt_on_message(&t, m, 0);
  make_data(m, 5, 0, 0);
  zz9k_rt_on_message(&t, m, 0);
  make_data(m, 6, 0, 0);
  zz9k_rt_on_message(&t, m, 0);
  if (t.gaps != 1U || t.lost != 3U || t.messages != 3U) return 1;
  if (zz9k_rt_verdict(&t, 0U) == 0) return 2;
  return 0;
}

static int test_repeat_and_reorder_fail(void)
{
  ZZ9KRingTest t;
  uint8_t m[32];

  full_run(&t, 100U);
  make_data(m, 2, 0, 0);
  zz9k_rt_on_message(&t, m, 0);
  if (t.reordered != 1U || t.messages != 100U) return 1;
  if (zz9k_rt_verdict(&t, 0U) == 0) return 2;
  return 0;
}

static int test_wrong_bytes_are_counted(void)
{
  ZZ9KRingTest t;
  uint8_t m[32];

  clean_run(&t, 2U);
  make_data(m, 3, 0, 0);
  m[17] ^= 0x01U;
  m[18] ^= 0xffU;
  zz9k_rt_on_message(&t, m, 0);
  if (t.bad_messages != 1U || t.bad_bytes != 2U) return 1;
  /* A bad message does not move the sequence. */
  if (t.expected_seq != 3U || t.messages != 2U) return 2;
  if (zz9k_rt_verdict(&t, 0U) == 0) return 3;
  return 0;
}

static int test_done_total_exposes_tail_loss(void)
{
  ZZ9KRingTest t;

  full_run_ex(&t, 100U, 104U, 9U);
  if (t.tail_lost != 4U) return 1;
  if (zz9k_rt_verdict(&t, 0U) == 0) return 2;

  full_run_ex(&t, 100U, 97U, 9U);
  if (t.bad_done != 1U || zz9k_rt_verdict(&t, 0U) == 0) return 3;
  return 0;
}

static int test_counter_checks(void)
{
  ZZ9KRingTest t;
  uint8_t m[32];

  clean_run(&t, 5U); /* last counter 15 */
  zz9k_rt_on_counter(&t, 14U);
  if (t.counter_nonmono != 1U || t.counter_stale != 1U) return 1;

  clean_run(&t, 5U);
  zz9k_rt_on_counter(&t, 16U);
  zz9k_rt_on_counter(&t, 16U);
  if (zz9k_rt_failures(&t) != 0U) return 2;

  /* A wrap is not a regression. */
  zz9k_rt_init(&t);
  zz9k_rt_on_counter(&t, 0xfffffffeU);
  zz9k_rt_on_counter(&t, 0x00000002U);
  if (t.counter_nonmono != 0U) return 3;

  /* A read below the value the last message carried is stale. */
  clean_run(&t, 5U);
  make_data(m, 6, 0, 1000U);
  zz9k_rt_on_message(&t, m, 0);
  zz9k_rt_on_counter(&t, 999U);
  if (t.counter_stale < 1U) return 4;

  /* A sample above the app's final counter is impossible. */
  clean_run(&t, 5U);
  zz9k_rt_on_counter(&t, 5000U);
  make_done(m, 5U, 20U, 0U);
  zz9k_rt_on_message(&t, m, 0);
  if (t.counter_ahead != 1U) return 5;
  if (zz9k_rt_verdict(&t, 0U) == 0) return 6;
  return 0;
}

static int test_ping_round_trips_by_phase(void)
{
  ZZ9KRingTest t;
  uint8_t m[32];

  zz9k_rt_init(&t);
  t.phase = IDLE;
  zz9k_rt_note_ping(&t, 1, 1000U);
  zz9k_rt_note_ping(&t, 2, 2000U);
  t.phase = FLOOD;
  zz9k_rt_note_ping(&t, 3, 0x100000000ULL + 5U);
  make_pong(m, 1, 1000U);
  zz9k_rt_on_message(&t, m, 1300U);
  make_pong(m, 2, 2000U);
  zz9k_rt_on_message(&t, m, 2900U);
  make_pong(m, 3, 0x100000000ULL + 5U);
  zz9k_rt_on_message(&t, m, 0x100000000ULL + 20005U);
  if (t.rtt[IDLE].n != 2U || t.rtt[IDLE].min != 300U ||
      t.rtt[IDLE].max != 900U) {
    return 1;
  }
  if (zz9k_rt_rtt_avg(&t.rtt[IDLE]) != 600U) return 2;
  if (t.rtt[FLOOD].n != 1U || t.rtt[FLOOD].max != 20000U) return 3;
  if (t.pings_sent != 3U || t.pongs_received != 3U) return 4;
  if (zz9k_rt_failures(&t) != 0U) return 5;

  /* A ping belongs to the phase it was sent in, not the one it is answered
   * in. */
  zz9k_rt_init(&t);
  t.phase = FLOOD;
  zz9k_rt_note_ping(&t, 1, 1000U);
  t.phase = IDLE;
  make_pong(m, 1, 1000U);
  zz9k_rt_on_message(&t, m, 9000U);
  if (t.rtt[FLOOD].n != 1U || t.rtt[IDLE].n != 0U) return 6;
  return 0;
}

static int test_pong_must_match_a_sent_ping(void)
{
  ZZ9KRingTest t;
  uint8_t m[32];

  /* An id never sent. */
  full_run(&t, 100U);
  make_pong(m, 77, 5000U);
  zz9k_rt_on_message(&t, m, 6000U);
  if (t.pong_unmatched != 1U || t.rtt[IDLE].n != 6U) return 1;
  if (zz9k_rt_verdict(&t, 0U) == 0) return 2;

  /* A known id echoing another timestamp is not that ping's answer. */
  zz9k_rt_init(&t);
  zz9k_rt_note_ping(&t, 4, 8000U);
  make_pong(m, 4, 7000U);
  zz9k_rt_on_message(&t, m, 9000U);
  if (t.pong_unmatched != 1U || t.rtt[IDLE].n != 0U) return 3;

  /* A second answer to the same ping is a duplicate, not a second sample. */
  zz9k_rt_init(&t);
  zz9k_rt_note_ping(&t, 5, 8000U);
  make_pong(m, 5, 8000U);
  zz9k_rt_on_message(&t, m, 8300U);
  zz9k_rt_on_message(&t, m, 8400U);
  if (t.pong_duplicate != 1U || t.rtt[IDLE].n != 1U) return 4;
  if (zz9k_rt_failures(&t) != 1U) return 5;

  /* An id reused after the slot count wraps replaces the old ping. */
  zz9k_rt_init(&t);
  zz9k_rt_note_ping(&t, 1, 100U);
  zz9k_rt_note_ping(&t, 1U + ZZ9K_RT_PING_SLOTS, 200U);
  make_pong(m, 1, 100U);
  zz9k_rt_on_message(&t, m, 300U);
  if (t.pong_unmatched != 1U) return 6;

  /* An answer that arrives before the ping was sent. */
  zz9k_rt_init(&t);
  zz9k_rt_note_ping(&t, 6, 9000U);
  make_pong(m, 6, 9000U);
  zz9k_rt_on_message(&t, m, 8000U);
  if (t.pong_bad != 1U || t.rtt[IDLE].n != 0U) return 7;
  return 0;
}

static int test_done_reconciles_pong_count(void)
{
  ZZ9KRingTest t;
  uint8_t m[32];

  /* The app says it sent 10 pongs, the host saw 9. */
  full_run_ex(&t, 100U, 100U, 10U);
  if (t.pong_count_bad != 1U) return 1;
  if (zz9k_rt_verdict(&t, 0U) == 0) return 2;

  /* An unmatched PONG still counts as received, so it cannot hide a loss. */
  begin_run(&t);
  ping_round(&t, IDLE, 1U, 3U, 300U);
  make_pong(m, 99, 1U);
  zz9k_rt_on_message(&t, m, 2U);
  make_done(m, 0U, 0U, 4U);
  zz9k_rt_on_message(&t, m, 0);
  if (t.pong_count_bad != 0U || t.pong_unmatched != 1U) return 3;
  return 0;
}

static int test_one_way_excess_ignores_clock_offset(void)
{
  ZZ9KRingTest t;
  uint8_t m[32];
  uint32_t host_delay[4] = { 500U, 520U, 9500U, 510U };
  unsigned i;

  zz9k_rt_init(&t);
  make_info(m, 0, 70000U);
  zz9k_rt_on_message(&t, m, 3000000U);
  for (i = 0; i < 4U; i++) {
    uint32_t app_us = 70000U + (i + 1U) * 1000U;

    make_data(m, i + 1U, app_us, 0);
    /* Host clock runs 2.93 s ahead of the app's; delay varies. */
    zz9k_rt_on_message(&t, m, 3000000U + (i + 1U) * 1000U + host_delay[i]);
  }
  if (t.ow_n != 4U) return 1;
  if (zz9k_rt_ow_excess_max(&t) != 9000U) return 2;
  if (zz9k_rt_ow_excess_avg(&t) != (0U + 20U + 9000U + 10U) / 4U) return 3;
  return 0;
}

static int test_app_clock_wrap_is_handled(void)
{
  ZZ9KRingTest t;
  uint8_t m[32];

  zz9k_rt_init(&t);
  make_info(m, 0, 0xfffffff0U);
  zz9k_rt_on_message(&t, m, 1000U);
  make_data(m, 1, 0x00000010U, 0);
  zz9k_rt_on_message(&t, m, 1032U);
  if (t.ow_min != 0 || t.ow_max != 0) return 1;
  return 0;
}

static int test_verdict_reasons(void)
{
  ZZ9KRingTest t;
  uint8_t m[32];

  zz9k_rt_init(&t);
  if (zz9k_rt_verdict(&t, 0U) == 0) return 1; /* no INFO */
  make_info(m, 0, 0);
  zz9k_rt_on_message(&t, m, 0);
  if (zz9k_rt_verdict(&t, 0U) == 0) return 2; /* no data */

  full_run(&t, 100U);
  if (zz9k_rt_verdict(&t, 0U) != 0) return 3;

  memset(m, 0, sizeof(m));
  zz9k_rt_put(m, 0, 77U);
  zz9k_rt_on_message(&t, m, 0);
  if (t.unknown != 1U || zz9k_rt_verdict(&t, 0U) == 0) return 4;

  /* A second INFO is not accepted. */
  full_run(&t, 100U);
  make_info(m, 4, 4);
  zz9k_rt_on_message(&t, m, 0);
  if (t.unknown != 1U || t.block_offset != 0x00123000U) return 5;
  return 0;
}

/* MAXLAT bounds the idle phase. The flood phase measures queueing behind a
 * full ring and has no limit. */
static int test_latency_limit_applies_to_idle_only(void)
{
  ZZ9KRingTest t;

  /* full_run: idle pings 300 and 400 us, flood pings 12 ms. */
  full_run(&t, 100U);
  if (t.rtt[IDLE].max != 400U || t.rtt[FLOOD].max != 12000U) return 1;
  if (zz9k_rt_verdict(&t, 5000U) != 0) return 2; /* 12 ms flood is fine */
  if (zz9k_rt_verdict(&t, 1000U) != 0) return 3;
  if (zz9k_rt_verdict(&t, 399U) == 0) return 4; /* idle 400 us is over */
  if (zz9k_rt_verdict(&t, 400U) != 0) return 5;

  /* A slow idle ping fails the limit whatever the flood did. */
  begin_run(&t);
  ping_round(&t, IDLE, 1U, 3U, 300U);
  ping_round(&t, IDLE, 4U, 1U, 30000U);
  ctrl_round(&t, 1U, ZZ9K_RT_MODE_FLOOD, 0U);
  add_data(&t, 1U, 100U);
  ping_round(&t, FLOOD, 5U, 3U, 300U);
  ctrl_round(&t, 2U, ZZ9K_RT_MODE_QUIET, 100U);
  {
    uint8_t m[32];

    make_done(m, 100U, 300U, 7U);
    zz9k_rt_on_message(&t, m, 900000U);
  }
  if (zz9k_rt_verdict(&t, 0U) != 0) return 6;
  if (zz9k_rt_verdict(&t, 5000U) == 0) return 7;
  return 0;
}

static int test_missing_coverage_fails(void)
{
  ZZ9KRingTest t;
  uint8_t m[32];

  /* Everything but DONE. */
  begin_run(&t);
  ping_round(&t, IDLE, 1U, 3U, 300U);
  ctrl_round(&t, 1U, ZZ9K_RT_MODE_FLOOD, 0U);
  add_data(&t, 1U, 100U);
  ping_round(&t, FLOOD, 4U, 3U, 300U);
  ctrl_round(&t, 2U, ZZ9K_RT_MODE_QUIET, 100U);
  if (zz9k_rt_failures(&t) != 0U) return 1;
  if (zz9k_rt_verdict(&t, 0U) == 0) return 2;

  /* One counter sample short of the minimum. */
  full_run(&t, ZZ9K_RT_MIN_COUNTER_SAMPLES - 1U);
  if (zz9k_rt_failures(&t) != 0U) return 3;
  if (zz9k_rt_verdict(&t, 0U) == 0) return 4;
  full_run(&t, ZZ9K_RT_MIN_COUNTER_SAMPLES);
  if (zz9k_rt_verdict(&t, 0U) != 0) return 5;

  /* Data received but the counter never read. */
  begin_run(&t);
  ping_round(&t, IDLE, 1U, 3U, 300U);
  ctrl_round(&t, 1U, ZZ9K_RT_MODE_FLOOD, 0U);
  {
    uint32_t seq;

    for (seq = 1; seq <= 200U; seq++) {
      make_data(m, seq, 0, 0);
      zz9k_rt_on_message(&t, m, 0);
    }
  }
  ping_round(&t, FLOOD, 4U, 3U, 300U);
  ctrl_round(&t, 2U, ZZ9K_RT_MODE_QUIET, 200U);
  make_done(m, 200U, 0U, 6U);
  zz9k_rt_on_message(&t, m, 0);
  if (zz9k_rt_verdict(&t, 0U) == 0) return 6;

  /* Idle pings one short of the minimum; MAXLAT cannot rescue it. */
  begin_run(&t);
  ping_round(&t, IDLE, 1U, ZZ9K_RT_MIN_PONGS - 1U, 300U);
  ctrl_round(&t, 1U, ZZ9K_RT_MODE_FLOOD, 0U);
  add_data(&t, 1U, 100U);
  ping_round(&t, FLOOD, 10U, 3U, 300U);
  ctrl_round(&t, 2U, ZZ9K_RT_MODE_QUIET, 100U);
  make_done(m, 100U, 300U, 5U);
  zz9k_rt_on_message(&t, m, 0);
  if (zz9k_rt_failures(&t) != 0U) return 7;
  if (zz9k_rt_verdict(&t, 0U) == 0) return 8;
  if (zz9k_rt_verdict(&t, 100000U) == 0) return 9;

  /* Flood pings one short of the minimum. */
  begin_run(&t);
  ping_round(&t, IDLE, 1U, 3U, 300U);
  ctrl_round(&t, 1U, ZZ9K_RT_MODE_FLOOD, 0U);
  add_data(&t, 1U, 100U);
  ping_round(&t, FLOOD, 10U, ZZ9K_RT_MIN_PONGS - 1U, 300U);
  ctrl_round(&t, 2U, ZZ9K_RT_MODE_QUIET, 100U);
  make_done(m, 100U, 300U, 5U);
  zz9k_rt_on_message(&t, m, 0);
  if (zz9k_rt_failures(&t) != 0U) return 10;
  if (zz9k_rt_verdict(&t, 0U) == 0) return 11;

  /* No pings at all with a latency limit set. */
  clean_run(&t, 100U);
  make_done(m, 100U, 300U, 0U);
  zz9k_rt_on_message(&t, m, 0);
  if (zz9k_rt_verdict(&t, 5000U) == 0) return 12;
  return 0;
}

static int test_mode_changes_are_checked(void)
{
  ZZ9KRingTest t;
  uint8_t m[32];

  /* An ACK for a CTRL that was never sent. */
  full_run(&t, 100U);
  make_ack(m, 55U, ZZ9K_RT_MODE_QUIET, 100U);
  zz9k_rt_on_message(&t, m, 0);
  if (t.ack_unmatched != 1U || zz9k_rt_verdict(&t, 0U) == 0) return 1;

  /* An ACK that answers the id with the wrong mode. */
  begin_run(&t);
  zz9k_rt_note_ctrl(&t, 1U, ZZ9K_RT_MODE_FLOOD);
  make_ack(m, 1U, ZZ9K_RT_MODE_QUIET, 0U);
  zz9k_rt_on_message(&t, m, 0);
  if (t.ack_unmatched != 1U) return 2;

  /* The same ACK twice. */
  begin_run(&t);
  zz9k_rt_note_ctrl(&t, 1U, ZZ9K_RT_MODE_FLOOD);
  make_ack(m, 1U, ZZ9K_RT_MODE_FLOOD, 0U);
  zz9k_rt_on_message(&t, m, 0);
  zz9k_rt_on_message(&t, m, 0);
  if (t.ack_unmatched != 1U || t.ctrls_acked != 1U) return 3;

  /* The ACK says the app sent more DATA than arrived before it. */
  begin_run(&t);
  add_data(&t, 1U, 10U);
  zz9k_rt_note_ctrl(&t, 2U, ZZ9K_RT_MODE_QUIET);
  make_ack(m, 2U, ZZ9K_RT_MODE_QUIET, 12U);
  zz9k_rt_on_message(&t, m, 0);
  if (t.ack_count_bad != 1U) return 4;
  if (zz9k_rt_verdict(&t, 0U) == 0) return 5;

  /* DATA after the app acknowledged QUIET. */
  begin_run(&t);
  add_data(&t, 1U, 10U);
  ctrl_round(&t, 2U, ZZ9K_RT_MODE_QUIET, 10U);
  add_data(&t, 11U, 11U);
  if (t.data_in_quiet != 1U) return 6;
  if (zz9k_rt_verdict(&t, 0U) == 0) return 7;
  /* Flooding again clears it. */
  ctrl_round(&t, 3U, ZZ9K_RT_MODE_FLOOD, 11U);
  add_data(&t, 12U, 12U);
  if (t.data_in_quiet != 1U) return 8;

  /* A CTRL the app never acknowledged fails once DONE arrives. */
  full_run(&t, 100U);
  zz9k_rt_note_ctrl(&t, 3U, ZZ9K_RT_MODE_FLOOD);
  if (zz9k_rt_ctrls_outstanding(&t) != 1U) return 9;
  if (zz9k_rt_failures(&t) != 1U) return 10;
  if (zz9k_rt_verdict(&t, 0U) == 0) return 11;
  return 0;
}

typedef struct FakeRing {
  uint32_t remaining; /* messages left; 0xffffffff never runs dry */
  uint32_t next_seq;
  int fail_after;     /* get() returns -5 after this many; -1 never */
  uint32_t got;
  uint32_t samples;
} FakeRing;

static int fake_get(void *ctx, uint8_t m[32])
{
  FakeRing *r = (FakeRing *)ctx;

  if (r->fail_after >= 0 && r->got >= (uint32_t)r->fail_after) return -5;
  if (r->remaining == 0U) return 1;
  if (r->remaining != 0xffffffffU) r->remaining--;
  make_data(m, r->next_seq, 0, r->next_seq);
  r->next_seq++;
  r->got++;
  return 0;
}

static uint64_t fake_now(void *ctx)
{
  (void)ctx;
  return 0U;
}

static void fake_sample(void *ctx, ZZ9KRingTest *t)
{
  FakeRing *r = (FakeRing *)ctx;

  r->samples++;
  zz9k_rt_on_counter(t, r->next_seq);
}

static int test_drain_is_bounded(void)
{
  ZZ9KRingTest t;
  FakeRing ring;
  ZZ9KRtIo io;
  uint32_t taken = 0U;
  uint8_t m[32];

  zz9k_rt_init(&t);
  make_info(m, 0, 0);
  zz9k_rt_on_message(&t, m, 0);
  memset(&ring, 0, sizeof(ring));
  ring.remaining = 1000000U; /* the producer is far ahead of the reader */
  ring.next_seq = 1U;
  ring.fail_after = -1;
  io.get = fake_get;
  io.now = fake_now;
  io.sample = fake_sample;
  io.ctx = &ring;

  if (zz9k_rt_drain(&t, &io, ZZ9K_RT_DRAIN_BATCH, &taken) != 0) return 1;
  if (taken != ZZ9K_RT_DRAIN_BATCH || ring.got != ZZ9K_RT_DRAIN_BATCH) return 2;
  if (ring.samples != ZZ9K_RT_DRAIN_BATCH || t.messages != taken) return 3;
  /* The next pass carries on where this one stopped. */
  if (zz9k_rt_drain(&t, &io, 10U, &taken) != 0 || taken != 10U) return 4;
  if (t.expected_seq != ZZ9K_RT_DRAIN_BATCH + 10U + 1U) return 5;
  return 0;
}

static int test_drain_stops_when_ring_is_empty_or_fails(void)
{
  ZZ9KRingTest t;
  FakeRing ring;
  ZZ9KRtIo io;
  uint32_t taken = 99U;

  zz9k_rt_init(&t);
  memset(&ring, 0, sizeof(ring));
  ring.remaining = 5U;
  ring.next_seq = 1U;
  ring.fail_after = -1;
  io.get = fake_get;
  io.now = fake_now;
  io.sample = 0;
  io.ctx = &ring;
  if (zz9k_rt_drain(&t, &io, ZZ9K_RT_DRAIN_BATCH, &taken) != 0) return 1;
  if (taken != 5U) return 2;
  if (zz9k_rt_drain(&t, &io, ZZ9K_RT_DRAIN_BATCH, &taken) != 0) return 3;
  if (taken != 0U) return 4;

  memset(&ring, 0, sizeof(ring));
  ring.remaining = 100U;
  ring.next_seq = 1U;
  ring.fail_after = 3;
  zz9k_rt_init(&t);
  if (zz9k_rt_drain(&t, &io, ZZ9K_RT_DRAIN_BATCH, &taken) != -5) return 5;
  if (taken != 3U) return 6;
  return 0;
}

static int test_unanswered_pings_fail(void)
{
  ZZ9KRingTest t;
  uint8_t m[32];

  /* Every ping answered: nothing outstanding, run passes. */
  full_run(&t, 100U);
  if (zz9k_rt_pings_outstanding(&t) != 0U) return 1;
  if (zz9k_rt_verdict(&t, 0U) != 0) return 2;

  /* A tenth ping was sent and never answered, so host-to-app delivery was
   * not shown. */
  full_run(&t, 100U);
  zz9k_rt_note_ping(&t, 10U, 2000000ULL);
  if (zz9k_rt_pings_outstanding(&t) != 1U) return 3;
  if (zz9k_rt_failures(&t) != 1U) return 4;
  if (zz9k_rt_verdict(&t, 0U) == 0) return 5;

  /* An unmatched PONG does not stand in for an unanswered ping. */
  begin_run(&t);
  ping_round(&t, IDLE, 1U, 3U, 300U);
  zz9k_rt_note_ping(&t, 4U, 2000000ULL);
  make_pong(m, 90U, 2000000ULL);
  zz9k_rt_on_message(&t, m, 2000300U);
  make_done(m, 0U, 0U, 4U);
  zz9k_rt_on_message(&t, m, 3000000U);
  if (zz9k_rt_pings_outstanding(&t) != 1U) return 6;
  if (zz9k_rt_verdict(&t, 0U) == 0) return 7;
  return 0;
}

static int test_ticks_to_microseconds(void)
{
  if (zz9k_rt_ticks_to_us(709379U, 709379U) != 1000000U) return 1;
  if (zz9k_rt_ticks_to_us(715909U * 3600ULL, 715909U) != 3600000000ULL) {
    return 2;
  }
  if (zz9k_rt_ticks_to_us(5U, 0U) != 0U) return 3;
  return 0;
}

int main(void)
{
  int rc;

  rc = test_pattern_known_answers();
  if (rc) return 10 + rc;
  rc = test_word_layout_is_little_endian();
  if (rc) return 20 + rc;
  rc = test_full_run_passes();
  if (rc) return 30 + rc;
  rc = test_gap_is_counted_and_fails();
  if (rc) return 40 + rc;
  rc = test_repeat_and_reorder_fail();
  if (rc) return 50 + rc;
  rc = test_wrong_bytes_are_counted();
  if (rc) return 60 + rc;
  rc = test_done_total_exposes_tail_loss();
  if (rc) return 70 + rc;
  rc = test_counter_checks();
  if (rc) return 80 + rc;
  rc = test_ping_round_trips_by_phase();
  if (rc) return 90 + rc;
  rc = test_one_way_excess_ignores_clock_offset();
  if (rc) return 100 + rc;
  rc = test_app_clock_wrap_is_handled();
  if (rc) return 110 + rc;
  rc = test_verdict_reasons();
  if (rc) return 120 + rc;
  rc = test_ticks_to_microseconds();
  if (rc) return 130 + rc;
  rc = test_pong_must_match_a_sent_ping();
  if (rc) return 140 + rc;
  rc = test_done_reconciles_pong_count();
  if (rc) return 150 + rc;
  rc = test_missing_coverage_fails();
  if (rc) return 160 + rc;
  rc = test_drain_is_bounded();
  if (rc) return 170 + rc;
  rc = test_drain_stops_when_ring_is_empty_or_fails();
  if (rc) return 180 + rc;
  rc = test_unanswered_pings_fail();
  if (rc) return 190 + rc;
  rc = test_latency_limit_applies_to_idle_only();
  if (rc) return 200 + rc;
  rc = test_mode_changes_are_checked();
  if (rc) return 210 + rc;
  return 0;
}
