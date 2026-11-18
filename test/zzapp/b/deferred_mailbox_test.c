#define _POSIX_C_SOURCE 200809L
/*
 * Exercises the production claim and deferred APP_LOAD path through the SDK's
 * host request encoder and zz9k.library synchronous call facade: CLAIM, then
 * LOAD_BEGIN and LOAD_CHUNK of the claim, the image going through the load
 * buffer LOAD_BEGIN names. The host-only flag disables zzapp_services.c's
 * legacy inline completion shortcut so this test drives the core-0 dispatch,
 * core-1 service tick, and completion harvest as separate steps, matching
 * hardware ordering. A claim completes inline; the load operations are
 * deferred and carry their reply token back through the completion.
 */
#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"

#include "mock_client.h"
#include "zz9k/library.h"
#include "zz9k/request.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

void zzapp_boot_init(void);
struct test_mailbox {
    ZZ9KMailboxDescriptor descriptor;
    ZZ9KMailboxWireEntry request_ring[4];
    ZZ9KMailboxWireEntry completion_ring[4];
};

struct call_args {
    ZZ9KLibrary *library;
    ZZ9KRequest request;
    ZZ9KMailboxEntry reply;
    int status;
};

static struct test_mailbox mailbox;
/* Requests the model has taken so far; the ring index of the next. */
static uint32_t request_count;

static void init_mailbox(void)
{
    memset(&mailbox, 0, sizeof(mailbox));
    zz9k_put_be32(mailbox.descriptor.magic, ZZ9K_ABI_MAGIC);
    zz9k_put_be16(mailbox.descriptor.abi_major, ZZ9K_ABI_VERSION_MAJOR);
    zz9k_put_be16(mailbox.descriptor.abi_minor, ZZ9K_ABI_VERSION_MINOR);
    zz9k_put_be32(mailbox.descriptor.descriptor_size,
                  (uint32_t)sizeof(mailbox.descriptor));
    zz9k_put_be32(mailbox.descriptor.request_ring_offset,
                  (uint32_t)offsetof(struct test_mailbox, request_ring));
    zz9k_put_be32(mailbox.descriptor.request_ring_entries, 4U);
    zz9k_put_be32(mailbox.descriptor.completion_ring_offset,
                  (uint32_t)offsetof(struct test_mailbox, completion_ring));
    zz9k_put_be32(mailbox.descriptor.completion_ring_entries, 4U);
    zz9k_put_be32(mailbox.descriptor.capability_bits, ZZ9K_CAP_MAILBOX);
}

static void *call_on_host(void *opaque)
{
    struct call_args *args = opaque;

    args->status = ZZ9KCall(args->library, &args->request, &args->reply,
                             100000000U);
    return NULL;
}

static void wait_for_request(void)
{
    unsigned spins;

    for (spins = 0U; spins != 1000U; ++spins) {
        struct timespec pause = { 0, 1000000L };

        if (zz9k_get_be32(mailbox.descriptor.request_tail) !=
            zz9k_get_be32(mailbox.descriptor.request_head))
            return;
        assert(nanosleep(&pause, NULL) == 0);
    }
    assert(!"host call did not encode a request");
}

/* Posts a completion for the request at ring index `index`. */
static void post_completion(uint32_t index, uint16_t status,
                            const uint8_t *payload, uint16_t payload_len)
{
    uint32_t tail = zz9k_get_be32(mailbox.descriptor.completion_tail);
    ZZ9KMailboxWireEntry *completion = &mailbox.completion_ring[tail];

    zz9k_put_be32(completion->request_id,
                  zz9k_get_be32(mailbox.request_ring[index].request_id));
    zz9k_put_be16(completion->opcode,
                  zz9k_get_be16(mailbox.request_ring[index].opcode));
    zz9k_put_be16(completion->status, status);
    zz9k_put_be16(completion->flags, 0U);
    zz9k_put_be16(completion->payload_len, payload_len);
    zz9k_put_be32(completion->user_cookie,
                  zz9k_get_be32(mailbox.request_ring[index].user_cookie));
    if (payload_len != 0U)
        memcpy(completion->payload, payload, payload_len);
    zz9k_put_be32(mailbox.descriptor.completion_tail, (tail + 1U) % 4U);
}

/* Core 0 takes the request off the ring. Returns the dispatcher's status;
 * `reply` and `reply_len` are its inline reply. */
static uint16_t dispatch_request(uint32_t *index, uint8_t *reply,
                                 uint16_t *reply_len)
{
    uint32_t head = zz9k_get_be32(mailbox.descriptor.request_head);
    ZZ9KMailboxWireEntry *entry = &mailbox.request_ring[head];
    uint16_t opcode = zz9k_get_be16(entry->opcode);
    uint16_t payload_len = zz9k_get_be16(entry->payload_len);
    uint16_t status;

    assert(payload_len <= sizeof(entry->payload));
    assert(zz9k_get_be32(entry->request_id) != 0U);
    *reply_len = 0U;
    zzapp_services_test_set_core(0);
    status = zzapp_mailbox_dispatch(opcode, entry->payload, payload_len,
                                    reply, reply_len, 1,
                                    zz9k_get_be32(entry->request_id),
                                    zz9k_get_be32(entry->user_cookie));
    zz9k_put_be32(mailbox.descriptor.request_head, (head + 1U) % 4U);
    *index = head;
    return status;
}

static void post_deferred_completion(uint32_t request_index, uint8_t *reply,
                                     uint16_t *reply_len)
{
    uint32_t request_id;
    uint32_t user_cookie;
    uint16_t opcode;
    uint16_t status;
    uint16_t payload_len = 0U;
    uint8_t payload[ZZAPP_MAILBOX_PAYLOAD_BYTES];

    assert(zzapp_mailbox_harvest(&request_id, &user_cookie, &opcode, &status,
                                 payload, &payload_len));
    assert(request_id == zz9k_get_be32(
        mailbox.request_ring[request_index].request_id));
    assert(user_cookie == zz9k_get_be32(
        mailbox.request_ring[request_index].user_cookie));
    assert(opcode == zz9k_get_be16(mailbox.request_ring[request_index].opcode));
    post_completion(request_index, status, payload, payload_len);
    zzapp_mailbox_harvest_ack();
    assert(!zzapp_mailbox_harvest(NULL, NULL, NULL, NULL, NULL, NULL));
    memcpy(reply, payload, payload_len);
    *reply_len = payload_len;
}

/* One call through the facade: encoded on the host thread, taken by core 0,
 * run on core 1 when it is deferred, completed through the ring. */
static void run_call(struct call_args *args, uint8_t *reply, uint16_t *reply_len)
{
    pthread_t thread;
    uint32_t request_index;
    uint16_t status;

    assert(pthread_create(&thread, NULL, call_on_host, args) == 0);
    wait_for_request();
    status = dispatch_request(&request_index, reply, reply_len);
    if (status == SDK_STATUS_QUEUED) {
        assert(*reply_len == 0U);
        /* Core 0 can harvest before or after core 1 runs, but never execute. */
        assert(!zzapp_mailbox_harvest(NULL, NULL, NULL, NULL, NULL, NULL));
        zzapp_services_test_set_core(0);
        zzapp_services_poll();
        assert(!zzapp_mailbox_harvest(NULL, NULL, NULL, NULL, NULL, NULL));
        zzapp_services_test_set_core(1);
        zzapp_services_poll();
        zzapp_services_poll();
        post_deferred_completion(request_index, reply, reply_len);
    } else {
        post_completion(request_index, status, reply, *reply_len);
    }
    assert(pthread_join(thread, NULL) == 0);
    assert(args->status == ZZ9K_STATUS_OK);
    assert(args->reply.status == ZZ9K_STATUS_OK);
    request_count++;
}

int main(void)
{
    ZZ9KContext *context;
    ZZ9KBoard board;
    ZZ9KLibrary library;
    struct call_args claim;
    struct call_args begin;
    struct call_args chunk;
    zzapp_xport_env_t env;
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_len = 0U;
    uint32_t handle;
    zzapp_rec_t *rec;
    volatile uint8_t *cb;
    uint8_t *load;

    zzapp_boot_init();
    zzapp_services_test_reset();
    mock_heap_reset();
    env = mock_heap_env();
    zzapp_xport_init(&env);
    init_mailbox();
    memset(&board, 0, sizeof(board));
    board.zorro_version = 3U;
    assert(zz9k_attach_mailbox(&context, &board, &mailbox.descriptor, NULL,
                               NULL) == ZZ9K_STATUS_OK);
    ZZ9KInit(&library);
    assert(ZZ9KAttachContext(&library, context, 1) == ZZ9K_STATUS_OK);

    /* CLAIM completes inline and names the claim and its control block. */
    memset(&claim, 0, sizeof(claim));
    claim.library = &library;
    zz9k_request_init(&claim.request, ZZAPP_OP_CLAIM);
    run_call(&claim, reply, &reply_len);
    assert(reply_len == ZZAPP_CLAIM_REPLY_BYTES);
    handle = zzapp_get_be32(reply + ZZAPP_CLAIM_HANDLE);
    rec = zzapp_rec_by_handle(handle);
    assert(rec != NULL);
    cb = mock_heap_map(zzapp_get_be32(reply + ZZAPP_CLAIM_CB_TOKEN),
                       AAR_CB_BYTES);
    assert(cb != NULL);
    assert(aar_get_be32(cb + AAR_CB_STATE) == AAR_STATE_CLAIMED);

    /* LOAD_BEGIN is deferred: its completion carries the load buffer. */
    memset(&begin, 0, sizeof(begin));
    begin.library = &library;
    zz9k_request_init(&begin.request, ZZAPP_OP_LOAD_BEGIN);
    begin.request.entry.payload_len = ZZAPP_MAILBOX_PAYLOAD_BYTES;
    zzapp_build_load_begin_request(begin.request.entry.payload.inline_data,
                                   handle, 0U);
    run_call(&begin, reply, &reply_len);
    assert(reply_len == ZZAPP_LOAD_BEGIN_REPLY_BYTES);
    assert(rec->claim.state == ZZAPP_ST_LOADING);
    assert(aar_get_be32(cb + AAR_CB_STATE) == AAR_STATE_LOADING);
    /* The client-visible state is diagnostic only: a stale host cache line
     * must not change the loader's private state machine. */
    aar_put_be32(cb + AAR_CB_STATE, AAR_STATE_IDLE);

    load = mock_heap_map(zzapp_get_be32(reply + ZZAPP_LOAD_REPLY_TOKEN),
                         ZZAPP_LOAD_BUFFER_BYTES);
    assert(load != NULL);
    memcpy(load, "hello", 5U);
    memset(&chunk, 0, sizeof(chunk));
    chunk.library = &library;
    zz9k_request_init(&chunk.request, ZZAPP_OP_LOAD_CHUNK);
    chunk.request.entry.payload_len = ZZAPP_MAILBOX_PAYLOAD_BYTES;
    zzapp_build_load_chunk_request(chunk.request.entry.payload.inline_data,
                                   handle, 0U, 5U);
    run_call(&chunk, reply, &reply_len);
    assert(rec->claim.state == ZZAPP_ST_LOADING);
    assert(zz9k_get_be16(mailbox.request_ring[2].flags) ==
           ZZ9K_ENTRY_INLINE_PAYLOAD);
    assert(zz9k_get_be16(mailbox.request_ring[2].payload_len) ==
           ZZAPP_MAILBOX_PAYLOAD_BYTES);
    assert(zzapp_get_be32(mailbox.request_ring[2].payload +
                          ZZAPP_SEL_HANDLE) == handle);
    assert(zzapp_get_be32(mailbox.request_ring[2].payload +
                          ZZAPP_CHUNK_FILE_OFF) == 0U);
    assert(zzapp_get_be32(mailbox.request_ring[2].payload +
                          ZZAPP_CHUNK_LENGTH) == 5U);

    zzapp_services_select_pid(rec->hdr.pid);
    assert(zzapp_loader_abort() == AAR_OK);
    ZZ9KClose(&library);
    return 0;
}
