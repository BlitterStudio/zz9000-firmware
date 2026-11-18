/*
 * Pure logic of zz9k-qprobe: latency statistics, the ping id queue, the
 * error log, the per-part tally, the verdict and the argument helpers. No
 * AmigaOS calls, so a host test can drive it. The wire contract is in
 * zz9k-qprobe.h.
 *
 * A temporary diagnostic for the AAR cutover, removed with the probe.
 *
 * A part is one run of the probe: a stream run on some queues, or the
 * doorbell run. Each queue ends with one ZZ9KQrQueue result, which the
 * tally adds up. The verdict then names the first rule that failed, or
 * returns NULL for a pass. A run that tested nothing never passes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ9K_QPROBE_RUN_H
#define ZZ9K_QPROBE_RUN_H

#include <stdint.h>
#include <string.h>

#include "zz9k-qprobe.h"

/* Down entries a stream queue must see before a run may pass. */
#define ZZ9K_QR_MIN_STREAM_ENTRIES 1000U
/* Errors kept with their raw words; all are counted. */
#define ZZ9K_QR_LOG_ERRORS 4U
/* PINGs one queue may have outstanding. */
#define ZZ9K_QR_PING_SLOTS 16U
/* A stream run stops early once it has seen this many Amiga-side errors. */
#define ZZ9K_QR_ABORT_ERRORS 100U
#define ZZ9K_QR_MAX_COUNTS 8U

/* Stream owner choice on the command line. MIX alternates core 0 and 1. */
#define ZZ9K_QR_OWNER_MIX 3U

/* min/avg/max of one latency, in the ticks of whatever clock the caller
 * used. Convert with zz9k_qr_ticks_to_ns. */
typedef struct ZZ9KQrStats {
  uint32_t n;
  uint64_t min;
  uint64_t max;
  uint64_t sum;
} ZZ9KQrStats;

static inline void zz9k_qr_stats_init(ZZ9KQrStats *s)
{
  memset(s, 0, sizeof(*s));
}

static inline void zz9k_qr_stats_add(ZZ9KQrStats *s, uint64_t value)
{
  if (s->n == 0U || value < s->min) s->min = value;
  if (value > s->max) s->max = value;
  s->sum += value;
  s->n++;
}

static inline uint64_t zz9k_qr_stats_avg(const ZZ9KQrStats *s)
{
  return s->n ? s->sum / s->n : 0U;
}

/* Rounded to the nearest nanosecond. 0 when the clock rate is unknown. */
static inline uint64_t zz9k_qr_ticks_to_ns(uint64_t ticks, uint32_t hz)
{
  if (hz == 0U) return 0U;
  return (ticks * 1000000000ULL + hz / 2U) / hz;
}

/* The ids of PINGs sent and not yet answered, oldest first. The firmware
 * answers in order, so a PONG must carry the oldest id. */
typedef struct ZZ9KQrPings {
  uint32_t ids[ZZ9K_QR_PING_SLOTS];
  uint32_t head;
  uint32_t count;
  uint32_t next_id;
} ZZ9KQrPings;

static inline void zz9k_qr_pings_init(ZZ9KQrPings *p)
{
  memset(p, 0, sizeof(*p));
  p->next_id = 1U;
}

/* Takes the next id for a PING about to be sent. 0 when all slots are in
 * use. */
static inline uint32_t zz9k_qr_pings_push(ZZ9KQrPings *p)
{
  uint32_t id;

  if (p->count >= ZZ9K_QR_PING_SLOTS) return 0U;
  id = p->next_id++;
  p->ids[(p->head + p->count) % ZZ9K_QR_PING_SLOTS] = id;
  p->count++;
  return id;
}

/* 1 when `id` is the oldest outstanding PING, which is then answered. */
static inline int zz9k_qr_pings_match(ZZ9KQrPings *p, uint32_t id)
{
  if (p->count == 0U || p->ids[p->head] != id) return 0;
  p->head = (p->head + 1U) % ZZ9K_QR_PING_SLOTS;
  p->count--;
  return 1;
}

typedef struct ZZ9KQrError {
  uint32_t queue;
  uint32_t seq;
  uint32_t code;
  uint32_t words[ZZ9K_QP_ENTRY_WORDS]; /* native values, 0 if not an entry */
} ZZ9KQrError;

/* What one queue did, and what the firmware says about it. */
typedef struct ZZ9KQrQueue {
  uint32_t queue;
  uint32_t owner;
  uint32_t down_consumed;
  uint32_t stream_entries;
  uint32_t up_produced;
  uint32_t pings_sent;
  uint32_t pings_answered;
  uint32_t pong_bad;
  uint32_t have_status;
  uint32_t fw_down_produced;
  uint32_t fw_up_checked;
  uint32_t fw_up_errors;
  uint32_t fw_first_error;
  uint32_t fw_first_error_seq;
  uint32_t fw_ring_full;
  uint32_t fw_passes;
  uint32_t fw_completed;
  uint32_t fw_pongs;
} ZZ9KQrQueue;

/* Takes the words of a STATUS reply (ZZ9K_QP_STATUS_WORDS of them). */
static inline void zz9k_qr_queue_set_status(ZZ9KQrQueue *q,
                                            const uint32_t *w)
{
  q->have_status = 1U;
  q->fw_down_produced = w[ZZ9K_QP_STATUS_DOWN_PRODUCED];
  q->fw_up_checked = w[ZZ9K_QP_STATUS_UP_CHECKED];
  q->fw_up_errors = w[ZZ9K_QP_STATUS_UP_ERRORS];
  q->fw_first_error = w[ZZ9K_QP_STATUS_FIRST_ERROR];
  q->fw_first_error_seq = w[ZZ9K_QP_STATUS_FIRST_ERROR_SEQ];
  q->fw_ring_full = w[ZZ9K_QP_STATUS_RING_FULL];
  q->fw_passes = w[ZZ9K_QP_STATUS_PASSES];
  q->fw_completed = w[ZZ9K_QP_STATUS_COMPLETED];
  q->fw_pongs = w[ZZ9K_QP_STATUS_PONGS];
}

typedef struct ZZ9KQrTally {
  uint32_t stream;             /* queues ran in STREAM mode */
  uint32_t min_stream_entries; /* per queue, stream only */
  uint32_t min_pings;          /* answered PINGs per queue */
  uint32_t setup_failed;       /* attach, mapping or header problem */
  uint32_t interrupted;        /* Ctrl-C */
  uint32_t call_failures;      /* mailbox calls that failed or were short */

  uint32_t queues;
  uint32_t down_entries;
  uint32_t stream_entries;
  uint32_t up_entries;
  uint32_t pings_sent;
  uint32_t pings_answered;
  uint32_t pong_bad;
  uint32_t min_queue_stream;
  uint32_t min_queue_pings;

  uint32_t no_status;     /* queues without a firmware STATUS */
  uint32_t down_lost;     /* queues where firmware produced more than the
                             Amiga consumed */
  uint32_t up_unconsumed; /* queues where firmware checked fewer up entries
                             than the Amiga produced */
  uint32_t fw_up_errors;
  uint32_t fw_first_error;
  uint32_t fw_first_error_queue;
  uint32_t fw_first_error_seq;

  uint32_t errors; /* Amiga-side check errors */
  uint32_t by_code[ZZ9K_QP_ERR_COUNT];
  uint32_t logged;
  ZZ9KQrError log[ZZ9K_QR_LOG_ERRORS];
} ZZ9KQrTally;

static inline void zz9k_qr_tally_init(ZZ9KQrTally *t, int stream,
                                      uint32_t min_stream_entries,
                                      uint32_t min_pings)
{
  memset(t, 0, sizeof(*t));
  t->stream = stream ? 1U : 0U;
  t->min_stream_entries = min_stream_entries;
  t->min_pings = min_pings;
}

/* Records an Amiga-side error. `words` are the 16 native words of the entry
 * that failed, or NULL when the error is not about an entry. */
static inline void zz9k_qr_note_error(ZZ9KQrTally *t, uint32_t queue,
                                      uint32_t seq, uint32_t code,
                                      const uint32_t *words)
{
  t->errors++;
  t->by_code[code < ZZ9K_QP_ERR_COUNT ? code : 0U]++;
  if (t->logged < ZZ9K_QR_LOG_ERRORS) {
    ZZ9KQrError *e = &t->log[t->logged++];

    e->queue = queue;
    e->seq = seq;
    e->code = code;
    if (words)
      memcpy(e->words, words, sizeof(e->words));
    else
      memset(e->words, 0, sizeof(e->words));
  }
}

/* Adds a finished queue to the tally. */
static inline void zz9k_qr_queue_done(ZZ9KQrTally *t, const ZZ9KQrQueue *q)
{
  if (t->queues == 0U || q->stream_entries < t->min_queue_stream)
    t->min_queue_stream = q->stream_entries;
  if (t->queues == 0U || q->pings_answered < t->min_queue_pings)
    t->min_queue_pings = q->pings_answered;
  t->queues++;
  t->down_entries += q->down_consumed;
  t->stream_entries += q->stream_entries;
  t->up_entries += q->up_produced;
  t->pings_sent += q->pings_sent;
  t->pings_answered += q->pings_answered;
  t->pong_bad += q->pong_bad;
  if (!q->have_status) {
    t->no_status++;
    return;
  }
  if (q->fw_up_errors != 0U && t->fw_up_errors == 0U) {
    t->fw_first_error = q->fw_first_error;
    t->fw_first_error_queue = q->queue;
    t->fw_first_error_seq = q->fw_first_error_seq;
  }
  t->fw_up_errors += q->fw_up_errors;
  if (q->fw_down_produced != q->down_consumed) t->down_lost++;
  if (q->fw_up_checked != q->up_produced) t->up_unconsumed++;
}

/* NULL when the part passes, else a one-word reason without spaces, so it
 * can sit in a key=value result line. The first rule that fails wins. */
static inline const char *zz9k_qr_verdict(const ZZ9KQrTally *t)
{
  if (t->setup_failed) return "setup";
  if (t->interrupted) return "break";
  if (t->call_failures) return "call_failed";
  if (t->queues == 0U) return "no_queues";
  if (t->no_status) return "no_status";
  if (t->errors) return "amiga_errors";
  if (t->fw_up_errors) return "fw_errors";
  if (t->pong_bad) return "pong_wrong";
  if (t->pings_answered < t->pings_sent) return "pong_missing";
  if (t->down_lost) return "down_lost";
  if (t->up_unconsumed) return "up_unconsumed";
  if (t->min_queue_pings < t->min_pings) return "low_traffic";
  if (t->stream && t->min_queue_stream < t->min_stream_entries)
    return "low_traffic";
  return 0;
}

/* Owner of queue `index` for a stream run: CORE0, CORE1 or MIX. */
static inline uint32_t zz9k_qr_queue_owner(uint32_t choice, uint32_t index)
{
  if (choice == ZZ9K_QR_OWNER_MIX)
    return (index & 1U) ? ZZ9K_QP_OWNER_CORE1 : ZZ9K_QP_OWNER_CORE0;
  return choice;
}

/* Origins allowed in a queue's down ring: the owner writes it. */
static inline uint32_t zz9k_qr_origin_mask(uint32_t owner)
{
  return owner == ZZ9K_QP_OWNER_CORE1 ? (1U << ZZ9K_QP_ORIGIN_CORE1)
                                      : (1U << ZZ9K_QP_ORIGIN_CORE0);
}

static inline const char *zz9k_qr_owner_name(uint32_t owner)
{
  switch (owner) {
  case ZZ9K_QP_OWNER_CORE0: return "core0";
  case ZZ9K_QP_OWNER_CORE1: return "core1";
  case ZZ9K_QP_OWNER_DOORBELL: return "doorbell";
  case ZZ9K_QR_OWNER_MIX: return "mix";
  default: return "?";
  }
}

static inline char zz9k_qr_lower(char c)
{
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive match of `text` with `word`. */
static inline int zz9k_qr_equals(const char *text, const char *word)
{
  while (*text && *word) {
    if (zz9k_qr_lower(*text) != zz9k_qr_lower(*word)) return 0;
    text++;
    word++;
  }
  return *text == *word;
}

/* The value of a KEY=value argument, or NULL when `arg` is another key. */
static inline const char *zz9k_qr_arg_value(const char *arg,
                                            const char *key)
{
  while (*key) {
    if (zz9k_qr_lower(*arg) != zz9k_qr_lower(*key)) return 0;
    arg++;
    key++;
  }
  return *arg == '=' ? arg + 1 : 0;
}

/* Parses an owner name: core0, core1 or mix. 1 on success. */
static inline int zz9k_qr_parse_owner(const char *text, uint32_t *owner)
{
  if (zz9k_qr_equals(text, "core0")) *owner = ZZ9K_QP_OWNER_CORE0;
  else if (zz9k_qr_equals(text, "core1")) *owner = ZZ9K_QP_OWNER_CORE1;
  else if (zz9k_qr_equals(text, "mix")) *owner = ZZ9K_QR_OWNER_MIX;
  else return 0;
  return 1;
}

/* Parses the decimal number at the start of `text`, in [min, max]. Returns
 * the position after the digits, or NULL. */
static inline const char *zz9k_qr_parse_number(const char *text,
                                               uint32_t min, uint32_t max,
                                               uint32_t *out)
{
  uint64_t value = 0U;
  const char *start = text;

  while (*text >= '0' && *text <= '9') {
    value = value * 10U + (uint32_t)(*text - '0');
    if (value > max) return 0;
    text++;
  }
  if (text == start || value < min) return 0;
  *out = (uint32_t)value;
  return text;
}

/* Parses a single number: nothing may follow the digits. */
static inline int zz9k_qr_parse_u32(const char *text, uint32_t min,
                                    uint32_t max, uint32_t *out)
{
  const char *end = zz9k_qr_parse_number(text, min, max, out);

  return end && *end == '\0';
}

/* Parses a queue count list such as "1,8,32": each in 1..32, at most
 * `capacity`. Returns how many, 0 on any error. */
static inline uint32_t zz9k_qr_parse_counts(const char *text,
                                            uint32_t *counts,
                                            uint32_t capacity)
{
  uint32_t n = 0U;

  for (;;) {
    if (n >= capacity) return 0U;
    text = zz9k_qr_parse_number(text, 1U, ZZ9K_QP_MAX_QUEUES, &counts[n]);
    if (!text) return 0U;
    n++;
    if (*text == '\0') return n;
    if (*text != ',') return 0U;
    text++;
  }
}

#endif
