#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "zzapp.h"
#include "zzapp_proto.h"
#include "zzapp_runtime.h"
void *zzapp_host_alloc(uint32_t bytes, uint32_t align, uint32_t flags);
void zzapp_host_exit(int code);
int zzapp_host_mem_info(zzapp_mem_info *out);


#define IMAGE_BYTES 0x200u
#define PHOFF 52u
#define LOAD_OFF 0x100u
#define NOTE_OFF 0x0a0u
#define RELA_OFF 0x0d0u
#define SHOFF 0x180u
#define MB(n) ((uint32_t)(n) << 20)

enum teardown_path {
    TEARDOWN_EXIT,
    TEARDOWN_STOP,
    TEARDOWN_FAULT,
    TEARDOWN_STALL,
    TEARDOWN_STUCK_CALL,
    TEARDOWN_LEASE,
    TEARDOWN_AMIGA_RESET,
    TEARDOWN_CORE_RESTART,
    TEARDOWN_DIRECT_STOP_PARKED,
};

static uint8_t windows[ZZAPP_SLOT_COUNT][ZZAPP_WINDOW_BYTES];
static uint32_t request_id;
static enum teardown_path path;
static uint32_t live_slot1_commit;
static uint32_t live_slot2_commit;

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

static void make_image(uint8_t image[IMAGE_BYTES], uint32_t minor,
                       uint32_t flags)
{
    uint8_t *ph;
    uint8_t *note;
    uint8_t *sh;

    memset(image, 0, IMAGE_BYTES);
    image[0] = 0x7f; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 1; image[5] = 1; image[6] = 1;
    put16(image + 16, 3);                 /* ET_DYN */
    put16(image + 18, 40);                /* EM_ARM */
    put32(image + 24, LOAD_OFF);
    put32(image + 28, PHOFF);
    put32(image + 32, SHOFF);
    put32(image + 36, 0x400);             /* hard-float */
    put16(image + 40, 52);
    put16(image + 42, 32);
    put16(image + 44, 2);
    put16(image + 46, 40);
    put16(image + 48, 1);

    ph = image + PHOFF;
    put32(ph + 0, 1);                     /* PT_LOAD */
    put32(ph + 4, LOAD_OFF);
    put32(ph + 8, LOAD_OFF);
    put32(ph + 16, 4);
    put32(ph + 20, 8);
    put32(ph + 24, 1);                    /* PF_X */
    ph += 32;
    put32(ph + 0, 4);                     /* PT_NOTE */
    put32(ph + 4, NOTE_OFF);
    put32(ph + 16, 44);

    note = image + NOTE_OFF;
    put32(note + 0, 6);
    put32(note + 4, 24);
    put32(note + 8, ZZAPP_NOTE_TYPE);
    memcpy(note + 12, "ZZAPP", 6);
    put32(note + 20, ZZAPP_NOTE_MAGIC);
    put32(note + 28, minor);
    put32(note + 36, flags);

    put32(image + RELA_OFF + 0, LOAD_OFF);
    put32(image + RELA_OFF + 4, 23);      /* R_ARM_RELATIVE, symbol zero */
    put32(image + RELA_OFF + 8, LOAD_OFF);
    sh = image + SHOFF;
    put32(sh + 4, 4);                     /* SHT_RELA */
    put32(sh + 16, RELA_OFF);
    put32(sh + 20, 12);
    put32(sh + 36, 12);
}

static void extended_request(uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES],
                             uint32_t slot, uint32_t generation)
{
    memset(request, 0, ZZAPP_MAILBOX_PAYLOAD_BYTES);
    zzapp_put_be32(request + ZZAPP_SEL_SLOT, slot);
    zzapp_put_be32(request + ZZAPP_SEL_GENERATION, generation);
}

static uint16_t dispatch(uint16_t opcode, const uint8_t *request,
                         uint16_t request_bytes)
{
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0;
    uint16_t status;

    status = zzapp_mailbox_dispatch(opcode, request, request_bytes, reply,
                                    &reply_bytes, 1, ++request_id, 0);
    assert(status == SDK_STATUS_QUEUED);
    zzapp_worker_hook();
    assert(zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL,
                                 &reply_bytes));
    zzapp_mailbox_harvest_ack();
    return status;
}

static uint32_t claim_slot(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0;

    extended_request(request, 0U, 0U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
                                  sizeof(request), reply, &reply_bytes,
                                  1, 0U, 0U) == SDK_STATUS_OK);
    assert(reply_bytes == ZZAPP_ALLOC_EXT_REPLY_BYTES);
    return zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
}

static void load_slot(uint32_t slot, uint32_t generation, uint32_t minor,
                      uint32_t flags)
{
    uint8_t image[IMAGE_BYTES];
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];

    make_image(image, minor, flags);
    extended_request(request, slot, generation);
    assert(dispatch(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request)) ==
           SDK_STATUS_OK);
    memcpy(windows[slot] + ZZAPP_STAGING_OFF, image, sizeof(image));
    extended_request(request, slot, generation);
    zzapp_put_be32(request + ZZAPP_CHUNK_FILE_OFF, 0U);
    zzapp_put_be32(request + ZZAPP_CHUNK_LENGTH, sizeof(image));
    zzapp_put_be32(request + ZZAPP_CHUNK_STAGING, ZZAPP_STAGING_OFF);
    assert(dispatch(ZZAPP_OP_LOAD_CHUNK, request, sizeof(request)) ==
           SDK_STATUS_OK);
    extended_request(request, slot, generation);
    assert(dispatch(ZZAPP_OP_LOAD_END, request, sizeof(request)) ==
           SDK_STATUS_OK);
}

static void list_slot(uint32_t slot, uint32_t *state, uint32_t *committed,
                      uint8_t *reason)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0;

    extended_request(request, slot, 0U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LIST, request, sizeof(request),
                                  reply, &reply_bytes, 1, 0U, 0U) ==
           SDK_STATUS_OK);
    assert(reply_bytes == ZZAPP_LIST_REPLY_BYTES);
    *state = zzapp_get_be32(reply + ZZAPP_LIST_STATE);
    *committed = zzapp_get_be32(reply + ZZAPP_LIST_COMMITTED);
    *reason = reply[ZZAPP_LIST_REASONS + 1U];
}

static void select_test_slot(uint32_t slot)
{
    zzapp_services_select_slot(slot);
}

static void request_stop(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0;

    extended_request(request, 1U, zzapp_current_generation());
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_STOP, request, sizeof(request),
                                  reply, &reply_bytes, 1, 0U, 0U) ==
           SDK_STATUS_OK);
}

static void running_slot2(uint32_t generation)
{
    select_test_slot(2U);
    assert(zzapp_proc_slot(2U)->memory.committed == ZZAPP_ARENA_BYTES);
    zzapp_services_set_state(ZZAPP_ST_RUNNING);
    zzapp_test_mark_running();
    assert(zzapp_current_generation() == generation);
    select_test_slot(1U);
}

static void app_entry(const void *api, int argc, char **argv)
{
    void *old_break;
    void *host_visible;
    uint8_t *window;

    (void)api;
    (void)argc;
    (void)argv;
    assert(zzapp_proc_heap_grow(MB(1), &old_break) == ZZ_OK);
    assert(old_break != NULL);
    host_visible = zzapp_host_alloc(4096U, 256U, ZZ_ALLOC_HOST_VISIBLE);
    assert(host_visible != NULL);
    window = zzapp_services_window();
    assert(window != NULL);
    zzapp_put_be32(window + ZZAPP_LIFE_FLAGS_OFF,
                   ZZAPP_F_DISPLAY | ZZAPP_F_VSYNC);
    zzapp_put_be32(window + ZZAPP_LIFE_CARD_FRONT_OFF, 0x00200000U);
    zzapp_put_be32(window + ZZAPP_LIFE_CARD_BACK_OFF, 0x00220000U);
    live_slot1_commit = zzapp_proc_slot(1U)->memory.committed;
    live_slot2_commit = zzapp_proc_slot(2U)->memory.committed;
    assert(live_slot1_commit == MB(3));
    assert(live_slot2_commit == ZZAPP_ARENA_BYTES);

    switch (path) {
    case TEARDOWN_EXIT:
        zzapp_host_exit(0);
        break;
    case TEARDOWN_STOP:
        request_stop();
        zzapp_host_tick_at(1U);
        zzapp_host_tick_at(ZZAPP_STOP_GRACE_US + 1U);
        break;
    case TEARDOWN_FAULT:
        zzapp_take_app_fault(ZZAPP_FAULT_DATA, 1U, 2U, 3U, 4U, 5U, 0U);
        break;
    case TEARDOWN_STALL:
        zzapp_host_tick_at(ZZAPP_APP_STALL_US);
        break;
    case TEARDOWN_STUCK_CALL:
        zzapp_api_enter();
        zzapp_host_tick_at(1U);
        zzapp_host_tick_at(ZZAPP_API_STUCK_US + 1U);
        break;
    case TEARDOWN_LEASE:
        zzapp_kernel_set_current_thread(-1);
        zzapp_test_set_xtime(ZZAPP_LEASE_US - 1U);
        zzapp_host_tick_at(ZZAPP_LEASE_US - 1U);
        select_test_slot(2U);
        zzapp_host_tick_at(ZZAPP_LEASE_US - 1U);
        zzapp_put_be32(zzapp_services_window() + ZZAPP_LIFE_HEARTBEAT_OFF, 1U);
        zzapp_core0_poll();
        select_test_slot(1U);
        zzapp_test_set_xtime(ZZAPP_LEASE_US);
        zzapp_core0_poll();
        zzapp_host_tick_at(ZZAPP_LEASE_US);
        zzapp_host_tick_at(ZZAPP_LEASE_US + ZZAPP_STOP_GRACE_US);
        break;
    case TEARDOWN_AMIGA_RESET:
        zzapp_kernel_set_current_thread(-1);
        zzapp_on_amiga_reset();
        break;
    case TEARDOWN_CORE_RESTART:
        zzapp_kernel_set_current_thread(-1);
        zzapp_on_core1_restart();
        break;
    case TEARDOWN_DIRECT_STOP_PARKED:
        /* The system thread is active while slot 1 waits.  ZZRun writes
         * slot 1's lifecycle word directly, without APP_STOP. */
        assert(zzapp_kernel_block(zzapp_runtime_kernel(), 1U) == 0);
        zzapp_kernel_set_current_thread(-1);
        select_test_slot(2U);
        zzapp_put_be32(windows[1] + ZZAPP_LIFE_OFF + ZZAPP_LIFE_STOP_OFF, 1U);
        zzapp_host_tick_at(ZZAPP_TICK_US);
        zzapp_worker_hook();
        assert(zzapp_runtime_kernel()->threads[1].state == ZZAPP_THREAD_BLOCKED);
        zzapp_host_tick_at(ZZAPP_TICK_US + ZZAPP_STOP_GRACE_US);
        zzapp_worker_hook();
        assert(zzapp_runtime_kernel()->threads[1].state == ZZAPP_THREAD_STOPPED);
        break;
    }
}

static void assert_slot1_reclaimed(uint8_t expected_reason)
{
    zzapp_mem_info memory;
    uint32_t state;
    uint32_t committed;
    uint8_t reason;
    uint8_t *window;

    select_test_slot(1U);
    list_slot(1U, &state, &committed, &reason);
    assert(state == ZZAPP_ST_IDLE);
    assert(reason == expected_reason);
    assert(committed == 0U);
    assert(zzapp_proc_slot(1U)->memory.committed == 0U);
    assert(zzapp_proc_slot(1U)->memory.frame_count == 0U);
    assert(zzapp_proc_slot(1U)->as.active == 0U);
    assert(zzapp_proc_slot(1U)->as.l1 == NULL);
    assert(zzapp_host_mem_info(&memory) == ZZ_OK);
    assert(memory.arena_size == 0U);
    assert(memory.host_visible_used == 0U);
    window = zzapp_services_window();
    if (expected_reason == ZZAPP_END_AMIGA_RESET) {
        assert(window == NULL);
    } else {
        /* Terminal status remains readable, but app-owned display state does
         * not survive the released slot. */
        assert(window != NULL);
        assert(zzapp_get_be32(window + ZZAPP_LIFE_CARD_FRONT_OFF) == 0U);
        assert(zzapp_get_be32(window + ZZAPP_LIFE_CARD_BACK_OFF) == 0U);
    }
}

static void assert_slot2_unchanged(void)
{
    zzapp_mem_info memory;
    uint32_t state;
    uint32_t committed;
    uint8_t reason;

    select_test_slot(2U);
    list_slot(2U, &state, &committed, &reason);
    assert(state == ZZAPP_ST_RUNNING);
    assert(reason == ZZAPP_END_NONE);
    assert(committed == live_slot2_commit);
    assert(zzapp_proc_slot(2U)->memory.committed == live_slot2_commit);
    assert(zzapp_host_mem_info(&memory) == ZZ_OK);
    assert(memory.arena_size == ZZAPP_ARENA_BYTES);
}

static uint8_t expected_reason(enum teardown_path value)
{
    static const uint8_t reasons[] = {
        ZZAPP_END_EXIT,
        ZZAPP_END_FORCED,
        ZZAPP_END_USER_FAULT,
        ZZAPP_END_STALL,
        ZZAPP_END_STUCK_CALL,
        ZZAPP_END_LEASE,
        ZZAPP_END_AMIGA_RESET,
        ZZAPP_END_CORE_RESTART,
        ZZAPP_END_FORCED,
    };

    return reasons[value];
}

static void run_case(enum teardown_path value)
{
    uint32_t generation[3];
    uint32_t slot;

    memset(windows, 0, sizeof(windows));
    request_id = 0U;
    live_slot1_commit = 0U;
    live_slot2_commit = 0U;
    path = value;
    zzapp_boot_init();
    zzapp_services_test_reset();
    for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++)
        zzapp_services_test_set_window_slot(slot, windows[slot],
                                            sizeof(windows[slot]));
    zzapp_test_set_xtime(0U);
    zzapp_test_set_entry(app_entry);

    generation[0] = claim_slot();
    generation[1] = claim_slot();
    generation[2] = claim_slot();
    load_slot(1U, generation[1], 3U, ZZAPP_NOTE_FLAG_DYNAMIC_HEAP);
    load_slot(2U, generation[2], 2U, 0U);
    running_slot2(generation[2]);

    {
        uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
        uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
        uint16_t reply_bytes = 0;
        uint16_t status;

        extended_request(request, 1U, generation[1]);
        status = zzapp_mailbox_dispatch(ZZAPP_OP_START, request,
                                        sizeof(request), reply, &reply_bytes,
                                        1, ++request_id, 0U);
        assert(status == SDK_STATUS_QUEUED);
        zzapp_worker_hook();
        zzapp_worker_hook();
        if (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL,
                                  &reply_bytes)) {
            assert(status == SDK_STATUS_OK);
            zzapp_mailbox_harvest_ack();
        } else {
            assert(value == TEARDOWN_AMIGA_RESET ||
                   value == TEARDOWN_CORE_RESTART);
        }
    }

    assert(live_slot1_commit == MB(3));
    if (value == TEARDOWN_AMIGA_RESET) {
        select_test_slot(1U);
        zzapp_worker_hook();
    }
    assert_slot1_reclaimed(expected_reason(value));
    if (value == TEARDOWN_AMIGA_RESET || value == TEARDOWN_CORE_RESTART) {
        uint32_t state;
        uint32_t committed;
        uint8_t reason;

        select_test_slot(2U);
        list_slot(2U, &state, &committed, &reason);
        assert(state == ZZAPP_ST_IDLE);
        assert(committed == 0U);
        assert(reason == expected_reason(value));
    } else {
        assert_slot2_unchanged();
        zzapp_on_core1_restart();
    }
}

static void test_abort_republishes_service_state(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0;
    uint32_t generation;
    uint32_t state;
    uint32_t committed;
    uint8_t reason;

    memset(windows, 0, sizeof(windows));
    request_id = 0U;
    zzapp_boot_init();
    zzapp_services_test_reset();
    zzapp_services_test_set_window_slot(0U, windows[0], sizeof(windows[0]));
    generation = claim_slot();
    extended_request(request, 0U, generation);
    assert(dispatch(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request)) ==
           SDK_STATUS_OK);
    select_test_slot(0U);
    list_slot(0U, &state, &committed, &reason);
    assert(state == ZZAPP_ST_LOADING);
    assert(zzapp_loader_abort() == ZZ_OK);
    list_slot(0U, &state, &committed, &reason);
    assert(state == ZZAPP_ST_IDLE);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    extended_request(request, 0U, 0U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request, sizeof(request),
                                  reply, &reply_bytes, 1, 9U, 0U) ==
           SDK_STATUS_OK);
    assert(zzapp_get_be32(reply + ZZAPP_ALLOC_SLOT) == 0U);
}

static void test_stopped_load_aborts_when_another_slot_is_selected(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0;
    uint32_t generation[2];
    uint32_t state;
    uint32_t committed;
    uint8_t reason;
    uint16_t status;

    memset(windows, 0, sizeof(windows));
    request_id = 0U;
    zzapp_boot_init();
    zzapp_services_test_reset();
    zzapp_services_test_set_window_slot(0U, windows[0], sizeof(windows[0]));
    zzapp_services_test_set_window_slot(1U, windows[1], sizeof(windows[1]));
    generation[0] = claim_slot();
    generation[1] = claim_slot();
    extended_request(request, 0U, generation[0]);
    assert(dispatch(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request)) ==
           SDK_STATUS_OK);
    extended_request(request, 1U, generation[1]);
    status = zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request),
                                    reply, &reply_bytes, 1, 20U, 0U);
    assert(status == SDK_STATUS_QUEUED);
    zzapp_put_be32(windows[0] + ZZAPP_LIFE_OFF + ZZAPP_LIFE_STOP_OFF, 1U);
    zzapp_host_latch_stop(0U);
    zzapp_worker_hook();
    select_test_slot(0U);
    list_slot(0U, &state, &committed, &reason);
    assert(state == ZZAPP_ST_IDLE);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
}

static uint32_t list_cpu(uint32_t slot)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;

    extended_request(request, slot, 0U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LIST, request, sizeof(request),
                                  reply, &reply_bytes, 1, 0U, 0U) ==
           SDK_STATUS_OK);
    assert(reply_bytes == ZZAPP_LIST_REPLY_BYTES);
    return zzapp_get_be32(reply + ZZAPP_LIST_CPU_MS);
}

static void test_list_cpu_starts_at_new_generation(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint32_t generation;

    memset(windows, 0, sizeof(windows));
    request_id = 0U;
    zzapp_boot_init();
    zzapp_services_test_reset();
    zzapp_services_test_set_window_slot(0U, windows[0], sizeof(windows[0]));
    zzapp_test_set_entry(NULL);
    generation = claim_slot();
    load_slot(0U, generation, 2U, 0U);
    extended_request(request, 0U, generation);
    assert(dispatch(ZZAPP_OP_START, request, sizeof(request)) == SDK_STATUS_OK);
    zzapp_runtime_kernel()->threads[0].user_ticks = 17U;
    zzapp_runtime_kernel()->threads[0].svc_ticks = 23U;
    assert(list_cpu(0U) == 40U);
    generation = claim_slot();
    assert(list_cpu(0U) == 0U);
    load_slot(0U, generation, 2U, 0U);
    assert(list_cpu(0U) == 0U);
}

static void test_core_restart_aborts_unstarted_load(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint32_t generation[2];
    uint32_t state;
    uint32_t committed;
    uint8_t reason;
    uint32_t card;

    memset(windows, 0, sizeof(windows));
    request_id = 0U;
    zzapp_boot_init();
    zzapp_services_test_reset();
    zzapp_services_test_set_window_slot(0U, windows[0], sizeof(windows[0]));
    zzapp_services_test_set_window_slot(1U, windows[1], sizeof(windows[1]));
    generation[0] = claim_slot();
    generation[1] = claim_slot();
    extended_request(request, 0U, generation[0]);
    assert(dispatch(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request)) ==
           SDK_STATUS_OK);
    select_test_slot(0U);
    card = zzapp_test_arena_card();
    assert(card != 0U);
    select_test_slot(1U);
    zzapp_services_set_state(ZZAPP_ST_RUNNING);
    zzapp_test_mark_running();
    zzapp_on_core1_restart();
    select_test_slot(0U);
    list_slot(0U, &state, &committed, &reason);
    assert(state == ZZAPP_ST_IDLE);
    assert(reason == ZZAPP_END_CORE_RESTART);
    assert(committed == 0U);
    assert(zzapp_test_arena_card() == 0U);
    extended_request(request, 0U, generation[0]);
    assert(dispatch(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request)) ==
           SDK_STATUS_OK);
    assert(zzapp_test_arena_card() != 0U);
    assert(zzapp_loader_abort() == ZZ_OK);
}

int main(void)
{
    enum teardown_path value;

    for (value = TEARDOWN_EXIT; value <= TEARDOWN_DIRECT_STOP_PARKED; value++)
        run_case(value);
    test_abort_republishes_service_state();
    test_stopped_load_aborts_when_another_slot_is_selected();
    test_core_restart_aborts_unstarted_load();
    test_list_cpu_starts_at_new_generation();
    return 0;
}
