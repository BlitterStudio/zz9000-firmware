/*
 * Dead-owner detection for zz9k.library app handles and openers,
 * parameterized by its Exec bindings so a host test can drive it.
 *
 * A client task that dies without ZZ9KAppClose or CloseLibrary leaves its
 * handle claimed, and the VBlank lease keeps the firmware slot alive for as
 * long as the library stays open. The library's service task therefore asks,
 * about once a second, whether each owner still exists. The check runs at
 * task level: the interrupt only counts vertical blanks.
 *
 * Identity. A task address alone proves nothing, because Exec can hand the
 * same address to a new task. An owner counts as present only when the task
 * found at its address still carries the fields recorded at Open time (name
 * pointer, node type, CLI task number) and, for an app handle, still has the
 * signal bit the library allocated from it set in tc_SigAlloc. A live owner
 * holds that bit from Open to Close and a new task starts with a fresh
 * allocation mask, but a new task can allocate the same bit, so the other
 * fields carry the rest. Stack bounds are left out on purpose: StackSwap
 * changes them in a live task, and a wrong "gone" would stop a running app.
 * A replacement task that matches every field is still taken for the owner.
 * That leaves the slot held, which is the behaviour without this check; a
 * false "gone" cannot arise from address reuse.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ9K_LIBRARY_OWNER_REAP_H
#define ZZ9K_LIBRARY_OWNER_REAP_H

#include <stdint.h>

#if !defined(ZZ9K_LIB_REAP_SLOT_HANDLE) || \
    !defined(ZZ9K_LIB_REAP_HANDLE_CLAIMED) || \
    !defined(ZZ9K_LIB_REAP_HANDLE_DEAD) || \
    !defined(ZZ9K_LIB_REAP_HANDLE_STAMP) || \
    !defined(ZZ9K_LIB_REAP_HANDLE_SIGNAL_MASK) || \
    !defined(ZZ9K_LIB_REAP_MARK_HANDLE_DEAD) || \
    !defined(ZZ9K_LIB_REAP_SERVICE_HANDLE) || \
    !defined(ZZ9K_LIB_REAP_TASK_LOOKUP) || \
    !defined(ZZ9K_LIB_REAP_CLOCK) || \
    !defined(ZZ9K_LIB_REAP_LAST_CHECK) || \
    !defined(ZZ9K_LIB_REAP_SCAN_PENDING) || \
    !defined(ZZ9K_LIB_REAP_HANDLES_FIRST) || \
    !defined(ZZ9K_LIB_REAP_HANDLE_NEXT) || \
    !defined(ZZ9K_LIB_REAP_RELEASE_HANDLE) || \
    !defined(ZZ9K_LIB_REAP_OPENERS_FIRST) || \
    !defined(ZZ9K_LIB_REAP_OPENER_NEXT) || \
    !defined(ZZ9K_LIB_REAP_OPENER_DEAD) || \
    !defined(ZZ9K_LIB_REAP_OPENER_CLOSING) || \
    !defined(ZZ9K_LIB_REAP_HANDLE_EVENT_POSTED) || \
    !defined(ZZ9K_LIB_REAP_HANDLE_BURY) || \
    !defined(ZZ9K_LIB_REAP_HANDLE_FREE) || \
    !defined(ZZ9K_LIB_REAP_GRAVEYARD_TAKE) || \
    !defined(ZZ9K_LIB_REAP_NODE_SUCC) || \
    !defined(ZZ9K_LIB_REAP_LOCK) || \
    !defined(ZZ9K_LIB_REAP_UNLOCK) || \
    !defined(ZZ9K_LIB_REAP_BACKOFF) || \
    !defined(ZZ9K_LIB_REAP_STOP_DISPATCHER) || \
    !defined(ZZ9K_LIB_REAP_RELEASE_OPENER)
#error "owner reap bindings are required"
#endif

#include "zz9k_library_task_stamp.h"

/* A signal mask of zero skips the signal-bit test (an opener allocates no
 * signal). */
static inline int zz9k_lib_reap_owner_gone(void *base,
                                           const ZZ9KTaskStamp *owner,
                                           uint32_t signal_mask)
{
  ZZ9KTaskStamp now;

  if (!ZZ9K_LIB_REAP_TASK_LOOKUP(base, owner->task, &now)) {
    return 1;
  }
  if (!zz9k_task_stamp_equal(owner, &now)) {
    return 1;
  }
  return signal_mask != 0U && (now.sig_alloc & signal_mask) != signal_mask;
}

/*
 * One service pass, under the app lock. A claimed handle whose owner is gone
 * is marked dead before it is serviced, so the lease is not renewed again;
 * every other handle is serviced as before. A due check also asks the
 * release step, which holds the transport lock the opener list needs, to
 * look at the openers.
 */
static inline void zz9k_lib_reap_service(void *base, uint32_t slot_count)
{
  uint32_t slot;
  int due = zz9k_lib_reap_due(&ZZ9K_LIB_REAP_LAST_CHECK(base),
                              ZZ9K_LIB_REAP_CLOCK(base),
                              ZZ9K_LIB_REAP_PERIOD_TICKS);

  for (slot = 0U; slot < slot_count; slot++) {
    void *handle = ZZ9K_LIB_REAP_SLOT_HANDLE(base, slot);

    if (!handle || ZZ9K_LIB_REAP_HANDLE_DEAD(handle)) {
      continue;
    }
    if (due && ZZ9K_LIB_REAP_HANDLE_CLAIMED(handle) &&
        zz9k_lib_reap_owner_gone(base, ZZ9K_LIB_REAP_HANDLE_STAMP(handle),
                                 ZZ9K_LIB_REAP_HANDLE_SIGNAL_MASK(handle))) {
      ZZ9K_LIB_REAP_MARK_HANDLE_DEAD(base, handle);
      continue;
    }
    ZZ9K_LIB_REAP_SERVICE_HANDLE(handle);
  }
  if (due) {
    ZZ9K_LIB_REAP_SCAN_PENDING(base) = 1U;
  }
}

/*
 * Releases what the last pass found, with the transport lock held. A due
 * check first looks at the handles the service pass does not visit (those
 * detached from their slot by a newer claim) and at the openers; the opener
 * list belongs to the transport lock. Each dead handle's generation is then
 * stopped and its slot freed, and each dead opener's client id released. An
 * opener whose release the firmware answered with BUSY stays dead and is
 * offered again on the next pass.
 */
static inline void zz9k_lib_reap_release(void *base)
{
  void *handle;
  void *opener;
  void *next;

  if (ZZ9K_LIB_REAP_SCAN_PENDING(base)) {
    ZZ9K_LIB_REAP_SCAN_PENDING(base) = 0U;
    for (handle = ZZ9K_LIB_REAP_HANDLES_FIRST(base); handle;
         handle = ZZ9K_LIB_REAP_HANDLE_NEXT(handle)) {
      if (!ZZ9K_LIB_REAP_HANDLE_DEAD(handle) &&
          !ZZ9K_LIB_REAP_HANDLE_CLAIMED(handle) &&
          zz9k_lib_reap_owner_gone(base, ZZ9K_LIB_REAP_HANDLE_STAMP(handle),
                                   ZZ9K_LIB_REAP_HANDLE_SIGNAL_MASK(handle))) {
        ZZ9K_LIB_REAP_MARK_HANDLE_DEAD(base, handle);
      }
    }
    for (opener = ZZ9K_LIB_REAP_OPENERS_FIRST(base); opener;
         opener = ZZ9K_LIB_REAP_OPENER_NEXT(opener)) {
      if (zz9k_lib_opener_markable(ZZ9K_LIB_REAP_OPENER_DEAD(opener),
                                   ZZ9K_LIB_REAP_OPENER_CLOSING(opener)) &&
          zz9k_lib_reap_owner_gone(base, ZZ9K_LIB_REAP_OPENER_STAMP(opener),
                                   0U)) {
        ZZ9K_LIB_REAP_OPENER_DEAD(opener) = 1U;
      }
    }
  }
  for (handle = ZZ9K_LIB_REAP_HANDLES_FIRST(base); handle; handle = next) {
    next = ZZ9K_LIB_REAP_HANDLE_NEXT(handle);
    if (ZZ9K_LIB_REAP_HANDLE_DEAD(handle)) {
      ZZ9K_LIB_REAP_RELEASE_HANDLE(base, handle);
      /* The handle embeds the event message it posted to the owner's
       * notify port. While that message may still be linked on a port, which
       * can outlive the owner, the record must stay allocated: it is kept
       * until the library's last close instead of freed. */
      if (ZZ9K_LIB_REAP_HANDLE_EVENT_POSTED(handle)) {
        ZZ9K_LIB_REAP_HANDLE_BURY(base, handle);
      } else {
        ZZ9K_LIB_REAP_HANDLE_FREE(base, handle);
      }
    }
  }
  for (opener = ZZ9K_LIB_REAP_OPENERS_FIRST(base); opener; opener = next) {
    next = ZZ9K_LIB_REAP_OPENER_NEXT(opener);
    /* An opener in CloseLibrary belongs to that call, which frees it. */
    if (zz9k_lib_opener_reapable(ZZ9K_LIB_REAP_OPENER_DEAD(opener),
                                 ZZ9K_LIB_REAP_OPENER_CLOSING(opener))) {
      (void)ZZ9K_LIB_REAP_RELEASE_OPENER(base, opener);
    }
  }
}

/* The library's last close: no port can reach a buried handle any more. */
static inline void zz9k_lib_reap_drain_graveyard(void *base)
{
  void *handle;

  while ((handle = ZZ9K_LIB_REAP_GRAVEYARD_TAKE(base)) != 0) {
    ZZ9K_LIB_REAP_HANDLE_FREE(base, handle);
  }
}

/*
 * The last CloseLibrary, before the dispatcher stops. A dead opener whose
 * release the firmware answered BUSY is still linked for the service task to
 * retry, and once the dispatcher is gone nothing would finish it. This call
 * takes over every such opener (marking it closing, so the service task
 * leaves it) and retries each release, backing off with the lock dropped,
 * until the firmware accepts it and the record is freed. The caller holds no
 * lock.
 */
static inline void zz9k_lib_reap_finish_openers(void *base)
{
  void *opener;
  void *next;
  int pending;

  for (opener = ZZ9K_LIB_REAP_OPENERS_FIRST(base); opener;
       opener = ZZ9K_LIB_REAP_OPENER_NEXT(opener)) {
    if (zz9k_lib_opener_reapable(ZZ9K_LIB_REAP_OPENER_DEAD(opener),
                                 ZZ9K_LIB_REAP_OPENER_CLOSING(opener))) {
      ZZ9K_LIB_REAP_OPENER_CLOSING(opener) = 1U;
    }
  }
  do {
    pending = 0;
    for (opener = ZZ9K_LIB_REAP_OPENERS_FIRST(base); opener; opener = next) {
      next = ZZ9K_LIB_REAP_OPENER_NEXT(opener);
      if (ZZ9K_LIB_REAP_OPENER_DEAD(opener) &&
          ZZ9K_LIB_REAP_OPENER_CLOSING(opener) &&
          !ZZ9K_LIB_REAP_RELEASE_OPENER(base, opener)) {
        pending = 1;
      }
    }
    if (pending) {
      ZZ9K_LIB_REAP_BACKOFF(base);
    }
  } while (pending);
}

static inline void zz9k_lib_reap_final_close(void *base)
{
  ZZ9K_LIB_REAP_LOCK(base);
  zz9k_lib_reap_finish_openers(base);
  ZZ9K_LIB_REAP_UNLOCK(base);
  ZZ9K_LIB_REAP_STOP_DISPATCHER(base);
}

/* Whether `wanted` is a task Exec still knows: the running task, or a node
 * on the ready or the waiting list. The caller keeps interrupts disabled for
 * the walk. Each list ends at the node whose successor is null. */
static inline int zz9k_lib_reap_task_listed(const void *wanted,
                                            const void *running,
                                            const void *ready_head,
                                            const void *wait_head)
{
  const void *node;

  if (running == wanted) {
    return 1;
  }
  for (node = ready_head; ZZ9K_LIB_REAP_NODE_SUCC(node);
       node = ZZ9K_LIB_REAP_NODE_SUCC(node)) {
    if (node == wanted) {
      return 1;
    }
  }
  for (node = wait_head; ZZ9K_LIB_REAP_NODE_SUCC(node);
       node = ZZ9K_LIB_REAP_NODE_SUCC(node)) {
    if (node == wanted) {
      return 1;
    }
  }
  return 0;
}

#endif
