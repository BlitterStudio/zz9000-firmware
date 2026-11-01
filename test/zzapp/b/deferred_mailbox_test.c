#define _POSIX_C_SOURCE 200809L
/*
 * Exercises the production deferred APP_LOAD path through the SDK's host
 * request encoder and zz9k.library synchronous call facade.  The host-only
 * flag disables zzapp_services.c's legacy inline completion shortcut so this
 * test drives the core-0 dispatch, core-1 service tick, and completion harvest
 * as separate steps, matching hardware ordering.
 */
#ifndef ZZAPP_HOST_DEFERRED_TEST
#define ZZAPP_HOST_DEFERRED_TEST
#endif
#include "zzapp_services.c"

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
static uint8_t launcher_window[ZZAPP_WINDOW_BYTES];

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

static uint32_t dispatch_request(void)
{
    uint32_t head = zz9k_get_be32(mailbox.descriptor.request_head);
    ZZ9KMailboxWireEntry *entry = &mailbox.request_ring[head];
    uint16_t opcode = zz9k_get_be16(entry->opcode);
    uint16_t payload_len = zz9k_get_be16(entry->payload_len);
    uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint16_t reply_len = 0U;
    uint16_t status;
    assert(payload_len <= sizeof(entry->payload));
    assert(zz9k_get_be32(entry->request_id) != 0U);

    status = zzapp_mailbox_dispatch(opcode, entry->payload, payload_len,
                                    reply, &reply_len, 1,
                                    zz9k_get_be32(entry->request_id),
                                    zz9k_get_be32(entry->user_cookie));
    assert(status == SDK_STATUS_QUEUED);
    assert(reply_len == 0U);
    zz9k_put_be32(mailbox.descriptor.request_head, (head + 1U) % 4U);
    return head;
}

static void post_deferred_completion(uint32_t request_index)
{
    uint32_t request_id;
    uint32_t user_cookie;
    uint16_t opcode;
    uint16_t status;
    uint16_t payload_len = 0U;
    uint8_t payload[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint32_t tail;
    ZZ9KMailboxWireEntry *completion;

    assert(zzapp_mailbox_harvest(&request_id, &user_cookie, &opcode, &status,
                                 payload, &payload_len));
    assert(request_id == zz9k_get_be32(
        mailbox.request_ring[request_index].request_id));
    assert(user_cookie == zz9k_get_be32(
        mailbox.request_ring[request_index].user_cookie));
    assert(opcode == zz9k_get_be16(mailbox.request_ring[request_index].opcode));
    tail = zz9k_get_be32(mailbox.descriptor.completion_tail);
    completion = &mailbox.completion_ring[tail];
    zz9k_put_be32(completion->request_id, request_id);
    zz9k_put_be16(completion->opcode, opcode);
    zz9k_put_be16(completion->status, status);
    zz9k_put_be16(completion->flags, 0U);
    zz9k_put_be16(completion->payload_len, payload_len);
    zz9k_put_be32(completion->user_cookie, user_cookie);
    if (payload_len != 0U)
        memcpy(completion->payload, payload, payload_len);
    zz9k_put_be32(mailbox.descriptor.completion_tail, (tail + 1U) % 4U);
    zzapp_mailbox_harvest_ack();
}

static void run_deferred_call(struct call_args *args)
{
    pthread_t thread;
    uint32_t request_index;

    assert(pthread_create(&thread, NULL, call_on_host, args) == 0);
    wait_for_request();
    request_index = dispatch_request();
    zzapp_services_poll();
    post_deferred_completion(request_index);
    assert(pthread_join(thread, NULL) == 0);
    assert(args->status == ZZ9K_STATUS_OK);
    assert(args->reply.status == ZZ9K_STATUS_OK);
}

int main(void)
{
    ZZ9KContext *context;
    ZZ9KBoard board;
    ZZ9KLibrary library;
    struct call_args begin;
    struct call_args chunk;

    zzapp_boot_init();
    zzapp_services_test_reset();
    memset(launcher_window, 0, sizeof(launcher_window));
    zzapp_services_test_set_window(launcher_window, sizeof(launcher_window));
    init_mailbox();
    memset(&board, 0, sizeof(board));
    board.zorro_version = 3U;
    assert(zz9k_attach_mailbox(&context, &board, &mailbox.descriptor, NULL,
                               NULL) == ZZ9K_STATUS_OK);
    ZZ9KInit(&library);
    assert(ZZ9KAttachContext(&library, context, 1) == ZZ9K_STATUS_OK);

    memset(&begin, 0, sizeof(begin));
    begin.library = &library;
    zz9k_request_init(&begin.request, ZZAPP_OP_LOAD_BEGIN);
    run_deferred_call(&begin);
    assert(zzapp_services_state() == ZZAPP_ST_LOADING);

    memcpy(launcher_window + ZZAPP_STAGING_OFF, "hello", 5U);
    memset(&chunk, 0, sizeof(chunk));
    chunk.library = &library;
    zz9k_request_init(&chunk.request, ZZAPP_OP_LOAD_CHUNK);
    chunk.request.entry.payload_len = ZZAPP_CHUNK_REQ_BYTES;
    zzapp_build_chunk_request(chunk.request.entry.payload.inline_data, 0U, 5U);
    run_deferred_call(&chunk);
    assert(zzapp_services_state() == ZZAPP_ST_LOADING);
    assert(zz9k_get_be16(mailbox.request_ring[1].flags) ==
           ZZ9K_ENTRY_INLINE_PAYLOAD);
    assert(zz9k_get_be16(mailbox.request_ring[1].payload_len) ==
           ZZAPP_CHUNK_REQ_BYTES);
    assert(zzapp_get_be32(mailbox.request_ring[1].payload +
                          ZZAPP_CHUNK_FILE_OFF) == 0U);
    assert(zzapp_get_be32(mailbox.request_ring[1].payload +
                          ZZAPP_CHUNK_LENGTH) == 5U);
    assert(zzapp_get_be32(mailbox.request_ring[1].payload +
                          ZZAPP_CHUNK_STAGING) == ZZAPP_STAGING_OFF);

    assert(zzapp_loader_abort() == ZZ_OK);
    ZZ9KClose(&library);
    return 0;
}
