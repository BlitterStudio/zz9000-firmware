/*
 * Task identity record and the check clock for zz9k.library dead-owner
 * detection. See zz9k_library_owner_reap.h for how they are used.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ9K_LIBRARY_TASK_STAMP_H
#define ZZ9K_LIBRARY_TASK_STAMP_H

#include <stdint.h>

/* One check per this many vertical blanks: 50 Hz PAL or 60 Hz NTSC, so a
 * little under or about one second. */
#define ZZ9K_LIB_REAP_PERIOD_TICKS 50U

typedef struct ZZ9KTaskStamp {
  const void *task;
  const void *name;
  uint32_t task_num;
  uint32_t sig_alloc;
  uint8_t type;
} ZZ9KTaskStamp;

/* sig_alloc is the live mask at lookup time, not part of the identity. */
static inline int zz9k_task_stamp_equal(const ZZ9KTaskStamp *a,
                                        const ZZ9KTaskStamp *b)
{
  return a->task == b->task && a->name == b->name &&
         a->task_num == b->task_num && a->type == b->type;
}

/* True once `period` ticks have passed since `last`; moves `last` forward.
 * Unsigned subtraction keeps it correct across a counter wrap. */
static inline int zz9k_lib_reap_due(uint32_t *last, uint32_t now,
                                    uint32_t period)
{
  if ((uint32_t)(now - *last) < period) {
    return 0;
  }
  *last = now;
  return 1;
}

/*
 * Who owns an opener record. Three parties can reach one: the task's own
 * OpenCnt calls, CloseLibrary's final release, and the service task's reaper.
 * Under the library lock a record is in exactly one of: live, closing (set
 * by CloseLibrary before it drops the lock), or dead (set by the service task
 * or by Open when the address was reused). Only the party that owns it frees
 * it.
 */

/* Open found a record at the caller's address. It is stale, and must not be
 * shared with the caller, when the record is dead or was made by a different
 * task that had this address. */
static inline int zz9k_lib_opener_stale(int dead, const ZZ9KTaskStamp *recorded,
                                        const ZZ9KTaskStamp *current)
{
  return dead || !zz9k_task_stamp_equal(recorded, current);
}

enum {
  /* Another reference remains: only the count drops. */
  ZZ9K_OPENER_CLOSE_UNREFERENCE = 0,
  /* The service task already owns the record: it releases and frees it. */
  ZZ9K_OPENER_CLOSE_LEAVE_TO_REAPER = 1,
  /* This call releases the client id and frees the record. */
  ZZ9K_OPENER_CLOSE_RELEASE = 2
};

static inline int zz9k_lib_opener_close_action(int dead, uint32_t references)
{
  if (references != 1U) {
    return ZZ9K_OPENER_CLOSE_UNREFERENCE;
  }
  return dead ? ZZ9K_OPENER_CLOSE_LEAVE_TO_REAPER : ZZ9K_OPENER_CLOSE_RELEASE;
}

/* The service task may declare a live record dead, and may release a dead
 * one, only while CloseLibrary is not releasing it. */
static inline int zz9k_lib_opener_markable(int dead, int closing)
{
  return !dead && !closing;
}

static inline int zz9k_lib_opener_reapable(int dead, int closing)
{
  return dead && !closing;
}

#endif
