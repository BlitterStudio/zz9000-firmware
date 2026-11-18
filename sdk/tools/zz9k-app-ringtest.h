/*
 * Pure logic of zz9k-app-ringtest: the message layout shared with
 * sdk/zzapp/templates/ringflood.c, the integrity and latency accounting, and
 * the verdict. No AmigaOS calls, so a host test can drive it.
 *
 * Messages are 32 bytes, eight little-endian 32-bit words:
 *
 *   INFO  (app to host, first)  1, version, counter block offset (or
 *                               ZZ9K_RT_NO_COUNTER), block bytes, app time us
 *   DATA  (app to host)         2, seq (from 1), app time us, counter,
 *                               pattern words 0..3 of seq
 *   DONE  (app to host, last)   3, data messages sent, app time us, final
 *                               counter, ring-full retries, pongs sent
 *   PING  (host to app)         5, id, host time us low, host time us high
 *   PONG  (app to host)         6, id, host time us low, host time us high,
 *                               app time us, counter
 *   CTRL  (host to app)         7, id, mode (ZZ9K_RT_MODE_QUIET or _FLOOD)
 *   ACK   (app to host)         8, id, mode, data messages sent so far,
 *                               counter
 *
 * The app starts QUIET: it answers PINGs and sends no DATA. After CTRL FLOOD
 * it sends DATA as fast as the ring takes it; after CTRL QUIET it stops. The
 * ACK follows every DATA message the app sent before the change, so the
 * count in it must equal the DATA messages received so far, and no DATA may
 * follow an ACK for QUIET.
 *
 * Ping round trips are kept apart by the phase the ping was sent in. IDLE
 * pings go out while the app is not flooding, so they time the transport.
 * FLOOD pings queue behind a full ring of DATA and the 68k's drain; that
 * figure is reported but has no limit.
 *
 * The counter block word is stored big-endian, so the 68k reads it as a
 * native long.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ9K_APP_RINGTEST_H
#define ZZ9K_APP_RINGTEST_H

#include <stdint.h>
#include <string.h>

#define ZZ9K_RT_VERSION 1U
#define ZZ9K_RT_KIND_INFO 1U
#define ZZ9K_RT_KIND_DATA 2U
#define ZZ9K_RT_KIND_DONE 3U
#define ZZ9K_RT_KIND_PING 5U
#define ZZ9K_RT_KIND_PONG 6U
#define ZZ9K_RT_KIND_CTRL 7U
#define ZZ9K_RT_KIND_ACK 8U
#define ZZ9K_RT_MODE_QUIET 0U
#define ZZ9K_RT_MODE_FLOOD 1U
#define ZZ9K_RT_PHASE_IDLE 0U
#define ZZ9K_RT_PHASE_FLOOD 1U
#define ZZ9K_RT_NO_COUNTER 0xffffffffU
#define ZZ9K_RT_MESSAGE_BYTES 32U

/* A passing run must show at least this much. A run that never got DONE,
 * never read the counter or never saw a ping answered in each phase has not
 * tested what it claims to. */
#define ZZ9K_RT_MIN_COUNTER_SAMPLES 100U
#define ZZ9K_RT_MIN_PONGS 3U
/* PINGs the host may have outstanding, matched by id modulo this. */
#define ZZ9K_RT_PING_SLOTS 128U
/* CTRL messages the host may have outstanding. */
#define ZZ9K_RT_CTRL_SLOTS 8U
/* Messages taken from the ring per pass, so one pass cannot outlast the
 * ping, status and break checks around it. */
#define ZZ9K_RT_DRAIN_BATCH 64U

static inline uint32_t zz9k_rt_get(const uint8_t message[32], unsigned word)
{
  const uint8_t *p = message + word * 4U;

  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static inline void zz9k_rt_put(uint8_t message[32], unsigned word,
                               uint32_t value)
{
  uint8_t *p = message + word * 4U;

  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16);
  p[3] = (uint8_t)(value >> 24);
}

/* The pattern words are the murmur3 finalizer of seq * 4 + index. */
static inline uint32_t zz9k_rt_pattern(uint32_t seq, unsigned index)
{
  uint32_t h = seq * 4U + index;

  h ^= h >> 16;
  h *= 0x85ebca6bU;
  h ^= h >> 13;
  h *= 0xc2b2ae35U;
  h ^= h >> 16;
  return h;
}

static inline void zz9k_rt_build_ping(uint8_t message[32], uint32_t id,
                                      uint64_t host_us)
{
  memset(message, 0, ZZ9K_RT_MESSAGE_BYTES);
  zz9k_rt_put(message, 0, ZZ9K_RT_KIND_PING);
  zz9k_rt_put(message, 1, id);
  zz9k_rt_put(message, 2, (uint32_t)host_us);
  zz9k_rt_put(message, 3, (uint32_t)(host_us >> 32));
}

static inline void zz9k_rt_build_ctrl(uint8_t message[32], uint32_t id,
                                      uint32_t mode)
{
  memset(message, 0, ZZ9K_RT_MESSAGE_BYTES);
  zz9k_rt_put(message, 0, ZZ9K_RT_KIND_CTRL);
  zz9k_rt_put(message, 1, id);
  zz9k_rt_put(message, 2, mode);
}

/* Round trips of the pings sent in one phase, microseconds. */
typedef struct ZZ9KRtRtt {
  uint32_t n;
  uint64_t sum;
  uint64_t min;
  uint64_t max;
} ZZ9KRtRtt;

typedef struct ZZ9KRingTest {
  /* INFO */
  int have_info;
  uint32_t block_offset;
  uint32_t block_bytes;
  uint32_t info_app_us;
  uint64_t info_host_us;

  /* DATA integrity */
  uint32_t expected_seq;
  uint32_t messages;
  uint32_t gaps;
  uint32_t lost;
  uint32_t reordered;
  uint32_t bad_messages;
  uint32_t bad_bytes;
  uint32_t unknown;

  /* DONE */
  int have_done;
  uint32_t done_total;
  uint32_t done_counter;
  uint32_t done_busy;
  uint32_t done_pongs;
  uint32_t tail_lost;
  uint32_t bad_done;

  /* Counter word */
  uint32_t floor;
  int counter_have;
  uint32_t counter_last;
  uint32_t counter_hi;
  uint32_t counter_samples;
  uint32_t counter_nonmono;
  uint32_t counter_stale;
  uint32_t counter_ahead;

  /* Mode changes. quiet is set once an ACK for QUIET has arrived. */
  int quiet;
  uint32_t ctrls_sent;
  uint32_t ctrls_acked;
  uint32_t ack_unmatched;
  uint32_t ack_count_bad;
  uint32_t data_in_quiet;
  uint32_t ctrl_id[ZZ9K_RT_CTRL_SLOTS];
  uint32_t ctrl_mode[ZZ9K_RT_CTRL_SLOTS];
  uint8_t ctrl_state[ZZ9K_RT_CTRL_SLOTS]; /* 0 free, 1 sent, 2 acked */

  /* Pings. The caller sets phase before sending one. */
  uint32_t phase;
  ZZ9KRtRtt rtt[2]; /* indexed by ZZ9K_RT_PHASE_* */
  uint32_t pong_bad;
  uint32_t pings_sent;
  uint32_t pings_answered;
  uint32_t pongs_received;
  uint32_t pong_unmatched;
  uint32_t pong_duplicate;
  uint32_t pong_count_bad;
  uint32_t ping_id[ZZ9K_RT_PING_SLOTS];
  uint64_t ping_stamp[ZZ9K_RT_PING_SLOTS];
  uint8_t ping_phase[ZZ9K_RT_PING_SLOTS];
  uint8_t ping_state[ZZ9K_RT_PING_SLOTS]; /* 0 free, 1 sent, 2 answered */

  /* One-way delay of DATA messages, raw: host receipt minus app stamp,
   * both measured from INFO. The clocks differ by an unknown constant, so
   * only the spread above the minimum means anything. */
  uint32_t ow_n;
  int64_t ow_min;
  int64_t ow_max;
  int64_t ow_sum;
} ZZ9KRingTest;

static inline void zz9k_rt_init(ZZ9KRingTest *t)
{
  memset(t, 0, sizeof(*t));
  t->expected_seq = 1U;
  t->block_offset = ZZ9K_RT_NO_COUNTER;
  t->rtt[0].min = UINT64_MAX;
  t->rtt[1].min = UINT64_MAX;
}

static inline uint32_t zz9k_rt_kind(const uint8_t message[32])
{
  return zz9k_rt_get(message, 0);
}

static inline void zz9k_rt_on_info(ZZ9KRingTest *t, const uint8_t m[32],
                                   uint64_t host_us)
{
  if (zz9k_rt_get(m, 1) != ZZ9K_RT_VERSION || t->have_info) {
    t->unknown++;
    return;
  }
  t->have_info = 1;
  t->block_offset = zz9k_rt_get(m, 2);
  t->block_bytes = zz9k_rt_get(m, 3);
  t->info_app_us = zz9k_rt_get(m, 4);
  t->info_host_us = host_us;
}

/* Checks the pattern and the sequence, and records the one-way delay and
 * the counter value the app wrote before sending. */
static inline void zz9k_rt_on_data(ZZ9KRingTest *t, const uint8_t m[32],
                                   uint64_t host_us)
{
  uint32_t seq = zz9k_rt_get(m, 1);
  uint32_t bad = 0;
  unsigned i;
  unsigned b;

  for (i = 0; i < 4U; i++) {
    uint32_t want = zz9k_rt_pattern(seq, i);
    uint32_t got = zz9k_rt_get(m, 4U + i);

    for (b = 0; b < 4U; b++) {
      if (((want ^ got) >> (b * 8U)) & 0xffU) bad++;
    }
  }
  if (bad) {
    t->bad_messages++;
    t->bad_bytes += bad;
    return;
  }
  if (t->quiet) t->data_in_quiet++;
  if (seq == t->expected_seq) {
    t->expected_seq++;
  } else if (seq > t->expected_seq) {
    t->gaps++;
    t->lost += seq - t->expected_seq;
    t->expected_seq = seq + 1U;
  } else {
    t->reordered++;
    return;
  }
  t->messages++;
  t->floor = zz9k_rt_get(m, 3);
  if (t->have_info) {
    int64_t rel_host = (int64_t)(host_us - t->info_host_us);
    int64_t rel_app = (int64_t)(uint32_t)(zz9k_rt_get(m, 2) - t->info_app_us);
    int64_t raw = rel_host - rel_app;

    if (t->ow_n == 0U || raw < t->ow_min) t->ow_min = raw;
    if (t->ow_n == 0U || raw > t->ow_max) t->ow_max = raw;
    t->ow_sum += raw;
    t->ow_n++;
  }
}

/* Records a PING the host sent, in the current phase, so its PONG can be
 * matched. Ids start at 1 and the caller records one only after the send
 * succeeded. */
static inline void zz9k_rt_note_ping(ZZ9KRingTest *t, uint32_t id,
                                     uint64_t host_us)
{
  unsigned slot = id % ZZ9K_RT_PING_SLOTS;

  t->ping_id[slot] = id;
  t->ping_stamp[slot] = host_us;
  t->ping_phase[slot] = (uint8_t)(t->phase == ZZ9K_RT_PHASE_FLOOD ?
                                  ZZ9K_RT_PHASE_FLOOD : ZZ9K_RT_PHASE_IDLE);
  t->ping_state[slot] = 1U;
  t->pings_sent++;
}

/* A PONG counts only if it answers a PING that was sent, echoes that PING's
 * timestamp, and has not been answered before. Its round trip goes to the
 * phase the ping was sent in. */
static inline void zz9k_rt_on_pong(ZZ9KRingTest *t, const uint8_t m[32],
                                   uint64_t host_us)
{
  uint32_t id = zz9k_rt_get(m, 1);
  uint64_t stamp = (uint64_t)zz9k_rt_get(m, 2) |
                   ((uint64_t)zz9k_rt_get(m, 3) << 32);
  unsigned slot = id % ZZ9K_RT_PING_SLOTS;
  ZZ9KRtRtt *r;
  uint64_t rtt;

  t->pongs_received++;
  if (t->ping_state[slot] == 0U || t->ping_id[slot] != id ||
      t->ping_stamp[slot] != stamp) {
    t->pong_unmatched++;
    return;
  }
  if (t->ping_state[slot] == 2U) {
    t->pong_duplicate++;
    return;
  }
  t->ping_state[slot] = 2U;
  t->pings_answered++;
  if (host_us < stamp) {
    t->pong_bad++;
    return;
  }
  rtt = host_us - stamp;
  r = &t->rtt[t->ping_phase[slot]];
  if (rtt < r->min) r->min = rtt;
  if (rtt > r->max) r->max = rtt;
  r->sum += rtt;
  r->n++;
}

/* Records a CTRL the host sent. */
static inline void zz9k_rt_note_ctrl(ZZ9KRingTest *t, uint32_t id,
                                     uint32_t mode)
{
  unsigned slot = id % ZZ9K_RT_CTRL_SLOTS;

  t->ctrl_id[slot] = id;
  t->ctrl_mode[slot] = mode;
  t->ctrl_state[slot] = 1U;
  t->ctrls_sent++;
}

/* An ACK must answer a CTRL that was sent, carry its mode, and report the
 * number of DATA messages the host has seen by this point in the ring. */
static inline void zz9k_rt_on_ack(ZZ9KRingTest *t, const uint8_t m[32])
{
  uint32_t id = zz9k_rt_get(m, 1);
  uint32_t mode = zz9k_rt_get(m, 2);
  unsigned slot = id % ZZ9K_RT_CTRL_SLOTS;

  if (t->ctrl_state[slot] != 1U || t->ctrl_id[slot] != id ||
      t->ctrl_mode[slot] != mode) {
    t->ack_unmatched++;
    return;
  }
  t->ctrl_state[slot] = 2U;
  t->ctrls_acked++;
  if (zz9k_rt_get(m, 3) != t->expected_seq - 1U) t->ack_count_bad++;
  t->quiet = mode == ZZ9K_RT_MODE_QUIET;
}

static inline uint32_t zz9k_rt_ctrls_outstanding(const ZZ9KRingTest *t)
{
  return t->ctrls_sent - t->ctrls_acked;
}

/* DONE: the totals must agree with what arrived, PONGs included. */
static inline void zz9k_rt_on_done(ZZ9KRingTest *t, const uint8_t m[32])
{
  uint32_t seen = t->expected_seq - 1U;

  if (t->have_done) {
    t->unknown++;
    return;
  }
  t->have_done = 1;
  t->done_total = zz9k_rt_get(m, 1);
  t->done_counter = zz9k_rt_get(m, 3);
  t->done_busy = zz9k_rt_get(m, 4);
  t->done_pongs = zz9k_rt_get(m, 5);
  if (t->done_total > seen) {
    t->tail_lost = t->done_total - seen;
  } else if (t->done_total < seen) {
    t->bad_done++;
  }
  if (t->done_pongs != t->pongs_received) t->pong_count_bad++;
  if (t->counter_have && (int32_t)(t->counter_hi - t->done_counter) > 0)
    t->counter_ahead++;
}

/* Routes one app-to-host message. */
static inline void zz9k_rt_on_message(ZZ9KRingTest *t, const uint8_t m[32],
                                      uint64_t host_us)
{
  switch (zz9k_rt_kind(m)) {
  case ZZ9K_RT_KIND_INFO: zz9k_rt_on_info(t, m, host_us); break;
  case ZZ9K_RT_KIND_DATA: zz9k_rt_on_data(t, m, host_us); break;
  case ZZ9K_RT_KIND_PONG: zz9k_rt_on_pong(t, m, host_us); break;
  case ZZ9K_RT_KIND_ACK: zz9k_rt_on_ack(t, m); break;
  case ZZ9K_RT_KIND_DONE: zz9k_rt_on_done(t, m); break;
  default: t->unknown++; break;
  }
}

/* Where drain() gets its messages, the clock, and the counter read. */
typedef struct ZZ9KRtIo {
  /* 0: a message was stored; 1: the ring is empty; negative: an error. */
  int (*get)(void *ctx, uint8_t message[32]);
  uint64_t (*now)(void *ctx);
  /* Called after each DATA message: read the counter and feed it to
   * zz9k_rt_on_counter(). May be NULL when sampling is off. */
  void (*sample)(void *ctx, ZZ9KRingTest *t);
  void *ctx;
} ZZ9KRtIo;

/* Takes up to max messages and stops early when the ring is empty. A ring
 * that refills as fast as it is read still returns after max, so the caller
 * gets back to its timeout, ping, status and break checks. Returns 0, or the
 * negative error get() reported. */
static inline int zz9k_rt_drain(ZZ9KRingTest *t, const ZZ9KRtIo *io,
                                uint32_t max, uint32_t *taken)
{
  uint8_t message[32];
  uint32_t n = 0;
  int rc = 0;

  while (n < max) {
    rc = io->get(io->ctx, message);
    if (rc != 0) break;
    zz9k_rt_on_message(t, message, io->now(io->ctx));
    n++;
    if (io->sample && zz9k_rt_kind(message) == ZZ9K_RT_KIND_DATA)
      io->sample(io->ctx, t);
  }
  if (taken) *taken = n;
  return rc < 0 ? rc : 0;
}

/* One read of the counter word. It must not run backwards, and after a
 * message it must be at least the value that message carried. Differences
 * are taken as signed 32-bit, so a wrap is not an error. */
static inline void zz9k_rt_on_counter(ZZ9KRingTest *t, uint32_t value)
{
  t->counter_samples++;
  if (t->counter_have) {
    if ((int32_t)(value - t->counter_last) < 0) t->counter_nonmono++;
    if ((int32_t)(value - t->counter_hi) > 0) t->counter_hi = value;
  } else {
    t->counter_hi = value;
  }
  if (t->messages && (int32_t)(value - t->floor) < 0) t->counter_stale++;
  t->counter_last = value;
  t->counter_have = 1;
}

/* PINGs sent that no matching PONG has answered. */
static inline uint32_t zz9k_rt_pings_outstanding(const ZZ9KRingTest *t)
{
  return t->pings_sent - t->pings_answered;
}

/* Counts every error. Unanswered PINGs and CTRLs count once DONE has
 * arrived: the app answers within one loop pass, so after DONE (and the
 * caller's grace for replies in flight) an outstanding one never got there. */
static inline uint32_t zz9k_rt_failures(const ZZ9KRingTest *t)
{
  return t->lost + t->tail_lost + t->reordered + t->bad_messages +
         t->unknown + t->bad_done + t->counter_nonmono + t->counter_stale +
         t->counter_ahead + t->pong_bad + t->pong_unmatched +
         t->pong_duplicate + t->pong_count_bad + t->ack_unmatched +
         t->ack_count_bad + t->data_in_quiet +
         (t->have_done ? zz9k_rt_pings_outstanding(t) +
                         zz9k_rt_ctrls_outstanding(t) : 0U);
}

/* Why the run fails, or NULL when it passes. Errors come first, then missing
 * coverage: no DONE, too few counter samples, too few answered PINGs in
 * either phase. max_rtt_us limits the IDLE phase round trip only; zero
 * leaves it unchecked. The FLOOD phase has no limit: it measures queueing
 * behind a full ring. */
static inline const char *zz9k_rt_verdict(const ZZ9KRingTest *t,
                                          uint64_t max_rtt_us)
{
  if (!t->have_info) return "no INFO message from the app";
  if (t->messages == 0U) return "no data messages";
  if (t->lost || t->tail_lost) return "messages lost";
  if (t->reordered) return "messages out of order";
  if (t->bad_messages) return "wrong bytes in a message";
  if (t->unknown || t->bad_done) return "unexpected message";
  if (t->ack_unmatched || t->ack_count_bad)
    return "ACK for a CTRL that was not sent, or with the wrong count";
  if (t->data_in_quiet) return "data after the app acknowledged QUIET";
  if (t->counter_nonmono) return "counter ran backwards";
  if (t->counter_stale) return "counter read older than a delivered message";
  if (t->counter_ahead) return "counter read ahead of the app";
  if (t->pong_bad) return "ping answered before it was sent";
  if (t->pong_unmatched || t->pong_duplicate)
    return "PONG for a ping that was not sent, or answered twice";
  if (t->pong_count_bad) return "PONG count differs from the app's DONE";
  if (!t->have_done) return "no DONE message from the app";
  if (zz9k_rt_pings_outstanding(t)) return "pings sent were never answered";
  if (zz9k_rt_ctrls_outstanding(t)) return "mode change was never acknowledged";
  if (t->counter_samples < ZZ9K_RT_MIN_COUNTER_SAMPLES)
    return "too few counter samples";
  if (t->rtt[ZZ9K_RT_PHASE_IDLE].n < ZZ9K_RT_MIN_PONGS)
    return "too few idle ping round trips";
  if (t->rtt[ZZ9K_RT_PHASE_FLOOD].n < ZZ9K_RT_MIN_PONGS)
    return "too few ping round trips during the flood";
  if (max_rtt_us && t->rtt[ZZ9K_RT_PHASE_IDLE].max > max_rtt_us)
    return "idle ping round trip over the limit";
  return 0;
}

static inline uint64_t zz9k_rt_rtt_avg(const ZZ9KRtRtt *r)
{
  return r->n ? r->sum / r->n : 0U;
}

/* One-way delay above the best message seen, in microseconds. */
static inline uint64_t zz9k_rt_ow_excess_max(const ZZ9KRingTest *t)
{
  return t->ow_n ? (uint64_t)(t->ow_max - t->ow_min) : 0U;
}

static inline uint64_t zz9k_rt_ow_excess_avg(const ZZ9KRingTest *t)
{
  return t->ow_n ? (uint64_t)((t->ow_sum - t->ow_min * (int64_t)t->ow_n) /
                              (int64_t)t->ow_n)
                 : 0U;
}

/* EClock ticks to microseconds. */
static inline uint64_t zz9k_rt_ticks_to_us(uint64_t ticks, uint32_t hz)
{
  return hz ? ticks * 1000000U / hz : 0U;
}

#endif /* ZZ9K_APP_RINGTEST_H */
