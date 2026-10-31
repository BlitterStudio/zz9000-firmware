#include <assert.h>
#include <stdint.h>

#include "zzapp.h"
#include "zzapp_proto.h"
#include "zzapp_runtime.h"

int main(void)
{
    uint32_t card;

    zzapp_boot_init();
    zzapp_test_set_xtime(0);
    zzapp_test_mark_running();

    zzapp_api_enter();
    zzapp_host_tick_at(ZZAPP_TICK_US);
    assert(zzapp_test_service_calls() == 0u);
    assert(zzapp_test_in_api() == 1u);
    zzapp_api_leave();
    assert(zzapp_test_in_api() == 0u);
    assert(zzapp_test_service_calls() == 1u);

    zzapp_boot_init();
    zzapp_test_set_xtime(0);
    zzapp_test_mark_running();
    zzapp_host_tick_at(ZZAPP_WATCHDOG_US);
    assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);

    zzapp_boot_init();
    assert(zzapp_loader_begin() == ZZ_OK);
    card = zzapp_test_arena_card();
    assert(card != 0u);
    zzapp_test_mark_running();
    assert(zzapp_fault_is_app(card + 4u, 0u));
    assert(!zzapp_fault_wants_restart(card + 4u, 0u));
    zzapp_take_app_fault(ZZAPP_FAULT_DATA, card + 4u, card + 8u,
                         card + 12u, 0x1234u, 0x5678u, 0x13u);
    assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);
    assert(zzapp_test_cold_restarts() == 0);
    return 0;
}
