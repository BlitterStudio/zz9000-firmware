#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "zzapp.h"
#include "card_pool.h"
#include "host_card_pool.h"
#include "zzapp_mem.h"
#include "zzapp_runtime.h"
#include "zzapp_services.h"
#define MB(n) ((uint32_t)(n) << 20)

void zzapp_host_register_low_memory_callback(zzapp_mem_t *memory);

int smp_cpu_id(void) { return 0; }
uint32_t smp_local_irq_save(void) { return 0u; }
void smp_local_irq_restore(uint32_t state) { (void)state; }
void smp_raw_spin_lock(volatile uint32_t *word) { *word = 1u; }
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word)
        return 0;
    *word = 1u;
    return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *word) { *word = 0u; }

static uint8_t windows[ZZAPP_SLOT_COUNT][ZZAPP_WINDOW_BYTES];
static zzapp_mem_t memory;
static zzapp_mem_proc_t proc;

static void mark_running(uint32_t slot)
{
    zzapp_services_select_slot(slot);
    zzapp_test_mark_running();
}

static uint16_t dispatch(uint16_t opcode, const uint8_t *request,
                         uint16_t request_len, uint8_t *reply,
                         uint16_t *reply_len)
{
    return zzapp_mailbox_dispatch(opcode, request, request_len, reply,
                                  reply_len, 1, 1U, 0U);
}

static void claim_slot_zero(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_len;

    assert(dispatch(ZZAPP_OP_ALLOC_WINDOW, request, sizeof(request), reply,
                    &reply_len) == SDK_STATUS_OK);
    assert(zzapp_get_be32(reply + ZZAPP_ALLOC_SLOT) == 0U);
}

static void expect_low_memory_event(uint32_t slot, uint32_t crossings)
{
    zzapp_event event[2];
    uint32_t count;

    zzapp_services_select_slot(slot);
    assert(zzapp_svc_input_poll(event, 2u, &count) == ZZ_OK);
    assert(count == 1u);
    assert(event[0].type == ZZAPP_EV_LOW_MEMORY);
    assert(zzapp_svc_input_poll(event, 2u, &count) == ZZ_OK);
    assert(count == 0u);
    assert(zzapp_get_be32(windows[slot] + ZZAPP_LIFE_OFF +
                          ZZAPP_LIFE_LOW_MEMORY_CROSSINGS_OFF) == crossings);
}


static void expect_no_low_memory_event(uint32_t slot, uint32_t crossings)
{
    zzapp_event event;
    uint32_t count;

    zzapp_services_select_slot(slot);
    assert(zzapp_svc_input_poll(&event, 1u, &count) == ZZ_OK);
    assert(count == 0u);
    assert(zzapp_get_be32(windows[slot] + ZZAPP_LIFE_OFF +
                          ZZAPP_LIFE_LOW_MEMORY_CROSSINGS_OFF) == crossings);
}
int main(void)
{
    uint32_t frames[128];
    uint32_t i;

    zzapp_boot_init();
    zzapp_services_test_reset();
    for (i = 0u; i < ZZAPP_SLOT_COUNT; i++)
        zzapp_services_test_set_window_slot(i, windows[i], sizeof(windows[i]));
    claim_slot_zero();
    mark_running(0u);
    mark_running(2u);
    host_card_pool_reset(MB(256));
    assert(zzapp_mem_init(&memory, &card_pool) == 0);
    zzapp_mem_proc_init(&proc, MB(128));
    zzapp_host_register_low_memory_callback(&memory);

    assert(zzapp_mem_alloc(&memory, &proc, 97u, frames) == 0);
    zzapp_services_select_slot(0u);
    zzapp_services_on_app_end();
    claim_slot_zero();
    expect_no_low_memory_event(0u, 0u);
    expect_low_memory_event(2u, 1u);
    mark_running(0u);

    assert(zzapp_mem_alloc(&memory, &proc, 1u, &frames[97]) == 0);
    expect_no_low_memory_event(0u, 0u);
    expect_no_low_memory_event(2u, 1u);
    for (i = 0u; i < 35u; i++)
        assert(zzapp_mem_release(&memory, &proc, frames[i]) == 0);
    assert(zzapp_mem_alloc(&memory, &proc, 34u, frames) == 0);
    expect_low_memory_event(0u, 1u);
    expect_low_memory_event(2u, 2u);
    return 0;
}
