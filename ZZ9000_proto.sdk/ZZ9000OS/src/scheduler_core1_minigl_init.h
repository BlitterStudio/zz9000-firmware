/*
 * Shared core-1 MiniGL worker initialization.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ9K_SCHEDULER_CORE1_MINIGL_INIT_H
#define ZZ9K_SCHEDULER_CORE1_MINIGL_INIT_H

#include "scheduler.h"

static inline void scheduler_core1_minigl_init(taskq_shared_t *sh)
{
  sh->core1_alive = 1;
#ifdef TASKQ_HOST_TEST
  __sync_synchronize();
#else
  dmb();
#endif
}

#endif /* ZZ9K_SCHEDULER_CORE1_MINIGL_INIT_H */
