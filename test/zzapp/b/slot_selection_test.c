#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"

#include "host_card_pool.h"
#include "zzapp_runtime.h"

#include <assert.h>
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
static void setup(void)
{
    uint32_t slot;

    memset(windows, 0, sizeof(windows));
    request_id = 0U;
    exit_after_stop = 0;
    slot1_starts = 0U;
    run_spin_template = 0;
    spin_now = 0U;
    service_now = 0U;
    zzapp_services_test_set_time(NULL);
    stop_generation = 0U;
    host_card_pool_reset(256U << 20);
    zzapp_boot_init();
    zzapp_services_test_reset();
    for (slot = 0U; slot < 2U; slot++)
        zzapp_services_test_set_window_slot(slot, windows[slot],
                                            sizeof(windows[slot]));
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

int main(void)
{
    test_force_stop_preserves_incremental_slot1();
    test_exit_does_not_follow_incremental_load_selection();
    test_reset_reason_does_not_survive_a_new_load();
    test_stop_grace_allows_cooperative_spin_exit();
    test_spin_template_stops_within_grace();
    test_start_uses_deferred_slot_not_caller_selection();
    test_slot_reset_preserves_incremental_slot1();
    test_short_slot_request_bypasses_long_load();
    test_concurrent_loads_each_advance();
    test_advancing_load_renews_lease();
    return 0;
}
