/*
 * Dead-owner detection for zz9k.library openers, parameterized by its Exec
 * bindings so a host test can drive it.
 *
 * A client task that dies without CloseLibrary leaves its opener record, and
 * with it a client id the firmware keeps worker state for, for as long as the
 * library stays open. The library's service task therefore asks, about once
 * a second, whether each opener's task still exists. The check runs at task
 * level: the interrupt only counts vertical blanks.
 *
 * Identity. A task address alone proves nothing, because Exec can hand the
 * same address to a new task. An owner counts as present only when the task
 * found at its address still carries the fields recorded at Open time (name
 * pointer, node type, CLI task number). A replacement task that matches every
 * field is still taken for the owner. That leaves the client id held, which
 * is the behaviour without this check; a false "gone" cannot arise from
 * address reuse.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ9K_LIBRARY_OWNER_REAP_H
#define ZZ9K_LIBRARY_OWNER_REAP_H

#include <stdint.h>

#if !defined(ZZ9K_LIB_REAP_TASK_LOOKUP) || \
    !defined(ZZ9K_LIB_REAP_CLOCK) || \
    !defined(ZZ9K_LIB_REAP_LAST_CHECK) || \
    !defined(ZZ9K_LIB_REAP_SCAN_PENDING) || \
    !defined(ZZ9K_LIB_REAP_OPENERS_FIRST) || \
    !defined(ZZ9K_LIB_REAP_OPENER_NEXT) || \
    !defined(ZZ9K_LIB_REAP_OPENER_DEAD) || \
    !defined(ZZ9K_LIB_REAP_OPENER_STAMP) || \
    !defined(ZZ9K_LIB_REAP_OPENER_CLOSING) || \
    !defined(ZZ9K_LIB_REAP_NODE_SUCC) || \
    !defined(ZZ9K_LIB_REAP_LOCK) || \
    !defined(ZZ9K_LIB_REAP_UNLOCK) || \
    !defined(ZZ9K_LIB_REAP_BACKOFF) || \
    !defined(ZZ9K_LIB_REAP_STOP_DISPATCHER) || \
    !defined(ZZ9K_LIB_REAP_RELEASE_OPENER)
#error "owner reap bindings are required"
#endif

#include "zz9k_library_task_stamp.h"

static inline int zz9k_lib_reap_owner_gone(void *base,
                                           const ZZ9KTaskStamp *owner)
{
  ZZ9KTaskStamp now;

  (void)base;
  if (!ZZ9K_LIB_REAP_TASK_LOOKUP(base, owner->task, &now)) {
    return 1;
  }
  return !zz9k_task_stamp_equal(owner, &now);
}

/*
 * One service pass. A due check asks the release step, which holds the
 * transport lock the opener list needs, to look at the openers.
 */
static inline void zz9k_lib_reap_service(void *base)
{
  if (zz9k_lib_reap_due(&ZZ9K_LIB_REAP_LAST_CHECK(base),
                        ZZ9K_LIB_REAP_CLOCK(base),
                        ZZ9K_LIB_REAP_PERIOD_TICKS)) {
    ZZ9K_LIB_REAP_SCAN_PENDING(base) = 1U;
  }
}

/*
 * Releases what the last pass found, with the transport lock held. A due
 * check first marks the openers whose task is gone; each dead opener's
 * client id is then released. An opener whose release the firmware answered
 * with BUSY stays dead and is offered again on the next pass.
 */
static inline void zz9k_lib_reap_release(void *base)
{
  void *opener;
  void *next;

  if (ZZ9K_LIB_REAP_SCAN_PENDING(base)) {
    ZZ9K_LIB_REAP_SCAN_PENDING(base) = 0U;
    for (opener = ZZ9K_LIB_REAP_OPENERS_FIRST(base); opener;
         opener = ZZ9K_LIB_REAP_OPENER_NEXT(opener)) {
      if (zz9k_lib_opener_markable(ZZ9K_LIB_REAP_OPENER_DEAD(opener),
                                   ZZ9K_LIB_REAP_OPENER_CLOSING(opener)) &&
          zz9k_lib_reap_owner_gone(base, ZZ9K_LIB_REAP_OPENER_STAMP(opener))) {
        ZZ9K_LIB_REAP_OPENER_DEAD(opener) = 1U;
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
