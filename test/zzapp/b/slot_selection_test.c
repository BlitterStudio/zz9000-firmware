#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"

#include "host_card_pool.h"
#include "zzapp_runtime.h"

#include <assert.h>
#include <setjmp.h>
#include <string.h>

#define IMAGE_BYTES 0x200u
#define PHOFF 52u
#define LOAD_OFF 0x100u
#define NOTE_OFF 0x0a0u
#define RELA_OFF 0x0d0u
#define SHOFF 0x180u

/*
 * The synthetic v3 image commits one image frame, three 1 MiB heap frames,
 * and the default one-frame stack. LOAD_END advances exactly one loader
 * service pass per worker hook. Bound its wait from that materialized work,
 * rather than assuming a fixed number of worker passes.
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


static uint8_t windows[2][ZZAPP_WINDOW_BYTES];

int smp_cpu_id(void) { return 0; }
void smp_raw_spin_lock(volatile uint32_t *word) { *word = 1U; }
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word)
        return 0;
    *word = 1U;
    return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *word) { *word = 0U; }
static uint32_t request_id;
static int exit_after_stop;
static unsigned slot1_starts;
static int run_spin_template;
static uint64_t spin_now;
static uint64_t service_now;
static uint32_t stop_generation;
static uint32_t load_host_mib;
static uint32_t app_host_alloc_mib;
static int entry_exit_code = -1;
static int entry_restart_after_exit;
static int entry_reset_after_exit;
static int in_reset_call;
static int teardown_in_reset_call;
/* 1: interrupt right after the exit stopped the thread; 2: interrupt just
 * before it does. */
static int exit_boundary_armed;
static int state_at_tick = -1;
static jmp_buf stranded_env;

/*
 * The host-visible allocator as the firmware has it: it frees by address and
 * does not know who is asking. The model remembers which allocation (serial)
 * owns each range, so a free by a stale owner is counted when the address
 * belongs to someone else by then. Both the slot windows and the app's
 * host-visible blocks go through it.
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
/* 1: halt (as a dead core 1 would) after the next block free; 2: after the
 * next window free. */
static int halt_after_free;
static jmp_buf halt_env;
/* Core 1's side of an Amiga reset, see zzapp_test_core1_step. */
static unsigned core1_delay;
static int core1_ran;
static int core1_halted;
static unsigned window_frees;
static unsigned window_frees_before_core1;
/* Runs at every step of core 0's wait, before core 1's turn. */
static void (*core1_step_extra)(uint32_t slot, unsigned pass);

static int card_overlaps(uint32_t addr, uint32_t size)
{
    unsigned i;

    for (i = 0U; i < card_block_count; i++)
        if (addr < card_blocks[i].addr + card_blocks[i].size &&
            card_blocks[i].addr < addr + size)
            return 1;
    return 0;
}

uint32_t zzapp_test_card_alloc_at(uint32_t addr, uint32_t bytes,
                                  uint32_t *serial)
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
            return zzapp_test_card_alloc_at(candidate, bytes, serial);
        candidate = card_blocks[i].addr + card_blocks[i].size;
    }
}

void zzapp_test_card_free(uint32_t addr, uint32_t serial)
{
    unsigned i;
    int window = addr >= CARD_MODEL_HI;

    for (i = 0U; i < card_block_count; i++)
        if (card_blocks[i].addr == addr)
            break;
    if (i == card_block_count)
        return;
    if (card_blocks[i].serial != serial)
        card_bad_frees++;
    card_blocks[i] = card_blocks[--card_block_count];
    card_last_freed = addr;
    if (window) {
        window_frees++;
        if (!core1_ran && !core1_halted)
            window_frees_before_core1++;
    }
    if (halt_after_free == (window ? 2 : 1)) {
        halt_after_free = 0;
        longjmp(halt_env, 1);
    }
}

/* The allocator is initialised again, as an Amiga reset does. */
static void card_model_init(void)
{
    card_block_count = 0U;
}

static void card_model_reset(void)
{
    card_model_init();
    card_bad_frees = 0U;
    card_last_freed = 0U;
    halt_after_free = 0;
}

/* The IRQ mask as the kernel sees it: an interrupt raised while it is
 * masked is taken when it is restored. */
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

uint64_t zzapp_kernel_now_ms(void)
{
    return kernel_ms;
}

void zzapp_test_set_entry(void (*fn)(const void *api, int argc, char **argv));
void zzapp_test_mark_running(void);
void zzapp_take_app_fault(uint32_t fault, uint32_t pc, uint32_t sp,
                          uint32_t lr, uint32_t far, uint32_t fsr,
                          uint32_t cpsr);
void zzapp_host_tick_at(uint64_t now);
void zzapp_test_set_time_hook(uint64_t (*hook)(void));
void zzapp_test_request_reset(uint32_t slot);
void zzapp_crt0_bind(const zzapp_api *api);
int spin_template_main(int argc, char **argv);

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void make_image(uint8_t image[IMAGE_BYTES])
{
    uint8_t *ph;
    uint8_t *note;
    uint8_t *sh;

    memset(image, 0, IMAGE_BYTES);
    image[0] = 0x7f; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 1; image[5] = 1; image[6] = 1;
    put16(image + 16, 3);
    put16(image + 18, 40);
    put32(image + 24, LOAD_OFF);
    put32(image + 28, PHOFF);
    put32(image + 32, SHOFF);
    put32(image + 36, 0x400);
    put16(image + 40, 52);
    put16(image + 42, 32);
    put16(image + 44, 2);
    put16(image + 46, 40);
    put16(image + 48, 1);

    ph = image + PHOFF;
    put32(ph + 0, 1);
    put32(ph + 4, LOAD_OFF);
    put32(ph + 8, LOAD_OFF);
    put32(ph + 16, 4);
    put32(ph + 20, 8);
    put32(ph + 24, 1);
    ph += 32;
    put32(ph + 0, 4);
    put32(ph + 4, NOTE_OFF);
    put32(ph + 16, 44);

    note = image + NOTE_OFF;
    put32(note + 0, 6);
    put32(note + 4, 24);
    put32(note + 8, ZZAPP_NOTE_TYPE);
    memcpy(note + 12, "ZZAPP", 6);
    put32(note + 20, ZZAPP_NOTE_MAGIC);
    put32(note + 28, 3);
    put32(note + 36, ZZAPP_NOTE_FLAG_DYNAMIC_HEAP);
    put32(note + 40, TEST_HEAP_FRAMES);
    put32(image + RELA_OFF + 0, LOAD_OFF);
    put32(image + RELA_OFF + 4, 23);
    put32(image + RELA_OFF + 8, LOAD_OFF);
    sh = image + SHOFF;
    put32(sh + 4, 4);
    put32(sh + 16, RELA_OFF);
    put32(sh + 20, 12);
    put32(sh + 36, 12);
}

static void request_for_slot(uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES],
                             uint32_t slot, uint32_t generation)
{
    memset(request, 0, ZZAPP_MAILBOX_PAYLOAD_BYTES);
    zzapp_put_be32(request + ZZAPP_SEL_SLOT, slot);
    zzapp_put_be32(request + ZZAPP_SEL_GENERATION, generation);
}
static unsigned request_pass_limit(uint16_t opcode)
{
    return opcode == ZZAPP_OP_LOAD_END ? TEST_LOAD_END_PASS_LIMIT : 1U;
}


static uint16_t run_request(uint16_t opcode, const uint8_t *request,
                            uint16_t request_bytes)
{
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;
    uint16_t status;
    unsigned pass;

    assert(zzapp_mailbox_dispatch(opcode, request, request_bytes, reply,
                                  &reply_bytes, 1, ++request_id, 0U) ==
           SDK_STATUS_QUEUED);
    zzapp_kernel_set_current_thread(-1);
    for (pass = 0U; pass < request_pass_limit(opcode); pass++) {
        zzapp_worker_hook();
        assert(zzapp_test_loader_zero_last_bytes() <=
               TEST_LOADER_ZERO_BYTES_PER_PASS);
        if (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL,
                                  &reply_bytes)) {
            zzapp_mailbox_harvest_ack();
            return status;
        }
    }
    assert(0 && "deferred request did not complete");
    return SDK_STATUS_IO_ERROR;
}

static uint32_t claim_slot(uint32_t slot)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;

    request_for_slot(request, slot, 0U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
                                  sizeof(request), reply, &reply_bytes,
                                  1, ++request_id, 0U) == SDK_STATUS_OK);
    return zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
}

static void load_begin_and_chunk_heap(uint32_t slot, uint32_t generation,
                                      uint32_t heap_frames)
{
    uint8_t image[IMAGE_BYTES];
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];

    make_image(image);
    put32(image + NOTE_OFF + 40U, heap_frames);
    request_for_slot(request, slot, generation);
    zzapp_put_be32(request + ZZAPP_LOAD_HOST_VISIBLE_MIB, load_host_mib);
    assert(run_request(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request)) ==
           SDK_STATUS_OK);
    assert(zzapp_get_be32(windows[slot] + ZZAPP_LIFE_GENERATION_OFF) ==
           generation);
    memcpy(windows[slot] + ZZAPP_STAGING_OFF, image, sizeof(image));
    request_for_slot(request, slot, generation);
    zzapp_put_be32(request + ZZAPP_CHUNK_FILE_OFF, 0U);
    zzapp_put_be32(request + ZZAPP_CHUNK_LENGTH, sizeof(image));
    zzapp_put_be32(request + ZZAPP_CHUNK_STAGING, ZZAPP_STAGING_OFF);
    assert(run_request(ZZAPP_OP_LOAD_CHUNK, request, sizeof(request)) ==
           SDK_STATUS_OK);
}

static void load_begin_and_chunk(uint32_t slot, uint32_t generation)
{
    load_begin_and_chunk_heap(slot, generation, TEST_HEAP_FRAMES);
}

static void finish_load(uint32_t slot, uint32_t generation)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];

    request_for_slot(request, slot, generation);
    assert(run_request(ZZAPP_OP_LOAD_END, request, sizeof(request)) ==
           SDK_STATUS_OK);
}

static void start_load_end(uint32_t slot, uint32_t generation)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;
    uint16_t status;

    request_for_slot(request, slot, generation);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_END, request, sizeof(request),
                                  reply, &reply_bytes, 1, ++request_id, 0U) ==
           SDK_STATUS_QUEUED);
    zzapp_services_poll();
    assert(!zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL,
                                  &reply_bytes));
}

static void complete_load_end(void)
{
    uint16_t status;
    uint16_t reply_bytes = 0U;
    unsigned pass;

    for (pass = 0U; pass < TEST_LOAD_END_PASS_LIMIT; pass++) {
        zzapp_services_poll();
        assert(zzapp_test_loader_zero_last_bytes() <=
               TEST_LOADER_ZERO_BYTES_PER_PASS);
        if (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL,
                                  &reply_bytes)) {
            assert(status == SDK_STATUS_OK);
            zzapp_mailbox_harvest_ack();
            return;
        }
    }
    assert(0 && "incremental load did not finish");
}

static void list_slot(uint32_t slot, uint32_t *state, uint8_t *reason)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;

    request_for_slot(request, slot, 0U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LIST, request, sizeof(request),
                                  reply, &reply_bytes, 1, 0U, 0U) ==
           SDK_STATUS_OK);
    *state = zzapp_get_be32(reply + ZZAPP_LIST_STATE);
    *reason = reply[ZZAPP_LIST_REASONS + 1U];
}

static uint64_t spin_time(void)
{
    spin_now += 100000U;
    if (spin_now == 100000U) {
        zzapp_put_be32(zzapp_services_window() + ZZAPP_LIFE_OFF +
                       ZZAPP_LIFE_STOP_OFF, 1U);
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

static void app_entry(const void *api, int argc, char **argv)
{
    const zzapp_api *table = (const zzapp_api *)api;

    (void)argc;
    (void)argv;
    if (run_spin_template) {
        zzapp_crt0_bind(table);
        zzapp_test_set_time_hook(spin_time);
        assert(spin_template_main(0, NULL) == 0);
        zzapp_test_set_time_hook(NULL);
        return;
    }
    if (zzapp_services_current_slot() == 1U)
        slot1_starts++;
    if (app_host_alloc_mib)
        assert(table->alloc(app_host_alloc_mib << 20, 4096u,
                            ZZ_ALLOC_HOST_VISIBLE) != NULL);
    if (entry_exit_code >= 0) {
        table->exit(entry_exit_code);
        /* The exit must come out of zzapp_host_exit itself: run_app also
         * ends an entry that merely returns, which would hide an exit that
         * records nothing. */
        assert(zzapp_test_exit_state(zzapp_services_current_slot()) != 0U);
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
    {
        uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
        uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
        uint16_t reply_bytes = 0U;

        request_for_slot(request, zzapp_services_current_slot(), stop_generation);
        assert(zzapp_mailbox_dispatch(ZZAPP_OP_STOP, request, sizeof(request),
                                      reply, &reply_bytes, 1, ++request_id,
                                      0U) == SDK_STATUS_OK);
    }
    zzapp_host_tick_at(1U);
    zzapp_host_tick_at(100000U);
    assert(table->should_stop());
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

void __real_zzapp_loader_release(void);

static int interrupt_teardown(void);

/* The tick that lands right at the exit's stop of the thread. */
static void tick_at_stop(void)
{
    state_at_tick = (int)zzapp_runtime_kernel()->threads[0].state;
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

void __real_zzapp_loader_release(void);
void __wrap_zzapp_loader_release(void)
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
    __real_zzapp_loader_release();
}
static void setup(void)
{
    uint32_t slot;

    card_model_reset();
    memset(windows, 0, sizeof(windows));
    request_id = 0U;
    load_host_mib = 0U;
    app_host_alloc_mib = 0U;
    exit_after_stop = 0;
    entry_exit_code = -1;
    probe_armed = 0;
    entry_restart_after_exit = 0;
    entry_reset_after_exit = 0;
    in_reset_call = 0;
    teardown_in_reset_call = 0;
    exit_boundary_armed = 0;
    state_at_tick = -1;
    test_irqs_masked = 0;
    test_pending_irq = NULL;
    probe_calls = 0U;
    probe_other_thread = -1;
    probe_stranded = 0;
    slot1_starts = 0U;
    run_spin_template = 0;
    spin_now = 0U;
    service_now = 0U;
    zzapp_services_test_set_time(NULL);
    stop_generation = 0U;
    host_card_pool_reset(256U << 20);
    zzapp_boot_init();
    zzapp_services_test_reset();
    for (slot = 0U; slot < 2U; slot++) {
        zzapp_services_test_set_window_slot(slot, windows[slot],
                                            sizeof(windows[slot]));
        zzapp_services_test_set_window_pool(slot, windows[slot]);
    }
    zzapp_test_set_entry(app_entry);
}

static void test_exit_does_not_follow_incremental_load_selection(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;
    uint16_t status;
    uint32_t generation[2];
    uint32_t state;
    uint8_t reason;

    setup();
    generation[0] = claim_slot(0U);
    generation[1] = claim_slot(1U);
    load_begin_and_chunk(0U, generation[0]);
    finish_load(0U, generation[0]);
    load_begin_and_chunk(1U, generation[1]);
    start_load_end(1U, generation[1]);
    request_for_slot(request, 0U, generation[0]);

    assert(zzapp_mailbox_dispatch(ZZAPP_OP_START, request, sizeof(request),
                                  reply, &reply_bytes, 1, ++request_id, 0U) ==
           SDK_STATUS_QUEUED);
    zzapp_services_select_slot(1U);
    zzapp_worker_hook();
    assert(zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL,
                                 &reply_bytes));
    assert(status == SDK_STATUS_OK);
    zzapp_mailbox_harvest_ack();
    list_slot(0U, &state, &reason);
    assert(state == ZZAPP_ST_IDLE);
    assert(reason == ZZAPP_END_EXIT);
    assert(zzapp_services_current_slot() == 1U);

    complete_load_end();
    list_slot(1U, &state, &reason);
    assert(state == ZZAPP_ST_LOADING);
    assert(reason == ZZAPP_END_NONE);
    zzapp_services_select_slot(1U);
    assert(zzapp_loader_abort() == ZZ_OK);

    generation[0] = claim_slot(0U);
    load_begin_and_chunk(0U, generation[0]);
    finish_load(0U, generation[0]);
    zzapp_services_select_slot(0U);
    assert(zzapp_loader_abort() == ZZ_OK);
}

static void test_reset_reason_does_not_survive_a_new_load(void)
{
    uint32_t generation;
    uint32_t state;
    uint8_t reason;

    setup();
    zzapp_on_amiga_reset();
    zzapp_worker_hook();
    zzapp_services_test_set_window_slot(0U, windows[0], sizeof(windows[0]));
    generation = claim_slot(0U);
    load_begin_and_chunk(0U, generation);
    finish_load(0U, generation);
    zzapp_services_select_slot(0U);
    zzapp_services_set_state(ZZAPP_ST_RUNNING);
    zzapp_test_mark_running();
    zzapp_take_app_fault(ZZAPP_FAULT_FORCED, 0U, 0U, 0U, 0U, 0U, 0U);
    zzapp_worker_hook();
    list_slot(0U, &state, &reason);
    assert(state == ZZAPP_ST_IDLE);
    assert(reason == ZZAPP_END_FORCED);
}

static void test_stop_grace_allows_cooperative_spin_exit(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint32_t generation;
    uint32_t state;
    uint8_t reason;

    setup();
    exit_after_stop = 1;
    generation = claim_slot(0U);
    load_begin_and_chunk(0U, generation);
    finish_load(0U, generation);
    stop_generation = generation;
    request_for_slot(request, 0U, generation);
    assert(run_request(ZZAPP_OP_START, request, sizeof(request)) ==
           SDK_STATUS_OK);
    list_slot(0U, &state, &reason);
    assert(state == ZZAPP_ST_IDLE);
    assert(reason == ZZAPP_END_EXIT);
}

static void test_spin_template_stops_within_grace(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint32_t generation;
    uint32_t state;
    uint8_t reason;

    setup();
    run_spin_template = 1;
    generation = claim_slot(0U);
    load_begin_and_chunk(0U, generation);
    finish_load(0U, generation);
    request_for_slot(request, 0U, generation);
    assert(run_request(ZZAPP_OP_START, request, sizeof(request)) ==
           SDK_STATUS_OK);
    list_slot(0U, &state, &reason);
    assert(state == ZZAPP_ST_IDLE);
    assert(reason == ZZAPP_END_EXIT);
}

static void test_start_uses_deferred_slot_not_caller_selection(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;
    uint16_t status;
    uint32_t generation;
    uint32_t state;
    uint8_t reason;

    setup();
    (void)claim_slot(0U);
    generation = claim_slot(1U);
    load_begin_and_chunk(1U, generation);
    finish_load(1U, generation);
    request_for_slot(request, 1U, generation);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_START, request, sizeof(request),
                                  reply, &reply_bytes, 1, ++request_id, 0U) ==
           SDK_STATUS_QUEUED);
    zzapp_services_select_slot(0U);
    zzapp_worker_hook();
    assert(zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL,
                                 &reply_bytes));
    assert(status == SDK_STATUS_OK);
    zzapp_mailbox_harvest_ack();
    assert(slot1_starts == 1U);
    list_slot(1U, &state, &reason);
    assert(state == ZZAPP_ST_IDLE);
    assert(reason == ZZAPP_END_EXIT);
}


static void prepare_running_slot0_with_incremental_slot1(uint32_t generation[2])
{
    generation[0] = claim_slot(0U);
    generation[1] = claim_slot(1U);
    load_begin_and_chunk(0U, generation[0]);
    finish_load(0U, generation[0]);
    zzapp_services_select_slot(0U);
    zzapp_services_set_state(ZZAPP_ST_RUNNING);
    zzapp_test_mark_running();
    load_begin_and_chunk(1U, generation[1]);
    start_load_end(1U, generation[1]);
}

static void reload_slot0(void)
{
    uint32_t generation = claim_slot(0U);

    load_begin_and_chunk(0U, generation);
    finish_load(0U, generation);
    zzapp_services_select_slot(0U);
    assert(zzapp_loader_abort() == ZZ_OK);
}

static void test_force_stop_preserves_incremental_slot1(void)
{
    uint32_t generation[2];
    uint32_t state;
    uint8_t reason;

    setup();
    prepare_running_slot0_with_incremental_slot1(generation);
    zzapp_services_select_slot(0U);
    zzapp_take_app_fault(ZZAPP_FAULT_FORCED, 0U, 0U, 0U, 0U, 0U, 0U);
    zzapp_worker_hook();
    list_slot(0U, &state, &reason);
    assert(state == ZZAPP_ST_IDLE);
    assert(reason == ZZAPP_END_FORCED);
    complete_load_end();
    list_slot(1U, &state, &reason);
    assert(state == ZZAPP_ST_LOADING);
    assert(reason == ZZAPP_END_NONE);
    zzapp_services_select_slot(0U);
    assert(zzapp_test_arena_card() == 0U);
    zzapp_services_select_slot(1U);
    assert(zzapp_loader_abort() == ZZ_OK);
    zzapp_host_tick_at(10000000U);
    zzapp_worker_hook();
    reload_slot0();
}

static void test_slot_reset_preserves_incremental_slot1(void)
{
    uint32_t generation[2];
    uint32_t state;
    uint8_t reason;

    setup();
    prepare_running_slot0_with_incremental_slot1(generation);
    zzapp_test_request_reset(0U);
    zzapp_worker_hook();
    list_slot(0U, &state, &reason);
    assert(state == ZZAPP_ST_IDLE);
    assert(reason == ZZAPP_END_AMIGA_RESET);
    complete_load_end();
    list_slot(1U, &state, &reason);
    assert(state == ZZAPP_ST_LOADING);
    assert(reason == ZZAPP_END_NONE);
    zzapp_services_select_slot(1U);
    assert(zzapp_loader_abort() == ZZ_OK);
    zzapp_services_test_set_window_slot(0U, windows[0], sizeof(windows[0]));
    reload_slot0();
}

static void test_short_slot_request_bypasses_long_load(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;
    uint16_t status;
    uint32_t generation[2];
    uint32_t begin_id;
    unsigned pass;

    setup();
    generation[0] = claim_slot(0U);
    generation[1] = claim_slot(1U);
    load_begin_and_chunk_heap(0U, generation[0], 30U);
    start_load_end(0U, generation[0]);

    request_for_slot(request, 1U, generation[1]);
    begin_id = ++request_id;
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, request,
                                  sizeof(request), reply, &reply_bytes, 1,
                                  begin_id, 0U) == SDK_STATUS_QUEUED);
    for (pass = 0U; pass < 4U; pass++) {
        uint32_t completed_id = 0U;

        zzapp_services_poll();
        if (zzapp_mailbox_harvest(&completed_id, NULL, NULL, &status, NULL,
                                  &reply_bytes)) {
            assert(completed_id == begin_id);
            assert(status == SDK_STATUS_OK);
            zzapp_mailbox_harvest_ack();
            break;
        }
    }
    assert(pass < 4U);

    zzapp_services_select_slot(0U);
    assert(zzapp_loader_abort() == ZZ_OK);
    zzapp_services_select_slot(1U);
    assert(zzapp_loader_abort() == ZZ_OK);
}

static void test_concurrent_loads_each_advance(void)
{
    uint32_t generation[2];
    uint32_t progress[2];

    setup();
    generation[0] = claim_slot(0U);
    generation[1] = claim_slot(1U);
    load_begin_and_chunk(0U, generation[0]);
    load_begin_and_chunk(1U, generation[1]);
    start_load_end(0U, generation[0]);
    start_load_end(1U, generation[1]);

    progress[0] = zzapp_get_be32(windows[0] + ZZAPP_LIFE_OFF +
                                 ZZAPP_LIFE_PROGRESS_LO_OFF);
    progress[1] = zzapp_get_be32(windows[1] + ZZAPP_LIFE_OFF +
                                 ZZAPP_LIFE_PROGRESS_LO_OFF);
    assert(progress[0] != 0U);
    assert(progress[1] != 0U);

    zzapp_services_select_slot(0U);
    assert(zzapp_loader_abort() == ZZ_OK);
    zzapp_services_select_slot(1U);
    assert(zzapp_loader_abort() == ZZ_OK);
}

static void test_advancing_load_renews_lease(void)
{
    uint32_t generation;
    uint32_t state;
    uint8_t reason;
    unsigned pass;

    setup();
    zzapp_services_test_set_time(service_time);
    generation = claim_slot(0U);
    load_begin_and_chunk_heap(0U, generation, 30U);
    start_load_end(0U, generation);
    for (pass = 0U; pass < 4U; pass++) {
        service_now += 10U * 1000U * 1000U;
        zzapp_services_expire_claims(service_now);
        zzapp_services_poll();
    }
    list_slot(0U, &state, &reason);
    assert(state == ZZAPP_ST_LOADING);
    assert(reason == ZZAPP_END_NONE);
    assert(zzapp_get_be32(windows[0] + ZZAPP_LIFE_OFF +
                          ZZAPP_LIFE_PROGRESS_LO_OFF) >= 5U);

    zzapp_services_select_slot(0U);
    assert(zzapp_loader_abort() == ZZ_OK);
    zzapp_services_test_set_time(NULL);
}

/* Card kit 70: a 34 MiB host-visible app runs and ends, a later launcher's
 * LOAD_BEGIN is refused for a HOSTMEM above the ceiling and its handle is
 * closed, and every LOAD_BEGIN after that must still be accepted. */
static void test_load_after_refused_hostmem_and_big_app(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;
    uint32_t generation;
    uint32_t state;
    uint8_t reason;
    unsigned round;

    setup();
    exit_after_stop = 1;
    load_host_mib = 40U;
    app_host_alloc_mib = 34U;
    generation = claim_slot(0U);
    load_begin_and_chunk(0U, generation);
    finish_load(0U, generation);
    stop_generation = generation;
    request_for_slot(request, 0U, generation);
    assert(run_request(ZZAPP_OP_START, request, sizeof(request)) ==
           SDK_STATUS_OK);
    list_slot(0U, &state, &reason);
    assert(state == ZZAPP_ST_IDLE);

    exit_after_stop = 0;
    app_host_alloc_mib = 0U;
    load_host_mib = 0U;
    for (round = 0U; round < 3U; round++) {
        generation = claim_slot(0U);
        request_for_slot(request, 0U, generation);
        zzapp_put_be32(request + ZZAPP_LOAD_HOST_VISIBLE_MIB, 63U);
        assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, request,
                                      sizeof(request), reply, &reply_bytes,
                                      1, ++request_id, 0U) ==
               SDK_STATUS_NO_MEMORY);
        request_for_slot(request, 0U, generation);
        assert(zzapp_mailbox_dispatch(ZZAPP_OP_STOP, request,
                                      sizeof(request), reply, &reply_bytes,
                                      1, ++request_id, 0U) == SDK_STATUS_OK);
        zzapp_worker_hook();
        list_slot(0U, &state, &reason);
        assert(state == ZZAPP_ST_IDLE);

        /* The next launcher loads normally. */
        generation = claim_slot(0U);
        load_begin_and_chunk(0U, generation);
        finish_load(0U, generation);
        zzapp_services_select_slot(0U);
        assert(zzapp_loader_abort() == ZZ_OK);
    }
}

static void start_exiting_app(uint32_t slot, int exit_code)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint32_t generation;

    entry_exit_code = exit_code;
    generation = claim_slot(slot);
    load_begin_and_chunk(slot, generation);
    finish_load(slot, generation);
    request_for_slot(request, slot, generation);
    assert(run_request(ZZAPP_OP_START, request, sizeof(request)) ==
           SDK_STATUS_OK);
}

static void load_and_abort(uint32_t slot)
{
    uint32_t generation = claim_slot(slot);

    load_begin_and_chunk(slot, generation);
    finish_load(slot, generation);
    zzapp_services_select_slot(slot);
    assert(zzapp_loader_abort() == ZZ_OK);
}

/* Card kit 70: an app that exited normally holding a large host-visible
 * block kept slot 0's loader arena, and every later LOAD_BEGIN on the slot
 * was refused. The unmap that drops the arena is long enough for a scheduler
 * tick; a teardown run on the exiting thread, which exit has already stopped
 * for good, is lost to it. The teardown must run on a thread the scheduler
 * resumes, with the other app getting the CPU meanwhile. */
static void test_exit_teardown_survives_a_scheduler_interrupt(void)
{
    zzapp_kernel_t *kernel;
    uint32_t state;
    uint8_t reason;

    setup();
    kernel = zzapp_runtime_kernel();
    assert(zzapp_kernel_prepare_app(kernel, 1, 0x2000u, 2u, 0u, 0u,
                                    0x9000u) == 0);
    assert(zzapp_kernel_start_app(kernel, 1) == 0);
    probe_armed = 1;
    start_exiting_app(0U, 7);

    assert(probe_calls == 1U);
    assert(!probe_stranded);
    assert(probe_other_thread == 1);
    list_slot(0U, &state, &reason);
    assert(state == ZZAPP_ST_IDLE);
    assert(reason == ZZAPP_END_EXIT);
    zzapp_services_select_slot(0U);
    assert(zzapp_test_arena_card() == 0U);
    assert(zzapp_get_be32(windows[0] + ZZAPP_LIFE_OFF +
                          ZZAPP_LIFE_EXIT_OFF) == 7U);

    entry_exit_code = -1;
    load_and_abort(0U);
}

/* A slot that exits again and again ends each generation with its own exit
 * code and reason, and nothing left over from the previous one runs. */
static void test_exit_state_does_not_outlive_its_generation(void)
{
    uint32_t state;
    uint8_t reason;
    int code;

    setup();
    for (code = 1; code <= 3; code++) {
        start_exiting_app(0U, code);
        list_slot(0U, &state, &reason);
        assert(state == ZZAPP_ST_IDLE);
        assert(reason == ZZAPP_END_EXIT);
        assert(zzapp_get_be32(windows[0] + ZZAPP_LIFE_OFF +
                              ZZAPP_LIFE_EXIT_OFF) == (uint32_t)code);
        zzapp_services_select_slot(0U);
        assert(zzapp_test_arena_card() == 0U);
    }

    entry_exit_code = -1;
    load_and_abort(0U);
}

/* START without waiting for its completion: an exit that restarts core 1 or
 * resets the card takes the request's reply with it. */
static void start_without_waiting(uint32_t slot, uint32_t generation)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;
    uint16_t status;

    request_for_slot(request, slot, generation);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_START, request, sizeof(request),
                                  reply, &reply_bytes, 1, ++request_id,
                                  0U) == SDK_STATUS_QUEUED);
    zzapp_kernel_set_current_thread(-1);
    zzapp_worker_hook();
    if (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL, &reply_bytes))
        zzapp_mailbox_harvest_ack();
}

static uint32_t load_ready_slot(uint32_t slot)
{
    uint32_t generation = claim_slot(slot);

    load_begin_and_chunk(slot, generation);
    finish_load(slot, generation);
    return generation;
}

/* A tick at the exit's stop of the thread, either side of it. The thread is
 * gone for good once stopped, so the exit must already be recorded; and the
 * system thread must not begin the teardown while the thread still runs. Both
 * hold when the interrupt is taken only once the transition ends. */
static void exit_with_interrupt_at_stop(int mode)
{
    zzapp_kernel_t *kernel;
    uint32_t generation;
    uint32_t state;
    uint8_t reason;
    unsigned pass;

    setup();
    kernel = zzapp_runtime_kernel();
    assert(zzapp_kernel_prepare_app(kernel, 1, 0x2000u, 2u, 0u, 0u,
                                    0x9000u) == 0);
    assert(zzapp_kernel_start_app(kernel, 1) == 0);
    entry_exit_code = 4;
    generation = load_ready_slot(0U);
    exit_boundary_armed = mode;
    if (setjmp(stranded_env) == 0) {
        start_without_waiting(0U, generation);
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
        uint16_t reply_bytes = 0U;

        if (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL,
                                  &reply_bytes))
            zzapp_mailbox_harvest_ack();
    }
    list_slot(0U, &state, &reason);
    assert(state == ZZAPP_ST_IDLE);
    assert(reason == ZZAPP_END_EXIT);
    zzapp_services_select_slot(0U);
    assert(zzapp_test_arena_card() == 0U);
    assert(zzapp_test_exit_state(0U) == 0U);
    assert(state_at_tick == (int)ZZAPP_THREAD_STOPPED);
    entry_exit_code = -1;
    load_and_abort(0U);
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
    uint32_t generation;
    uint32_t state;
    uint8_t reason;

    setup();
    entry_exit_code = 6;
    entry_reset_after_exit = 1;
    generation = load_ready_slot(0U);
    start_without_waiting(0U, generation);

    assert(teardown_in_reset_call);
    assert(zzapp_test_exit_state(0U) == 0U);
    zzapp_services_select_slot(0U);
    assert(zzapp_test_arena_card() == 0U);
    list_slot(0U, &state, &reason);
    assert(state == ZZAPP_ST_IDLE);
    assert(reason == ZZAPP_END_AMIGA_RESET);
    entry_exit_code = -1;
    entry_reset_after_exit = 0;
    load_and_abort(0U);
}

/* A core 1 restart between the exit and its teardown: the restart owns the
 * slot's end and the slot loads again afterwards. */
static void test_core1_restart_during_a_deferred_exit(void)
{
    uint32_t generation;
    uint32_t state;
    uint8_t reason;

    setup();
    entry_exit_code = 9;
    entry_restart_after_exit = 1;
    generation = load_ready_slot(0U);
    start_without_waiting(0U, generation);

    assert(zzapp_test_exit_state(0U) == 0U);
    zzapp_services_select_slot(0U);
    assert(zzapp_test_arena_card() == 0U);
    list_slot(0U, &state, &reason);
    assert(state == ZZAPP_ST_IDLE);
    assert(reason == ZZAPP_END_CORE_RESTART);
    entry_exit_code = -1;
    entry_restart_after_exit = 0;
    load_and_abort(0U);
}

/* A restart forgets the window of a claimed slot. Its surface block has to
 * be freed with it, or the next claim asks for the occupied range. */
static void test_claim_after_a_restart_reclaims_the_window(void)
{
    uint32_t state;
    uint8_t reason;

    setup();
    (void)claim_slot(0U);
    zzapp_on_core1_restart();
    list_slot(0U, &state, &reason);
    assert(state == ZZAPP_ST_IDLE);
    assert(reason == ZZAPP_END_CORE_RESTART);
    load_and_abort(0U);
}

/* Core 1 dies in the middle of an exit's teardown, right after it freed a
 * host-visible block, and core 0 hands that address to someone else before
 * the restart replays the teardown. The replay must not free it again: the
 * allocator frees by address, so the block's record has to be gone by the
 * time the block is. */
static void test_restart_replay_does_not_free_a_reused_block(void)
{
    uint32_t generation;
    uint32_t state;
    uint8_t reason;
    uint32_t serial;

    setup();
    load_host_mib = 2U;
    app_host_alloc_mib = 1U;
    entry_exit_code = 3;
    generation = load_ready_slot(0U);
    halt_after_free = 1;
    if (setjmp(halt_env) == 0) {
        start_without_waiting(0U, generation);
        assert(0 && "the teardown ran past the block free");
    }
    assert(zzapp_test_exit_state(0U) != 0U);
    assert(card_last_freed != 0U);
    assert(zzapp_test_card_alloc_at(card_last_freed, 0x100000U, &serial) ==
           card_last_freed);

    zzapp_kernel_set_current_thread(-1);
    zzapp_on_core1_restart();
    {
        uint16_t status;
        uint16_t reply_bytes = 0U;

        if (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL,
                                  &reply_bytes))
            zzapp_mailbox_harvest_ack();
    }
    assert(card_bad_frees == 0U);
    list_slot(0U, &state, &reason);
    assert(state == ZZAPP_ST_IDLE);
    assert(reason == ZZAPP_END_CORE_RESTART);
    /* No STATUS in between: the launcher goes from claim to LOAD_BEGIN. */
    entry_exit_code = -1;
    app_host_alloc_mib = 0U;
    load_host_mib = 0U;
    load_and_abort(0U);
}

/* The same for the slot's window: a teardown that halted after the window's
 * block was freed, replayed once the address has a new owner. */
static void test_replayed_reset_does_not_free_a_reused_window(void)
{
    uint32_t serial;

    setup();
    (void)claim_slot(0U);
    zzapp_services_select_slot(0U);
    halt_after_free = 2;
    if (setjmp(halt_env) == 0) {
        zzapp_services_on_reset();
        assert(0 && "the reset ran past the window free");
    }
    assert(zzapp_test_card_alloc_at(ZZAPP_WINDOW_Z3_SLOT_ADDRESS(0U),
                                    ZZAPP_WINDOW_BYTES, &serial) != 0U);
    zzapp_services_select_slot(0U);
    zzapp_services_on_reset();
    assert(card_bad_frees == 0U);
}

/* An idle slot holds its window across an Amiga reset, which resets the
 * allocator. The window has to be given back before that and the pointer
 * dropped, or a later reset frees whatever owns the address by then. */
static void test_amiga_reset_retires_idle_windows(void)
{
    uint32_t serial;

    setup();
    zzapp_on_amiga_reset();
    card_model_init();
    assert(zzapp_test_card_alloc_at(ZZAPP_WINDOW_Z3_SLOT_ADDRESS(1U),
                                    ZZAPP_WINDOW_BYTES, &serial) != 0U);
    zzapp_services_select_slot(1U);
    zzapp_services_on_reset();
    assert(card_bad_frees == 0U);
}

/* The side of core 1 while core 0 waits in an Amiga reset: it sits out
 * `core1_delay` steps, then runs its worker, whose reset path releases the
 * slots' windows. */
void zzapp_test_core1_step(uint32_t slot, unsigned pass)
{
    if (core1_step_extra)
        core1_step_extra(slot, pass);
    if (pass < core1_delay)
        return;
    core1_ran = 1;
    zzapp_worker_hook();
}

/* An Amiga reset releases the window of an idle slot from core 1's reset
 * path. Core 0 posts the reset, waits for the acknowledgement and only then
 * proceeds: it must not free or drop a window core 1 has not finished with.
 * Both orders: core 1 answering at once, and core 1 sitting out several of
 * core 0's steps. */
static void test_amiga_reset_leaves_idle_windows_to_core1(void)
{
    unsigned delay;

    for (delay = 0U; delay <= 3U; delay += 3U) {
        setup();
        core1_delay = delay;
        core1_ran = 0;
        window_frees = 0U;
        window_frees_before_core1 = 0U;
        zzapp_on_amiga_reset();
        /* Nothing is freed before core 1 has run its reset path. */
        assert(window_frees_before_core1 == 0U);
        assert(core1_ran);
        /* Windows of slots 0 and 1, each given back once. */
        assert(window_frees == 2U);
        assert(card_bad_frees == 0U);
        core1_delay = 0U;
    }
}

/* Core 0's recovery starts by taking every lock the dead core held; in this
 * test that marks the point where core 1 is known to be halted. */
void zzapp_recover_external_locks(void)
{
    core1_halted = 1;
}

/* Core 1 does not run at all while core 0 waits for the acknowledgement of
 * an Amiga reset, as if its worker were delayed past the wait. Core 0
 * must then halt it and recover every slot itself, not clean up beside a
 * worker that may still run. The allocator is reset and its addresses reused
 * after that, and only then does the delayed worker run: it has nothing left
 * to free. */
static void reset_timeout_then_reuse(int claimed)
{
    uint32_t serial;
    uint32_t state;
    uint8_t reason;
    unsigned pass;

    setup();
    core1_delay = 1000U;
    core1_ran = 0;
    core1_halted = 0;
    window_frees = 0U;
    window_frees_before_core1 = 0U;
    if (claimed)
        (void)claim_slot(0U);

    zzapp_on_amiga_reset();
    assert(window_frees_before_core1 == 0U);
    assert(!core1_ran);
    assert(core1_halted);
    assert(window_frees == 2U);
    list_slot(0U, &state, &reason);
    assert(state == ZZAPP_ST_IDLE);
    assert(reason == ZZAPP_END_AMIGA_RESET || !claimed);

    /* The allocator is reset and both window addresses go to new owners. */
    card_model_init();
    assert(zzapp_test_card_alloc_at(ZZAPP_WINDOW_Z3_SLOT_ADDRESS(0U),
                                    ZZAPP_WINDOW_BYTES, &serial) != 0U);
    assert(zzapp_test_card_alloc_at(ZZAPP_WINDOW_Z3_SLOT_ADDRESS(1U),
                                    ZZAPP_WINDOW_BYTES, &serial) != 0U);

    /* The worker is released and runs its passes. */
    core1_delay = 0U;
    zzapp_kernel_set_current_thread(-1);
    for (pass = 0U; pass < 3U; pass++)
        zzapp_worker_hook();
    assert(card_bad_frees == 0U);
    core1_halted = 0;
}

static void test_reset_timeout_recovers_idle_slots_before_the_allocator_reset(void)
{
    reset_timeout_then_reuse(0);
}

static void test_reset_timeout_recovers_a_claimed_slot_before_the_allocator_reset(void)
{
    reset_timeout_then_reuse(1);
}

/* A LOAD_BEGIN posted to a slot that is idle by the time of the reset (the
 * slot's state is IDLE and the request still sits in its deferred queue).
 * Core 1 never answers the reset, so core 0 recovers it, and the restarted
 * worker then runs before the allocator is reset: the posted request must be
 * gone by then, or the worker begins a load and allocates an arena the
 * allocator reset orphans. */
static void test_posted_load_begin_is_cancelled_by_halted_recovery(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;
    uint32_t generation;
    unsigned pass;

    setup();
    generation = claim_slot(0U);
    request_for_slot(request, 0U, generation);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, request,
                                  sizeof(request), reply, &reply_bytes, 1,
                                  ++request_id, 0U) == SDK_STATUS_QUEUED);
    slots[0].state = ZZAPP_ST_IDLE;

    core1_delay = 1000U;
    core1_ran = 0;
    core1_halted = 0;
    zzapp_on_amiga_reset();
    assert(core1_halted);

    /* The restarted worker, before the allocator is reset. */
    core1_delay = 0U;
    zzapp_kernel_set_current_thread(-1);
    for (pass = 0U; pass < 4U; pass++)
        zzapp_worker_hook();
    zzapp_services_select_slot(0U);
    assert(zzapp_test_arena_card() == 0U);
    assert(zzapp_loader_state() == ZZAPP_ST_IDLE);
    core1_halted = 0;
}

/* A running app whose audio close is pending is STOPPING once it is stopped,
 * which is not IDLE: its window must still be retired by the halted recovery,
 * or the allocator reset leaves a stale pointer for the next reset to free. */
static void test_halted_recovery_retires_the_window_of_a_stopping_slot(void)
{
    uint32_t serial;
    unsigned pass;

    setup();
    (void)load_ready_slot(0U);
    zzapp_services_select_slot(0U);
    zzapp_test_mark_running();
    zzapp_services_test_set_audio_request(AUDIO_REQUEST_CLOSE);

    core1_delay = 1000U;
    core1_ran = 0;
    core1_halted = 0;
    window_frees = 0U;
    window_frees_before_core1 = 0U;
    zzapp_on_amiga_reset();
    assert(core1_halted);
    assert(window_frees_before_core1 == 0U);
    assert(window_frees == 2U);
    /* The audio close is still owed to core 0: the slot was STOPPING when it
     * was stopped, not IDLE. */
    assert(runtime[0].audio_teardown);

    card_model_init();
    assert(zzapp_test_card_alloc_at(ZZAPP_WINDOW_Z3_SLOT_ADDRESS(0U),
                                    ZZAPP_WINDOW_BYTES, &serial) != 0U);
    assert(zzapp_test_card_alloc_at(ZZAPP_WINDOW_Z3_SLOT_ADDRESS(1U),
                                    ZZAPP_WINDOW_BYTES, &serial) != 0U);
    core1_delay = 0U;
    zzapp_kernel_set_current_thread(-1);
    for (pass = 0U; pass < 3U; pass++)
        zzapp_worker_hook();
    assert(card_bad_frees == 0U);
    core1_halted = 0;
}

static void finish_commit_at_pass_3(uint32_t slot, unsigned pass)
{
    if (slot == 0U && pass == 3U) {
        zzapp_services_select_slot(0U);
        zzapp_host_commit_end();
    }
}

/* Coordinator coverage through zzapp_on_amiga_reset. A host-visible commit
 * in flight keeps core 1 from acknowledging. One that completes within the
 * wait is acknowledged then and needs no recovery; one that does not is
 * recovered with core 1 halted, which drops the guard of the commit that can
 * never finish. */
static void test_reset_waits_for_a_commit_that_finishes(void)
{
    setup();
    zzapp_services_select_slot(0U);
    zzapp_host_commit_begin();
    core1_step_extra = finish_commit_at_pass_3;
    core1_delay = 0U;
    core1_ran = 0;
    core1_halted = 0;
    window_frees = 0U;
    window_frees_before_core1 = 0U;
    zzapp_on_amiga_reset();
    core1_step_extra = NULL;
    assert(!core1_halted);
    assert(core1_ran);
    assert(window_frees_before_core1 == 0U);
    assert(window_frees == 2U);
}

static void test_reset_recovers_a_commit_that_never_finishes(void)
{
    setup();
    zzapp_services_select_slot(0U);
    zzapp_host_commit_begin();
    core1_delay = 0U;
    core1_ran = 0;
    core1_halted = 0;
    window_frees = 0U;
    window_frees_before_core1 = 0U;
    zzapp_on_amiga_reset();
    assert(core1_halted);
    assert(window_frees_before_core1 == 0U);
    assert(window_frees == 2U);
    /* The slot claims and loads again. */
    load_and_abort(0U);
    core1_halted = 0;
}

int main(void)
{
    test_force_stop_preserves_incremental_slot1();
    test_exit_does_not_follow_incremental_load_selection();
    test_reset_reason_does_not_survive_a_new_load();
    test_stop_grace_allows_cooperative_spin_exit();
    test_spin_template_stops_within_grace();
    test_load_after_refused_hostmem_and_big_app();
    test_exit_teardown_survives_a_scheduler_interrupt();
    test_exit_survives_an_interrupt_after_the_thread_stopped();
    test_teardown_does_not_start_before_the_thread_stopped();
    test_amiga_reset_waits_for_a_deferred_exit();
    test_core1_restart_during_a_deferred_exit();
    test_claim_after_a_restart_reclaims_the_window();
    test_restart_replay_does_not_free_a_reused_block();
    test_replayed_reset_does_not_free_a_reused_window();
    test_amiga_reset_retires_idle_windows();
    test_amiga_reset_leaves_idle_windows_to_core1();
    test_reset_timeout_recovers_idle_slots_before_the_allocator_reset();
    test_reset_timeout_recovers_a_claimed_slot_before_the_allocator_reset();
    test_posted_load_begin_is_cancelled_by_halted_recovery();
    test_halted_recovery_retires_the_window_of_a_stopping_slot();
    test_reset_waits_for_a_commit_that_finishes();
    test_reset_recovers_a_commit_that_never_finishes();
    test_exit_state_does_not_outlive_its_generation();
    test_start_uses_deferred_slot_not_caller_selection();
    test_slot_reset_preserves_incremental_slot1();
    test_short_slot_request_bypasses_long_load();
    test_concurrent_loads_each_advance();
    test_advancing_load_renews_lease();
    return 0;
}
