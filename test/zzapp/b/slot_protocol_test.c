#define ZZAPP_SERVICES_STANDALONE_TEST
#include "zzapp_services.c"

#include <assert.h>
#include <string.h>

static uint8_t windows[ZZAPP_SLOT_COUNT][ZZAPP_WINDOW_BYTES];

static uint16_t dispatch(uint16_t opcode, const uint8_t *request,
                         uint16_t request_len, uint8_t *reply,
                         uint16_t *reply_len)
{
    return zzapp_mailbox_dispatch(opcode, request, request_len, reply,
                                  reply_len, 1, 1U, 0U);
}

static void extended_request(uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES],
                             uint32_t slot, uint32_t generation)
{
    memset(request, 0, ZZAPP_MAILBOX_PAYLOAD_BYTES);
    zzapp_put_be32(request + ZZAPP_SEL_SLOT, slot);
    zzapp_put_be32(request + ZZAPP_SEL_GENERATION, generation);
}

int main(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_len;
    uint32_t generation[ZZAPP_SLOT_COUNT];
    uint32_t i;

    zzapp_services_test_reset();
    for (i = 0; i < ZZAPP_SLOT_COUNT; i++)
        zzapp_services_test_set_window_slot(i, windows[i], sizeof(windows[i]));

    for (i = 0; i < ZZAPP_SLOT_COUNT; i++) {
        extended_request(request, 0U, 0U);
        assert(dispatch(ZZAPP_OP_ALLOC_WINDOW, request, sizeof(request), reply,
                        &reply_len) == SDK_STATUS_OK);
        assert(reply_len == ZZAPP_ALLOC_EXT_REPLY_BYTES);
        assert(zzapp_get_be32(reply + ZZAPP_ALLOC_SLOT) == i);
        generation[i] = zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
        assert(generation[i] != 0U);
        assert(zzapp_get_be32(windows[i] + ZZAPP_LIFE_STATE_OFF) ==
               ZZAPP_ST_CLAIMED);
        assert(zzapp_get_be32(windows[i] + ZZAPP_LIFE_GENERATION_OFF) ==
               generation[i]);
    }
    extended_request(request, 0U, 0U);
    assert(dispatch(ZZAPP_OP_ALLOC_WINDOW, request, sizeof(request), reply,
                    &reply_len) == SDK_STATUS_BUSY);

    memset(reply, 0xa5, sizeof(reply));
    assert(dispatch(ZZAPP_OP_QUERY, NULL, 0U, reply, &reply_len) ==
           SDK_STATUS_OK);
    assert(reply_len == ZZAPP_QUERY_REPLY_BYTES);
    {
        uint8_t legacy[ZZAPP_QUERY_REPLY_BYTES] = { 0 };

        zzapp_put_be32(legacy + ZZAPP_QUERY_ABI_MAJOR, ZZAPP_ABI_MAJOR);
        zzapp_put_be32(legacy + ZZAPP_QUERY_ABI_MINOR, ZZAPP_ABI_MINOR);
        zzapp_put_be32(legacy + ZZAPP_QUERY_STATE, ZZAPP_ST_CLAIMED);
        zzapp_put_be32(legacy + ZZAPP_QUERY_CAPS,
                       ZZAPP_CAP_Z3 | ZZAPP_CAP_DISPLAY | ZZAPP_CAP_AUDIO |
                       ZZAPP_CAP_GL | ZZAPP_CAP_CLUT8 | ZZAPP_CAP_SLOTS);
        assert(memcmp(reply, legacy, sizeof(legacy)) == 0);
    }
    extended_request(request, 0U, 0U);
    assert(dispatch(ZZAPP_OP_QUERY, request, sizeof(request), reply, &reply_len) ==
           SDK_STATUS_OK);
    assert(reply_len == ZZAPP_QUERY_EXT_REPLY_BYTES);
    assert(zzapp_get_be32(reply + ZZAPP_QUERY_CAPS) & (1U << 6));
    assert(zzapp_get_be32(reply + ZZAPP_QUERY_SLOTS) == ZZAPP_SLOT_COUNT);
    assert(zzapp_get_be32(reply + ZZAPP_QUERY_FREE_SLOTS) == 0U);
    extended_request(request, 2U, generation[2]);
    zzapp_put_be32(request + ZZAPP_LOAD_MEM_LIMIT_MIB, 16U);
    assert(dispatch(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request), reply,
                    &reply_len) == SDK_STATUS_QUEUED);
    assert(zzapp_services_memory_limit() == 16U * 1024U * 1024U);

    extended_request(request, 2U, generation[2] + 1U);
    assert(dispatch(ZZAPP_OP_STOP, request, sizeof(request), reply, &reply_len) ==
           SDK_STATUS_BAD_HANDLE);
    assert(zzapp_get_be32(windows[2] + ZZAPP_LIFE_STOP_OFF) == 0U);

    extended_request(request, 1U, 0U);
    assert(dispatch(ZZAPP_OP_LIST, request, sizeof(request), reply, &reply_len) ==
           SDK_STATUS_OK);
    assert(reply_len == ZZAPP_LIST_REPLY_BYTES);
    assert(zzapp_get_be32(reply + ZZAPP_LIST_STATE) == ZZAPP_ST_CLAIMED);
    assert(zzapp_get_be32(reply + ZZAPP_LIST_GENERATION) == generation[1]);
    service_test_kernel.threads[1].state = ZZAPP_THREAD_BLOCKED;
    runtime[1].wait_kind = ZZAPP_WAIT_FILE;
    assert(dispatch(ZZAPP_OP_LIST, request, sizeof(request), reply, &reply_len) ==
           SDK_STATUS_OK);
    assert(reply[ZZAPP_LIST_REASONS] == ZZAPP_BLOCK_FILE);
    runtime[1].wait_kind = ZZAPP_WAIT_NONE;
    service_test_kernel.threads[1].state = ZZAPP_THREAD_SLEEPING;
    assert(dispatch(ZZAPP_OP_LIST, request, sizeof(request), reply, &reply_len) ==
           SDK_STATUS_OK);
    assert(reply[ZZAPP_LIST_REASONS] == ZZAPP_BLOCK_SLEEP);
    service_test_kernel.threads[1].state = ZZAPP_THREAD_BLOCKED;
    runtime[1].wait_kind = ZZAPP_WAIT_FILE;
    extended_request(request, 1U, generation[1]);
    assert(dispatch(ZZAPP_OP_STATUS, request, sizeof(request), reply, &reply_len) ==
           SDK_STATUS_OK);
    zzapp_services_on_app_end();
    service_test_kernel.threads[1].state = ZZAPP_THREAD_STOPPED;
    assert(dispatch(ZZAPP_OP_LIST, request, sizeof(request), reply, &reply_len) ==
           SDK_STATUS_OK);
    assert(zzapp_get_be32(reply + ZZAPP_LIST_STATE) == ZZAPP_ST_IDLE);
    assert(reply[ZZAPP_LIST_REASONS] == ZZAPP_BLOCK_NONE);

    assert(dispatch(ZZAPP_OP_STATUS, NULL, 0U, reply, &reply_len) ==
           SDK_STATUS_OK);
    assert(zzapp_get_be32(reply + ZZAPP_STATUS_GENERATION) == generation[0]);
    /* A claim whose launcher never starts a load expires independently. */
    zzapp_services_test_reset();
    for (i = 0; i < ZZAPP_SLOT_COUNT; i++)
        zzapp_services_test_set_window_slot(i, windows[i], sizeof(windows[i]));
    __atomic_store_n(&deferred[0].state, DEFER_POSTED, __ATOMIC_RELEASE);
    assert(dispatch(ZZAPP_OP_ALLOC_WINDOW, NULL, 0U, reply, &reply_len) ==
           SDK_STATUS_BUSY);
    __atomic_store_n(&deferred[0].state, DEFER_IDLE, __ATOMIC_RELEASE);
    extended_request(request, 0U, 0U);
    assert(dispatch(ZZAPP_OP_ALLOC_WINDOW, request, sizeof(request), reply,
                    &reply_len) == SDK_STATUS_OK);
    generation[0] = zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION);
    extended_request(request, 0U, generation[0]);
    assert(dispatch(ZZAPP_OP_LOAD_BEGIN, request, sizeof(request), reply,
                    &reply_len) == SDK_STATUS_QUEUED);
    assert(__atomic_load_n(&deferred[0].state, __ATOMIC_ACQUIRE) ==
           DEFER_POSTED);
    zzapp_services_expire_claims(1999999U);
    extended_request(request, 0U, 0U);
    assert(dispatch(ZZAPP_OP_LIST, request, sizeof(request), reply, &reply_len) ==
           SDK_STATUS_OK);
    assert(zzapp_get_be32(reply + ZZAPP_LIST_STATE) == ZZAPP_ST_CLAIMED);
    assert(reply[ZZAPP_LIST_REASONS + 1U] == ZZAPP_END_NONE);
    zzapp_services_expire_claims(2000000U);
    assert(dispatch(ZZAPP_OP_LIST, request, sizeof(request), reply, &reply_len) ==
           SDK_STATUS_OK);
    assert(zzapp_get_be32(reply + ZZAPP_LIST_STATE) == ZZAPP_ST_IDLE);
    assert(reply[ZZAPP_LIST_REASONS + 1U] == ZZAPP_END_LEASE);
    assert(__atomic_load_n(&deferred[0].state, __ATOMIC_ACQUIRE) ==
           DEFER_IDLE);
    extended_request(request, 0U, 0U);
    assert(dispatch(ZZAPP_OP_ALLOC_WINDOW, request, sizeof(request), reply,
                    &reply_len) == SDK_STATUS_OK);
    assert(zzapp_get_be32(reply + ZZAPP_ALLOC_GENERATION) == generation[0] + 2U);
    zzapp_put_be32(windows[0] + ZZAPP_LIFE_LOW_MEMORY_CROSSINGS_OFF, 7U);
    zzapp_services_on_app_end();
    assert(dispatch(ZZAPP_OP_ALLOC_WINDOW, request, sizeof(request), reply,
                    &reply_len) == SDK_STATUS_OK);
    assert(zzapp_get_be32(windows[0] + ZZAPP_LIFE_LOW_MEMORY_CROSSINGS_OFF) == 0U);
}
