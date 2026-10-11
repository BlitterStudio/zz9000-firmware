/*
 * Host backing for the firmware card pool, shared by the host suites whose
 * code allocates from it (arenas, sessions). The pool deals in 32-bit card
 * addresses, so it is laid over memory mapped below 4 GB.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef HOST_CARD_POOL_H
#define HOST_CARD_POOL_H

#include <stdint.h>

/* Re-initialise the global card_pool with one open range of `bytes` (at
 * most 256 MB) of real memory, dropping every block. */
void host_card_pool_reset(uint32_t bytes);
/* Bytes the pool currently hands out, all classes. */
uint32_t host_card_pool_used(void);

#endif /* HOST_CARD_POOL_H */
