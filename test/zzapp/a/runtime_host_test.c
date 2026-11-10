#include <assert.h>
#include <stdint.h>

#include "zzapp.h"
#include "zzapp_proto.h"
#include "zzapp_runtime.h"
void *zzapp_host_alloc(uint32_t bytes, uint32_t align, uint32_t flags);
int zzapp_host_address(const void *p, uint32_t *host_offset);
int zzapp_host_mem_info(zzapp_mem_info *out);
static uint32_t selected_slot;
static int api_wait_hook_called;

static uint32_t service_poll_depth;
static uint32_t service_poll_max_depth;
static int tick_during_service_poll;

void zzapp_services_poll(void)
{
    service_poll_depth++;
    if (service_poll_depth > service_poll_max_depth)
        service_poll_max_depth = service_poll_depth;
    if (tick_during_service_poll) {
        tick_during_service_poll = 0;
        zzapp_host_tick_at(ZZAPP_TICK_US);
    }
    service_poll_depth--;
}

uint32_t zzapp_services_current_slot(void)
{
    return selected_slot;
}

void zzapp_services_select_slot(uint32_t slot)
{
    selected_slot = slot;
}

void zzapp_test_api_wait_hook(void)
{
    assert(zzapp_runtime_kernel()->api_lock_owner == 0);
    selected_slot = 0U;
    zzapp_kernel_set_current_thread(0);
    zzapp_api_leave();
    selected_slot = 1U;
    zzapp_kernel_set_current_thread(1);
    api_wait_hook_called++;
}


int main(void)
{
    uint32_t card;
    uint32_t host_offset;
    void *host_visible;
    zzapp_mem_info mem;

    zzapp_boot_init();
    /*
     * This host harness cannot execute the ARM entry trampoline or read its
     * hardware FPSCR; it only verifies the baseline constant it supplies.
     */
    assert(ZZAPP_ENTRY_FPSCR == 0u);

    zzapp_test_set_xtime(0);
    zzapp_test_mark_running();

    zzapp_api_enter();
    zzapp_host_tick_at(ZZAPP_TICK_US);
    assert(zzapp_test_service_calls() == 0u);
    assert(zzapp_test_in_api() == 1u);
    zzapp_api_leave();
    assert(zzapp_test_in_api() == 0u);
    assert(zzapp_test_service_calls() == 0u);
    zzapp_worker_hook();
    assert(zzapp_test_service_calls() == 1u);
    zzapp_boot_init();
    zzapp_test_set_xtime(0);
    zzapp_test_mark_running();
    tick_during_service_poll = 1;
    service_poll_max_depth = 0u;
    zzapp_worker_hook();
    assert(service_poll_max_depth == 1u);

    /* An app context resolves to its scheduler slot while the system thread
     * keeps the independently selected service slot. */
    zzapp_boot_init();
    selected_slot = 1U;
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_kernel_set_current_thread(0);
    zzapp_api_enter();
    zzapp_kernel_set_current_thread(-1);
    assert(zzapp_test_in_api() == 0U);
    selected_slot = 0U;
    assert(zzapp_test_in_api() == 1U);
    selected_slot = 1U;
    zzapp_kernel_set_current_thread(0);
    zzapp_api_leave();
    zzapp_kernel_set_current_thread(-1);
    selected_slot = 0U;

    /* A contending API call parks until the owner releases the lock, before
     * its handler can execute. */
    zzapp_boot_init();
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 1U) == 0);
    zzapp_kernel_set_current_thread(0);
    zzapp_api_enter();
    selected_slot = 1U;
    zzapp_kernel_set_current_thread(1);
    zzapp_api_enter();
    assert(api_wait_hook_called == 1);
    assert(zzapp_runtime_kernel()->api_lock_owner == 1);
    zzapp_api_leave();
    zzapp_kernel_set_current_thread(-1);
    selected_slot = 0U;

    /* A blocked app can keep an old per-slot tick while the core continues
     * servicing another context. That must not trigger a cold restart. */
    zzapp_boot_init();
    selected_slot = 1U;
    zzapp_test_set_xtime(0);
    zzapp_test_mark_running();
    zzapp_test_set_xtime(ZZAPP_WATCHDOG_US + ZZAPP_TICK_US);
    selected_slot = 0U;
    zzapp_host_tick_at(ZZAPP_WATCHDOG_US + ZZAPP_TICK_US);
    zzapp_core0_poll();
    selected_slot = 1U;
    assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);
    selected_slot = 0U;


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
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_kernel_set_current_thread(0);
    zzapp_api_enter();
    zzapp_test_set_xtime(ZZAPP_API_STUCK_US + ZZAPP_TICK_US);
    zzapp_host_yield_wait();
    zzapp_host_tick_at(ZZAPP_API_STUCK_US + 2u * ZZAPP_TICK_US);
    assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);
    zzapp_api_leave();
    zzapp_kernel_set_current_thread(-1);

    zzapp_boot_init();
    zzapp_test_set_xtime(0);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_kernel_set_current_thread(0);
    zzapp_api_enter();
    zzapp_host_yield_wait();
    assert(zzapp_runtime_kernel()->threads[0].state == ZZAPP_THREAD_SLEEPING);
    assert(zzapp_runtime_kernel()->threads[0].sleep_deadline == 1U);

    zzapp_boot_init();
    zzapp_test_set_wait_registered(0U, 1);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_test_mark_running();
    zzapp_kernel_set_current_thread(0);
    zzapp_api_enter();
    zzapp_host_yield_wait();
    assert(zzapp_runtime_kernel()->threads[0].state == ZZAPP_THREAD_BLOCKED);
    zzapp_host_tick_at(ZZAPP_API_STUCK_US);
    assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);
    zzapp_kernel_set_current_thread(-1);

    {
        uint32_t service_calls = zzapp_test_service_calls();

        zzapp_host_yield_wait();
        assert(zzapp_test_service_calls() == service_calls);
        zzapp_worker_hook();
        assert(zzapp_test_service_calls() == service_calls + 1U);
    }

    zzapp_boot_init();
    zzapp_test_set_xtime(0);
    zzapp_test_mark_running();
    /* Long app computation between API calls (GLQuake's level load) is
     * not a hang; only ZZAPP_APP_STALL_US without any call stops it. */
    {
        uint32_t service_calls = zzapp_test_service_calls();

        zzapp_host_tick_at(ZZAPP_WATCHDOG_US + ZZAPP_TICK_US);
        assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);
        assert(zzapp_test_service_calls() == service_calls);
        zzapp_host_tick_at(ZZAPP_APP_STALL_US);
        assert(zzapp_runtime_kernel()->threads[0].state == ZZAPP_THREAD_STOPPED);
        assert(zzapp_test_service_calls() == service_calls);
        zzapp_worker_hook();
        assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);
    }

    zzapp_boot_init();
    zzapp_test_set_xtime(0);
    zzapp_test_mark_running();
    zzapp_api_enter();
    zzapp_host_tick_at(ZZAPP_API_STUCK_US);
    assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);
    assert(zzapp_runtime_kernel()->threads[0].state == ZZAPP_THREAD_STOPPED);
    zzapp_worker_hook();
    assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);

    /* should_stop and time_us are API calls. Polling only those must not
     * trip the 30 s stall watchdog. */
    zzapp_boot_init();
    zzapp_test_set_xtime(0);
    zzapp_test_mark_running();
    assert(zzapp_api_table()->should_stop() == 0);
    assert(zzapp_api_table()->time_us() == 0u);
    zzapp_host_tick_at(ZZAPP_APP_STALL_US);
    assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);

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
    assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);
    zzapp_worker_hook();
    assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);
    assert(zzapp_test_cold_restarts() == 0);
    assert(zzapp_host_address(host_visible, &host_offset) == ZZ_EINVAL);
    return 0;
}
