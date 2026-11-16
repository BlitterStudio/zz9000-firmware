#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "zzapp.h"
#include "zzapp_proto.h"

void zzapp_boot_init(void);
void zzapp_worker_hook(void);
void zzapp_on_amiga_reset(void);
void zzapp_host_yield(void);
void zzapp_host_exit(int code);
uint32_t zzapp_runtime_state(void);
void zzapp_test_set_entry(void (*fn)(const void *api, int argc, char **argv));
void zzapp_test_mark_running(void);
void zzapp_take_app_fault(uint32_t fault, uint32_t pc, uint32_t sp,
                          uint32_t lr, uint32_t far, uint32_t fsr,
                          uint32_t cpsr);
int zzapp_loader_abort(void);
void *zzapp_host_alloc(uint32_t bytes, uint32_t align, uint32_t flags);
void *zzapp_sbrk(int increment);
int zzapp_host_mem_info(zzapp_mem_info *out);
uint8_t *zzapp_test_stack_canary(void);
uint32_t zzapp_test_fault_code(void);
void zzapp_test_set_service_hook(void (*fn)(void));
void zzapp_test_set_fail_start(int fail);
void zzapp_test_set_start_race_hook(void (*fn)(void));
void zzapp_test_set_admission_hook(void (*fn)(void));
static void start_and_exit(void);
/* syscalls.o is linked for zzapp_sbrk; it reads this table pointer. */
const zzapp_api *zzapp_api_ptr;

#define IMAGE_BYTES 0x200u
#define PHOFF 52u
#define LOAD_OFF 0x100u
#define NOTE_OFF 0x0a0u
#define RELA_OFF 0x0d0u
#define SHOFF 0x180u

static uint8_t launcher_window[ZZAPP_WINDOW_BYTES];
static uint32_t request_id;
static unsigned app_runs;
static int reset_running;
static int damage_canary;
static int direct_stop_running;
static unsigned direct_stop_seen;
static const zzapp_api *app_api;
static int start_stop_written;
static int lease_exit_running;
static int launcher_stop_exit_running;

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void make_image(uint8_t image[IMAGE_BYTES])
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

    put32(image + RELA_OFF + 0, LOAD_OFF);
    put32(image + RELA_OFF + 4, 23);      /* R_ARM_RELATIVE, symbol zero */
    put32(image + RELA_OFF + 8, LOAD_OFF);
    sh = image + SHOFF;
    put32(sh + 4, 4);                     /* SHT_RELA */
    put32(sh + 16, RELA_OFF);
    put32(sh + 20, 12);
    put32(sh + 36, 12);
}

static void write_start_stop_word(void)
{
    assert(start_stop_written == 0);
    start_stop_written = 1;
    zzapp_put_be32(launcher_window + ZZAPP_LIFE_OFF +
                   ZZAPP_LIFE_STOP_OFF, 1U);
    zzapp_test_set_service_hook(NULL);
}

static void app_entry(const void *api, int argc, char **argv)
{
    const zzapp_api *table = (const zzapp_api *)api;
    void *brk;
    void *heap_base = NULL;
    uint32_t heap_bytes = 0u;

    (void)argc;
    (void)argv;
    app_runs++;
    /* The arena heap is newlib's alone: the table allocates host-visible
     * memory only, so it can never hand out bytes malloc already owns. */
    zzapp_api_ptr = table;
    app_api = table;
    assert(table->alloc(64u, 8u, 0u) == NULL);
    table->heap_range(&heap_base, &heap_bytes);
    /* zzapp_sbrk keeps its span in app BSS, which a real launch starts
     * fresh; in this harness it survives relaunches, so check it once. */
    if (app_runs == 1) {
        brk = zzapp_sbrk(64);
        assert(brk == heap_base && heap_bytes >= 64u);
    }
    if (lease_exit_running) {
        zzapp_host_tick_at(ZZAPP_LEASE_US - 1U);
        zzapp_test_set_xtime(ZZAPP_LEASE_US);
        zzapp_core0_poll();
        assert(table->should_stop() == 1);
        zzapp_host_exit(0);
        return;
    }
    if (launcher_stop_exit_running) {
        uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
        uint16_t reply_bytes = 0U;

        assert(zzapp_mailbox_dispatch(ZZAPP_OP_STOP, NULL, 0U, reply,
                                      &reply_bytes, 1, ++request_id, 0) ==
               SDK_STATUS_OK);
        assert(table->should_stop() == 1);
        zzapp_host_exit(0);
        return;
    }
    if (direct_stop_running) {
        zzapp_put_be32(launcher_window + ZZAPP_LIFE_OFF +
                       ZZAPP_LIFE_STOP_OFF, 1U);
        zzapp_host_tick_at(ZZAPP_TICK_US);
        direct_stop_seen = (unsigned)table->should_stop();
        assert(direct_stop_seen == 1U);
        zzapp_host_tick_at(ZZAPP_TICK_US + ZZAPP_STOP_GRACE_US);
        zzapp_worker_hook();
        return;
    }
    if (damage_canary) {
        uint8_t *canary = zzapp_test_stack_canary();

        assert(canary != NULL);
        canary[0] ^= 0xA5u;
        zzapp_host_yield();
        return;
    }
    if (reset_running) {
        zzapp_mem_info mem;
        void *host_visible;

        host_visible = zzapp_host_alloc(4096u, 256u, ZZ_ALLOC_HOST_VISIBLE);
        assert(host_visible != NULL);
        assert(zzapp_host_mem_info(&mem) == ZZ_OK);
        assert(mem.host_visible_used == 4096u);
        zzapp_on_amiga_reset();
        zzapp_host_yield();
    }
    zzapp_host_exit(0);
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
    if (!zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL,
                               &reply_bytes)) {
        assert(opcode == ZZAPP_OP_START && reset_running);
        return SDK_STATUS_QUEUED;
    }
    zzapp_mailbox_harvest_ack();
    return status;
}

static uint16_t status(void)
{
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0;

    return zzapp_mailbox_dispatch(ZZAPP_OP_STATUS, NULL, 0, reply,
                                  &reply_bytes, 1, 0, 0);
}

static uint8_t terminal_reason(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0;

    zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LIST, request, sizeof(request),
                                  reply, &reply_bytes, 1, 0, 0) ==
           SDK_STATUS_OK);
    assert(reply_bytes == ZZAPP_LIST_REPLY_BYTES);
    return reply[ZZAPP_LIST_REASONS + 1U];
}

static void bind_window(void)
{
    memset(launcher_window, 0, sizeof(launcher_window));
    zzapp_services_test_set_window(launcher_window, sizeof(launcher_window));
}

static void begin_load(void)
{
    assert(dispatch(ZZAPP_OP_LOAD_BEGIN, NULL, 0) == SDK_STATUS_OK);
    assert(zzapp_runtime_state() == ZZAPP_ST_LOADING);
}

static void load_hello(void)
{
    uint8_t image[IMAGE_BYTES];
    uint8_t request[ZZAPP_CHUNK_REQ_BYTES];

    make_image(image);
    begin_load();
    memcpy(launcher_window + ZZAPP_STAGING_OFF, image, sizeof(image));
    zzapp_build_chunk_request(request, 0, sizeof(image));
    assert(dispatch(ZZAPP_OP_LOAD_CHUNK, request, sizeof(request)) ==
           SDK_STATUS_OK);
    assert(dispatch(ZZAPP_OP_LOAD_END, NULL, 0) == SDK_STATUS_OK);
}

static void expire_loading_claim(uint32_t *stale_generation,
                                 uint32_t *current_generation)
{
    zzapp_boot_init();
    zzapp_services_test_reset();
    bind_window();
    zzapp_test_set_xtime(0U);
    begin_load();
    *stale_generation = zzapp_get_be32(launcher_window + ZZAPP_LIFE_OFF +
                                        ZZAPP_LIFE_GENERATION_OFF);

    /* Core 0 can expire a live LOAD_BEGIN before core 1 gets its STOP. */
    zzapp_services_expire_claims(ZZAPP_LOAD_LEASE_US);
    assert(zzapp_services_state() == ZZAPP_ST_STOPPING);
    assert(zzapp_runtime_state() == ZZAPP_ST_LOADING);
    zzapp_worker_hook();
    assert(zzapp_runtime_state() == ZZAPP_ST_STOPPING);
    *current_generation = zzapp_get_be32(launcher_window + ZZAPP_LIFE_OFF +
                                          ZZAPP_LIFE_GENERATION_OFF);
}

static void release_expired_load_quarantine(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;
    uint32_t stale_generation;
    uint32_t current_generation;

    expire_loading_claim(&stale_generation, &current_generation);

    /*
     * The stale contact releases the reclaimed quarantine.  The services
     * callback must switch the host to IDLE before this makes another claim
     * admissible, otherwise the next real LOAD_BEGIN sees loader EBUSY.
     */
    zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
    zzapp_put_be32(request + ZZAPP_SEL_GENERATION, stale_generation);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, request,
                                  sizeof(request), reply, &reply_bytes, 1,
                                  ++request_id, 0) == SDK_STATUS_BAD_HANDLE);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    assert(current_generation != stale_generation);
    begin_load();
    assert(zzapp_loader_abort() == ZZ_OK);
}

static void reset_expired_load_quarantine(void)
{
    uint32_t stale_generation;
    uint32_t current_generation;

    expire_loading_claim(&stale_generation, &current_generation);
    assert(current_generation != stale_generation);
    zzapp_on_amiga_reset();
    zzapp_worker_hook();
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    bind_window();
    begin_load();
    assert(zzapp_loader_abort() == ZZ_OK);
}

static void explicitly_release_expired_load_quarantine(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;
    uint32_t stale_generation;
    uint32_t current_generation;

    expire_loading_claim(&stale_generation, &current_generation);
    zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
    zzapp_put_be32(request + ZZAPP_SEL_GENERATION, current_generation);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_RELEASE, request, sizeof(request),
                                  reply, &reply_bytes, 1, ++request_id, 0) ==
           SDK_STATUS_OK);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    assert(current_generation != stale_generation);
    begin_load();
    assert(zzapp_loader_abort() == ZZ_OK);
}

static void cancel_deferred_load_before_loader_begin(void)
{
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint32_t stale_generation;
    uint32_t current_generation;

    zzapp_boot_init();
    zzapp_services_test_reset();
    bind_window();
    zzapp_test_set_xtime(0U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
                                  sizeof(request), reply, &reply_bytes, 1,
                                  ++request_id, 0) == SDK_STATUS_OK);
    stale_generation = zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
    assert(zzapp_services_state() == ZZAPP_ST_CLAIMED);
    zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
    zzapp_put_be32(request + ZZAPP_SEL_GENERATION, stale_generation);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, request,
                                  sizeof(request), reply, &reply_bytes, 1,
                                  ++request_id, 0) == SDK_STATUS_QUEUED);
    assert(deferred[0].state == DEFER_POSTED);
    /*
     * This is the production poll boundary after it selects this request
     * but before run_deferred can enter zzapp_loader_begin().  Core 0
     * expires the claim here, so the loader never sees an arena and
     * zzapp_loader_abort() reports EBUSY from host IDLE.
     */
    deferred[0].state = DEFER_RUNNING;
    zzapp_services_expire_claims(ZZAPP_CLAIM_LEASE_US);
    assert(zzapp_services_state() == ZZAPP_ST_STOPPING);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    run_deferred();
    current_generation = zzapp_get_be32(launcher_window + ZZAPP_LIFE_OFF +
                                         ZZAPP_LIFE_GENERATION_OFF);
    assert(current_generation != stale_generation);
    /*
     * Cancellation must still reclaim the quarantine with no loader arena,
     * so the quarantined generation can release its slot explicitly.
     */
    zzapp_put_be32(request + ZZAPP_SEL_GENERATION, current_generation);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_RELEASE, request, sizeof(request),
                                  reply, &reply_bytes, 1, ++request_id, 0) ==
           SDK_STATUS_OK);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    begin_load();
    assert(zzapp_loader_abort() == ZZ_OK);
}
static uint8_t race_request[ZZAPP_MAILBOX_PAYLOAD_BYTES];

static int race_hook_fired;

static void expire_and_record_intent(void)
{
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;

    zzapp_test_set_start_race_hook(NULL);
    zzapp_test_set_service_hook(NULL);
    race_hook_fired = 1;
    zzapp_services_expire_claims(ZZAPP_LOAD_LEASE_US);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_CHUNK, race_request,
                                  sizeof(race_request), reply, &reply_bytes,
                                  1, ++request_id, 0) ==
           SDK_STATUS_BAD_HANDLE);
}

static void start_racing_expiry_with_release_intent(void)
{
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t start_request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint32_t stale_generation;
    uint16_t status;

    zzapp_boot_init();
    zzapp_services_test_reset();
    bind_window();
    zzapp_test_set_xtime(0U);
    load_hello();
    stale_generation = zzapp_get_be32(launcher_window + ZZAPP_LIFE_OFF +
                                      ZZAPP_LIFE_GENERATION_OFF);
    zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
    zzapp_put_be32(request + ZZAPP_SEL_GENERATION, stale_generation);
    /*
     * Queue START and inject the expiry at the single point between START
     * preparation and its completion: core 0 expires the slot there and the
     * old launcher records its release intent, so run_app reaches
     * start_entered already cancelled.
     */
    memcpy(race_request, request, sizeof(race_request));
    zzapp_put_be32(start_request + ZZAPP_SEL_SLOT, 0U);
    zzapp_put_be32(start_request + ZZAPP_SEL_GENERATION, stale_generation);
    zzapp_put_be32(start_request + ZZAPP_START_ARGC, 0U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_START, start_request,
                                  sizeof(start_request), reply, &reply_bytes,
                                  1, ++request_id, 0) == SDK_STATUS_QUEUED);
    race_hook_fired = 0;
    zzapp_test_set_start_race_hook(expire_and_record_intent);
    zzapp_worker_hook();
    zzapp_test_set_start_race_hook(NULL);
    assert(race_hook_fired == 1);
    while (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL, NULL))
        zzapp_mailbox_harvest_ack();
    /*
     * The rejection teardown is the final host-state writer: after it the
     * released slot must be IDLE, never re-published STOPPING.
     */
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    assert(zzapp_services_state() == ZZAPP_ST_IDLE);
    begin_load();
    assert(zzapp_loader_abort() == ZZ_OK);
}

static uint32_t admitted_generation;

/* Core 0 harvests finished records, then makes a real (legacy, slot 0)
 * ALLOC_WINDOW claim. */
static uint16_t claim_slot0_from_core0(void)
{
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;
    uint16_t status;

    zzapp_services_test_set_core(0);
    while (zzapp_mailbox_harvest(NULL, NULL, NULL, &status, NULL, NULL))
        zzapp_mailbox_harvest_ack();
    status = zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, NULL, 0U, reply,
                                    &reply_bytes, 1, ++request_id, 0);
    zzapp_services_test_set_core(1);
    return status;
}
static void assert_window_payload_zero(void)
{
    uint32_t offset;

    for (offset = ZZAPP_LIFE_BYTES; offset < ZZAPP_WINDOW_BYTES; offset++)
        assert(launcher_window[offset] == 0U);
}

static void window_claim_initialization_lifecycle(void)
{
    zzapp_boot_init();
    zzapp_services_test_reset();
    zzapp_services_test_set_core(0);
    memset(launcher_window, 0xa5, sizeof(launcher_window));
    zzapp_services_test_set_window(launcher_window, sizeof(launcher_window));
    assert_window_payload_zero();
    assert(claim_slot0_from_core0() == SDK_STATUS_OK);
    assert_window_payload_zero();

    /* A normal app end retains the mapped window. A later claim therefore
     * does not pay the whole-window initialization cost again. */
    launcher_window[ZZAPP_STAGING_OFF] = 0x5aU;
    zzapp_services_on_app_end();
    assert(claim_slot0_from_core0() == SDK_STATUS_OK);
    assert(launcher_window[ZZAPP_STAGING_OFF] == 0x5aU);

    /* Reset forgets the mapping. No claim is published until core 0 binds
     * and completely initializes the replacement window. */
    zzapp_services_on_reset();
    assert(claim_slot0_from_core0() == SDK_STATUS_NO_MEMORY);
    zzapp_services_test_set_core(0);
    memset(launcher_window, 0xa5, sizeof(launcher_window));
    zzapp_services_test_set_window(launcher_window, sizeof(launcher_window));
    assert_window_payload_zero();
    assert(claim_slot0_from_core0() == SDK_STATUS_OK);
    assert_window_payload_zero();
    zzapp_services_test_set_core(1);
}


/* Claims slot 0 the moment it is admissible again; BUSY means core 1
 * still holds it. */
static void claim_when_admissible(void)
{
    uint16_t status = claim_slot0_from_core0();

    if (status == SDK_STATUS_BUSY)
        return;
    assert(status == SDK_STATUS_OK);
    admitted_generation = slots[0].generation;
    zzapp_test_set_admission_hook(NULL);
}

static uint32_t list_state(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;

    zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LIST, request, sizeof(request),
                                  reply, &reply_bytes, 1, 0, 0) ==
           SDK_STATUS_OK);
    return zzapp_get_be32(reply + ZZAPP_LIST_STATE);
}

/*
 * An Amiga reset while core 1 owns a RUNNING record cancels it but leaves
 * the slot STOPPING: completing the cancellation is the admission point.
 * Until then ALLOC_WINDOW is BUSY; a claim made afterwards stays CLAIMED.
 */
static void reset_keeps_running_record_stopping(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;
    uint32_t generation;

    zzapp_boot_init();
    zzapp_services_test_reset();
    bind_window();
    zzapp_test_set_xtime(0U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_ALLOC_WINDOW, request,
                                  sizeof(request), reply, &reply_bytes, 1,
                                  ++request_id, 0) == SDK_STATUS_OK);
    generation = zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
    zzapp_put_be32(request + ZZAPP_SEL_SLOT, 0U);
    zzapp_put_be32(request + ZZAPP_SEL_GENERATION, generation);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_LOAD_BEGIN, request,
                                  sizeof(request), reply, &reply_bytes, 1,
                                  ++request_id, 0) == SDK_STATUS_QUEUED);
    /* The poll boundary: core 1 owns the record, run_deferred not yet in. */
    deferred[0].state = DEFER_RUNNING;
    zzapp_on_amiga_reset();
    zzapp_worker_hook();
    bind_window();
    assert(deferred[0].state == DEFER_RUNNING && deferred[0].cancelled);
    assert(list_state() == ZZAPP_ST_STOPPING);
    assert(claim_slot0_from_core0() == SDK_STATUS_BUSY);
    zzapp_services_select_slot(0U);
    run_deferred();
    assert(deferred[0].state == DEFER_IDLE);
    assert(list_state() == ZZAPP_ST_IDLE);
    assert(claim_slot0_from_core0() == SDK_STATUS_OK);
    generation = slots[0].generation;
    zzapp_worker_hook();
    assert(list_state() == ZZAPP_ST_CLAIMED);
    assert(slots[0].generation == generation);
    assert(zzapp_get_be32(launcher_window + ZZAPP_LIFE_OFF +
                          ZZAPP_LIFE_STATE_OFF) == ZZAPP_ST_CLAIMED);
    begin_load();
    assert(zzapp_loader_abort() == ZZ_OK);
}

/* Core 0 cancels the running START the way STOP cancels a claim: no
 * quarantine, so start_entered completes the record and reopens the slot. */
static void cancel_start_without_quarantine(void)
{
    zzapp_test_set_service_hook(NULL);
    race_hook_fired = 1;
    slot_state_lock_acquire();
    assert(stop_current_deferred() == 0);
    begin_claim_cancellation(&slots[0], ZZAPP_END_FORCED);
    slot_state_lock_release();
}

/*
 * A START that core 0 cancels mid-flight is torn down by core 1, which may
 * reopen the slot before its last write.  A claim core 0 makes at any such
 * admission point must keep CLAIMED and its generation afterwards.
 */
static void claim_survives_start_teardown(void (*race)(void), int fail_start)
{
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_bytes = 0U;
    uint8_t start_request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint32_t stale_generation;

    zzapp_boot_init();
    zzapp_services_test_reset();
    bind_window();
    zzapp_test_set_xtime(0U);
    load_hello();
    stale_generation = zzapp_get_be32(launcher_window + ZZAPP_LIFE_OFF +
                                      ZZAPP_LIFE_GENERATION_OFF);
    memset(race_request, 0, sizeof(race_request));
    zzapp_put_be32(race_request + ZZAPP_SEL_SLOT, 0U);
    zzapp_put_be32(race_request + ZZAPP_SEL_GENERATION, stale_generation);
    memcpy(start_request, race_request, sizeof(start_request));
    zzapp_put_be32(start_request + ZZAPP_START_ARGC, 0U);
    assert(zzapp_mailbox_dispatch(ZZAPP_OP_START, start_request,
                                  sizeof(start_request), reply, &reply_bytes,
                                  1, ++request_id, 0) == SDK_STATUS_QUEUED);
    race_hook_fired = 0;
    admitted_generation = 0U;
    zzapp_test_set_start_race_hook(race);
    zzapp_test_set_admission_hook(claim_when_admissible);
    zzapp_test_set_fail_start(fail_start);
    zzapp_worker_hook();
    zzapp_test_set_start_race_hook(NULL);
    zzapp_test_set_admission_hook(NULL);
    assert(race_hook_fired == 1);
    assert(admitted_generation != 0U && admitted_generation != stale_generation);
    assert(slots[0].state == ZZAPP_ST_CLAIMED);
    assert(slots[0].generation == admitted_generation);
    assert(zzapp_get_be32(launcher_window + ZZAPP_LIFE_OFF +
                          ZZAPP_LIFE_STATE_OFF) == ZZAPP_ST_CLAIMED);
    assert(zzapp_get_be32(launcher_window + ZZAPP_LIFE_OFF +
                          ZZAPP_LIFE_GENERATION_OFF) == admitted_generation);
    /* The new owner can load: core 1 left the host IDLE. */
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    begin_load();
    assert(zzapp_loader_abort() == ZZ_OK);
}

static void start_failure_releases_host_state(void)
{
    uint8_t request[ZZAPP_START_REQ_BYTES] = { 0 };
    unsigned runs = app_runs;

    zzapp_boot_init();
    zzapp_services_test_reset();
    bind_window();
    load_hello();
    zzapp_test_set_fail_start(1);
    /*
     * The worker publishes STARTING before run_app; its failure path must
     * still normalize the host to IDLE, or the next LOAD_BEGIN sees EBUSY.
     */
    assert(dispatch(ZZAPP_OP_START, request, sizeof(request)) ==
           SDK_STATUS_IO_ERROR);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    load_hello();
    start_and_exit();
    assert(app_runs == runs + 1u);
}

static void start_and_exit(void)
{
    uint8_t request[ZZAPP_START_REQ_BYTES] = { 0 };

    assert(dispatch(ZZAPP_OP_START, request, sizeof(request)) ==
           (reset_running ? SDK_STATUS_QUEUED : SDK_STATUS_OK));
    if (reset_running)
        zzapp_worker_hook();
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
}

int main(void)
{
    window_claim_initialization_lifecycle();
    zzapp_boot_init();
    zzapp_services_test_reset();
    zzapp_test_set_entry(app_entry);

    /* Every boot observes an Amiga reset before the launcher gets a window. */
    zzapp_on_amiga_reset();
    zzapp_worker_hook();
    bind_window();
    load_hello();
    start_and_exit();
    assert(app_runs == 1u);
    release_expired_load_quarantine();
    reset_expired_load_quarantine();
    explicitly_release_expired_load_quarantine();
    cancel_deferred_load_before_loader_begin();
    start_racing_expiry_with_release_intent();
    claim_survives_start_teardown(cancel_start_without_quarantine, 0);
    claim_survives_start_teardown(expire_and_record_intent, 0);
    claim_survives_start_teardown(expire_and_record_intent, 1);
    reset_keeps_running_record_stopping();
    start_failure_releases_host_state();

    /* A launcher-only lifecycle write is observed on the next timer tick,
     * then force-stopped after the normal grace without APP_STOP. */
    direct_stop_running = 1;
    load_hello();
    {
        uint8_t request[ZZAPP_START_REQ_BYTES] = { 0 };

        assert(dispatch(ZZAPP_OP_START, request, sizeof(request)) ==
               SDK_STATUS_OK);
    }
    direct_stop_running = 0;
    assert(direct_stop_seen == 1U);
    assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);

    /* APP_STOP before launch is latched by its firmware handler; real START
     * processing cancels the pending launch.  The next launch clears it. */
    {
        uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
        uint16_t reply_bytes = 0U;
        uint16_t reply_status;
        uint8_t request[ZZAPP_START_REQ_BYTES] = { 0 };
        unsigned before;

        zzapp_boot_init();
        zzapp_services_test_reset();
        bind_window();
        load_hello();
        reply_status = zzapp_mailbox_dispatch(ZZAPP_OP_STOP, NULL, 0U, reply,
                                               &reply_bytes, 1, ++request_id, 0);
        assert(reply_status == SDK_STATUS_OK);
        assert(app_api != NULL);
        assert(app_api->should_stop() == 1);
        before = app_runs;
        (void)dispatch(ZZAPP_OP_START, request, sizeof(request));
        assert(app_runs == before);
        assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
        load_hello();
        start_and_exit();
        assert(app_runs == before + 1U);
    }
    /* A lifecycle word written after the worker's watch but before the
     * pending START is consumed is caught by START's own sampler. */
    {
        uint8_t request[ZZAPP_START_REQ_BYTES] = { 0 };
        unsigned before;

        zzapp_boot_init();
        zzapp_services_test_reset();
        bind_window();
        load_hello();
        before = app_runs;
        start_stop_written = 0;
        zzapp_test_set_service_hook(write_start_stop_word);
        assert(dispatch(ZZAPP_OP_START, request, sizeof(request)) ==
               SDK_STATUS_IO_ERROR);
        assert(start_stop_written == 1);
        assert(app_runs == before);
        assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    }
    zzapp_loader_release();
    zzapp_boot_init();
    zzapp_services_test_reset();
    bind_window();

    /* A reset during a partial upload abandons it exactly once. */
    begin_load();
    zzapp_on_amiga_reset();
    zzapp_worker_hook();
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    zzapp_worker_hook();
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    bind_window();
    begin_load();
    assert(zzapp_loader_abort() == ZZ_OK);

    /* Input and log cursors are per launch, never inherited by the next app. */
    zzapp_put_be32(launcher_window + ZZAPP_INPUT_OFF, 7u);
    zzapp_put_be32(launcher_window + ZZAPP_INPUT_OFF + 4u, 3u);
    zzapp_put_be32(launcher_window + ZZAPP_LOG_OFF, 9u);
    zzapp_put_be32(launcher_window + ZZAPP_LOG_OFF + 4u, 2u);
    zzapp_put_be32(launcher_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_PALETTE_GEN_OFF, 7u);
    zzapp_put_be32(launcher_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_PALETTE_FIRST_OFF, 3u);
    zzapp_put_be32(launcher_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_PALETTE_COUNT_OFF, 4u);
    zzapp_put_be32(launcher_window + ZZAPP_LIFE_OFF + ZZAPP_LIFE_CARD_SPARE_OFF, 0x00280000u);
    memset(launcher_window + ZZAPP_PALETTE_OFF, 0xa5, ZZAPP_PALETTE_BYTES);
    load_hello();
    assert(zzapp_get_be32(launcher_window + ZZAPP_INPUT_OFF) == 0u);
    assert(zzapp_get_be32(launcher_window + ZZAPP_INPUT_OFF + 4u) == 0u);
    assert(zzapp_get_be32(launcher_window + ZZAPP_LOG_OFF) == 0u);
    assert(zzapp_get_be32(launcher_window + ZZAPP_LOG_OFF + 4u) == 0u);
    assert(zzapp_get_be32(launcher_window + ZZAPP_LIFE_OFF +
                          ZZAPP_LIFE_PALETTE_GEN_OFF) == 0u);
    assert(zzapp_get_be32(launcher_window + ZZAPP_LIFE_OFF +
                          ZZAPP_LIFE_PALETTE_FIRST_OFF) == 0u);
    assert(zzapp_get_be32(launcher_window + ZZAPP_LIFE_OFF +
                          ZZAPP_LIFE_PALETTE_COUNT_OFF) == 0u);
    assert(zzapp_get_be32(launcher_window + ZZAPP_LIFE_OFF +
                          ZZAPP_LIFE_CARD_SPARE_OFF) == 0u);
    for (unsigned i = 0; i < ZZAPP_PALETTE_BYTES; i++)
        assert(launcher_window[ZZAPP_PALETTE_OFF + i] == 0u);
    start_and_exit();
    assert(app_runs == 5u);

    /* A running app sees reset as an immediate forced stop, then can relaunch. */
    reset_running = 1;
    load_hello();
    start_and_exit();
    {
        zzapp_mem_info mem;

        assert(zzapp_host_mem_info(&mem) == ZZ_OK);
        assert(mem.host_visible_used == 0u);
    }
    reset_running = 0;
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    bind_window();
    load_hello();
    start_and_exit();
    assert(app_runs == 7u);

    /* A damaged stack canary force-stops with the documented data fault. */
    damage_canary = 1;
    load_hello();
    {
        uint8_t request[ZZAPP_START_REQ_BYTES] = { 0 };

        assert(dispatch(ZZAPP_OP_START, request, sizeof(request)) ==
               SDK_STATUS_OK);
    }
    damage_canary = 0;
    assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);
    assert(terminal_reason() == ZZAPP_END_USER_FAULT);
    assert(status() == SDK_STATUS_OK);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    bind_window();
    /* A clean launch clears the fault latch before the fault-ack scenario. */
    load_hello();
    start_and_exit();
    assert(app_runs == 9u);
    /* A fault is reported once, then STATUS acknowledges it for a new launch. */
    zzapp_test_mark_running();
    zzapp_take_app_fault(ZZAPP_FAULT_DATA, 0, 0, 0, 0, 0, 0);
    zzapp_worker_hook();
    assert(zzapp_runtime_state() == ZZAPP_ST_FAULTED);
    assert(terminal_reason() == ZZAPP_END_USER_FAULT);
    assert(status() == SDK_STATUS_OK);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    load_hello();
    start_and_exit();
    assert(app_runs == 10u);

    zzapp_boot_init();
    zzapp_services_test_reset();
    bind_window();
    zzapp_test_set_xtime(0U);
    zzapp_test_mark_running();
    zzapp_host_tick_at(30000000U);
    zzapp_worker_hook();
    assert(terminal_reason() == ZZAPP_END_STALL);

    zzapp_boot_init();
    zzapp_services_test_reset();
    bind_window();
    zzapp_test_set_xtime(0U);
    zzapp_test_mark_running();
    zzapp_api_enter();
    zzapp_host_tick_at(1U);
    zzapp_host_tick_at(10000001U);
    zzapp_worker_hook();
    assert(terminal_reason() == ZZAPP_END_STUCK_CALL);

    zzapp_boot_init();
    zzapp_services_test_reset();
    bind_window();
    zzapp_test_set_xtime(0U);
    zzapp_test_mark_running();
    zzapp_test_set_xtime(1999999U);
    zzapp_host_tick_at(1999999U);
    zzapp_test_set_xtime(2000000U);
    zzapp_core0_poll();
    zzapp_host_tick_at(2000000U);
    zzapp_host_tick_at(2500000U);
    zzapp_worker_hook();
    assert(terminal_reason() == ZZAPP_END_LEASE);

    zzapp_boot_init();
    zzapp_services_test_reset();
    bind_window();
    zzapp_test_mark_running();
    zzapp_on_amiga_reset();
    zzapp_worker_hook();
    assert(terminal_reason() == ZZAPP_END_AMIGA_RESET);

    zzapp_boot_init();
    zzapp_services_test_reset();
    bind_window();
    zzapp_test_mark_running();
    zzapp_on_core1_restart();
    assert(terminal_reason() == ZZAPP_END_CORE_RESTART);
    /* A normal exit remains EXIT. Firmware-latched lease loss survives a
     * cooperative exit, while a launcher STOP does not supply a reason. */
    zzapp_boot_init();
    zzapp_services_test_reset();
    bind_window();
    zzapp_test_set_xtime(0U);
    load_hello();
    start_and_exit();
    assert(terminal_reason() == ZZAPP_END_EXIT);
    assert(zzapp_get_be32(launcher_window + ZZAPP_LIFE_OFF +
                          ZZAPP_LIFE_TERMINAL_OFF) == ZZAPP_END_EXIT);

    lease_exit_running = 1;
    load_hello();
    start_and_exit();
    lease_exit_running = 0;
    assert(terminal_reason() == ZZAPP_END_LEASE);
    assert(status() == SDK_STATUS_OK);
    assert(zzapp_get_be32(launcher_window + ZZAPP_LIFE_OFF +
                          ZZAPP_LIFE_TERMINAL_OFF) == ZZAPP_END_LEASE);

    launcher_stop_exit_running = 1;
    load_hello();
    start_and_exit();
    launcher_stop_exit_running = 0;
    assert(terminal_reason() == ZZAPP_END_EXIT);
    assert(zzapp_get_be32(launcher_window + ZZAPP_LIFE_OFF +
                          ZZAPP_LIFE_TERMINAL_OFF) == ZZAPP_END_EXIT);
    return 0;
}
