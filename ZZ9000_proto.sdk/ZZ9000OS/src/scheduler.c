/*
 * ZZ9000 dual-core task scheduler -- portable core.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host-testable: contains NO xil_ or firmware includes. The ARM build gets the
 * real LDREX/STREX compare-swap and a dmb-release store from the #else branch;
 * the host test build (-DTASKQ_HOST_TEST) gets an injectable single-threaded
 * model so the claim-race path is exercisable deterministically.
 */
#include "scheduler.h"

/* ---- atomic primitive seam ---- */
#ifdef TASKQ_HOST_TEST
int taskq_test_force_cas_fail = 0;

int taskq_cas_u32(volatile uint32_t *p, uint32_t expected, uint32_t desired)
{
  if (taskq_test_force_cas_fail > 0) {
    taskq_test_force_cas_fail--;
    return 0;                       /* simulate losing the exclusive access */
  }
  if (*p == expected) {
    *p = desired;
    return 1;
  }
  return 0;
}

static void taskq_store_release(volatile uint32_t *p, uint32_t v)
{
  *p = v;
}

static void taskq_acquire_barrier(void)
{
}
#else
int taskq_cas_u32(volatile uint32_t *p, uint32_t expected, uint32_t desired)
{
  uint32_t old, ok;
  __asm__ __volatile__(
    "1: ldrex %0, [%2]\n"
    "   cmp   %0, %3\n"
    "   bne   2f\n"
    "   strex %1, %4, [%2]\n"
    "   cmp   %1, #0\n"
    "   bne   1b\n"
    "   dmb   ish\n"
    "   mov   %1, #1\n"
    "   b     3f\n"
    "2: clrex\n"
    "   mov   %1, #0\n"
    "3:\n"
    : "=&r"(old), "=&r"(ok)
    : "r"(p), "r"(expected), "r"(desired)
    : "cc", "memory");
  (void)old;
  return (int)ok;
}

static void taskq_store_release(volatile uint32_t *p, uint32_t v)
{
  __asm__ __volatile__("dmb ish" ::: "memory");
  *p = v;
}

static void taskq_acquire_barrier(void)
{
  __asm__ __volatile__("dmb ish" ::: "memory");
}
#endif

/* ---- coherency stress model (used by the Phase 0 two-core torture harness) ---- */
uint32_t taskq_stress_fill(uint32_t seed, uint32_t index)
{
  uint32_t x = seed ^ (index * 2654435761u);
  x ^= x >> 15;
  x *= 2246822519u;
  x ^= x >> 13;
  return x;
}

int taskq_stress_check(uint32_t seed, uint32_t index, uint32_t value)
{
  return taskq_stress_fill(seed, index) == value ? 1 : 0;
}

/* ---- queue lifecycle: init + single-producer enqueue ---- */
void taskq_init(taskq_t *q)
{
  uint32_t i;
  for (i = 0; i < TASKQ_CAPACITY; i++) {
    q->descs[i].state = TASK_FREE;
  }
  q->enqueue_cursor = 0;
}

int taskq_enqueue(taskq_t *q, uint32_t opcode, taskq_class_t cls,
                  uint32_t in_addr, uint32_t in_len,
                  uint32_t out_addr, uint32_t out_cap,
                  uint32_t request_id, uint32_t user_cookie,
                  const void *op_params, uint32_t param_len)
{
  uint32_t i, idx;
  for (i = 0; i < TASKQ_CAPACITY; i++) {
    idx = (q->enqueue_cursor + i) % TASKQ_CAPACITY;
    if (q->descs[idx].state == TASK_FREE) {
      taskq_desc_t *d = &q->descs[idx];
      uint32_t k;
      /* Callers pass structs smaller than the 48-byte op_params field, so copy
       * only param_len source bytes and zero-fill the rest -- reading a full
       * TASKQ_OP_PARAM_BYTES from the caller's struct would be an out-of-bounds
       * read that leaks unrelated stack into the shared queue. */
      uint32_t n = (param_len < TASKQ_OP_PARAM_BYTES) ? param_len
                                                      : TASKQ_OP_PARAM_BYTES;
      d->cls = (uint32_t)cls;
      d->opcode = opcode;
      d->in_addr = in_addr;
      d->in_len = in_len;
      d->out_addr = out_addr;
      d->out_cap = out_cap;
      d->request_id = request_id;
      d->user_cookie = user_cookie;
      d->result_status = 0;
      d->result_len = 0;
      /* Copy the opaque input params before publishing, so a consumer that
       * claims the QUEUED slot always sees complete params. */
      for (k = 0; k < TASKQ_OP_PARAM_BYTES; k++) {
        d->op_params[k] = (op_params && k < n) ? ((const uint8_t *)op_params)[k]
                                               : 0;
      }
      taskq_store_release(&d->state, TASK_QUEUED);  /* publish last */
      q->enqueue_cursor = (idx + 1) % TASKQ_CAPACITY;
      return (int)idx;
    }
  }
  return -1;
}

/* ---- atomic claim (consumer): CAS QUEUED -> CLAIMED ---- */
int taskq_claim_any(taskq_t *q)
{
  uint32_t i;
  for (i = 0; i < TASKQ_CAPACITY; i++) {
    volatile uint32_t *s = &q->descs[i].state;
    if (*s == TASK_QUEUED && taskq_cas_u32(s, TASK_QUEUED, TASK_CLAIMED)) {
      return (int)i;
    }
  }
  return -1;
}

int taskq_claim_short(taskq_t *q)
{
  uint32_t i;
  for (i = 0; i < TASKQ_CAPACITY; i++) {
    if (q->descs[i].state != TASK_QUEUED) {
      continue;
    }
    /* Acquire: order the cls load after observing TASK_QUEUED, pairing with the
     * dmb-release store of state in taskq_enqueue. Without it the weakly-ordered
     * A9 could pair a freshly published QUEUED state with a stale cls left over
     * from this slot's previous task -- a SHORT->LONG reuse would then read
     * cls == TASK_SHORT and run a LONG crypto op inline on core 0. */
    taskq_acquire_barrier();
    if (q->descs[i].cls == (uint32_t)TASK_SHORT) {
      volatile uint32_t *s = &q->descs[i].state;
      if (taskq_cas_u32(s, TASK_QUEUED, TASK_CLAIMED)) {
        return (int)i;
      }
    }
  }
  return -1;
}

/* ---- completion transitions ---- */
void taskq_complete(taskq_t *q, int slot, uint16_t status,
                    const uint8_t *payload, uint16_t payload_len)
{
  taskq_desc_t *d = &q->descs[slot];
  uint16_t n = payload_len > TASKQ_RESULT_PAYLOAD ? TASKQ_RESULT_PAYLOAD
                                                  : payload_len;
  uint16_t k;
  d->result_status = status;
  d->result_len = n;
  for (k = 0; k < n; k++) {
    d->result_payload[k] = payload ? payload[k] : 0;
  }
  taskq_store_release(&d->state, TASK_DONE);
}

void taskq_fail(taskq_t *q, int slot, uint16_t status)
{
  taskq_desc_t *d = &q->descs[slot];
  d->result_status = status;
  d->result_len = 0;
  taskq_store_release(&d->state, TASK_FAILED);
}

int taskq_harvest(taskq_t *q)
{
  uint32_t i;
  for (i = 0; i < TASKQ_CAPACITY; i++) {
    uint32_t st = q->descs[i].state;
    if (st == TASK_DONE || st == TASK_FAILED) {
      /* Acquire: order the consumer's later result_status/result_payload reads
       * after this terminal-state load. Pairs with the dmb-release store in
       * taskq_complete()/taskq_fail(); without it the weakly-ordered A9 may
       * observe TASK_DONE yet read stale/zero result fields. */
      taskq_acquire_barrier();
      return (int)i;
    }
  }
  return -1;
}

void taskq_release(taskq_t *q, int slot)
{
  taskq_store_release(&q->descs[slot].state, TASK_FREE);
}

/* True while any slot is QUEUED or CLAIMED -- i.e. the worker may still be about
 * to run it or is mid-execution (still writing the task's resolved data
 * buffers). The mailbox-reset quiesce waits on this before freeing buffers. */
int taskq_has_active(taskq_t *q)
{
  uint32_t i;
  for (i = 0; i < TASKQ_CAPACITY; i++) {
    uint32_t st = q->descs[i].state;
    if (st == TASK_QUEUED || st == TASK_CLAIMED) {
      return 1;
    }
  }
  return 0;
}

/* ---- dispatch policy ---- */
taskq_class_t taskq_class_for_opcode(uint32_t opcode, uint32_t in_len)
{
  switch (opcode) {
  case TASKQ_OP_CRYPTO_KX:
  case TASKQ_OP_CRYPTO_VERIFY:
    return TASK_SHORT;              /* asymmetric ops: always small/fast */
  case TASKQ_OP_CRYPTO_HASH:
  case TASKQ_OP_CRYPTO_STREAM:
  case TASKQ_OP_CRYPTO_AEAD:
    return (in_len <= TASKQ_SHORT_MAX_BYTES) ? TASK_SHORT : TASK_LONG;
  case TASKQ_OP_DECOMPRESS:
    return TASK_LONG;
  case TASKQ_OP_SCALE_IMAGE:
  case TASKQ_OP_SCALE_IMAGE_CLIPPED:
    /* in_len carries the destination-rect byte count */
    return (in_len <= TASKQ_SHORT_MAX_BYTES) ? TASK_SHORT : TASK_LONG;
  case TASKQ_OP_DECODE_JPEG:
    return TASK_LONG;               /* full image decode: always heavy */
  case TASKQ_OP_DECODE_MP3:
    return TASK_LONG;               /* full audio decode: always heavy */
  case TASKQ_OP_IMAGE_SESSION_FEED:
  case TASKQ_OP_IMAGE_SESSION_CLOSE:
    /* MUST stay LONG regardless of size: the session codec state lives
     * in core 1's cache, so a core-0 SHORT drain would corrupt it. */
    return TASK_LONG;
  case TASKQ_OP_AUDIO_STREAM_FEED:
  case TASKQ_OP_AUDIO_STREAM_READ:
    /* Same constraint: the stream's mp3 staging ring is cache-owned by
     * core 1 for core-1-affine streams. */
    return TASK_LONG;
  default:
    return TASK_LONG;              /* unknown/heavy: never drained on core 0 */
  }
}

int taskq_should_drain(int zorro_pending, int display_pending, int core1_enabled)
{
  if (!core1_enabled) {
    return 0;                      /* fallback path drains all inline elsewhere */
  }
  return (!zorro_pending && !display_pending) ? 1 : 0;
}

/* ---- fault watchdog: trips core 1 off after too many consecutive faults ---- */
void taskq_watchdog_init(taskq_watchdog_t *w, uint32_t threshold)
{
  w->consecutive_faults = 0;
  w->fault_threshold = threshold;
  w->core1_enabled = 1;
}

void taskq_watchdog_on_fault(taskq_watchdog_t *w)
{
  w->consecutive_faults++;
  if (w->fault_threshold != 0 && w->consecutive_faults >= w->fault_threshold) {
    w->core1_enabled = 0;
  }
}

void taskq_watchdog_on_success(taskq_watchdog_t *w)
{
  w->consecutive_faults = 0;
}

int taskq_watchdog_core1_enabled(const taskq_watchdog_t *w)
{
  return w->core1_enabled;
}
