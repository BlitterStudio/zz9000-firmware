/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Exits, restarts and resets of claims next to other claims, through the real
 * runtime, where the two cores and the scheduler can interleave:
 *   - an app that ends while another claim is part way through its load;
 *   - how apps leave: by exit with a code, by stopping inside the grace;
 *   - the loader's pacing and the lease that a progressing load renews;
 *   - the exit's teardown run on the system thread, with a scheduler tick at
 *     every point of it that could strand it;
 *   - an Amiga reset or a core 1 restart in the middle of an exit, and a
 *     core 1 that dies in the middle of a teardown, whose replay must not
 *     free a block that was handed to another owner meanwhile;
 *   - an Amiga reset that core 1 answers at once, late, or never, and the
 *     records, claims and transport blocks it has to give back before the
 *     allocator is reset.
 * The host-visible heap is the mock one (every transport block) plus a model
 * of the card allocator for the app's own blocks, so a free by a stale owner
 * is counted.
 */
#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"
#include "rt_env.h"
#include "aar_internal.h"

void zzapp_take_app_fault(uint32_t fault, uint32_t pc, uint32_t sp,
                          uint32_t lr, uint32_t far, uint32_t fsr,
                          uint32_t cpsr);
void zzapp_test_request_reset(uint32_t pid);
int spin_template_main(int argc, char **argv);

/*
 * The synthetic image commits one image frame, three 1 MiB heap frames and
 * the default one-frame stack. LOAD_END advances exactly one loader service
 * pass per worker hook. Bound its wait from that materialized work, rather
 * than assuming a fixed number of worker passes.
 */
#define TEST_HEAP_FRAMES 3U
#define TEST_IMAGE_BYTES ZZAPP_MEM_FRAME_BYTES
#define TEST_LOAD_BYTES \
    (TEST_IMAGE_BYTES + TEST_HEAP_FRAMES * ZZAPP_MEM_FRAME_BYTES + \
     ZZAPP_STACK_DEFAULT)
#define TEST_LOADER_ZERO_BYTES_PER_PASS (32U * 1024U)
#define TEST_LOADER_COPY_BYTES_PER_PASS (48U * 1024U)
#define TEST_LOADER_MAP_BYTES_PER_PASS (96U * 1024U)
#define TEST_RELOC_ENTRIES 1U
/*
 * Besides material chunks, the incremental loader needs one worker pass to
 * leave relocation extraction, allocate frames, observe zero completion, set
 * up the address space, leave copy, enter image mapping, flush a full image
 * frame, enter tail mapping, and finish the tail: 9 transitions, plus the
 * initial preparation pass that parses the ELF and allocates relocation
 * storage.
 */
#define TEST_LOADER_PHASE_PASSES (9U + 1U)
#define DIV_ROUND_UP(value, unit) (((value) + (unit) - 1U) / (unit))
#define TEST_LOAD_END_PASS_LIMIT \
    (DIV_ROUND_UP(TEST_LOAD_BYTES, TEST_LOADER_ZERO_BYTES_PER_PASS) + \
     DIV_ROUND_UP(TEST_IMAGE_BYTES, TEST_LOADER_COPY_BYTES_PER_PASS) + \
     2U * DIV_ROUND_UP(TEST_IMAGE_BYTES, TEST_LOADER_MAP_BYTES_PER_PASS) + \
     DIV_ROUND_UP(TEST_RELOC_ENTRIES, 512U) + \
     DIV_ROUND_UP(TEST_RELOC_ENTRIES, 256U) + \
     TEST_LOADER_PHASE_PASSES)
_Static_assert((uint64_t)TEST_LOAD_END_PASS_LIMIT * 1000U <
               ZZAPP_LOAD_LEASE_US,
               "synthetic LOAD_END must finish inside the load lease");

/* The record index the scheduler tests give a second thread to: no claim of
 * these tests reaches it. */
#define PROBE_PID 7U
#define APPS 8U

int smp_cpu_id(void) { return 0; }

/* ---- Models ---------------------------------------------------------------- */

/*
 * The host-visible allocator as the firmware has it for the app's own blocks:
 * it frees by address and does not know who is asking. The model remembers
 * which allocation (serial) owns each range, so a free by a stale owner is
 * counted when the address belongs to someone else by then.
 */
#define CARD_MODEL_LO 0x04000000u
#define CARD_MODEL_HI 0x07e00000u
#define CARD_MODEL_MAX 64U

struct card_block {
    uint32_t addr;
    uint32_t size;
    uint32_t serial;
};

static struct card_block card_blocks[CARD_MODEL_MAX];
static unsigned card_block_count;
static uint32_t card_next_serial = 1U;
static unsigned card_bad_frees;
static uint32_t card_last_freed;
/* 1: halt (as a dead core 1 would) after the next app block free. */
static int halt_after_free;
static jmp_buf halt_env;

static int card_overlaps(uint32_t addr, uint32_t size)
{
    unsigned i;

    for (i = 0U; i < card_block_count; i++)
        if (addr < card_blocks[i].addr + card_blocks[i].size &&
            card_blocks[i].addr < addr + size)
            return 1;
    return 0;
}

static uint32_t card_alloc_at(uint32_t addr, uint32_t bytes, uint32_t *serial)
{
    if (card_block_count == CARD_MODEL_MAX || card_overlaps(addr, bytes))
        return 0U;
    card_blocks[card_block_count].addr = addr;
    card_blocks[card_block_count].size = bytes;
    card_blocks[card_block_count].serial = card_next_serial;
    card_block_count++;
    *serial = card_next_serial++;
    return addr;
}

uint32_t zzapp_test_card_alloc(uint32_t bytes, uint32_t align,
                               uint32_t *serial)
{
    uint32_t candidate = CARD_MODEL_LO;
    unsigned i;

    bytes = (bytes + 0xfffU) & ~0xfffU;
    if (align < 0x1000U)
        align = 0x1000U;
    for (;;) {
        candidate = (candidate + align - 1U) & ~(align - 1U);
        if (candidate + bytes > CARD_MODEL_HI)
            return 0U;
        for (i = 0U; i < card_block_count; i++)
            if (candidate < card_blocks[i].addr + card_blocks[i].size &&
                card_blocks[i].addr < candidate + bytes)
                break;
        if (i == card_block_count)
            return card_alloc_at(candidate, bytes, serial);
        candidate = card_blocks[i].addr + card_blocks[i].size;
    }
}

void zzapp_test_card_free(uint32_t addr, uint32_t serial)
{
    unsigned i;

    for (i = 0U; i < card_block_count; i++)
        if (card_blocks[i].addr == addr)
            break;
    if (i == card_block_count)
        return;
    if (card_blocks[i].serial != serial)
        card_bad_frees++;
    card_blocks[i] = card_blocks[--card_block_count];
    card_last_freed = addr;
    if (halt_after_free) {
        halt_after_free = 0;
        longjmp(halt_env, 1);
    }
}

/* The allocator is initialised again, as an Amiga reset does. */
static void card_model_init(void)
{
    card_block_count = 0U;
}

/* The transport's heap: blocks freed, and whether core 1 had run its reset
 * path or been halted by then; and a halt right after the next free. */
static unsigned heap_frees;
static unsigned heap_frees_before_core1;
static int core1_ran;
static int core1_halted;
static int heap_halt_after_free;
static jmp_buf heap_halt_env;

static void heap_released(void *ptr, uint32_t card)
{
    (void)ptr;
    (void)card;
    heap_frees++;
    if (!core1_ran && !core1_halted)
        heap_frees_before_core1++;
}

static void heap_freed(uint32_t card)
{
    (void)card;
    if (heap_halt_after_free) {
        heap_halt_after_free = 0;
        longjmp(heap_halt_env, 1);
    }
}

/* The IRQ mask as the kernel sees it: an interrupt raised while it is masked
 * is taken when it is restored. */
static int test_irqs_masked;
static void (*test_pending_irq)(void);

uint32_t smp_local_irq_save(void)
{
    uint32_t prior = (uint32_t)test_irqs_masked;

    test_irqs_masked = 1;
    return prior;
}

void smp_local_irq_restore(uint32_t state)
{
    test_irqs_masked = (int)state;
    if (!test_irqs_masked && test_pending_irq) {
        void (*fn)(void) = test_pending_irq;

        test_pending_irq = NULL;
        fn();
    }
}

static void request_irq(void (*fn)(void))
{
    if (test_irqs_masked)
        test_pending_irq = fn;
    else
        fn();
}

/* A scheduler interrupt injected into the exit teardown, between its steps.
 * The real private timer switches the interrupted context away; a context the
 * scheduler never picks again loses the rest of what it was doing. */
static uint64_t kernel_ms;
static int probe_armed;
static unsigned probe_calls;
static int probe_other_thread = -1;
static int probe_stranded;
/* 1: interrupt right after the exit stopped the thread; 2: interrupt just
 * before it does. */
static int exit_boundary_armed;
static int state_at_tick = -1;
static jmp_buf stranded_env;
static int in_reset_call;
static int teardown_in_reset_call;

uint64_t zzapp_kernel_now_ms(void)
{
    return kernel_ms;
}

/* Returns whether the interrupted context runs again. */
static int interrupt_teardown(void)
{
    zzapp_kernel_t *kernel = zzapp_runtime_kernel();
    int interrupted = zzapp_kernel_current_thread();
    zzapp_arm_frame_t frame;
    zzapp_arm_frame_t *next;

    memset(&frame, 0, sizeof(frame));
    kernel->system_context_valid = 1U;
    kernel->slice_started_ms = kernel_ms;
    kernel->slice_valid = 1U;
    kernel->idle = 0U;
    /* The tick only marks service due; a teardown that ran for the service
     * budget is then preempted for the runnable app. */
    kernel_ms += ZZAPP_KERNEL_SERVICE_LIMIT_MS + 1U;
    zzapp_kernel_service_due(kernel);
    next = zzapp_kernel_switch_frame(kernel, &frame,
                                     ZZAPP_KERNEL_DISPATCH_TICK, 4U);
    if (zzapp_kernel_current_thread() != interrupted) {
        probe_other_thread = zzapp_kernel_current_thread();
        kernel_ms += ZZAPP_KERNEL_SERVICE_WINDOW_MS + 50U;
        next = zzapp_kernel_switch_frame(kernel, next,
                                         ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0U);
    }
    (void)next;
    return zzapp_kernel_current_thread() == interrupted;
}

/* The tick that lands right at the exit's stop of the thread. */
static void tick_at_stop(void)
{
    state_at_tick = (int)zzapp_rec_by_pid(rt_pid[0])->thread.state;
    if (!interrupt_teardown())
        longjmp(stranded_env, 1);
}

void __real_zzapp_kernel_stop_app(zzapp_kernel_t *kernel, uint32_t slot);

void __wrap_zzapp_kernel_stop_app(zzapp_kernel_t *kernel, uint32_t slot)
{
    int armed = exit_boundary_armed;

    exit_boundary_armed = 0;
    if (armed == 2)
        request_irq(tick_at_stop);
    __real_zzapp_kernel_stop_app(kernel, slot);
    if (armed == 1)
        request_irq(tick_at_stop);
}

void __real_zzapp_loader_retire_resources(void);
void __wrap_zzapp_loader_retire_resources(void)
{
    if (in_reset_call)
        teardown_in_reset_call = 1;
    if (probe_armed) {
        probe_armed = 0;
        probe_calls++;
        if (!interrupt_teardown()) {
            probe_stranded = 1;
            return;
        }
    }
    __real_zzapp_loader_retire_resources();
}

/* The side of core 1 while core 0 waits in an Amiga reset: it sits out
 * `core1_delay` steps, then runs its worker, whose reset path retires every
 * record. */
static unsigned core1_delay;
/* Runs at every step of core 0's wait, before core 1's turn. */
static void (*core1_step_extra)(uint32_t pid, unsigned pass);

void zzapp_test_core1_step(uint32_t pid, unsigned pass)
{
    if (core1_step_extra)
        core1_step_extra(pid, pass);
    if (pass < core1_delay)
        return;
    core1_ran = 1;
    zzapp_worker_hook();
}

/* Core 0's recovery starts by taking every lock the dead core held; in this
 * test that marks the point where core 1 is known to be halted. */
void zzapp_recover_external_locks(void)
{
    core1_halted = 1;
}

/* ---- The apps ---------------------------------------------------------------- */

static int exit_after_stop;
static int run_spin_template;
static uint64_t spin_now;
static uint64_t service_now;
static uint32_t load_host_mib;
static uint32_t app_host_alloc_mib;
static int entry_exit_code = -1;
static int entry_restart_after_exit;
static int entry_reset_after_exit;
static uint32_t entry_client;
static uint32_t entry_pids[RT_MAX_APPS];
static unsigned entry_pid_runs[APPS];
static struct rt_counts g_start;

static uint64_t spin_time(void)
{
    spin_now += 100000U;
    if (spin_now == 100000U) {
        mock_client_request_stop(&rt_cl[entry_client]);
        zzapp_host_tick_at(spin_now);
    } else if (spin_now == 600000U) {
        zzapp_host_tick_at(spin_now);
    }
    return spin_now;
}

static uint64_t service_time(void)
{
    return service_now;
}

static void app_entry(const aar_api *table)
{
    uint32_t pid = zzapp_services_current_pid();

    if (pid < APPS)
        entry_pid_runs[pid]++;
    if (run_spin_template) {
        aar_crt0_bind(table);
        zzapp_test_set_time_hook(spin_time);
        assert(spin_template_main(0, NULL) == 0);
        zzapp_test_set_time_hook(NULL);
        return;
    }
    if (app_host_alloc_mib) {
        aar_shared shared;

        memset(&shared, 0, sizeof(shared));
        assert(table->shared_alloc((uint64_t)app_host_alloc_mib << 20, 4096u,
                                   0u, &shared) == AAR_OK);
        assert(shared.ptr != NULL);
    }
    if (entry_exit_code >= 0) {
        table->exit(entry_exit_code);
        /* The exit must come out of zzapp_host_exit itself: run_app also
         * ends an entry that merely returns, which would hide an exit that
         * records nothing. */
        assert(zzapp_test_exit_state(pid) != 0U);
        if (entry_restart_after_exit)
            zzapp_on_core1_restart();
        if (entry_reset_after_exit) {
            in_reset_call = 1;
            zzapp_on_amiga_reset();
            in_reset_call = 0;
        }
        return;
    }
    if (!exit_after_stop)
        return;
    assert(rt_simple(entry_client, ZZAPP_OP_STOP) == SDK_STATUS_OK);
    zzapp_host_tick_at(1U);
    zzapp_host_tick_at(100000U);
    assert(table->should_stop());
}

/* An app that exits by returning, or runs on (rt_entry NULL). */
static void entry_returns(const aar_api *api)
{
    app_entry(api);
}

static void setup(void)
{
    unsigned i;

    rt_boot(RT_MB(256), APPS);
    card_model_init();
    card_bad_frees = 0U;
    card_last_freed = 0U;
    halt_after_free = 0;
    heap_frees = heap_frees_before_core1 = 0U;
    heap_halt_after_free = 0;
    core1_ran = core1_halted = 0;
    core1_delay = 0U;
    core1_step_extra = NULL;
    exit_after_stop = 0;
    run_spin_template = 0;
    spin_now = 0U;
    service_now = 0U;
    load_host_mib = 0U;
    app_host_alloc_mib = 0U;
    entry_exit_code = -1;
    entry_restart_after_exit = 0;
    entry_reset_after_exit = 0;
    entry_client = 0U;
    probe_armed = 0;
    probe_calls = 0U;
    probe_other_thread = -1;
    probe_stranded = 0;
    exit_boundary_armed = 0;
    state_at_tick = -1;
    in_reset_call = 0;
    teardown_in_reset_call = 0;
    test_irqs_masked = 0;
    test_pending_irq = NULL;
    kernel_ms = 0U;
    memset(entry_pids, 0, sizeof(entry_pids));
    for (i = 0U; i < APPS; i++)
        entry_pid_runs[i] = 0U;
    zzapp_services_test_set_time(NULL);
    rt_entry = entry_returns;
    rt_pass_limit = TEST_LOAD_END_PASS_LIMIT;
    g_start = rt_counts();
}

/* The loader moves at most a bounded amount per pass. */
static void pace_hook(unsigned pass)
{
    (void)pass;
    assert(zzapp_test_loader_zero_last_bytes() <= TEST_LOADER_ZERO_BYTES_PER_PASS);
}

/* The client's limits and a load of `heap_frames` heap up to LOAD_CHUNK. */
static void load_begin_and_chunk_heap(uint32_t n, uint32_t heap_frames)
{
    uint8_t image[RT_IMAGE_BYTES];

    rt_image(image, heap_frames);
    mock_client_set_limits(&rt_cl[n], 0U, load_host_mib, 0U);
    rt_pass_hook = pace_hook;
    assert(rt_load_image(n, image) == SDK_STATUS_OK);
}

static void load_begin_and_chunk(uint32_t n)
{
    load_begin_and_chunk_heap(n, TEST_HEAP_FRAMES);
}

static void finish_load(uint32_t n)
{
    assert(rt_simple(n, ZZAPP_OP_LOAD_END) == SDK_STATUS_OK);
}

/* LOAD_END posted and one pass run: the loader is under way. */
static void start_load_end(uint32_t n)
{
    uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t bytes = 0U;
    uint16_t status;

    zzapp_build_handle(req, rt_cl[n].handle);
    zzapp_services_test_set_core(0);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_END, req, sizeof(req), reply,
                                  &bytes, 1, 900U + n, 0U) == SDK_STATUS_QUEUED);
    zzapp_services_test_set_core(1);
    zzapp_kernel_set_current_thread(-1);
    zzapp_services_poll();
    assert(!zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL, &bytes));
}

static void complete_load_end(void)
{
    uint16_t status;
    uint16_t bytes = 0U;
    unsigned pass;

    zzapp_services_test_set_core(1);
    for (pass = 0U; pass < 4000U; pass++) {
        zzapp_services_poll();
        assert(zzapp_test_loader_zero_last_bytes() <= TEST_LOADER_ZERO_BYTES_PER_PASS);
        if (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL, &bytes)) {
            assert(status == SDK_STATUS_OK);
            zzapp_mailbox_harvest_ack();
            return;
        }
    }
    assert(0 && "incremental load did not finish");
}

/* START without waiting for its completion: an exit that restarts core 1 or
 * resets the card takes the request's reply with it. */
static void start_without_waiting(uint32_t n)
{
    uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t bytes = 0U;
    uint16_t status;

    zzapp_build_start_app_request(req, rt_cl[n].handle, 0U);
    zzapp_services_test_set_core(0);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_START, req, sizeof(req), reply,
                                  &bytes, 1, 800U + n, 0U) == SDK_STATUS_QUEUED);
    zzapp_services_test_set_core(1);
    zzapp_kernel_set_current_thread(-1);
    zzapp_worker_hook();
    if (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL, &bytes))
        zzapp_mailbox_harvest_ack();
}

/* A claim, loaded to the end of LOAD_END and ready to START. */
static void load_ready(uint32_t n)
{
    rt_claim(n);
    load_begin_and_chunk(n);
    finish_load(n);
}

/* The client of every claim still held lets go; everything is back. A
 * revoked shell goes under the handle LIST reports. */
static void cleanup(void)
{
    uint32_t n;

    zzapp_services_test_set_core(1);
    zzapp_kernel_set_current_thread(-1);
    for (n = 0U; n < RT_MAX_APPS; n++) {
        uint32_t listed;

        if (!rt_cl[n].attached || zzapp_rec_state(rt_pid[n]) == ZZAPP_REC_FREE)
            continue;
        listed = rt_listed_handle(rt_pid[n]);
        if (listed != 0U)
            rt_cl[n].handle = listed;
        (void)rt_simple(n, ZZAPP_OP_RELEASE);
    }
    rt_pass_hook = NULL;
    rt_settle(64U);
    rt_assert_back(g_start);
    assert(card_block_count == 0U);
    assert(card_bad_frees == 0U);
}

/* ---- Another claim part way through its load ------------------------------- */

/* App 0 runs and app 1 is part way through LOAD_END: a forced stop of app 0
 * ends app 0 only, and app 1 goes on loading. */
static void prepare_running_app0_with_incremental_app1(void)
{
    rt_entry = NULL;
    load_ready(0U);
    assert(rt_start(0U) == SDK_STATUS_OK);
    rt_claim(1U);
    load_begin_and_chunk(1U);
    start_load_end(1U);
}

static void test_force_stop_preserves_an_incremental_load(void)
{
    setup();
    prepare_running_app0_with_incremental_app1();
    zzapp_services_select_pid(rt_pid[0]);
    zzapp_take_app_fault(ZZAPP_FAULT_FORCED, 0U, 0U, 0U, 0U, 0U, 0U);
    zzapp_worker_hook();
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(RT_REASON(0U) == AAR_END_FORCED);
    complete_load_end();
    assert(RT_STATE(1U) == AAR_STATE_LOADING);
    assert(RT_REASON(1U) == AAR_END_NONE);
    zzapp_services_select_pid(rt_pid[0]);
    assert(zzapp_test_arena_card() == 0U);
    zzapp_services_select_pid(rt_pid[1]);
    assert(zzapp_test_arena_card() != 0U);
    /* App 0's slot of the world is claimed and loaded again. */
    rt_entry = entry_returns;
    zzapp_host_tick_at(10000000U);
    zzapp_worker_hook();
    load_ready(2U);
    cleanup();
}

static void test_core_reset_of_one_claim_preserves_an_incremental_load(void)
{
    setup();
    prepare_running_app0_with_incremental_app1();
    zzapp_test_request_reset(rt_pid[0]);
    zzapp_worker_hook();
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(RT_REASON(0U) == ZZAPP_END_AMIGA_RESET);
    complete_load_end();
    assert(RT_STATE(1U) == AAR_STATE_LOADING);
    assert(RT_REASON(1U) == AAR_END_NONE);
    load_ready(2U);
    cleanup();
}

/* App 0 exits while app 1 is selected and part way through its load: the exit
 * is app 0's, not whatever the selection says. */
static void test_exit_does_not_follow_the_selection(void)
{
    setup();
    load_ready(0U);
    rt_claim(1U);
    load_begin_and_chunk(1U);
    start_load_end(1U);
    zzapp_services_select_pid(rt_pid[1]);
    start_without_waiting(0U);
    zzapp_services_select_pid(rt_pid[1]);
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(RT_REASON(0U) == AAR_END_EXIT);
    assert(zzapp_services_current_pid() == rt_pid[1]);
    complete_load_end();
    assert(RT_STATE(1U) == AAR_STATE_LOADING);
    assert(RT_REASON(1U) == AAR_END_NONE);
    cleanup();
}

/* START runs the record the request named, whichever is selected. */
static void test_start_uses_the_deferred_record_not_the_selection(void)
{
    setup();
    rt_claim(0U);
    load_ready(1U);
    zzapp_services_select_pid(rt_pid[0]);
    start_without_waiting(1U);
    assert(entry_pid_runs[rt_pid[1]] == 1U);
    assert(entry_pid_runs[rt_pid[0]] == 0U);
    assert(RT_STATE(1U) == AAR_STATE_IDLE);
    assert(RT_REASON(1U) == AAR_END_EXIT);
    assert(RT_STATE(0U) == AAR_STATE_CLAIMED);
    cleanup();
}

/* An Amiga reset's reason does not follow a claim to the next one. */
static void test_reset_reason_does_not_survive_a_new_claim(void)
{
    setup();
    zzapp_on_amiga_reset();
    zzapp_worker_hook();
    rt_entry = NULL;
    load_ready(0U);
    assert(rt_start(0U) == SDK_STATUS_OK);
    zzapp_services_select_pid(rt_pid[0]);
    zzapp_take_app_fault(ZZAPP_FAULT_FORCED, 0U, 0U, 0U, 0U, 0U, 0U);
    zzapp_worker_hook();
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(RT_REASON(0U) == AAR_END_FORCED);
    cleanup();
}

/* A short request of one claim is not held up behind another claim's long
 * load: its LOAD_BEGIN completes within a few passes. */
static void test_short_request_bypasses_a_long_load(void)
{
    uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t bytes = 0U;
    uint16_t status;
    uint32_t begin_id = 700U;
    unsigned pass;

    setup();
    rt_claim(0U);
    rt_claim(1U);
    load_begin_and_chunk_heap(0U, 30U);
    start_load_end(0U);
    zzapp_build_load_begin_request(req, rt_cl[1].handle, 0U);
    zzapp_services_test_set_core(0);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, req, sizeof(req), reply,
                                  &bytes, 1, begin_id, 0U) == SDK_STATUS_QUEUED);
    zzapp_services_test_set_core(1);
    for (pass = 0U; pass < 4U; pass++) {
        uint32_t completed_id = 0U;

        zzapp_services_poll();
        if (zzapp_mailbox_harvest(&completed_id, NULL, NULL, &status, NULL,
                                  &bytes)) {
            assert(completed_id == begin_id);
            assert(status == SDK_STATUS_OK);
            zzapp_mailbox_harvest_ack();
            break;
        }
    }
    assert(pass < 4U);
    assert(RT_STATE(1U) == AAR_STATE_LOADING);
    complete_load_end();
    cleanup();
}

/* Two loads under way each make progress, and each says so in its control
 * block. */
static void test_concurrent_loads_each_advance(void)
{
    setup();
    rt_claim(0U);
    rt_claim(1U);
    load_begin_and_chunk(0U);
    load_begin_and_chunk(1U);
    start_load_end(0U);
    start_load_end(1U);
    assert(mock_client_cb_get(&rt_cl[0], AAR_CB_PROGRESS_LO) != 0U);
    assert(mock_client_cb_get(&rt_cl[1], AAR_CB_PROGRESS_LO) != 0U);
    complete_load_end();
    complete_load_end();
    cleanup();
}

/* A load that keeps advancing renews its lease: it outlives the 30 s the
 * client would otherwise have. */
static void test_advancing_load_renews_the_lease(void)
{
    unsigned pass;

    setup();
    zzapp_services_test_set_time(service_time);
    rt_claim(0U);
    load_begin_and_chunk_heap(0U, 30U);
    start_load_end(0U);
    for (pass = 0U; pass < 4U; pass++) {
        service_now += 10U * 1000U * 1000U;
        zzapp_services_test_set_core(0);
        zzapp_services_expire_claims(service_now);
        zzapp_services_test_set_core(1);
        zzapp_services_poll();
    }
    assert(RT_STATE(0U) == AAR_STATE_LOADING);
    assert(RT_REASON(0U) == AAR_END_NONE);
    assert(mock_client_cb_get(&rt_cl[0], AAR_CB_PROGRESS_LO) >= 5U);
    zzapp_services_test_set_time(NULL);
    complete_load_end();
    cleanup();
}

/* ---- How apps leave ----------------------------------------------------------- */

/* The client's STOP inside the grace is answered by a cooperative exit, not a
 * forced stop. */
static void test_stop_grace_allows_a_cooperative_exit(void)
{
    setup();
    exit_after_stop = 1;
    entry_client = 0U;
    load_ready(0U);
    assert(rt_start(0U) == SDK_STATUS_OK);
    rt_settle(6U);
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(RT_REASON(0U) == AAR_END_EXIT);
    cleanup();
}

/* The spin template (it only polls its stop request and the clock) leaves
 * within the grace. */
static void test_spin_template_stops_within_the_grace(void)
{
    setup();
    run_spin_template = 1;
    entry_client = 0U;
    load_ready(0U);
    assert(rt_start(0U) == SDK_STATUS_OK);
    rt_settle(6U);
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(RT_REASON(0U) == AAR_END_EXIT);
    cleanup();
}

/* Card kit 70: a 34 MiB host-visible app runs and ends, a later client's
 * LOAD_BEGIN is refused for a host-visible limit above the ceiling and its
 * claim is closed, and every LOAD_BEGIN after that must still be accepted. */
static void test_load_after_a_refused_limit_and_a_big_app(void)
{
    unsigned round;
    uint8_t image[RT_IMAGE_BYTES];

    setup();
    exit_after_stop = 1;
    load_host_mib = 40U;
    app_host_alloc_mib = 34U;
    entry_client = 0U;
    load_ready(0U);
    assert(rt_start(0U) == SDK_STATUS_OK);
    rt_settle(6U);
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(RT_REASON(0U) == AAR_END_EXIT);
    cleanup();

    exit_after_stop = 0;
    app_host_alloc_mib = 0U;
    load_host_mib = 0U;
    rt_image(image, TEST_HEAP_FRAMES);
    for (round = 0U; round < 3U; round++) {
        rt_claim(0U);
        mock_client_set_limits(&rt_cl[0], 0U, 63U, 0U);
        assert(mock_client_load_begin(&rt_cl[0], 0U, NULL, NULL) ==
               SDK_STATUS_NO_MEMORY);
        /* The client closes its claim. */
        assert(rt_simple(0U, ZZAPP_OP_STOP) == SDK_STATUS_OK);
        rt_settle(2U);
        assert(RT_STATE(0U) == AAR_STATE_IDLE);
        cleanup();

        /* The next client loads normally. */
        load_ready(0U);
        cleanup();
    }
}

/* Final console writes can sit behind an outstanding write, a ring wrap or a
 * full SQ when the app exits. IDLE lets the launcher release the claim, so
 * it must not become visible until the client has completed those writes. */
static int console_exit_code;
static int console_fill_sq;
static const char console_prefix[] = "PASS cleanup\n";
static const char console_summary[] = "filehash: final summary\n";

static void entry_with_console_tail(const aar_api *api)
{
    uint32_t n;

    if (console_fill_sq) {
        uint8_t msg[AAR_MSG_BYTES] = { 0 };
        while (zzapp_svc_msg_send(msg) == AAR_OK)
            ;
    }
    assert(zzapp_svc_console_write(console_prefix, sizeof(console_prefix) - 1U,
                                    &n) == AAR_OK);
    assert(n == sizeof(console_prefix) - 1U);
    assert(zzapp_svc_console_write(console_summary, sizeof(console_summary) - 1U,
                                    &n) == AAR_OK);
    assert(n == sizeof(console_summary) - 1U);
    if (console_exit_code >= 0)
        api->exit(console_exit_code);
}

static void test_exit_waits_for_the_final_console_output(void)
{
    unsigned mode;

    for (mode = 0U; mode < 3U; mode++) {
        zzapp_rec_t *rec;
        unsigned pass;

        setup();
        load_ready(0U);
        rec = rt_rec(0U);
        console_exit_code = mode == 0U ? -1 : 10;
        console_fill_sq = mode == 2U;
        rt_entry = entry_with_console_tail;
        rt_cl[0].frozen = 1;
        if (mode == 1U) {
            /* A partial client write and the ring wrap both need reposts. */
            runtime_of(rec)->console_head = ZZAPP_XPORT_CONSOLE_OUT_BYTES - 4U;
            runtime_of(rec)->console_tail = ZZAPP_XPORT_CONSOLE_OUT_BYTES - 4U;
            rt_cl[0].console_out_chunk_cap = 3U;
        }
        assert(rt_start(0U) == SDK_STATUS_OK);
        rt_settle(4U);
        assert(zzapp_rec_state(rt_pid[0]) == ZZAPP_REC_RECLAIMED);
        assert(RT_STATE(0U) == AAR_STATE_STOPPING);
        assert(rt_cl[0].console_out_len == 0U);
        /* Retirement did not keep the app's frames while waiting for the
         * client. Only its transport shell and console bytes remain. */
        assert(rt_counts().commit_free == g_start.commit_free);
        rt_cl[0].frozen = 0;
        for (pass = 0U; pass < 64U && RT_STATE(0U) != AAR_STATE_IDLE; pass++)
            rt_settle(1U);
        assert(RT_STATE(0U) == AAR_STATE_IDLE);
        assert(RT_REASON(0U) == AAR_END_EXIT);
        assert(RT_EXIT(0U) == (mode == 0U ? 0U : 10U));
        assert(rt_cl[0].console_out_len ==
               sizeof(console_prefix) + sizeof(console_summary) - 2U);
        assert(memcmp(rt_cl[0].console_out, console_prefix,
                       sizeof(console_prefix) - 1U) == 0);
        assert(memcmp(rt_cl[0].console_out + sizeof(console_prefix) - 1U,
                       console_summary, sizeof(console_summary) - 1U) == 0);
        cleanup();
    }
}

/* RELEASE must still be able to discard pending output if a client no longer
 * wants it. Waiting for that client's completion would strand the shell. */
static void test_release_discards_an_exited_apps_pending_console(void)
{
    setup();
    load_ready(0U);
    console_exit_code = -1;
    console_fill_sq = 0;
    rt_entry = entry_with_console_tail;
    rt_cl[0].frozen = 1;
    assert(rt_start(0U) == SDK_STATUS_OK);
    rt_settle(4U);
    cleanup();
}

/* ---- The exit's teardown ------------------------------------------------------ */

static void start_exiting_app(uint32_t n, int exit_code)
{
    entry_exit_code = exit_code;
    load_ready(n);
    assert(rt_start(n) == SDK_STATUS_OK);
}

static void load_and_end(uint32_t n)
{
    load_ready(n);
    cleanup();
}

/* Card kit 70: an app that exited normally holding a large host-visible block
 * kept its loader arena, and every later LOAD_BEGIN was refused. The unmap
 * that drops the arena is long enough for a scheduler tick; a teardown run on
 * the exiting thread, which exit has already stopped for good, is lost to it.
 * The teardown must run on a thread the scheduler resumes, with the other app
 * getting the CPU meanwhile. */
static void test_exit_teardown_survives_a_scheduler_interrupt(void)
{
    zzapp_kernel_t *kernel;

    setup();
    kernel = zzapp_runtime_kernel();
    assert(zzapp_kernel_prepare_app(kernel, PROBE_PID, 0x2000u, 2u, 0u, 0u,
                                    0x9000u) == 0);
    assert(zzapp_kernel_start_app(kernel, PROBE_PID) == 0);
    probe_armed = 1;
    start_exiting_app(0U, 7);

    assert(probe_calls == 1U);
    assert(!probe_stranded);
    assert(probe_other_thread == (int)PROBE_PID);
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(RT_REASON(0U) == AAR_END_EXIT);
    zzapp_services_select_pid(rt_pid[0]);
    assert(zzapp_test_arena_card() == 0U);
    assert(RT_EXIT(0U) == 7U);
    entry_exit_code = -1;
    cleanup();
    load_and_end(0U);
}

/* An app that exits again and again ends each claim with its own exit code
 * and reason, and nothing left over from the previous one runs. */
static void test_exit_state_does_not_outlive_its_claim(void)
{
    int code;

    setup();
    for (code = 1; code <= 3; code++) {
        start_exiting_app(0U, code);
        assert(RT_STATE(0U) == AAR_STATE_IDLE);
        assert(RT_REASON(0U) == AAR_END_EXIT);
        assert(RT_EXIT(0U) == (uint32_t)code);
        zzapp_services_select_pid(rt_pid[0]);
        assert(zzapp_test_arena_card() == 0U);
        assert(zzapp_test_exit_state(rt_pid[0]) == 0U);
        cleanup();
    }
    entry_exit_code = -1;
    load_and_end(0U);
}

/* A tick at the exit's stop of the thread, either side of it. The thread is
 * gone for good once stopped, so the exit must already be recorded; and the
 * system thread must not begin the teardown while the thread still runs. Both
 * hold when the interrupt is taken only once the transition ends. */
static void exit_with_interrupt_at_stop(int mode)
{
    zzapp_kernel_t *kernel;
    unsigned pass;

    setup();
    kernel = zzapp_runtime_kernel();
    assert(zzapp_kernel_prepare_app(kernel, PROBE_PID, 0x2000u, 2u, 0u, 0u,
                                    0x9000u) == 0);
    assert(zzapp_kernel_start_app(kernel, PROBE_PID) == 0);
    entry_exit_code = 4;
    load_ready(0U);
    exit_boundary_armed = mode;
    if (setjmp(stranded_env) == 0) {
        start_without_waiting(0U);
        /* Not reached once the thread is stopped; a tick that was taken
         * while it still ran lets it go on. */
        assert(state_at_tick == (int)ZZAPP_THREAD_STOPPED);
        assert(0 && "the exiting thread ran on after the interrupt");
    }
    /* Only the system thread runs from here. */
    zzapp_kernel_set_current_thread(-1);
    for (pass = 0U; pass < 3U; pass++)
        zzapp_worker_hook();
    {
        uint16_t status;
        uint16_t bytes = 0U;

        if (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL, &bytes))
            zzapp_mailbox_harvest_ack();
    }
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(RT_REASON(0U) == AAR_END_EXIT);
    zzapp_services_select_pid(rt_pid[0]);
    assert(zzapp_test_arena_card() == 0U);
    assert(zzapp_test_exit_state(rt_pid[0]) == 0U);
    assert(state_at_tick == (int)ZZAPP_THREAD_STOPPED);
    entry_exit_code = -1;
    cleanup();
    load_and_end(0U);
}

static void test_exit_survives_an_interrupt_after_the_thread_stopped(void)
{
    exit_with_interrupt_at_stop(1);
}

static void test_teardown_does_not_start_before_the_thread_stopped(void)
{
    exit_with_interrupt_at_stop(2);
}

/* An app that exited has resources until its teardown ends. A card reset
 * resets the surface allocator once core 1 has acknowledged it, so the
 * acknowledgement waits for that teardown. */
static void test_amiga_reset_waits_for_a_deferred_exit(void)
{
    uint32_t pid;

    setup();
    entry_exit_code = 6;
    entry_reset_after_exit = 1;
    load_ready(0U);
    pid = rt_pid[0];
    start_without_waiting(0U);

    assert(teardown_in_reset_call);
    assert(zzapp_test_exit_state(pid) == 0U);
    zzapp_services_select_pid(pid);
    assert(zzapp_test_arena_card() == 0U);
    /* The reset released the shell with the claim: the freed record keeps
     * its last words. */
    assert(zzapp_rec_state(pid) == ZZAPP_REC_FREE);
    assert(zzapp_rec_by_pid(pid)->claim.terminal_reason == ZZAPP_END_AMIGA_RESET);
    entry_exit_code = -1;
    entry_reset_after_exit = 0;
    cleanup();
    load_and_end(0U);
}

/* A core 1 restart between the exit and its teardown: the restart owns the
 * claim's end, and a claim loads again afterwards. */
static void test_core1_restart_during_a_deferred_exit(void)
{
    setup();
    entry_exit_code = 9;
    entry_restart_after_exit = 1;
    load_ready(0U);
    start_without_waiting(0U);

    assert(zzapp_test_exit_state(rt_pid[0]) == 0U);
    zzapp_services_select_pid(rt_pid[0]);
    assert(zzapp_test_arena_card() == 0U);
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(RT_REASON(0U) == ZZAPP_END_CORE_RESTART);
    entry_exit_code = -1;
    entry_restart_after_exit = 0;
    cleanup();
    load_and_end(0U);
}

/* A restart ends a claimed record that never loaded: its shell stays for the
 * client, and its pair goes back with the release, not before. */
static void test_restart_ends_a_claim_that_never_loaded(void)
{
    setup();
    rt_claim(0U);
    zzapp_on_core1_restart();
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(RT_REASON(0U) == ZZAPP_END_CORE_RESTART);
    assert(mock_heap.live >= 1U);
    cleanup();
    load_and_end(0U);
}

/* Core 1 dies in the middle of an exit's teardown, right after it freed a
 * host-visible block, and core 0 hands that address to someone else before
 * the restart replays the teardown. The replay must not free it again: the
 * allocator frees by address, so the block's record has to be gone by the
 * time the block is. */
static void test_restart_replay_does_not_free_a_reused_block(void)
{
    uint32_t serial;

    setup();
    load_host_mib = 2U;
    app_host_alloc_mib = 1U;
    entry_exit_code = 3;
    load_ready(0U);
    halt_after_free = 1;
    if (setjmp(halt_env) == 0) {
        start_without_waiting(0U);
        assert(0 && "the teardown ran past the block free");
    }
    assert(zzapp_test_exit_state(rt_pid[0]) != 0U);
    assert(card_last_freed != 0U);
    assert(card_alloc_at(card_last_freed, 0x100000U, &serial) == card_last_freed);

    zzapp_kernel_set_current_thread(-1);
    zzapp_on_core1_restart();
    {
        uint16_t status;
        uint16_t bytes = 0U;

        if (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL, &bytes))
            zzapp_mailbox_harvest_ack();
    }
    /* The new owner's block is still its own. */
    assert(card_bad_frees == 0U);
    assert(card_overlaps(card_last_freed, 0x100000U));
    assert(RT_STATE(0U) == AAR_STATE_IDLE);
    assert(RT_REASON(0U) == ZZAPP_END_CORE_RESTART);
    /* The claim goes straight to a new load, with no STATUS in between. */
    entry_exit_code = -1;
    app_host_alloc_mib = 0U;
    load_host_mib = 0U;
    card_model_init();
    cleanup();
    load_and_end(0U);
}

/* The same for the claim's transport: a teardown that halted right after the
 * pair was freed, replayed once the heap has a new owner of that range. The
 * record's pointer was cleared first, so the replay frees nothing again and
 * does not touch the new owner's block. */
static void test_replayed_release_does_not_free_a_reused_pair(void)
{
    zzapp_xport_env_t env;
    void *other_ptr = NULL;
    uint32_t other_card = 0U;
    uint32_t handle;
    zzapp_rec_t *rec;

    setup();
    rt_claim(0U);
    handle = rt_cl[0].handle;
    rec = rt_rec(0U);
    mock_heap_on_release = NULL;
    mock_heap_on_freed = heap_freed;
    heap_halt_after_free = 1;
    zzapp_services_test_set_core(1);
    if (setjmp(heap_halt_env) == 0) {
        /* The reset's own order: retire, then release the shell, which
         * returns the pair last. */
        zzapp_services_on_reset();
        assert(0 && "the reset ran past the pair free");
    }
    /* Core 1 died holding the slot-state lock; recovery resets it first. */
    zzapp_services_reset_core1_lock();
    /* The pointer was detached before the block went; the record is still
     * held. */
    assert(!zzapp_xport_live(rec));
    assert(zzapp_rec_state(rt_pid[0]) != ZZAPP_REC_FREE);
    assert(mock_heap.frees >= 1U);
    /* Someone else gets heap memory meanwhile. */
    env = mock_heap_env();
    assert(env.alloc(NULL, ZZAPP_XPORT_PAIR_ALLOC, ZZAPP_XPORT_PAIR_ALIGN, 0U,
                     0U, &other_ptr, &other_card) == 0);
    mock_heap_on_freed = NULL;
    /* The replay. */
    zzapp_services_on_reset();
    assert(mock_heap.bad_frees == 0U);
    assert(zzapp_rec_state(rt_pid[0]) == ZZAPP_REC_FREE);
    assert(zzapp_rec_by_handle(handle) == NULL);
    assert(mock_heap.live == 1U);
    assert(env.release(NULL, other_ptr, other_card) == 0);
    rt_assert_back(g_start);
    rt_cl[0].attached = 0;
    cleanup();
}

/* ---- Resets ------------------------------------------------------------------ */

/* An Amiga reset gives the transport of every claim back to the heap, which
 * is reset next: nothing is freed once core 1 has been through its reset
 * path, and a second reset after the allocator reset frees nothing it no
 * longer owns. */
static void test_amiga_reset_gives_the_transport_back_before_the_allocator_reset(void)
{
    setup();
    rt_claim(0U);
    rt_claim(1U);
    load_ready(2U);
    assert(mock_heap.live >= 3U);
    zzapp_on_amiga_reset();
    assert(mock_heap.live == 0U);
    assert(mock_heap.bad_frees == 0U);
    /* The allocator is reset; a later reset frees nothing again. */
    mock_heap_reset();
    zzapp_on_amiga_reset();
    assert(mock_heap.bad_frees == 0U);
    rt_assert_back(g_start);
    rt_cl[0].attached = rt_cl[1].attached = rt_cl[2].attached = 0;
    cleanup();
}

/* An Amiga reset releases the transport of a claim from core 1's reset path.
 * Core 0 posts the reset, waits for the acknowledgement and only then
 * proceeds: it must not free a pair core 1 has not finished with. Both
 * orders: core 1 answering at once, and core 1 sitting out several of core
 * 0's steps. */
static void test_amiga_reset_leaves_the_transport_to_core1(void)
{
    unsigned delay;

    for (delay = 0U; delay <= 3U; delay += 3U) {
        unsigned pairs;

        setup();
        rt_claim(0U);
        rt_claim(1U);
        pairs = mock_heap.live;
        assert(pairs >= 2U);
        mock_heap_on_release = heap_released;
        core1_delay = delay;
        core1_ran = 0;
        zzapp_on_amiga_reset();
        /* Nothing is freed before core 1 has run its reset path. */
        assert(heap_frees_before_core1 == 0U);
        assert(core1_ran);
        assert(heap_frees == pairs);
        assert(mock_heap.bad_frees == 0U);
        assert(mock_heap.live == 0U);
        rt_cl[0].attached = rt_cl[1].attached = 0;
        mock_heap_on_release = NULL;
        cleanup();
    }
}

/* Core 1 does not run at all while core 0 waits for the acknowledgement of an
 * Amiga reset, as if its worker were delayed past the wait. Core 0 must then
 * halt it and recover every claim itself, not clean up beside a worker that
 * may still run. The allocator is reset after that, and only then does the
 * delayed worker run: it has nothing left to free. */
static void reset_timeout_then_reuse(int claimed)
{
    unsigned pass;
    uint32_t pid = 0U;

    setup();
    core1_delay = UINT32_MAX;
    if (claimed) {
        rt_claim(0U);
        pid = rt_pid[0];
    }
    mock_heap_on_release = heap_released;

    zzapp_on_amiga_reset();
    assert(heap_frees_before_core1 == 0U);
    assert(!core1_ran);
    /* With no record there is nothing to wait for and nothing to recover. */
    assert(core1_halted == claimed);
    if (claimed) {
        assert(zzapp_rec_state(pid) == ZZAPP_REC_FREE);
        assert(zzapp_rec_by_pid(pid)->claim.terminal_reason == ZZAPP_END_AMIGA_RESET);
        assert(heap_frees >= 1U);
    }
    assert(mock_heap.live == 0U);

    /* The allocator is reset and its addresses go to new owners. */
    mock_heap_reset();
    card_model_init();

    /* The worker is released and runs its passes. */
    core1_delay = 0U;
    zzapp_kernel_set_current_thread(-1);
    for (pass = 0U; pass < 3U; pass++)
        zzapp_worker_hook();
    assert(mock_heap.bad_frees == 0U);
    assert(card_bad_frees == 0U);
    core1_halted = 0;
    rt_cl[0].attached = 0;
    rt_assert_back(g_start);
}

static void test_reset_timeout_recovers_idle_records_before_the_allocator_reset(void)
{
    reset_timeout_then_reuse(0);
}

static void test_reset_timeout_recovers_a_claim_before_the_allocator_reset(void)
{
    reset_timeout_then_reuse(1);
}

/* A LOAD_BEGIN posted to a claim that is gone by the time of the reset (the
 * request still sits in its deferred queue). Core 1 never answers the reset,
 * so core 0 recovers it, and the restarted worker then runs before the
 * allocator is reset: the posted request must be gone by then, or the worker
 * begins a load and allocates an arena the allocator reset orphans. */
static void test_posted_load_begin_is_cancelled_by_halted_recovery(void)
{
    uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t bytes = 0U;
    uint32_t pid;
    unsigned pass;

    setup();
    rt_claim(0U);
    pid = rt_pid[0];
    zzapp_build_load_begin_request(req, rt_cl[0].handle, 0U);
    zzapp_services_test_set_core(0);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, req, sizeof(req), reply,
                                  &bytes, 1, 55U, 0U) == SDK_STATUS_QUEUED);

    core1_delay = UINT32_MAX;
    core1_ran = 0;
    core1_halted = 0;
    zzapp_on_amiga_reset();
    assert(core1_halted);
    assert(zzapp_rec_state(pid) == ZZAPP_REC_FREE);

    /* The restarted worker, before the allocator is reset. */
    core1_delay = 0U;
    zzapp_kernel_set_current_thread(-1);
    for (pass = 0U; pass < 4U; pass++)
        zzapp_worker_hook();
    zzapp_services_select_pid(pid);
    assert(zzapp_test_arena_card() == 0U);
    assert(zzapp_loader_state() == ZZAPP_ST_IDLE);
    assert(mock_heap.live == 0U);
    core1_halted = 0;
    while (zzapp_mailbox_harvest(NULL, NULL, NULL, NULL, NULL, NULL))
        zzapp_mailbox_harvest_ack();
    rt_cl[0].attached = 0;
    rt_assert_back(g_start);
}

/* A running app whose audio close is pending is STOPPING once it is stopped,
 * which is not IDLE: its claim must still be recovered by the halted
 * recovery, or the allocator reset leaves a stale pointer for the next reset
 * to free. The audio close itself stays owed to core 0. */
static void test_halted_recovery_gives_back_the_claim_of_a_stopping_app(void)
{
    unsigned pass;
    uint32_t pid;

    setup();
    rt_entry = NULL;
    load_ready(0U);
    assert(rt_start(0U) == SDK_STATUS_OK);
    pid = rt_pid[0];
    zzapp_services_select_pid(pid);
    zzapp_services_test_set_audio_request(AUDIO_REQUEST_CLOSE);
    mock_heap_on_release = heap_released;

    core1_delay = UINT32_MAX;
    zzapp_on_amiga_reset();
    assert(core1_halted);
    assert(heap_frees_before_core1 == 0U);
    assert(mock_heap.live == 0U);
    /* The audio close is still owed to core 0: the claim was STOPPING when
     * it was stopped, not IDLE. */
    assert(runtime_of(zzapp_rec_by_pid(pid))->audio_teardown);

    mock_heap_reset();
    card_model_init();
    core1_delay = 0U;
    zzapp_kernel_set_current_thread(-1);
    for (pass = 0U; pass < 3U; pass++)
        zzapp_worker_hook();
    assert(mock_heap.bad_frees == 0U);
    assert(card_bad_frees == 0U);
    core1_halted = 0;
    rt_cl[0].attached = 0;
}

static void finish_commit_at_pass_3(uint32_t pid, unsigned pass)
{
    if (pid == rt_pid[0] && pass == 3U) {
        zzapp_services_select_pid(rt_pid[0]);
        zzapp_host_commit_end();
    }
}

/* Coordinator coverage through zzapp_on_amiga_reset. A host-visible commit in
 * flight keeps core 1 from acknowledging. One that completes within the wait
 * is acknowledged then and needs no recovery; one that does not is recovered
 * with core 1 halted, which drops the guard of the commit that can never
 * finish. */
static void test_reset_waits_for_a_commit_that_finishes(void)
{
    unsigned pairs;

    setup();
    rt_claim(0U);
    rt_claim(1U);
    pairs = mock_heap.live;
    mock_heap_on_release = heap_released;
    zzapp_services_test_set_core(1);
    zzapp_services_select_pid(rt_pid[0]);
    zzapp_host_commit_begin();
    core1_step_extra = finish_commit_at_pass_3;
    zzapp_on_amiga_reset();
    core1_step_extra = NULL;
    assert(!core1_halted);
    assert(core1_ran);
    assert(heap_frees_before_core1 == 0U);
    assert(heap_frees == pairs);
    rt_cl[0].attached = rt_cl[1].attached = 0;
    mock_heap_on_release = NULL;
    cleanup();
}

static void test_reset_recovers_a_commit_that_never_finishes(void)
{
    unsigned pairs;

    setup();
    rt_claim(0U);
    rt_claim(1U);
    pairs = mock_heap.live;
    mock_heap_on_release = heap_released;
    zzapp_services_test_set_core(1);
    zzapp_services_select_pid(rt_pid[0]);
    zzapp_host_commit_begin();
    zzapp_on_amiga_reset();
    assert(core1_halted);
    assert(heap_frees_before_core1 == 0U);
    assert(heap_frees == pairs);
    rt_cl[0].attached = rt_cl[1].attached = 0;
    mock_heap_on_release = NULL;
    core1_halted = 0;
    /* The card claims and loads again. */
    load_and_end(0U);
}

int main(void)
{
    test_force_stop_preserves_an_incremental_load();
    test_exit_does_not_follow_the_selection();
    test_reset_reason_does_not_survive_a_new_claim();
    test_stop_grace_allows_a_cooperative_exit();
    test_spin_template_stops_within_the_grace();
    test_load_after_a_refused_limit_and_a_big_app();
    test_exit_waits_for_the_final_console_output();
    test_release_discards_an_exited_apps_pending_console();
    test_exit_teardown_survives_a_scheduler_interrupt();
    test_exit_survives_an_interrupt_after_the_thread_stopped();
    test_teardown_does_not_start_before_the_thread_stopped();
    test_amiga_reset_waits_for_a_deferred_exit();
    test_core1_restart_during_a_deferred_exit();
    test_restart_ends_a_claim_that_never_loaded();
    test_restart_replay_does_not_free_a_reused_block();
    test_replayed_release_does_not_free_a_reused_pair();
    test_amiga_reset_gives_the_transport_back_before_the_allocator_reset();
    test_amiga_reset_leaves_the_transport_to_core1();
    test_reset_timeout_recovers_idle_records_before_the_allocator_reset();
    test_reset_timeout_recovers_a_claim_before_the_allocator_reset();
    test_posted_load_begin_is_cancelled_by_halted_recovery();
    test_halted_recovery_gives_back_the_claim_of_a_stopping_app();
    test_reset_waits_for_a_commit_that_finishes();
    test_reset_recovers_a_commit_that_never_finishes();
    test_exit_state_does_not_outlive_its_claim();
    test_start_uses_the_deferred_record_not_the_selection();
    test_core_reset_of_one_claim_preserves_an_incremental_load();
    test_short_request_bypasses_a_long_load();
    test_concurrent_loads_each_advance();
    test_advancing_load_renews_the_lease();
    puts("exit_reset_test: ok");
    return 0;
}
