/*
 * Unit checks for the zz9k-qprobe accounting, verdict and argument helpers,
 * and for the entry codec of the wire contract.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "../tools/zz9k-qprobe-run.h"

#include <stdint.h>
#include <string.h>

#define MIN_ENTRIES ZZ9K_QR_MIN_STREAM_ENTRIES

/* A queue that did everything right: `entries` STREAM entries, `pings`
 * answered PINGs, every entry completed, the firmware agrees. */
static ZZ9KQrQueue good_queue(uint32_t index, uint32_t entries,
                              uint32_t pings)
{
  ZZ9KQrQueue q;
  uint32_t status[ZZ9K_QP_STATUS_WORDS];

  memset(&q, 0, sizeof(q));
  q.queue = index;
  q.owner = ZZ9K_QP_OWNER_CORE0;
  q.stream_entries = entries;
  q.pings_sent = pings;
  q.pings_answered = pings;
  q.down_consumed = entries + pings;
  q.up_produced = q.down_consumed + pings;
  memset(status, 0, sizeof(status));
  status[ZZ9K_QP_STATUS_DOWN_PRODUCED] = q.down_consumed;
  status[ZZ9K_QP_STATUS_UP_CHECKED] = q.up_produced;
  status[ZZ9K_QP_STATUS_COMPLETED] = q.down_consumed;
  status[ZZ9K_QP_STATUS_PONGS] = pings;
  zz9k_qr_queue_set_status(&q, status);
  return q;
}

static const char *verdict_of(const ZZ9KQrQueue *queues, uint32_t n,
                              int stream)
{
  ZZ9KQrTally t;
  uint32_t i;

  zz9k_qr_tally_init(&t, stream, MIN_ENTRIES, 1U);
  for (i = 0; i < n; i++) zz9k_qr_queue_done(&t, &queues[i]);
  return zz9k_qr_verdict(&t);
}

static int same(const char *a, const char *b)
{
  if (!a || !b) return a == b;
  return strcmp(a, b) == 0;
}

static int test_stats(void)
{
  ZZ9KQrStats s;

  zz9k_qr_stats_init(&s);
  if (s.n != 0U || zz9k_qr_stats_avg(&s) != 0U) return 1;
  zz9k_qr_stats_add(&s, 30U);
  zz9k_qr_stats_add(&s, 10U);
  zz9k_qr_stats_add(&s, 20U);
  if (s.n != 3U || s.min != 10U || s.max != 30U || s.sum != 60U) return 2;
  if (zz9k_qr_stats_avg(&s) != 20U) return 3;
  /* A first value of zero is a real minimum. */
  zz9k_qr_stats_init(&s);
  zz9k_qr_stats_add(&s, 0U);
  zz9k_qr_stats_add(&s, 5U);
  if (s.min != 0U || s.max != 5U) return 4;
  /* EClock tick of 709379 Hz is 1409.7 ns; a 333 MHz tick is 3 ns. */
  if (zz9k_qr_ticks_to_ns(1U, 709379U) != 1410U) return 5;
  if (zz9k_qr_ticks_to_ns(1000U, 333333333U) != 3000U) return 6;
  if (zz9k_qr_ticks_to_ns(5U, 0U) != 0U) return 7;
  return 0;
}

static int test_pings(void)
{
  ZZ9KQrPings p;
  uint32_t a;
  uint32_t b;
  uint32_t i;

  zz9k_qr_pings_init(&p);
  if (zz9k_qr_pings_match(&p, 1U)) return 1;
  a = zz9k_qr_pings_push(&p);
  b = zz9k_qr_pings_push(&p);
  if (a != 1U || b != 2U) return 2;
  /* Answers come in order: the second id first is wrong. */
  if (zz9k_qr_pings_match(&p, b)) return 3;
  if (!zz9k_qr_pings_match(&p, a)) return 4;
  if (zz9k_qr_pings_match(&p, a)) return 5; /* duplicate */
  if (!zz9k_qr_pings_match(&p, b)) return 6;
  if (p.count != 0U) return 7;
  /* The slots wrap and run out. */
  for (i = 0; i < ZZ9K_QR_PING_SLOTS; i++) {
    if (zz9k_qr_pings_push(&p) == 0U) return 8;
  }
  if (zz9k_qr_pings_push(&p) != 0U) return 9;
  for (i = 0; i < ZZ9K_QR_PING_SLOTS; i++) {
    if (!zz9k_qr_pings_match(&p, 3U + i)) return 10;
  }
  if (zz9k_qr_pings_push(&p) != 3U + ZZ9K_QR_PING_SLOTS) return 11;
  return 0;
}

static int test_clean_run_passes(void)
{
  ZZ9KQrQueue q[2];

  q[0] = good_queue(0U, MIN_ENTRIES, 3U);
  q[1] = good_queue(1U, MIN_ENTRIES + 500U, 3U);
  if (!same(verdict_of(q, 2U, 1), 0)) return 1;
  return 0;
}

static int test_zero_traffic_fails(void)
{
  ZZ9KQrQueue q = good_queue(0U, 0U, 0U);
  ZZ9KQrTally t;

  if (!same(verdict_of(&q, 1U, 1), "low_traffic")) return 1;
  if (!same(verdict_of(&q, 1U, 0), "low_traffic")) return 2;
  /* No queues at all tested nothing either. */
  zz9k_qr_tally_init(&t, 1, MIN_ENTRIES, 1U);
  if (!same(zz9k_qr_verdict(&t), "no_queues")) return 3;
  return 0;
}

static int test_too_little_traffic_fails(void)
{
  ZZ9KQrQueue q[2];

  /* One queue short of the limit fails the part. */
  q[0] = good_queue(0U, MIN_ENTRIES, 2U);
  q[1] = good_queue(1U, MIN_ENTRIES - 1U, 2U);
  if (!same(verdict_of(q, 2U, 1), "low_traffic")) return 1;
  /* The doorbell part has no stream entries, only PINGs. */
  q[0] = good_queue(0U, 0U, 5U);
  q[1] = good_queue(1U, 0U, 5U);
  if (!same(verdict_of(q, 2U, 0), 0)) return 2;
  /* A doorbell queue that never saw a PONG is untested. */
  q[1] = good_queue(1U, 0U, 0U);
  if (!same(verdict_of(q, 2U, 0), "low_traffic")) return 3;
  return 0;
}

static int test_one_error_fails(void)
{
  ZZ9KQrQueue q = good_queue(0U, MIN_ENTRIES, 2U);
  ZZ9KQrTally t;
  uint32_t words[ZZ9K_QP_ENTRY_WORDS];

  memset(words, 0, sizeof(words));
  zz9k_qr_tally_init(&t, 1, MIN_ENTRIES, 1U);
  zz9k_qr_queue_done(&t, &q);
  if (zz9k_qr_verdict(&t)) return 1;
  zz9k_qr_note_error(&t, 0U, 77U, ZZ9K_QP_ERR_TORN, words);
  if (!same(zz9k_qr_verdict(&t), "amiga_errors")) return 2;
  if (t.errors != 1U || t.by_code[ZZ9K_QP_ERR_TORN] != 1U) return 3;
  return 0;
}

static int test_firmware_errors_fail(void)
{
  ZZ9KQrQueue q[2];
  ZZ9KQrTally t;
  uint32_t status[ZZ9K_QP_STATUS_WORDS];

  q[0] = good_queue(0U, MIN_ENTRIES, 2U);
  q[1] = good_queue(1U, MIN_ENTRIES, 2U);
  memset(status, 0, sizeof(status));
  status[ZZ9K_QP_STATUS_DOWN_PRODUCED] = q[1].down_consumed;
  status[ZZ9K_QP_STATUS_UP_CHECKED] = q[1].up_produced;
  status[ZZ9K_QP_STATUS_UP_ERRORS] = 3U;
  status[ZZ9K_QP_STATUS_FIRST_ERROR] = ZZ9K_QP_ERR_STALE;
  status[ZZ9K_QP_STATUS_FIRST_ERROR_SEQ] = 41U;
  zz9k_qr_queue_set_status(&q[1], status);
  zz9k_qr_tally_init(&t, 1, MIN_ENTRIES, 1U);
  zz9k_qr_queue_done(&t, &q[0]);
  zz9k_qr_queue_done(&t, &q[1]);
  if (!same(zz9k_qr_verdict(&t), "fw_errors")) return 1;
  if (t.fw_up_errors != 3U || t.fw_first_error != ZZ9K_QP_ERR_STALE ||
      t.fw_first_error_queue != 1U || t.fw_first_error_seq != 41U)
    return 2;
  /* A queue whose STATUS never arrived cannot pass. */
  q[1].have_status = 0U;
  zz9k_qr_tally_init(&t, 1, MIN_ENTRIES, 1U);
  zz9k_qr_queue_done(&t, &q[1]);
  if (!same(zz9k_qr_verdict(&t), "no_status")) return 3;
  return 0;
}

static int test_missing_or_wrong_pong_fails(void)
{
  ZZ9KQrQueue q = good_queue(0U, MIN_ENTRIES, 3U);

  q.pings_answered = 2U;
  if (!same(verdict_of(&q, 1U, 1), "pong_missing")) return 1;
  q = good_queue(0U, MIN_ENTRIES, 3U);
  q.pong_bad = 1U;
  if (!same(verdict_of(&q, 1U, 1), "pong_wrong")) return 2;
  return 0;
}

static int test_lost_entries_fail(void)
{
  ZZ9KQrQueue q = good_queue(0U, MIN_ENTRIES, 2U);

  /* The firmware produced an entry the Amiga never took. */
  q.fw_down_produced++;
  if (!same(verdict_of(&q, 1U, 1), "down_lost")) return 1;
  /* The firmware did not consume every up entry. */
  q = good_queue(0U, MIN_ENTRIES, 2U);
  q.fw_up_checked--;
  if (!same(verdict_of(&q, 1U, 1), "up_unconsumed")) return 2;
  return 0;
}

static int test_setup_and_break_and_calls(void)
{
  ZZ9KQrQueue q = good_queue(0U, MIN_ENTRIES, 2U);
  ZZ9KQrTally t;

  zz9k_qr_tally_init(&t, 1, MIN_ENTRIES, 1U);
  zz9k_qr_queue_done(&t, &q);
  t.call_failures = 1U;
  if (!same(zz9k_qr_verdict(&t), "call_failed")) return 1;
  t.interrupted = 1U;
  if (!same(zz9k_qr_verdict(&t), "break")) return 2;
  t.setup_failed = 1U;
  if (!same(zz9k_qr_verdict(&t), "setup")) return 3;
  return 0;
}

static int test_error_log(void)
{
  ZZ9KQrTally t;
  uint32_t words[ZZ9K_QP_ENTRY_WORDS];
  uint32_t i;

  zz9k_qr_tally_init(&t, 1, MIN_ENTRIES, 1U);
  for (i = 0; i < ZZ9K_QP_ENTRY_WORDS; i++) words[i] = 0x1000U + i;
  for (i = 0; i < ZZ9K_QR_LOG_ERRORS + 3U; i++)
    zz9k_qr_note_error(&t, i, 100U + i, ZZ9K_QP_ERR_STALE, words);
  zz9k_qr_note_error(&t, 9U, 1U, ZZ9K_QP_ERR_INDEX, 0);
  zz9k_qr_note_error(&t, 9U, 2U, 99U, 0); /* unknown code */
  if (t.errors != ZZ9K_QR_LOG_ERRORS + 5U) return 1;
  if (t.logged != ZZ9K_QR_LOG_ERRORS) return 2;
  if (t.log[0].queue != 0U || t.log[0].seq != 100U ||
      t.log[0].code != ZZ9K_QP_ERR_STALE || t.log[0].words[15] != 0x100fU)
    return 3;
  if (t.log[ZZ9K_QR_LOG_ERRORS - 1U].seq !=
      100U + ZZ9K_QR_LOG_ERRORS - 1U)
    return 4;
  if (t.by_code[ZZ9K_QP_ERR_STALE] != ZZ9K_QR_LOG_ERRORS + 3U ||
      t.by_code[ZZ9K_QP_ERR_INDEX] != 1U || t.by_code[0] != 1U)
    return 5;
  /* An error that is not about an entry logs zero words. */
  zz9k_qr_tally_init(&t, 1, MIN_ENTRIES, 1U);
  zz9k_qr_note_error(&t, 2U, 5U, ZZ9K_QP_ERR_INDEX, 0);
  for (i = 0; i < ZZ9K_QP_ENTRY_WORDS; i++)
    if (t.log[0].words[i] != 0U) return 6;
  return 0;
}

static int test_owner_helpers(void)
{
  if (zz9k_qr_queue_owner(ZZ9K_QR_OWNER_MIX, 0U) != ZZ9K_QP_OWNER_CORE0)
    return 1;
  if (zz9k_qr_queue_owner(ZZ9K_QR_OWNER_MIX, 1U) != ZZ9K_QP_OWNER_CORE1)
    return 2;
  if (zz9k_qr_queue_owner(ZZ9K_QP_OWNER_CORE1, 4U) != ZZ9K_QP_OWNER_CORE1)
    return 3;
  if (zz9k_qr_origin_mask(ZZ9K_QP_OWNER_CORE0) != 1U) return 4;
  if (zz9k_qr_origin_mask(ZZ9K_QP_OWNER_CORE1) != 2U) return 5;
  if (zz9k_qr_origin_mask(ZZ9K_QP_OWNER_DOORBELL) != 1U) return 6;
  return 0;
}

static int test_argument_parsing(void)
{
  uint32_t value = 0U;
  uint32_t counts[ZZ9K_QR_MAX_COUNTS];
  const char *v;

  v = zz9k_qr_arg_value("queues=4", "QUEUES");
  if (!v || strcmp(v, "4") != 0) return 1;
  if (zz9k_qr_arg_value("SECONDS=4", "QUEUES")) return 2;
  if (zz9k_qr_arg_value("QUEUES", "QUEUES")) return 3;
  if (zz9k_qr_arg_value("QUEUESX=1", "QUEUES")) return 4;
  if (!zz9k_qr_parse_owner("Core1", &value) || value != ZZ9K_QP_OWNER_CORE1)
    return 5;
  if (!zz9k_qr_parse_owner("MIX", &value) || value != ZZ9K_QR_OWNER_MIX)
    return 6;
  if (zz9k_qr_parse_owner("doorbell", &value)) return 7;
  if (!zz9k_qr_parse_u32("30", 1U, 3600U, &value) || value != 30U) return 8;
  if (zz9k_qr_parse_u32("0", 1U, 3600U, &value)) return 9;
  if (zz9k_qr_parse_u32("3601", 1U, 3600U, &value)) return 10;
  if (zz9k_qr_parse_u32("12x", 1U, 3600U, &value)) return 11;
  if (zz9k_qr_parse_u32("", 1U, 3600U, &value)) return 12;
  if (zz9k_qr_parse_u32("99999999999999999999", 1U, 3600U, &value))
    return 13;
  if (zz9k_qr_parse_counts("1,8,32", counts, ZZ9K_QR_MAX_COUNTS) != 3U ||
      counts[0] != 1U || counts[1] != 8U || counts[2] != 32U)
    return 14;
  if (zz9k_qr_parse_counts("5", counts, ZZ9K_QR_MAX_COUNTS) != 1U ||
      counts[0] != 5U)
    return 15;
  if (zz9k_qr_parse_counts("1,,8", counts, ZZ9K_QR_MAX_COUNTS) != 0U)
    return 16;
  if (zz9k_qr_parse_counts("1,33", counts, ZZ9K_QR_MAX_COUNTS) != 0U)
    return 17;
  if (zz9k_qr_parse_counts("1,0", counts, ZZ9K_QR_MAX_COUNTS) != 0U)
    return 18;
  if (zz9k_qr_parse_counts("1,2,", counts, ZZ9K_QR_MAX_COUNTS) != 0U)
    return 19;
  if (zz9k_qr_parse_counts("1,2,3", counts, 2U) != 0U) return 20;
  return 0;
}

/* The entry codec of the contract: what the Amiga and firmware both rely
 * on to see index-before-data and torn entries. */
static int test_entry_codec(void)
{
  uint32_t entry[ZZ9K_QP_ENTRY_WORDS];
  ZZ9KQpEntry e;
  uint32_t mask = 1U << ZZ9K_QP_ORIGIN_CORE1;

  zz9k_qp_entry_write(entry, 65U, ZZ9K_QP_KIND_STREAM, 3U,
                      ZZ9K_QP_ORIGIN_CORE1, 0x1234U, 0U);
  if (zz9k_qp_entry_check(entry, 65U, 3U, mask, &e) != ZZ9K_QP_OK) return 1;
  if (e.seq != 65U || e.kind != ZZ9K_QP_KIND_STREAM ||
      e.origin != ZZ9K_QP_ORIGIN_CORE1 || e.stamp != 0x1234U || e.arg != 0U)
    return 2;
  /* The slot of the previous lap: the head ran ahead of the data. */
  if (zz9k_qp_entry_check(entry, 129U, 3U, mask, 0) != ZZ9K_QP_ERR_STALE)
    return 3;
  if (zz9k_qp_entry_check(entry, 65U, 4U, mask, 0) != ZZ9K_QP_ERR_PATTERN)
    return 4;
  if (zz9k_qp_entry_check(entry, 65U, 3U, 1U << ZZ9K_QP_ORIGIN_CORE0, 0) !=
      ZZ9K_QP_ERR_QUEUE)
    return 5;
  entry[15] ^= 1U;
  if (zz9k_qp_entry_check(entry, 65U, 3U, mask, 0) != ZZ9K_QP_ERR_TORN)
    return 6;
  entry[15] ^= 1U;
  entry[9] ^= zz9k_qp_swap(0x10U);
  if (zz9k_qp_entry_check(entry, 65U, 3U, mask, 0) != ZZ9K_QP_ERR_PATTERN)
    return 7;
  if (zz9k_qp_available(70U, 10U) != 60) return 8;
  if (zz9k_qp_available(74U, 10U) != 64) return 9;
  if (zz9k_qp_available(75U, 10U) != -1) return 10;
  /* The indices are free-running and wrap. */
  if (zz9k_qp_available(3U, 0xfffffffdU) != 6) return 11;
  return 0;
}

int main(void)
{
  int rc;

  rc = test_stats();
  if (rc) return 10 + rc;
  rc = test_pings();
  if (rc) return 20 + rc;
  rc = test_clean_run_passes();
  if (rc) return 30 + rc;
  rc = test_zero_traffic_fails();
  if (rc) return 40 + rc;
  rc = test_too_little_traffic_fails();
  if (rc) return 50 + rc;
  rc = test_one_error_fails();
  if (rc) return 60 + rc;
  rc = test_firmware_errors_fail();
  if (rc) return 70 + rc;
  rc = test_missing_or_wrong_pong_fails();
  if (rc) return 80 + rc;
  rc = test_lost_entries_fail();
  if (rc) return 90 + rc;
  rc = test_setup_and_break_and_calls();
  if (rc) return 100 + rc;
  rc = test_error_log();
  if (rc) return 110 + rc;
  rc = test_owner_helpers();
  if (rc) return 120 + rc;
  rc = test_argument_parsing();
  if (rc) return 130 + rc;
  rc = test_entry_codec();
  if (rc) return 140 + rc;
  return 0;
}
