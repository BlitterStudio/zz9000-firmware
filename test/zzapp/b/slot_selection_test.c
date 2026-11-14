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
    put32(note + 40, 3);
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
    for (pass = 0U; pass < 64U; pass++) {
        zzapp_worker_hook();
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

static void load_begin_and_chunk(uint32_t slot, uint32_t generation)
{
    uint8_t image[IMAGE_BYTES];
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];

    make_image(image);
    request_for_slot(request, slot, generation);
    assert(run_request(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request)) ==
           SDK_STATUS_OK);
    memcpy(windows[slot] + ZZAPP_STAGING_OFF, image, sizeof(image));
    request_for_slot(request, slot, generation);
    zzapp_put_be32(request + ZZAPP_CHUNK_FILE_OFF, 0U);
    zzapp_put_be32(request + ZZAPP_CHUNK_LENGTH, sizeof(image));
    zzapp_put_be32(request + ZZAPP_CHUNK_STAGING, ZZAPP_STAGING_OFF);
    assert(run_request(ZZAPP_OP_LOAD_CHUNK, request, sizeof(request)) ==
           SDK_STATUS_OK);
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

    for (pass = 0U; pass < 64U; pass++) {
        zzapp_services_poll();
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
    zzapp_put_be32(zzapp_services_window() + ZZAPP_LIFE_OFF +
                   ZZAPP_LIFE_STOP_OFF, 1U);
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

int main(void)
{
    test_force_stop_preserves_incremental_slot1();
    test_exit_does_not_follow_incremental_load_selection();
    test_reset_reason_does_not_survive_a_new_load();
    test_stop_grace_allows_cooperative_spin_exit();
    test_spin_template_stops_within_grace();
    test_start_uses_deferred_slot_not_caller_selection();
    test_slot_reset_preserves_incremental_slot1();
    return 0;
}
