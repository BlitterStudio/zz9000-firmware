/*
 * Host backing for the firmware card pool; see host_card_pool.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "card_pool.h"
#include "host_card_pool.h"

#define HOST_POOL_MAX (256U << 20)

static uint32_t host_pool_base;

void host_card_pool_reset(uint32_t bytes)
{
    if (!host_pool_base) {
        /* MAP_32BIT keeps the mapping below 2 GB; one spare page lets the
         * base be aligned to the pool's 64 KB pages. */
        void *p = mmap(0, HOST_POOL_MAX + CARD_POOL_PAGE_BYTES,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT | MAP_NORESERVE,
                       -1, 0);
        if (p == MAP_FAILED) {
            perror("host_card_pool: mmap below 4 GB");
            exit(1);
        }
        host_pool_base = ((uint32_t)(uintptr_t)p + CARD_POOL_PAGE_BYTES - 1U) &
                         ~(CARD_POOL_PAGE_BYTES - 1U);
    }
    if (bytes > HOST_POOL_MAX)
        bytes = HOST_POOL_MAX;
    card_pool_init(&card_pool);
    if (card_pool_add_range(&card_pool, host_pool_base,
                            host_pool_base + bytes, 0) != 0) {
        fprintf(stderr, "host_card_pool: bad range size %u\n", bytes);
        exit(1);
    }
    card_pool_open_range(&card_pool, 0);
}

uint32_t host_card_pool_used(void)
{
    uint32_t used = 0;
    for (uint32_t c = 0; c < CARD_POOL_CLASS_COUNT; c++)
        used += card_pool.class_used[c];
    return used;
}
