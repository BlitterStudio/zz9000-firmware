/*
 * Shared resident synchronization steps, parameterized by their Exec bindings.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ9K_LIBRARY_RESIDENT_SYNC_H
#define ZZ9K_LIBRARY_RESIDENT_SYNC_H

#if !defined(ZZ9K_LIB_SYNC_OBTAIN) || !defined(ZZ9K_LIB_SYNC_ATTEMPT) || \
    !defined(ZZ9K_LIB_SYNC_REAP_SERVICE) || \
    !defined(ZZ9K_LIB_SYNC_REAP_RELEASE) || \
    !defined(ZZ9K_LIB_SYNC_RELEASE) || \
    !defined(ZZ9K_LIB_SYNC_CALL_SUBMIT) || \
    !defined(ZZ9K_LIB_SYNC_CALL_POLL) || \
    !defined(ZZ9K_LIB_SYNC_CALL_COMPLETED) || \
    !defined(ZZ9K_LIB_SYNC_CALL_DRAINED) || \
    !defined(ZZ9K_LIB_SYNC_CALL_STATUS) || \
    !defined(ZZ9K_LIB_SYNC_CALL_CANCEL) || \
    !defined(ZZ9K_LIB_SYNC_CALL_WAIT) || \
    !defined(ZZ9K_LIB_SYNC_CALL_SIGNAL) || \
    !defined(ZZ9K_LIB_SYNC_WAIT_BUSY_AVAILABLE) || \
    !defined(ZZ9K_LIB_SYNC_WAIT_BUSY_DONE) || \
    !defined(ZZ9K_LIB_SYNC_WAIT_MARK_BUSY_DONE) || \
    !defined(ZZ9K_LIB_SYNC_WAIT_START_BUSY) || \
    !defined(ZZ9K_LIB_SYNC_WAIT_BUSY_EXPIRED) || \
    !defined(ZZ9K_LIB_SYNC_WAIT_FINISH_BUSY) || \
    !defined(ZZ9K_LIB_SYNC_WAIT_TIMEOUT_ZERO) || \
    !defined(ZZ9K_LIB_SYNC_WAIT_TIMERS_AVAILABLE) || \
    !defined(ZZ9K_LIB_SYNC_WAIT_TICK_ARMED) || \
    !defined(ZZ9K_LIB_SYNC_WAIT_ARM_TICK) || \
    !defined(ZZ9K_LIB_SYNC_WAIT_OWNER_MASK) || \
    !defined(ZZ9K_LIB_SYNC_WAIT_DEADLINE_MASK) || \
    !defined(ZZ9K_LIB_SYNC_WAIT_TICK_MASK) || \
    !defined(ZZ9K_LIB_SYNC_WAIT_BREAK_MASK) || \
    !defined(ZZ9K_LIB_SYNC_WAIT_BLOCK) || \
    !defined(ZZ9K_LIB_SYNC_WAIT_FINISH_TICK) || \
    !defined(ZZ9K_LIB_SYNC_WAIT_FALLBACK) || \
    !defined(ZZ9K_LIB_SYNC_DISPATCHER_STOP_REQUESTED) || \
    !defined(ZZ9K_LIB_SYNC_DISPATCHER_POLL) || \
    !defined(ZZ9K_LIB_SYNC_DISPATCHER_RESIGNAL)
#error "resident synchronization bindings are required"
#endif

/*
 * One pass of the dispatcher: note whether an owner check is due, then, when
 * the transport lock is free, release what the check found. Stopping or
 * releasing a client id is a transport call, so it needs that lock.
 */
static inline int zz9k_lib_dispatcher_service(void *base, void *transport_lock)
{
  int got_transport;

  ZZ9K_LIB_SYNC_REAP_SERVICE(base);
  got_transport = ZZ9K_LIB_SYNC_ATTEMPT(transport_lock);
  if (got_transport) {
    ZZ9K_LIB_SYNC_REAP_RELEASE(base);
  }
  return got_transport;
}

/* Consecutive dispatcher wakes that may find the transport lock busy and
 * signal the dispatcher again before it stops retrying. */
#ifndef ZZ9K_LIB_DISPATCHER_MISS_LIMIT
#define ZZ9K_LIB_DISPATCHER_MISS_LIMIT 200U
#endif

/*
 * One wake of the dispatcher, run for every signal it receives (the card's
 * completion interrupt, the vertical-blank tick, a stop request). Returns 1
 * when a stop was requested and the caller must leave, 0 otherwise.
 *
 * The dispatcher only attempts the transport lock: an owner that holds it
 * may be waiting for the dispatcher to stop, so blocking could deadlock. A
 * wake that finds the lock busy has still consumed its signal. It signals
 * the dispatcher again, for at most ZZ9K_LIB_DISPATCHER_MISS_LIMIT
 * consecutive misses; after that the vertical-blank tick is the safety net.
 */
static inline int zz9k_lib_dispatcher_wake(void *base, void *transport_lock,
                                           unsigned int *misses)
{
  for (;;) {
    uint32_t completed = 0;
    int status;

    if (!zz9k_lib_dispatcher_service(base, transport_lock)) {
      /* The signal this wake consumed may have been the card's completion
       * interrupt. Raise it again so the retry drains the ring as soon as
       * the lock frees, instead of leaving the owner to its tick. */
      if (*misses < ZZ9K_LIB_DISPATCHER_MISS_LIMIT) {
        (*misses)++;
        ZZ9K_LIB_SYNC_DISPATCHER_RESIGNAL(base);
      }
      return 0;
    }
    *misses = 0U;
    if (ZZ9K_LIB_SYNC_DISPATCHER_STOP_REQUESTED(base)) {
      ZZ9K_LIB_SYNC_RELEASE(transport_lock);
      return 1;
    }
    status = ZZ9K_LIB_SYNC_DISPATCHER_POLL(base, &completed);
    ZZ9K_LIB_SYNC_RELEASE(transport_lock);
    if (status != ZZ9K_STATUS_OK || completed == 0U) {
      return 0;
    }
  }
}


static inline void zz9k_lib_sync_call_signal(void *task, uint32_t signal_mask)
{
  ZZ9K_LIB_SYNC_CALL_SIGNAL(task, signal_mask);
}

static inline int zz9k_lib_sync_call_wait_step(void *record)
{
  uint32_t signals;

  if (!ZZ9K_LIB_SYNC_WAIT_BUSY_DONE(record)) {
    if (!ZZ9K_LIB_SYNC_WAIT_BUSY_AVAILABLE(record)) {
      ZZ9K_LIB_SYNC_WAIT_MARK_BUSY_DONE(record);
    } else {
      ZZ9K_LIB_SYNC_WAIT_START_BUSY(record);
      if (!ZZ9K_LIB_SYNC_WAIT_BUSY_EXPIRED(record)) {
        return ZZ9K_STATUS_OK;
      }
      ZZ9K_LIB_SYNC_WAIT_FINISH_BUSY(record);
      ZZ9K_LIB_SYNC_WAIT_MARK_BUSY_DONE(record);
    }
  }

  if (ZZ9K_LIB_SYNC_WAIT_TIMEOUT_ZERO(record)) {
    return ZZ9K_STATUS_TIMEOUT;
  }
  if (!ZZ9K_LIB_SYNC_WAIT_TIMERS_AVAILABLE(record)) {
    return ZZ9K_LIB_SYNC_WAIT_FALLBACK(record);
  }
  if (!ZZ9K_LIB_SYNC_WAIT_TICK_ARMED(record)) {
    ZZ9K_LIB_SYNC_WAIT_ARM_TICK(record);
  }
  /*
   * A caller owns its break signals.  In particular, the break mask is
   * deliberately not passed to the blocking primitive.
   */
  signals = ZZ9K_LIB_SYNC_WAIT_BLOCK(
      record, ZZ9K_LIB_SYNC_WAIT_OWNER_MASK(record) |
                  ZZ9K_LIB_SYNC_WAIT_DEADLINE_MASK(record) |
                  ZZ9K_LIB_SYNC_WAIT_TICK_MASK(record));
  if (signals & ZZ9K_LIB_SYNC_WAIT_DEADLINE_MASK(record)) {
    return ZZ9K_STATUS_TIMEOUT;
  }
  if (signals & ZZ9K_LIB_SYNC_WAIT_TICK_MASK(record)) {
    ZZ9K_LIB_SYNC_WAIT_FINISH_TICK(record);
  }
  return ZZ9K_STATUS_OK;
}
/*
 * A synchronous record enters the core pending list while transport_lock is
 * held.  Every ring drain and every removal from that list takes the same
 * lock, but the wait itself never does.  A drain that completes another
 * task's record invokes that record's completion binding before releasing the
 * lock, so the owner can wake on its own signal.
 */
static inline int zz9k_lib_sync_call(void *base, void *transport_lock,
                                     void *record)
{
  int status;

  ZZ9K_LIB_SYNC_OBTAIN(transport_lock);
  status = ZZ9K_LIB_SYNC_CALL_SUBMIT(base, record);
  if (status != ZZ9K_STATUS_QUEUED) {
    ZZ9K_LIB_SYNC_RELEASE(transport_lock);
    return status;
  }

  for (;;) {
    status = ZZ9K_LIB_SYNC_CALL_POLL(base, record);
    if (status != ZZ9K_STATUS_OK) {
      (void)ZZ9K_LIB_SYNC_CALL_CANCEL(base, record);
      ZZ9K_LIB_SYNC_RELEASE(transport_lock);
      return status;
    }
    if (ZZ9K_LIB_SYNC_CALL_COMPLETED(record)) {
      status = ZZ9K_LIB_SYNC_CALL_STATUS(record);
      ZZ9K_LIB_SYNC_RELEASE(transport_lock);
      return status;
    }
    if (ZZ9K_LIB_SYNC_CALL_DRAINED(record)) {
      continue;
    }
    ZZ9K_LIB_SYNC_RELEASE(transport_lock);

    status = ZZ9K_LIB_SYNC_CALL_WAIT(record);
    ZZ9K_LIB_SYNC_OBTAIN(transport_lock);
    if (status == ZZ9K_STATUS_TIMEOUT) {
      do {
        status = ZZ9K_LIB_SYNC_CALL_POLL(base, record);
        if (status != ZZ9K_STATUS_OK) {
          (void)ZZ9K_LIB_SYNC_CALL_CANCEL(base, record);
          ZZ9K_LIB_SYNC_RELEASE(transport_lock);
          return status;
        }
        if (ZZ9K_LIB_SYNC_CALL_COMPLETED(record)) {
          status = ZZ9K_LIB_SYNC_CALL_STATUS(record);
          ZZ9K_LIB_SYNC_RELEASE(transport_lock);
          return status;
        }
      } while (ZZ9K_LIB_SYNC_CALL_DRAINED(record));
      (void)ZZ9K_LIB_SYNC_CALL_CANCEL(base, record);
      ZZ9K_LIB_SYNC_RELEASE(transport_lock);
      return ZZ9K_STATUS_TIMEOUT;
    }
    if (ZZ9K_LIB_SYNC_CALL_COMPLETED(record)) {
      status = ZZ9K_LIB_SYNC_CALL_STATUS(record);
      ZZ9K_LIB_SYNC_RELEASE(transport_lock);
      return status;
    }
    if (status != ZZ9K_STATUS_OK) {
      (void)ZZ9K_LIB_SYNC_CALL_CANCEL(base, record);
      ZZ9K_LIB_SYNC_RELEASE(transport_lock);
      return status;
    }
  }
}

#endif
