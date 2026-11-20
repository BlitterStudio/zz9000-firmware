#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "aar/app.h"
#include "zzapp_proto.h"
#include "zzapp_runtime.h"
#include "zzapp_xport.h"
void zzapp_loader_bound(uint32_t card, uint8_t *mem, uint32_t owner,
                        uint32_t generation);
void zzapp_loader_unbound(void);
void zzapp_test_set_time_hook(uint64_t (*hook)(void));
void zzapp_kernel_switch_address_space(uint32_t pid);

/* Every scenario starts from a fresh arena with four admitted records, which
 * services does at claim: record k is pid k, and the worker walks them. */
static void boot_with_records(void)
{
    uint32_t pid;

    (zzapp_boot_init)();
    for (pid = 0U; pid < 4U; pid++)
        assert(zzapp_test_admit() == pid);
    zzapp_services_select_pid(0U);
}
#define zzapp_boot_init() boot_with_records()
#define THREAD(pid) (*zzapp_kernel_thread(zzapp_runtime_kernel(), (pid)))
static uint32_t selected_slot;
static int api_wait_hook_called;
static int api_yield_service_calls;
static uint64_t api_yield_watchdog_tick;
static uint64_t api_wait_wake_tick;
static unsigned api_yield_post_resume_clock_reads;
static uint64_t api_yield_post_resume_irq_tick;
static uint8_t replacement_mem[4096];

/* These tests look at the state between a reset request and core 1 acting on
 * it, so they only post the request. zzapp_on_amiga_reset also waits for
 * core 1's acknowledgement, and halts and recovers core 1 when it does not
 * come. */
void zzapp_test_request_reset(uint32_t slot);

static uint64_t api_yield_post_resume_time(void)
{
    api_yield_post_resume_clock_reads++;
    if (api_yield_post_resume_clock_reads == 2U) {
        zzapp_test_set_time_hook(NULL);
        zzapp_host_tick_at(api_yield_post_resume_irq_tick);
    }
    return api_yield_post_resume_irq_tick;
}

static uint32_t service_poll_depth;
static uint32_t service_poll_max_depth;
static int tick_during_service_poll;
static unsigned service_core = 1U;

static unsigned kernel_wake_events;

void zzapp_test_kernel_wake_event(void)
{
    kernel_wake_events++;
}

static void set_service_core(unsigned core)
{
    service_core = core;
}




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

uint32_t zzapp_services_current_pid(void)
{
    int slot = zzapp_kernel_current_thread();

    if (service_core == 1U && slot >= 0 && slot < 4)
        return (uint32_t)slot;
    return selected_slot;
}

int zzapp_services_select_pid(uint32_t pid)
{
    selected_slot = pid;
    return 1;
}

void zzapp_test_api_wait_hook(void)
{
    assert(zzapp_runtime_kernel()->api_lock_owner == 0);
    selected_slot = 0U;
    zzapp_kernel_set_current_thread(0);
    zzapp_api_leave();
    selected_slot = 1U;
    zzapp_kernel_set_current_thread(1);
    if (api_wait_wake_tick != 0U) {
        zzapp_test_set_xtime(api_wait_wake_tick);
        zzapp_host_tick_at(api_wait_wake_tick);
    }
    api_wait_hook_called++;
}

void zzapp_test_api_yield_hook(void)
{
    zzapp_kernel_t *kernel = zzapp_runtime_kernel();

    assert(kernel->api_lock_owner == -1);
    assert(kernel->service_due != 0U);
    assert(zzapp_kernel_schedule(kernel, 0U) == ZZAPP_KERNEL_SYSTEM);
    assert(zzapp_kernel_system_lock(kernel) == 0);
    zzapp_worker_hook();
    assert(zzapp_kernel_system_unlock(kernel) == 0);
    if (api_yield_watchdog_tick != 0U) {
        zzapp_test_set_xtime(api_yield_watchdog_tick);
        zzapp_host_tick_at(api_yield_watchdog_tick);
    }
    if (api_yield_post_resume_irq_tick != 0U) {
        api_yield_post_resume_clock_reads = 0U;
        zzapp_test_set_time_hook(api_yield_post_resume_time);
    }
    api_yield_service_calls++;
}


#define TIMER_TICKS_PER_SECOND 333333343ULL
#define HUNDRED_YEARS_SECONDS 3155760000ULL

static uint64_t divided_ticks_to_us(uint64_t ticks)
{
    return (ticks / TIMER_TICKS_PER_SECOND) * 1000000ULL +
           ((ticks % TIMER_TICKS_PER_SECOND) * 1000000ULL) /
           TIMER_TICKS_PER_SECOND;
}

static void assert_ticks_to_us(uint64_t ticks)
{
    assert(zzapp_ticks_to_us(ticks) == divided_ticks_to_us(ticks));
}

static uint64_t next_random(uint64_t *state)
{
    uint64_t value = *state;

    value ^= value << 7;
    value ^= value >> 9;
    value ^= value << 8;
    *state = value;
    return value;
}

static void test_ticks_to_us(void)
{
    const uint64_t limit = TIMER_TICKS_PER_SECOND * HUNDRED_YEARS_SECONDS;
    uint64_t random = 0xd1b54a32d192ed03ULL;
    uint64_t microsecond;
    uint32_t i;

    assert_ticks_to_us(0U);
    assert_ticks_to_us(1U);
    assert_ticks_to_us(limit - 1U);
    assert_ticks_to_us(limit);
    assert_ticks_to_us(UINT64_MAX);
    /* Every result boundary in the first second, on both sides. */
    for (microsecond = 0U; microsecond <= 1000000U; ++microsecond) {
        uint64_t boundary =
            (microsecond * TIMER_TICKS_PER_SECOND + 999999ULL) / 1000000ULL;
        uint64_t delta;

        for (delta = 0U; delta < 3U; ++delta) {
            if (boundary >= delta)
                assert_ticks_to_us(boundary - delta);
            if (boundary <= limit - delta)
                assert_ticks_to_us(boundary + delta);
        }
    }
    for (i = 0U; i < 64U; ++i) {
        uint64_t boundary = 1ULL << i;
        uint64_t delta;

        for (delta = 0U; delta < 3U; ++delta) {
            if (boundary >= delta)
                assert_ticks_to_us(boundary - delta);
            if (boundary <= limit - delta)
                assert_ticks_to_us(boundary + delta);
        }
    }
    for (i = 0U; i < 100000U; ++i)
        assert_ticks_to_us(next_random(&random) % (limit + 1U));
}

static void run_system_pass_if_due(void)
{
    zzapp_arm_frame_t frame = { 0 };
    int service_due = zzapp_runtime_kernel()->service_due != 0U;

    if (!zzapp_kernel_svc_can_return()) {
        (void)zzapp_kernel_svc_return(&frame);
        if (service_due || zzapp_kernel_current_thread() < 0) {
            zzapp_kernel_set_current_thread(-1);
            zzapp_worker_hook();
        }
    }
}

static void test_yield_and_tick_scheduling(void)
{
    zzapp_kernel_t *kernel;
    uint32_t service_calls;

    zzapp_boot_init();
    selected_slot = 0U;
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_kernel_set_current_thread(0);
    zzapp_test_mark_running();
    service_calls = zzapp_test_service_calls();
    zzapp_api_table()->yield();
    assert(zzapp_kernel_svc_can_return());
    run_system_pass_if_due();
    assert(zzapp_test_service_calls() == service_calls);

    zzapp_boot_init();
    selected_slot = 0U;
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 1U) == 0);
    zzapp_kernel_set_current_thread(0);
    zzapp_test_mark_running();
    zzapp_api_table()->yield();
    assert(!zzapp_kernel_svc_can_return());
    run_system_pass_if_due();
    assert(zzapp_kernel_current_thread() == 1);

    zzapp_boot_init();
    selected_slot = 0U;
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 1U) == 0);
    zzapp_kernel_set_current_thread(0);
    zzapp_test_mark_running();
    zzapp_host_yield();
    {
        zzapp_arm_frame_t frame = { 0 };

        (void)zzapp_kernel_switch_frame(zzapp_runtime_kernel(), &frame,
                                        ZZAPP_KERNEL_DISPATCH_TICK, 0U);
    }
    assert(zzapp_kernel_current_thread() == 1);
    assert(zzapp_kernel_svc_can_return());

    zzapp_boot_init();
    selected_slot = 0U;
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_kernel_set_current_thread(0);
    zzapp_test_mark_running();
    service_calls = zzapp_test_service_calls();
    zzapp_kernel_service_due(zzapp_runtime_kernel());
    zzapp_api_table()->yield();
    assert(!zzapp_kernel_svc_can_return());
    run_system_pass_if_due();
    assert(zzapp_test_service_calls() > service_calls);

    zzapp_boot_init();
    selected_slot = 0U;
    zzapp_test_set_xtime(0U);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_kernel_set_current_thread(-1);
    assert(zzapp_runtime_kernel()->service_due == 0U);
    zzapp_test_system_tick_at(ZZAPP_TICK_US);
    assert(zzapp_kernel_dispatch(zzapp_runtime_kernel(),
                                 ZZAPP_KERNEL_DISPATCH_TICK, 0U) ==
           ZZAPP_KERNEL_SYSTEM);

    zzapp_boot_init();
    selected_slot = 0U;
    zzapp_test_set_xtime(0U);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_kernel_set_current_thread(0);
    zzapp_test_mark_running();
    service_calls = zzapp_test_service_calls();
    zzapp_host_tick_at(ZZAPP_TICK_US);
    assert(!zzapp_kernel_svc_can_return());
    run_system_pass_if_due();
    assert(zzapp_test_service_calls() > service_calls);

    zzapp_boot_init();
    selected_slot = 0U;
    zzapp_test_set_xtime(0U);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_kernel_set_current_thread(0);
    zzapp_test_mark_running();
    kernel_wake_events = 0U;
    assert(zzapp_kernel_sleep(zzapp_runtime_kernel(), 0U, 5U) == 0);
    zzapp_host_tick_at(ZZAPP_TICK_US);
    assert(!zzapp_kernel_svc_can_return());
    zzapp_kernel_advance(zzapp_runtime_kernel(), 5U);
    assert(kernel_wake_events == 1U);
    run_system_pass_if_due();
    assert(THREAD(0).state == ZZAPP_THREAD_RUNNABLE);

    zzapp_boot_init();
    selected_slot = 0U;
    zzapp_test_set_xtime(0U);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_kernel_set_current_thread(0);
    zzapp_test_mark_running();
    zzapp_host_tick_at(ZZAPP_APP_STALL_US);
    assert(!zzapp_kernel_svc_can_return());
    run_system_pass_if_due();
    assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);

    zzapp_boot_init();
    selected_slot = 0U;
    zzapp_test_set_xtime(0U);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_kernel_set_current_thread(0);
    zzapp_test_mark_running();
    zzapp_host_latch_stop(0U);
    zzapp_host_tick_at(1U);
    zzapp_host_tick_at(ZZAPP_STOP_GRACE_US + 1U);
    assert(!zzapp_kernel_svc_can_return());
    run_system_pass_if_due();
    assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);

    zzapp_boot_init();
    selected_slot = 0U;
    zzapp_test_set_xtime(0U);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_kernel_set_current_thread(0);
    zzapp_test_mark_running();
    zzapp_kernel_service_due(zzapp_runtime_kernel());
    zzapp_host_tick_at(ZZAPP_TICK_US);
    assert(!zzapp_kernel_svc_can_return());

    zzapp_boot_init();
    selected_slot = 0U;
    zzapp_test_set_xtime(0U);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 1U) == 0);
    zzapp_kernel_set_current_thread(1);
    zzapp_test_mark_running();
    assert(zzapp_kernel_system_lock(zzapp_runtime_kernel()) == 0);
    assert(zzapp_kernel_api_lock(zzapp_runtime_kernel(), 0U) != 0);
    assert(zzapp_kernel_api_block(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_host_tick_at(ZZAPP_TICK_US);
    assert(!zzapp_kernel_svc_can_return());
    kernel = zzapp_runtime_kernel();
    assert(kernel->service_due != 0U);
    zzapp_kernel_set_current_thread(-1);
}

int main(void)
{
    uint32_t card;
    aar_shared shared;
    aar_info info;

    zzapp_boot_init();
    /*
     * This host harness cannot execute the ARM entry trampoline or read its
     * hardware FPSCR; it only verifies the baseline constant it supplies.
     */
    assert(ZZAPP_ENTRY_FPSCR == 0u);
    test_ticks_to_us();
    test_yield_and_tick_scheduling();

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
    /* An app address-space switch cannot replace the system thread's service
     * selection, and core 0 ignores core 1's current app thread. */
    zzapp_boot_init();
    selected_slot = 1U;
    zzapp_kernel_switch_address_space(0U);
    assert(selected_slot == 1U);
    zzapp_kernel_set_current_thread(-1);
    zzapp_test_mark_running();
    zzapp_kernel_set_current_thread(0);
    set_service_core(0U);
    assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);
    set_service_core(1U);
    zzapp_kernel_set_current_thread(-1);
    selected_slot = 0U;

    /* A forced stop cannot tear down an in-flight commit. The timer and
     * worker leave the app runnable until its publication completes. */
    zzapp_boot_init();
    selected_slot = 0U;
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_kernel_set_current_thread(0);
    zzapp_test_mark_running();
    zzapp_host_commit_begin();
    zzapp_test_request_reset(0U);
    zzapp_host_tick_at(ZZAPP_STOP_GRACE_US);
    zzapp_worker_hook();
    assert(THREAD(0).state == ZZAPP_THREAD_RUNNABLE);
    zzapp_host_commit_end();
    zzapp_worker_hook();
    assert(THREAD(0).state == ZZAPP_THREAD_STOPPED);
    zzapp_kernel_set_current_thread(-1);


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

    /* API yield releases the kernel lock, so due service runs before the
     * yielding body reacquires it. */
    zzapp_boot_init();
    selected_slot = 0U;
    api_yield_service_calls = 0;
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_kernel_set_current_thread(0);
    zzapp_api_enter();
    THREAD(0).quantum_left = 0U;
    zzapp_host_yield();
    assert(api_yield_service_calls == 1);
    assert(zzapp_runtime_kernel()->api_lock_owner == 0);
    zzapp_api_leave();
    zzapp_kernel_set_current_thread(-1);
    /*
     * The yielding API body is descheduled while its safe-point service pass
     * runs.  That interval is not an in-flight API call for watchdog purposes.
     */
    zzapp_boot_init();
    selected_slot = 0U;
    zzapp_test_set_xtime(0U);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_test_mark_running();
    zzapp_kernel_set_current_thread(0);
    zzapp_api_enter();
    api_yield_watchdog_tick = ZZAPP_API_STUCK_US + ZZAPP_TICK_US;
    zzapp_host_yield();
    api_yield_watchdog_tick = 0U;
    zzapp_worker_hook();
    assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);
    zzapp_api_leave();
    zzapp_kernel_set_current_thread(-1);

    /*
     * Keep the parked state through the deadline refresh.  An IRQ between
     * reacquiring the lock and publishing the renewed API deadline is safe.
     */
    zzapp_boot_init();
    selected_slot = 0U;
    zzapp_test_set_xtime(0U);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_test_mark_running();
    zzapp_kernel_set_current_thread(0);
    zzapp_api_enter();
    zzapp_test_set_xtime(ZZAPP_API_STUCK_US + ZZAPP_TICK_US);
    api_yield_post_resume_irq_tick = ZZAPP_API_STUCK_US + ZZAPP_TICK_US;
    zzapp_host_yield();
    zzapp_test_set_time_hook(NULL);
    api_yield_post_resume_irq_tick = 0U;
    assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);
    assert(THREAD(0).state == ZZAPP_THREAD_RUNNABLE);
    zzapp_api_leave();
    zzapp_kernel_set_current_thread(-1);

    /*
     * A parked API can wait longer than the ordinary app-progress deadline.
     * Returning from that wait is progress, even before the next API entry.
     */
    zzapp_boot_init();
    selected_slot = 0U;
    zzapp_test_set_xtime(0U);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_test_mark_running();
    zzapp_kernel_set_current_thread(0);
    zzapp_api_enter();
    zzapp_host_tick_at(1U);
    api_yield_watchdog_tick = ZZAPP_APP_STALL_US + ZZAPP_TICK_US;
    zzapp_host_yield();
    api_yield_watchdog_tick = 0U;
    zzapp_api_leave();
    zzapp_host_tick_at(ZZAPP_APP_STALL_US + 2U * ZZAPP_TICK_US);
    assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);
    assert(THREAD(0).state == ZZAPP_THREAD_RUNNABLE);
    zzapp_kernel_set_current_thread(-1);

    /*
     * A queued API-lock waiter may wake only after several owners have used
     * the lock.  Its wake-to-entry interval is not a stalled app.
     */
    zzapp_boot_init();
    zzapp_test_set_xtime(0U);
    selected_slot = 0U;
    zzapp_test_mark_running();
    selected_slot = 1U;
    zzapp_test_mark_running();
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 1U) == 0);
    selected_slot = 0U;
    zzapp_kernel_set_current_thread(0);
    zzapp_api_enter();
    zzapp_test_set_xtime(1000000U);
    selected_slot = 1U;
    zzapp_kernel_set_current_thread(1);
    api_wait_wake_tick = 1000000U + 3U * 9900000U;
    zzapp_api_enter();
    api_wait_wake_tick = 0U;
    assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);
    assert(THREAD(1).state == ZZAPP_THREAD_RUNNABLE);
    zzapp_api_leave();
    zzapp_kernel_set_current_thread(-1);

    /*
     * A stopped parked app must not transfer its parked-watchdog exemption
     * to the next binding in the same slot.
     */
    zzapp_boot_init();
    selected_slot = 0U;
    zzapp_test_set_xtime(0U);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_test_mark_running();
    zzapp_kernel_set_current_thread(0);
    zzapp_api_enter();
    zzapp_host_sleep(1000U);
    zzapp_loader_unbound();
    zzapp_loader_bound(0x04000000U, replacement_mem, 1U, 2U);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_test_mark_running();
    zzapp_host_tick_at(ZZAPP_APP_STALL_US);
    assert(THREAD(0).state == ZZAPP_THREAD_STOPPED);
    zzapp_kernel_set_current_thread(-1);

    /*
     * A slot can go many timer ticks without running.  A later API entry
     * starts its own watchdog interval; it must not inherit that old tick.
     */
    zzapp_boot_init();
    selected_slot = 0U;
    zzapp_test_set_xtime(0U);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_test_mark_running();
    zzapp_kernel_set_current_thread(0);
    zzapp_api_enter();
    zzapp_api_leave();
    zzapp_host_tick_at(1U);
    zzapp_test_set_xtime(ZZAPP_API_STUCK_US + 1U);
    zzapp_api_enter();
    zzapp_host_tick_at(ZZAPP_API_STUCK_US + 2U);
    assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);
    assert(THREAD(0).state == ZZAPP_THREAD_RUNNABLE);
    zzapp_api_leave();
    zzapp_kernel_set_current_thread(-1);


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
    assert(THREAD(0).state == ZZAPP_THREAD_SLEEPING);
    assert(THREAD(0).sleep_deadline == 1U);

    /* An app that executes for 4 ms before calling sleep_us(2000) must sleep
     * for ~2 ms, not return at once due to stale scheduler time. */
    zzapp_boot_init();
    zzapp_test_set_xtime(0);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_test_mark_running();
    zzapp_kernel_set_current_thread(0);
    zzapp_api_enter();
    zzapp_test_set_xtime(4000u);
    zzapp_host_sleep(2000u);
    assert(THREAD(0).state == ZZAPP_THREAD_SLEEPING);
    assert(THREAD(0).sleep_deadline == 6U);
    zzapp_kernel_advance(zzapp_runtime_kernel(), 5U);
    assert(THREAD(0).state == ZZAPP_THREAD_SLEEPING);
    zzapp_kernel_advance(zzapp_runtime_kernel(), 1U);
    assert(THREAD(0).state == ZZAPP_THREAD_RUNNABLE);
    zzapp_api_leave();
    zzapp_kernel_set_current_thread(-1);

    zzapp_boot_init();
    zzapp_test_set_wait_registered(0U, 1);
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), 0U) == 0);
    zzapp_test_mark_running();
    zzapp_kernel_set_current_thread(0);
    zzapp_api_enter();
    zzapp_host_yield_wait();
    assert(THREAD(0).state == ZZAPP_THREAD_BLOCKED);
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
        assert(THREAD(0).state == ZZAPP_THREAD_STOPPED);
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
    assert(THREAD(0).state == ZZAPP_THREAD_STOPPED);
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

    /* A fault stop is immediately visible to the app's polling API. */
    zzapp_boot_init();
    zzapp_test_set_xtime(0);
    zzapp_test_mark_running();
    assert(zzapp_api_table()->should_stop() == 0);
    zzapp_take_app_fault(ZZAPP_FAULT_DATA, 0, 0, 0, 0, 0, 0);
    assert(zzapp_api_table()->should_stop() == 1);

    /* Reset requests latch a stop until reset cleanup completes. */
    zzapp_boot_init();
    zzapp_test_set_xtime(0);
    zzapp_test_mark_running();
    zzapp_test_request_reset(0U);
    assert(zzapp_api_table()->should_stop() == 1);
    zzapp_worker_hook();
    zzapp_test_mark_running();
    assert(zzapp_api_table()->should_stop() == 0);

    /* The timer watchdog follows the same latch path as a synchronous fault. */
    zzapp_boot_init();
    zzapp_test_set_xtime(0);
    zzapp_test_mark_running();
    zzapp_host_tick_at(ZZAPP_APP_STALL_US);
    assert(zzapp_api_table()->should_stop() == 1);

    /* Core 0 latches a lease expiry without the app reading its window. */
    zzapp_boot_init();
    zzapp_test_set_xtime(0);
    zzapp_test_mark_running();
    zzapp_host_tick_at(ZZAPP_LEASE_US - 1U);
    zzapp_test_set_xtime(ZZAPP_LEASE_US + 1U);
    zzapp_core0_poll();
    assert(zzapp_api_table()->should_stop() == 1);

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
    assert(zzapp_loader_begin() == AAR_OK);
    card = zzapp_test_arena_card();
    assert(card != 0u);
    zzapp_test_mark_running();
    memset(&shared, 0, sizeof(shared));
    assert(zzapp_host_shared_alloc(4096u, 256u, 0u, &shared) == AAR_OK);
    assert(shared.ptr != NULL && shared.haddr != 0u);
    assert(shared.bytes == 4096u && (shared.flags & AAR_SHAREDF_COHERENT) != 0u);
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    assert(zzapp_host_info(&info) == AAR_OK);
    assert(info.shared_used == 4096u + ZZAPP_XPORT_PAIR_ALLOC);
    assert(info.shared_limit >= info.shared_used);
    assert(zzapp_fault_is_app(card + 4u, 0u, ZZAPP_KERNEL_APP_CPSR));
    assert(!zzapp_fault_wants_restart(card + 4u, 0u, ZZAPP_KERNEL_APP_CPSR));
    /* SYS and SVC faults inside an API body are privileged, even while
     * this app is current and the PC is in its arena. */
    assert(!zzapp_fault_is_app(card + 4u, 0u, 0x1fu));
    assert(zzapp_fault_wants_restart(card + 4u, 0u, 0x1fu));
    assert(!zzapp_fault_is_app(card + 4u, 0u, 0x13u));
    assert(zzapp_fault_wants_restart(card + 4u, 0u, 0x13u));
    assert(zzapp_fault_wants_restart(card + 4u, 0u, 0u));
    zzapp_take_app_fault(ZZAPP_FAULT_DATA, card + 4u, card + 8u,
                         card + 12u, 0x1234u, 0x5678u, 0x13u);
    assert(zzapp_runtime_state() == ZZAPP_ST_RUNNING);
    zzapp_worker_hook();
    assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);
    assert(zzapp_test_cold_restarts() == 0);
    assert(zzapp_host_shared_range(shared.ptr, 1u) == AAR_E_INVAL);
    return 0;
}
