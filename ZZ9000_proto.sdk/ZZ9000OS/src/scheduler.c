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

#if ZZAPP_PERF_COUNTERS
uint32_t __attribute__((weak)) taskq_perf_enqueue_stamp(void)
{
  return 0U;
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
  q->claim_cursor = 0;
  q->short_claim_cursor = 0;
}

void taskq_shared_boot_init(taskq_shared_t *sh)
{
  if (!sh)
    return;
  taskq_init(&sh->queue);
  sh->core1_current_slot = -1;
  sh->core1_restart_request = 0;
  sh->core1_fault_code = 0;
  sh->core1_alive = 0;
  sh->tasks_on_core1 = 0;
  sh->tasks_on_core0 = 0;
  sh->core1_park_request = 0;
  sh->core1_parked = 0;
  sh->core1_reclaim_request = 0;
  sh->core1_reclaim_done = 0;
  sh->core1_clean_request = 0;
  sh->core1_clean_done = 0;
  sh->minigl_doorbell = 0;
  sh->minigl_reset_request = 0;
  sh->minigl_reset_ack = 0;
  sh->minigl_in_flight = 0;
  sh->minigl_submission_admission = 1U;
  sh->minigl_submission_generation = 0U;
  sh->minigl_submission_ack = 0U;
  sh->minigl_packets_on_core1 = 0;
  sh->minigl_packets_on_core0 = 0;
  sh->minigl_rix_fence_epoch = 0U;
  sh->minigl_rix_fence_sequence = 0U;
  sh->minigl_rix_fence_address = 0U;
  /* Producers compare-exchange this from zero. A leftover 2 is neither
   * serviced nor replaceable, so the worker never drains again. */
  sh->service_pending = 0;
#if ZZAPP_PERF_COUNTERS
  sh->perf_work_publications = 0U;
  sh->perf_doorbell_stamp = 0U;
  sh->perf_sgi_count = 0U;
#endif
}
#if ZZAPP_PERF_COUNTERS
void scheduler_perf_work_published(taskq_shared_t *shared)
{
  if (shared)
    (void)__atomic_add_fetch(&shared->perf_work_publications, 1U,
                             __ATOMIC_RELEASE);
}
#endif
void scheduler_core1_publish_work(taskq_shared_t *shared,
				  scheduler_core1_wake_fn wake)
{
	if (!shared || !wake)
		return;
#if ZZAPP_PERF_COUNTERS
	scheduler_perf_work_published(shared);
#endif
	wake(shared);
}
void scheduler_core1_service_clean_request_with(
    taskq_shared_t *shared, scheduler_core1_clean_cache_fn clean_cache,
    scheduler_core1_clean_publish_fn publish, void *context)
{
	if (!shared || !clean_cache || !publish ||
	    shared->core1_current_slot >= 0 || shared->core1_restart_request ||
	    !shared->core1_clean_request)
		return;
	shared->core1_clean_request = 0U;
	clean_cache(context);
	publish(context);
	shared->core1_clean_done = 1U;
	publish(context);
}

int scheduler_core1_clean_dcache_wait(taskq_shared_t *shared,
				      scheduler_core1_available_fn available,
				      scheduler_core1_clean_publish_fn publish,
				      scheduler_core1_clean_wait_fn wait,
				      void *context, uint32_t spins)
{
	uint32_t spin;

	if (!shared || !available || !publish || !wait || !available(context) ||
	    !shared->core1_alive)
		return 0;
	if (shared->core1_current_slot >= 0 || shared->core1_restart_request)
		return -1;
	shared->core1_clean_done = 0U;
	shared->core1_clean_request = 1U;
	publish(context);
	for (spin = 0U; spin < spins && !shared->core1_clean_done; spin++)
		wait(context, SCHED_CORE1_CLEAN_POLL_US);
	if (shared->core1_clean_done)
		return 0;
	shared->core1_clean_request = 0U;
	publish(context);
	return -1;
}
int scheduler_core1_service_park_request_with(
    taskq_shared_t *shared, scheduler_core1_clean_cache_fn clean_cache,
    scheduler_core1_clean_publish_fn publish, void *context)
{
	if (!shared || !clean_cache || !publish ||
	    !__atomic_load_n(&shared->core1_park_request, __ATOMIC_ACQUIRE))
		return 0;
	clean_cache(context);
	__atomic_store_n(&shared->core1_parked, 1U, __ATOMIC_RELEASE);
	publish(context);
	return 1;
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
#if ZZAPP_PERF_COUNTERS
      d->perf_enqueue_stamp = taskq_perf_enqueue_stamp();
#endif
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

int taskq_doorbell_request(volatile uint32_t *pending,
                           taskq_doorbell_send_fn send, void *context)
{
  if (!pending || !taskq_cas_u32(pending, 0U, 1U))
    return 0;
  taskq_store_release(pending, 1U);
  if (send)
    send(context);
  return 1;
}

uint32_t taskq_doorbell_drain(volatile uint32_t *pending,
                              taskq_doorbell_drain_fn drain,
                              taskq_doorbell_more_fn more, void *context,
                              uint32_t batch)
{
  uint32_t drained;

  if (!pending || !drain || !more || batch == 0U)
    return 0;
  drained = drain(context, batch);
  if (more(context))
    return drained;
  taskq_store_release(pending, 0U);
  taskq_acquire_barrier();
  if (!more(context))
    return drained;
  /* Work arrived between the empty check and clear. It is already either
   * accompanied by the producer's SGI or remains visible to the next worker
   * pass; retain the pending state in both cases. */
  (void)taskq_cas_u32(pending, 0U, 1U);
  return drained;
}

/* ---- atomic claim (consumer): CAS QUEUED -> CLAIMED ---- */
int taskq_claim_any(taskq_t *q)
{
  uint32_t i;
  uint32_t cursor = __atomic_load_n(&q->claim_cursor, __ATOMIC_RELAXED);
  for (i = 0; i < TASKQ_CAPACITY; i++) {
    uint32_t idx = (cursor + i) % TASKQ_CAPACITY;
    volatile uint32_t *s = &q->descs[idx].state;
    if (*s == TASK_QUEUED && taskq_cas_u32(s, TASK_QUEUED, TASK_CLAIMED)) {
      __atomic_store_n(&q->claim_cursor, (idx + 1U) % TASKQ_CAPACITY,
                       __ATOMIC_RELAXED);
      return (int)idx;
    }
  }
  return -1;
}

int taskq_claim_short(taskq_t *q)
{
  uint32_t i;
  uint32_t cursor = __atomic_load_n(&q->short_claim_cursor,
                                    __ATOMIC_RELAXED);
  for (i = 0; i < TASKQ_CAPACITY; i++) {
    uint32_t idx = (cursor + i) % TASKQ_CAPACITY;
    if (q->descs[idx].state != TASK_QUEUED) {
      continue;
    }
    /* Acquire: order cls after observing TASK_QUEUED, pairing with enqueue. */
    taskq_acquire_barrier();
    if (q->descs[idx].cls == (uint32_t)TASK_SHORT) {
      volatile uint32_t *s = &q->descs[idx].state;
      if (taskq_cas_u32(s, TASK_QUEUED, TASK_CLAIMED)) {
        __atomic_store_n(&q->short_claim_cursor, (idx + 1U) % TASKQ_CAPACITY,
                         __ATOMIC_RELAXED);
        return (int)idx;
      }
    }
  }
  return -1;
}

void taskq_requeue(taskq_t *q, int slot)
{
  if (!q || slot < 0 || slot >= (int)TASKQ_CAPACITY)
    return;
  taskq_store_release(&q->descs[slot].state, TASK_QUEUED);
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

void taskq_abort_active(taskq_t *q, uint16_t status)
{
  uint32_t i;

  for (i = 0; i < TASKQ_CAPACITY; i++) {
    uint32_t state = q->descs[i].state;

    if (state == TASK_QUEUED || state == TASK_CLAIMED) {
      taskq_fail(q, (int)i, status);
    }
  }
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
  case TASKQ_OP_IMAGE_ANIMATION_FRAME_NEXT:
  case TASKQ_OP_IMAGE_ANIMATION_FRAME_PRESENT:
  case TASKQ_OP_IMAGE_ANIMATION_FRAME_RETIRE:
  case TASKQ_OP_IMAGE_ANIMATION_RESTART:
    /* MUST stay LONG regardless of size: the session codec state lives
     * in core 1's cache, so a core-0 SHORT drain would corrupt it. */
    return TASK_LONG;
  case TASKQ_OP_AUDIO_STREAM_FEED:
  case TASKQ_OP_AUDIO_STREAM_READ:
  case TASKQ_OP_AUDIO_STREAM_CLOSE:
    /* Same constraint: the stream's mp3 staging ring (and a FLAC or
     * Vorbis stream's decoder heap) is cache-owned by core 1 for
     * core-1-affine streams. */
    return TASK_LONG;
  case TASKQ_OP_VIDEO_SESSION_WRITE:
  case TASKQ_OP_VIDEO_SESSION_DECODE:
  case TASKQ_OP_VIDEO_SESSION_CLOSE:
  case TASKQ_OP_MEDIA_SESSION_WRITE:
  case TASKQ_OP_MEDIA_SESSION_DECODE:
  case TASKQ_OP_MEDIA_SESSION_AUDIO_READ:
  case TASKQ_OP_MEDIA_SESSION_CLOSE:
    /* The selected codec backend and all of its heap state stay on core 1. */
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

void scheduler_core1_halt_before_recover(
    scheduler_core1_recovery_step_t halt,
    scheduler_core1_recovery_step_t recover)
{
  halt();
  recover();
}

void scheduler_core1_fallback_recover_with(
    scheduler_core1_recovery_step_t halt,
    scheduler_core1_recovery_step_t recover,
    scheduler_core1_queue_reclaim_fn reclaim_after_halt, taskq_t *queue,
    scheduler_core1_recovery_step_t reclaim_audio,
    scheduler_core1_module_quarantine_fn quarantine_module, uint32_t module,
    volatile uint32_t *core1_started)
{
  scheduler_core1_halt_before_recover(halt, recover);
  reclaim_after_halt(queue);
  reclaim_audio();
  quarantine_module(module);
  *core1_started = 0U;
}

/* ---- core-1 worker action policy (portable, host-tested) ---- */
sched_worker_action_t sched_worker_decide(int reset_requested, int task_available,
                                          int minigl_work_available, int minigl_bound)
{
  if (reset_requested) {
    return SCHED_WORKER_ACTION_RESET;
  }
  if (task_available) {
    return SCHED_WORKER_ACTION_TASK;
  }
  if (minigl_work_available) {
    return SCHED_WORKER_ACTION_MINIGL;
  }
  if (minigl_bound) {
    return SCHED_WORKER_ACTION_POLL_IDLE;
  }
  return SCHED_WORKER_ACTION_WFE;
}

int sched_worker_may_wfe(sched_worker_action_t action, int minigl_progress,
                         int task_active, int reset_requested, int doorbell,
                         int minigl_registered)
{
  if (task_active || reset_requested || doorbell)
    return 0;
  if (action == SCHED_WORKER_ACTION_WFE)
    return !minigl_registered;
  /* An idle bound worker keeps polling the 68k ring, which raises no event. */
  return action == SCHED_WORKER_ACTION_MINIGL && !minigl_progress;
}

int scheduler_minigl_submission_admit(taskq_shared_t *shared)
{
  if (shared->minigl_submission_admission != 0U)
    return 1;
  taskq_acquire_barrier();
  /* The reset request precedes closed admission. Do not acknowledge until the
   * worker's reset step has cleared it, even after a stale action selection. */
  if (shared->minigl_reset_request != 0U)
    return 0;
  taskq_store_release(&shared->minigl_submission_ack,
                      shared->minigl_submission_generation);
  return 0;
}
sched_worker_action_t scheduler_minigl_submission_action(
    taskq_shared_t *shared, sched_worker_action_t action)
{
  /* A reset step can still abort a feed and publish its accepted envelope.
   * Let it complete before acknowledging closed submission admission. */
  if (action == SCHED_WORKER_ACTION_RESET)
    return action;
  if (!scheduler_minigl_submission_admit(shared) &&
      (action == SCHED_WORKER_ACTION_MINIGL ||
       action == SCHED_WORKER_ACTION_POLL_IDLE))
    return SCHED_WORKER_ACTION_WFE;
  return action;
}

int scheduler_minigl_quiesce_done(taskq_shared_t *shared)
{
  return shared->minigl_submission_ack == shared->minigl_submission_generation &&
         !taskq_has_active(&shared->queue) && !shared->minigl_in_flight;
}

void scheduler_harvest_post_with(taskq_t *q, taskq_watchdog_t *watchdog,
                                 uint16_t failed_status,
                                 taskq_slot_gen_ok_fn gen_ok,
                                 taskq_slot_post_fn post,
                                 taskq_slot_forget_fn forget)
{
  int slot;

  while ((slot = taskq_harvest(q)) >= 0) {
    taskq_desc_t *d = &q->descs[slot];
    int failed;

    if (!gen_ok(slot)) {
      /* Task submitted under a previous mailbox lifetime (the mailbox was
       * re-initialised while it was in flight). Drop it without posting so
       * its stale request_id/user_cookie never lands in the new completion
       * ring. */
      forget(slot);
      taskq_release(q, slot);
      continue;
    }
    failed = (d->state == TASK_FAILED);
    if (!post(slot, d->request_id, d->user_cookie, (uint16_t)d->opcode,
              failed ? failed_status : d->result_status,
              d->result_payload, d->result_len)) {
      break;  /* completion ring full -> task stays harvested, retry next poll */
    }
    if (!failed && watchdog)
      taskq_watchdog_on_success(watchdog);
    taskq_release(q, slot);
  }
}

void scheduler_quiesce_finish(taskq_shared_t *shared, int timed_out,
                              scheduler_quiesce_step_fn cold_restart,
                              scheduler_quiesce_step_fn post_completions)
{
  if (timed_out) {
    /* A task outlived the wait and is still writing its buffers. Cold-reset
     * core 1 first: that halts the worker mid-op so no write survives into
     * the next lifetime, and the halt-path reclaim marks its in-flight
     * tasks FAILED. */
    cold_restart();
    shared->core1_restart_request = 0U; /* consumed here; don't double-restart */
    /* Deliver those failures to their waiting clients before the queue is
     * reinitialised: taskq_init below erases the request ids, and without
     * this posting the in-flight clients see only the mailbox wall-clock
     * timeout. */
    post_completions();
  }
  /* A restarted worker must find no queued descriptor. */
  taskq_init(&shared->queue);
  shared->core1_current_slot = -1;
}


sched_minigl_reset_phase_t sched_minigl_reset_phase(int core1_up, int posted,
                                                    uint32_t ack, int elapsed_ms,
                                                    int budget_ms)
{
  /* Core 1 could not stop the engines: stop them here, with it halted. */
  if (ack == SCHED_MINIGL_RESET_ACK_FAILED)
    return SCHED_MINIGL_RESET_ABORT;
  if (ack != 0U)
    return SCHED_MINIGL_RESET_ACKED;
  if (!core1_up)
    return posted ? SCHED_MINIGL_RESET_ABORT : SCHED_MINIGL_RESET_LOCAL;
  if (!posted)
    return SCHED_MINIGL_RESET_IDLE;
  if (budget_ms >= 0 && elapsed_ms >= budget_ms)
    return SCHED_MINIGL_RESET_ABORT;
  return SCHED_MINIGL_RESET_WAIT;
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
