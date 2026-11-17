#include "rasterix_fence_guard.h"

void rasterix_fence_guard_pool(card_pool_t *pool, int rasterix_active,
                               const rasterix_fence_guard_hooks_t *hooks,
                               uint32_t wait_ms)
{
    uint32_t address;
    uint32_t sequence;
    uint32_t started;
    int snapshot;
    if (!rasterix_active || !pool || !hooks || !hooks->snapshot ||
        !hooks->read_fence || !hooks->engine_idle || !hooks->clock_ms ||
        !hooks->sleep_ms)
        return;
    snapshot = hooks->snapshot(hooks->context, &address, &sequence);
    if (snapshot < 0) {
        card_pool_quarantine_minigl(pool);
        return;
    }
    if (snapshot == 0)
        return;

    started = hooks->clock_ms(hooks->context);
    for (;;) {
        if (hooks->read_fence(hooks->context, address) == (uint16_t)sequence &&
            hooks->engine_idle(hooks->context)) {
            if (hooks->forget)
                hooks->forget(hooks->context);
            return;
        }
        if ((uint32_t)(hooks->clock_ms(hooks->context) - started) >= wait_ms)
            break;
        hooks->sleep_ms(hooks->context, 1U);
        snapshot = hooks->snapshot(hooks->context, &address, &sequence);
        if (snapshot < 0)
            break;
        if (snapshot == 0)
            return;
    }
    card_pool_quarantine_minigl(pool);
}
