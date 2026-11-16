#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "scheduler.h"
#include "sdk_image_stream.h"
#include "sdk_mailbox.h"

struct SDKMailboxEntry {
    uint8_t request_id[4];
    uint8_t opcode[2];
    uint8_t status[2];
    uint8_t flags[2];
    uint8_t payload_len[2];
    uint8_t user_cookie[4];
    uint8_t payload[48];
};

extern uint16_t sdk_mailbox_test_image_begin_try_defer(
    volatile struct SDKMailboxEntry *req,
    const struct SDKImageStreamBegin *begin);

static taskq_shared_t shared;
static uint16_t begin_status;
static struct SDKImageStreamBegin observed_begin;
static uint16_t observed_owner;
static unsigned begin_calls;

int scheduler_core1_available(void)
{
    return 1;
}

taskq_shared_t *scheduler_shared(void)
{
    return &shared;
}

void scheduler_core1_publish(void) {}
int smp_cpu_id(void) { return 1; }

uint16_t sdk_image_stream_begin_owned(const struct SDKImageStreamBegin *begin,
                                      uint16_t owner_id,
                                      struct SDKImageStreamResult *result)
{
    begin_calls++;
    observed_begin = *begin;
    observed_owner = owner_id;
    memset(result, 0, sizeof(*result));
    result->session = 0x1234U;
    return begin_status;
}

int main(void)
{
    struct SDKMailboxEntry first_request;
    struct SDKMailboxEntry second_request;
    struct SDKImageStreamBegin first_begin;
    struct SDKImageStreamBegin second_begin;
    uint8_t result[TASKQ_RESULT_PAYLOAD];
    uint32_t result_len;
    uint16_t status;

    memset(&shared, 0, sizeof(shared));
    taskq_init(&shared.queue);
    memset(&first_request, 0, sizeof(first_request));
    memset(&second_request, 0, sizeof(second_request));
    memset(&first_begin, 0, sizeof(first_begin));
    memset(&second_begin, 0, sizeof(second_begin));
    first_begin.codec = SDK_IMAGE_CODEC_WEBP;
    first_begin.tile_address = 0x11110000U;
    first_begin.tile_length = 0x1111U;
    second_begin.codec = SDK_IMAGE_CODEC_WEBP;
    second_begin.tile_address = 0x22220000U;
    second_begin.tile_length = 0x2222U;

    assert(sdk_mailbox_test_image_begin_try_defer(&first_request,
                                                   &first_begin) ==
           SDK_STATUS_QUEUED);
    begin_status = SDK_TASK_IN_PROGRESS;
    result_len = 0U;
    status = sdk_mailbox_run_offload_task(&shared.queue.descs[0], result,
                                          &result_len);
    assert(status == SDK_TASK_IN_PROGRESS);
    assert(begin_calls == 1U);
    assert(observed_begin.tile_address == first_begin.tile_address);
    assert(observed_begin.tile_length == first_begin.tile_length);

    assert(sdk_mailbox_test_image_begin_try_defer(&second_request,
                                                   &second_begin) ==
           SDK_STATUS_QUEUED);
    begin_status = SDK_STATUS_OK;
    result_len = 0U;
    status = sdk_mailbox_run_offload_task(&shared.queue.descs[0], result,
                                          &result_len);
    assert(status == SDK_STATUS_OK);
    assert(begin_calls == 2U);
    assert(observed_begin.tile_address == first_begin.tile_address);
    assert(observed_begin.tile_length == first_begin.tile_length);
    assert(result_len == 48U);
    return 0;
}
