/* Exercises the shared ZZRun request encoders against the real app dispatcher. */
#define ZZAPP_SERVICES_TEST_LOADER_STUBS
#include "zzapp_services.c"

#include <assert.h>
#include <string.h>

static uint8_t launcher_window[ZZAPP_WINDOW_BYTES];
static unsigned begun;
static unsigned chunks;
static unsigned ended;


static uint32_t wire_loader_state;

static void drain_console_output(void)
{
    uint32_t head = zzapp_get_be32(launcher_window + ZZAPP_CONSOLE_OUT_OFF);

    zzapp_put_be32(launcher_window + ZZAPP_CONSOLE_OUT_OFF + 4U, head);
}

uint32_t zzapp_runtime_state(void)
{
    return wire_loader_state;
}

void zzapp_loader_set_state(uint32_t state)
{
    wire_loader_state = state;
}
int zzapp_loader_begin(void)
{
    begun++;
    zzapp_services_on_launch();
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

static uint16_t dispatch_deferred(uint16_t opcode, const uint8_t *request,
                                  uint16_t request_len, uint32_t request_id,
                                  uint8_t *reply, uint16_t *reply_len)
{
    uint16_t status;
    uint32_t completed_id;

    status = dispatch(opcode, request, request_len, request_id, reply,
                      reply_len);
    assert(status == SDK_STATUS_QUEUED);
    zzapp_services_test_set_core(1);
    zzapp_services_poll();
    zzapp_services_poll();
    assert(zzapp_mailbox_harvest(&completed_id, NULL, NULL, &status, reply,
                                 reply_len));
    assert(completed_id == request_id);
    zzapp_mailbox_harvest_ack();
    assert(!zzapp_mailbox_harvest(NULL, NULL, NULL, NULL, NULL, NULL));
    return status;
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
    zzapp_put_be32(launcher_window + ZZAPP_CONSOLE_OUT_OFF, 7u);
    zzapp_put_be32(launcher_window + ZZAPP_CONSOLE_OUT_OFF + 4U, 3u);
    zzapp_put_be32(launcher_window + ZZAPP_CONSOLE_IN_OFF, 5u);
    zzapp_put_be32(launcher_window + ZZAPP_CONSOLE_IN_OFF + 4U, 1u);
    zzapp_put_be32(launcher_window + ZZAPP_CONSOLE_IN_OFF +
                   ZZAPP_CONSOLE_EOF_OFF, 1u);
    zzapp_put_be32(launcher_window + ZZAPP_MSG_H2A_OFF, 2u);
    zzapp_put_be32(launcher_window + ZZAPP_MSG_A2H_OFF, 3u);
    zzapp_put_be32(launcher_window + ZZAPP_AUDIO_OFF +
                   ZZAPP_AUDIO_HEAD_OFF, 12u);
    zzapp_put_be32(launcher_window + ZZAPP_AUDIO_OFF +
                   ZZAPP_AUDIO_TAIL_OFF, 4u);
    zzapp_put_be32(launcher_window + ZZAPP_AUDIO_OFF +
                   ZZAPP_AUDIO_ROUTE_OFF, ZZAPP_AUDIO_ROUTE_AHI);

    status = dispatch_deferred(ZZAPP_OP_LOAD_BEGIN, 0, 0U, 1U, reply,
                               &reply_len);
    assert(status == SDK_STATUS_OK);
    assert(begun == 1U);
    assert(zzapp_services_state() == ZZAPP_ST_LOADING);
    assert(zzapp_get_be32(launcher_window + ZZAPP_CONSOLE_OUT_OFF) == 0u);
    assert(zzapp_get_be32(launcher_window + ZZAPP_CONSOLE_OUT_OFF + 4U) == 0u);
    assert(zzapp_get_be32(launcher_window + ZZAPP_CONSOLE_IN_OFF) == 0u);
    assert(zzapp_get_be32(launcher_window + ZZAPP_CONSOLE_IN_OFF + 4U) == 0u);
    assert(zzapp_get_be32(launcher_window + ZZAPP_CONSOLE_IN_OFF +
                          ZZAPP_CONSOLE_EOF_OFF) == 0u);
    assert(zzapp_get_be32(launcher_window + ZZAPP_MSG_H2A_OFF) == 0u);
    assert(zzapp_get_be32(launcher_window + ZZAPP_MSG_A2H_OFF) == 0u);
    assert(zzapp_get_be32(launcher_window + ZZAPP_AUDIO_OFF +
                          ZZAPP_AUDIO_HEAD_OFF) == 0u);
    assert(zzapp_get_be32(launcher_window + ZZAPP_AUDIO_OFF +
                          ZZAPP_AUDIO_TAIL_OFF) == 0u);
    assert(zzapp_get_be32(launcher_window + ZZAPP_AUDIO_OFF +
                          ZZAPP_AUDIO_ROUTE_OFF) == ZZAPP_AUDIO_ROUTE_CARD);

    memcpy(launcher_window + ZZAPP_STAGING_OFF, "hello", 5U);
    zzapp_put_be32(request + ZZAPP_CHUNK_FILE_OFF, 0U);
    zzapp_put_be32(request + ZZAPP_CHUNK_LENGTH, 5U);
    zzapp_put_be32(request + ZZAPP_CHUNK_STAGING, ZZAPP_STAGING_OFF - 0x1000U);
    status = dispatch_deferred(ZZAPP_OP_LOAD_CHUNK, request,
                               ZZAPP_CHUNK_REQ_BYTES, 2U, reply, &reply_len);
    assert(status == SDK_STATUS_BAD_REQUEST);
    assert(chunks == 0U);

    zzapp_build_chunk_request(request, 0U, 5U);
    status = dispatch_deferred(ZZAPP_OP_LOAD_CHUNK, request,
                               ZZAPP_CHUNK_REQ_BYTES, 3U, reply, &reply_len);
    assert(status == SDK_STATUS_OK);
    assert(chunks == 1U);

    status = dispatch_deferred(ZZAPP_OP_LOAD_END, 0, 0U, 4U, reply,
                               &reply_len);
    assert(status == SDK_STATUS_OK);
    assert(ended == 1U);

    zzapp_build_start_request(request, 0U);
    status = dispatch(ZZAPP_OP_START, request, ZZAPP_START_REQ_BYTES,
                      5U, reply, &reply_len);
    assert(status == SDK_STATUS_QUEUED);
    zzapp_services_test_set_core(1);
    zzapp_services_poll();
    zzapp_services_poll();
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
    {
        uint8_t input[3] = { 'o', 'k', '!' };
        uint8_t output[3] = { 0 };
        uint8_t message[ZZAPP_MSG_BYTES_PER];
        uint8_t reply_message[ZZAPP_MSG_BYTES_PER];
        uint32_t n = 0U;
        uint32_t i;

        zzapp_put_be32(launcher_window + ZZAPP_CONSOLE_OUT_OFF,
                        ZZAPP_CONSOLE_OUT_CAP);
        zzapp_put_be32(launcher_window + ZZAPP_CONSOLE_OUT_OFF + 4U, 0U);
        zzapp_services_test_set_yield(drain_console_output);
        assert(zzapp_svc_console_write("!", 1U, &n) == ZZ_OK);
        assert(n == 1U);
        assert(launcher_window[ZZAPP_CONSOLE_OUT_OFF + ZZAPP_CONSOLE_HDR] == '!');

        memcpy(launcher_window + ZZAPP_CONSOLE_IN_OFF + ZZAPP_CONSOLE_HDR,
               input, sizeof(input));
        zzapp_put_be32(launcher_window + ZZAPP_CONSOLE_IN_OFF, sizeof(input));
        zzapp_put_be32(launcher_window + ZZAPP_CONSOLE_IN_OFF + 4U, 0U);
        assert(zzapp_svc_console_read(output, sizeof(output), &n) == ZZ_OK);
        assert(n == sizeof(output));
        assert(memcmp(input, output, sizeof(input)) == 0);
        zzapp_put_be32(launcher_window + ZZAPP_CONSOLE_IN_OFF +
                       ZZAPP_CONSOLE_EOF_OFF, 1U);
        assert(zzapp_svc_console_read(output, sizeof(output), &n) == ZZ_OK);
        assert(n == 0U);

        for (i = 0U; i < sizeof(message); i++)
            message[i] = (uint8_t)i;
        assert(zzapp_svc_msg_send(message) == ZZ_OK);
        assert(zzapp_get_be32(launcher_window + ZZAPP_MSG_A2H_OFF) == 1U);
        memcpy(launcher_window + ZZAPP_MSG_H2A_OFF + ZZAPP_MSG_HDR, message,
               sizeof(message));
        zzapp_put_be32(launcher_window + ZZAPP_MSG_H2A_OFF, 1U);
        assert(zzapp_svc_msg_poll(reply_message) == 1);
        assert(memcmp(message, reply_message, sizeof(message)) == 0);
        assert(zzapp_svc_msg_poll(reply_message) == 0);
    }
    {
        zzapp_audio app_audio;
        uint32_t frame;

        zzapp_services_or_flags(ZZAPP_F_LAUNCHER_AUDIO_AHI);
        assert(zzapp_svc_audio_open(22050u, 2u, &app_audio) == ZZ_OK);
        for (frame = 0u; frame < 4u; frame++)
            app_audio.samples[frame * 2u] = (int16_t)frame;
        *app_audio.write_frame = 4u;
        zzapp_services_poll();
        assert(zzapp_get_be32(launcher_window + ZZAPP_AUDIO_OFF +
                              ZZAPP_AUDIO_HEAD_OFF) == 16u);
        zzapp_services_poll();
        assert(zzapp_get_be32(launcher_window + ZZAPP_AUDIO_OFF +
                              ZZAPP_AUDIO_HEAD_OFF) == 16u);
        zzapp_put_be32(launcher_window + ZZAPP_AUDIO_OFF +
                       ZZAPP_AUDIO_TAIL_OFF, 16u);
        zzapp_services_poll();
        assert(*app_audio.read_frame == 4u);
        assert(zzapp_svc_audio_close() == ZZ_OK);
        assert(zzapp_get_be32(launcher_window + ZZAPP_AUDIO_OFF +
                              ZZAPP_AUDIO_ROUTE_OFF) == ZZAPP_AUDIO_ROUTE_CARD);
        assert(zzapp_get_be32(launcher_window + ZZAPP_AUDIO_OFF +
                              ZZAPP_AUDIO_HEAD_OFF) == 0u);
        assert(zzapp_get_be32(launcher_window + ZZAPP_AUDIO_OFF +
                              ZZAPP_AUDIO_TAIL_OFF) == 0u);
        assert(zzapp_svc_audio_open(22050u, 2u, &app_audio) == ZZ_OK);
        assert(zzapp_get_be32(launcher_window + ZZAPP_AUDIO_OFF +
                              ZZAPP_AUDIO_HEAD_OFF) == 0u);
        assert(zzapp_get_be32(launcher_window + ZZAPP_AUDIO_OFF +
                              ZZAPP_AUDIO_TAIL_OFF) == 0u);
        assert(zzapp_svc_audio_close() == ZZ_OK);
    }
    return 0;
}
