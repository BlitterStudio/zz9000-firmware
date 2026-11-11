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
    assert(app_runs == 4u);

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
    assert(app_runs == 6u);

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
    assert(app_runs == 8u);
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
    assert(app_runs == 9u);

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
    return 0;
}
