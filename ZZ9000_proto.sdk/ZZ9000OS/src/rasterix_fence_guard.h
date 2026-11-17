#ifndef RASTERIX_FENCE_GUARD_H
#define RASTERIX_FENCE_GUARD_H

#include <stdint.h>

#include "card_pool.h"

typedef struct {
    void *context;
    int (*snapshot)(void *context, uint32_t *address, uint32_t *sequence);
    uint16_t (*read_fence)(void *context, uint32_t address);
    int (*engine_idle)(void *context);
    uint32_t (*clock_ms)(void *context);
    void (*sleep_ms)(void *context, uint32_t milliseconds);
    void (*forget)(void *context);
} rasterix_fence_guard_hooks_t;

/* Poll a published RasterIX fence for at most wait_ms. A timed-out fetch keeps
 * every RasterIX-owned block quarantined before the caller reclaims the pool. */
void rasterix_fence_guard_pool(card_pool_t *pool, int rasterix_active,
                               const rasterix_fence_guard_hooks_t *hooks,
                               uint32_t wait_ms);

#endif
