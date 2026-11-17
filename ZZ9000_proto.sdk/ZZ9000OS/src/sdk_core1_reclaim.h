/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef SDK_CORE1_RECLAIM_H
#define SDK_CORE1_RECLAIM_H

#include "scheduler.h"

/* Core 1 is halted. Poison core-1-owned SDK decoder pointers before releasing
 * their pool, then fail work the halted CPU can no longer execute. */
void sdk_core1_reclaim_after_halt(taskq_t *queue);

#endif
