/* Exercises the shared ZZRun request encoders against the real app dispatcher. */
#define ZZAPP_SERVICES_TEST_LOADER_STUBS
#include "zzapp_services.c"

#include <assert.h>
#include <string.h>

static uint8_t launcher_window[ZZAPP_WINDOW_BYTES];
static unsigned begun;
static unsigned chunks;
static unsigned ended;

int zzapp_loader_begin(void)
{
    begun++;
    return ZZ_OK;
}

int zzapp_loader_chunk(uint32_t file_off, const void *src, uint32_t len)
{
    assert(file_off == 0U);
    assert(len == 5U);
    assert(memcmp(src, "hello", 5U) == 0);
    chunks++;
    return ZZ_OK;
}

int zzapp_loader_end(void)
{
    ended++;
    return ZZ_OK;
}

int zzapp_loader_alloc_back(uint32_t bytes, uint32_t *addr_out)
{
    (void)bytes;
    (void)addr_out;
    return ZZ_ENOMEM;
}

void zzapp_loader_free_back(uint32_t addr)
{
    (void)addr;
}

static uint16_t dispatch(uint16_t opcode, const uint8_t *request,
                         uint16_t request_len, uint32_t request_id,
                         uint8_t *reply, uint16_t *reply_len)
{
    return zzapp_mailbox_dispatch(opcode, request, request_len, reply,
                                  reply_len, 1, request_id, 0U);
}

int main(void)
{
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES] = { 0 };
    uint16_t reply_len = 0U;
    uint16_t status;
    uint16_t completed_status;
    uint32_t request_id;
    uint32_t argc;

    zzapp_services_test_reset();
    memset(launcher_window, 0, sizeof(launcher_window));
    zzapp_services_test_set_window(launcher_window, sizeof(launcher_window));

    status = dispatch(ZZAPP_OP_LOAD_BEGIN, 0, 0U, 1U, reply, &reply_len);
    assert(status == SDK_STATUS_OK);
    assert(begun == 1U);
    assert(zzapp_services_state() == ZZAPP_ST_LOADING);

    memcpy(launcher_window + ZZAPP_STAGING_OFF, "hello", 5U);
    zzapp_put_be32(request + ZZAPP_CHUNK_FILE_OFF, 0U);
    zzapp_put_be32(request + ZZAPP_CHUNK_LENGTH, 5U);
    zzapp_put_be32(request + ZZAPP_CHUNK_STAGING, ZZAPP_STAGING_OFF - 0x1000U);
    status = dispatch(ZZAPP_OP_LOAD_CHUNK, request, ZZAPP_CHUNK_REQ_BYTES,
                      2U, reply, &reply_len);
    assert(status == SDK_STATUS_BAD_REQUEST);
    assert(chunks == 0U);

    zzapp_build_chunk_request(request, 0U, 5U);
    status = dispatch(ZZAPP_OP_LOAD_CHUNK, request, ZZAPP_CHUNK_REQ_BYTES,
                      3U, reply, &reply_len);
    assert(status == SDK_STATUS_OK);
    assert(chunks == 1U);

    status = dispatch(ZZAPP_OP_LOAD_END, 0, 0U, 4U, reply, &reply_len);
    assert(status == SDK_STATUS_OK);
    assert(ended == 1U);

    zzapp_build_start_request(request, 0U);
    status = dispatch(ZZAPP_OP_START, request, ZZAPP_START_REQ_BYTES,
                      5U, reply, &reply_len);
    assert(status == SDK_STATUS_QUEUED);
    assert(zzapp_services_take_start(&argc));
    assert(argc == 0U);
    zzapp_services_start_entered(ZZ_OK);
    assert(zzapp_mailbox_harvest(&request_id, 0, 0, &completed_status,
                                 reply, &reply_len));
    assert(request_id == 5U);
    assert(completed_status == SDK_STATUS_OK);
    zzapp_mailbox_harvest_ack();

    zzapp_services_set_exit(0);
    zzapp_services_set_state(ZZAPP_ST_IDLE);
    status = dispatch(ZZAPP_OP_STATUS, 0, 0U, 6U, reply, &reply_len);
    assert(status == SDK_STATUS_OK);
    assert(reply_len == ZZAPP_STATUS_REPLY_BYTES);
    assert(zzapp_get_be32(reply + ZZAPP_STATUS_STATE) == ZZAPP_ST_IDLE);
    assert(zzapp_get_be32(reply + ZZAPP_STATUS_EXIT) == 0U);
    return 0;
}
