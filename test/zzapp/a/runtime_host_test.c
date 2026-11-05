#include <assert.h>
#include <stdint.h>

#include "zzapp.h"
#include "zzapp_proto.h"
#include "zzapp_runtime.h"
void *zzapp_host_alloc(uint32_t bytes, uint32_t align, uint32_t flags);
int zzapp_host_address(const void *p, uint32_t *host_offset);
int zzapp_host_mem_info(zzapp_mem_info *out);

int main(void)
{
    uint32_t card;
    uint32_t host_offset;
    void *host_visible;
    zzapp_mem_info mem;

    zzapp_boot_init();
    /* zzapp_enter_arm establishes the VFP ABI baseline before app code. */
    assert(ZZAPP_ENTRY_FPSCR == 0u);

    zzapp_test_set_xtime(0);
    zzapp_test_mark_running();

    zzapp_api_enter();
    zzapp_host_tick_at(ZZAPP_TICK_US);
    assert(zzapp_test_service_calls() == 0u);
    assert(zzapp_test_in_api() == 1u);
    zzapp_api_leave();
    assert(zzapp_test_in_api() == 0u);
    assert(zzapp_test_service_calls() == 1u);

    /*
     * A display flip can wait for ZZRun's SafeMessage longer than the
     * ordinary API deadline.  Each declared wait renews that deadline,
     * unlike a wedged API call below.
     */
    zzapp_boot_init();
    zzapp_test_set_xtime(0);
    zzapp_test_mark_running();
    zzapp_api_enter();
    zzapp_test_set_xtime(ZZAPP_API_STUCK_US + ZZAPP_TICK_US);
    zzapp_host_yield_wait();
    assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);
    zzapp_api_leave();

    zzapp_boot_init();
    zzapp_test_set_xtime(0);
    zzapp_test_mark_running();
    /* Long app computation between API calls (GLQuake's level load) is
     * not a hang; only ZZAPP_APP_STALL_US without any call stops it. */
    zzapp_host_tick_at(ZZAPP_WATCHDOG_US + ZZAPP_TICK_US);
    assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);
    zzapp_host_tick_at(ZZAPP_APP_STALL_US);
    assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);

    zzapp_boot_init();
    zzapp_test_set_xtime(0);
    zzapp_test_mark_running();
    zzapp_api_enter();
    zzapp_host_tick_at(ZZAPP_API_STUCK_US);
    assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);

    /*
     * Core 0 can read its clock just before core 1 stores a newer progress
     * and tick stamp.  That stamp is not an ancient one: a healthy app must
     * not be cold-restarted (the unsigned age used to wrap to ~2^64).
     */
    zzapp_boot_init();
    zzapp_test_set_xtime(1000u);
    zzapp_test_mark_running();
    zzapp_host_tick_at(1000u);
    zzapp_test_set_xtime(999u);
    zzapp_core0_poll();
    assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);
    assert(zzapp_test_cold_restarts() == 0);

    zzapp_boot_init();
    assert(zzapp_loader_begin() == ZZ_OK);
    card = zzapp_test_arena_card();
    assert(card != 0u);
    zzapp_test_mark_running();
    host_visible = zzapp_host_alloc(4096u, 256u, ZZ_ALLOC_HOST_VISIBLE);
    assert(host_visible != NULL);
    assert(zzapp_host_address(host_visible, &host_offset) == ZZ_OK);
    assert(host_offset != 0u);
    assert(zzapp_host_mem_info(&mem) == ZZ_OK);
    assert(mem.host_visible_used == 4096u);
    assert(mem.host_visible_limit >= mem.host_visible_used);
    assert(zzapp_fault_is_app(card + 4u, 0u));
    assert(!zzapp_fault_wants_restart(card + 4u, 0u));
    zzapp_take_app_fault(ZZAPP_FAULT_DATA, card + 4u, card + 8u,
                         card + 12u, 0x1234u, 0x5678u, 0x13u);
    assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);
    assert(zzapp_test_cold_restarts() == 0);
    assert(zzapp_host_address(host_visible, &host_offset) == ZZ_EINVAL);
    return 0;
}
