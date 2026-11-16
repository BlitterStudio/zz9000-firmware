#include "scheduler.h"
#ifndef COUNTS_PER_SECOND
#define COUNTS_PER_SECOND 100000000U
#endif
taskq_shared_t *scheduler_shared(void);
int scheduler_core1_available(void);
void scheduler_core1_publish(void);
int scheduler_quiesce_for_reset(void);
int scheduler_core1_reclaim(void);
