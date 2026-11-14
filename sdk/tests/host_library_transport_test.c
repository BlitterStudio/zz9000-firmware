/*
 * Static host-client transport routing checks.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/host.h"
#include "zz9k/library.h"
#include <stdio.h>
#include <string.h>

typedef struct ZZ9KLibraryTransport {
  int (*open)(void *user, ZZ9KBoard *board, void **base,
              uint16_t *version, uint16_t *revision);
  void (*close)(void *base);
  int (*call)(void *base, ZZ9KRequest *request, ZZ9KMailboxEntry *reply,
              uint32_t timeout_ticks);
  int (*call_async)(void *base, ZZ9KAsyncRequest *async,
                    const ZZ9KRequest *request,
                    ZZ9KAsyncCallback callback, void *callback_user_data);
  int (*poll)(void *base, uint32_t max_completions, uint32_t *completed);
  int (*cancel)(void *base, ZZ9KAsyncRequest *async);
  void *user;
} ZZ9KLibraryTransport;

void zz9k_set_library_transport_for_test(
    const ZZ9KLibraryTransport *transport);

struct TestMailbox {
  ZZ9KMailboxDescriptor descriptor;
  ZZ9KMailboxWireEntry request_ring[4];
  ZZ9KMailboxWireEntry completion_ring[4];
};

struct FakeLibrary {
  uint32_t next_request_id;
  uint32_t capability_bits;
  uint32_t last_timeout_ticks;
  ZZ9KMailboxEntry reply;
  ZZ9KAsyncRequest *async;
  ZZ9KAsyncCallback callback;
  void *callback_user_data;
  unsigned char reply_ready;
  unsigned char completion_published;
  unsigned char cancel_saw_completed;
  unsigned char order[4];
  uint32_t order_count;
  uint32_t opens;
  uint32_t closes;
  uint32_t calls;
  uint32_t submits;
  uint32_t polls;
  uint32_t cancels;
  uint16_t revision;
};

static void init_mailbox(struct TestMailbox *mailbox)
{
  memset(mailbox, 0, sizeof(*mailbox));
  zz9k_put_be32(mailbox->descriptor.magic, ZZ9K_ABI_MAGIC);
  zz9k_put_be16(mailbox->descriptor.abi_major, ZZ9K_ABI_VERSION_MAJOR);
  zz9k_put_be32(mailbox->descriptor.request_ring_offset,
                (uint32_t)offsetof(struct TestMailbox, request_ring));
  zz9k_put_be32(mailbox->descriptor.request_ring_entries, 4U);
  zz9k_put_be32(mailbox->descriptor.completion_ring_offset,
                (uint32_t)offsetof(struct TestMailbox, completion_ring));
  zz9k_put_be32(mailbox->descriptor.completion_ring_entries, 4U);
  zz9k_put_be32(mailbox->descriptor.capability_bits, ZZ9K_CAP_MAILBOX);
}

static int fake_open(void *user, ZZ9KBoard *board, void **base,
                     uint16_t *version, uint16_t *revision)
{
  struct FakeLibrary *library = (struct FakeLibrary *)user;

  library->opens++;
  memset(board, 0, sizeof(*board));
  *base = library;
  *version = 2U;
  *revision = library->revision ? library->revision : 36U;
  return ZZ9K_STATUS_OK;
}

static void fake_close(void *base)
{
  ((struct FakeLibrary *)base)->closes++;
}

static int fake_call(void *base, ZZ9KRequest *request, ZZ9KMailboxEntry *reply,
                     uint32_t timeout_ticks)
{
  struct FakeLibrary *library = (struct FakeLibrary *)base;

  library->last_timeout_ticks = timeout_ticks;
  library->calls++;
  memset(reply, 0, sizeof(*reply));
  reply->request_id = ++library->next_request_id;
  reply->opcode = request->entry.opcode;
  reply->status = ZZ9K_STATUS_OK;
  if (request->entry.opcode == ZZ9K_OP_QUERY_CAPS) {
    reply->payload_len = 32U;
    zz9k_put_be32(&reply->payload.inline_data[0], ZZ9K_ABI_MAGIC);
    zz9k_put_be16(&reply->payload.inline_data[4], ZZ9K_ABI_VERSION_MAJOR);
    zz9k_put_be16(&reply->payload.inline_data[6], ZZ9K_ABI_VERSION_MINOR);
    zz9k_put_be32(&reply->payload.inline_data[8], library->capability_bits);
  }
  return ZZ9K_STATUS_OK;
}

static int fake_call_async(void *base, ZZ9KAsyncRequest *async,
                           const ZZ9KRequest *request,
                           ZZ9KAsyncCallback callback, void *callback_user_data)
{
  struct FakeLibrary *library = (struct FakeLibrary *)base;

  library->submits++;
  memset(async, 0, sizeof(*async));
  async->request = *request;
  async->request_id = ++library->next_request_id;
  async->request.entry.request_id = async->request_id;
  async->queued = 1;
  async->status = ZZ9K_STATUS_QUEUED;
  async->callback = callback;
  async->user_data = callback_user_data;
  library->async = async;
  library->callback = callback;
  library->callback_user_data = callback_user_data;
  return ZZ9K_STATUS_QUEUED;
}

static int fake_poll(void *base, uint32_t max_completions, uint32_t *completed)
{
  struct FakeLibrary *library = (struct FakeLibrary *)base;

  (void)max_completions;
  library->polls++;
  *completed = 0;
  if (!library->reply_ready) {
    return ZZ9K_STATUS_OK;
  }
  library->reply_ready = 0;
  library->completion_published = 1;
  library->order[library->order_count++] = 'P';
  library->async->reply = library->reply;
  library->async->status = library->reply.status;
  library->async->queued = 0;
  library->async->completed = 1;
  if (library->callback) {
    library->callback(library->async, library->callback_user_data);
  }
  *completed = 1;
  return ZZ9K_STATUS_OK;
}

static int fake_cancel(void *base, ZZ9KAsyncRequest *async)
{
  struct FakeLibrary *library = (struct FakeLibrary *)base;

  library->cancels++;
  library->order[library->order_count++] = 'C';
  library->cancel_saw_completed = async->completed &&
                                  async->reply.request_id == async->request_id;
  return async->completed ? ZZ9K_STATUS_BAD_REQUEST : ZZ9K_STATUS_OK;
}

static void init_transport(ZZ9KLibraryTransport *transport,
                           struct FakeLibrary *library)
{
  memset(transport, 0, sizeof(*transport));
  transport->open = fake_open;
  transport->close = fake_close;
  transport->call = fake_call;
  transport->call_async = fake_call_async;
  transport->poll = fake_poll;
  transport->cancel = fake_cancel;
  transport->user = library;
}

static int test_context_base_survives_other_close(int close_first)
{
  struct FakeLibrary first_library;
  struct FakeLibrary second_library;
  ZZ9KLibraryTransport first_transport;
  ZZ9KLibraryTransport second_transport;
  ZZ9KContext *first;
  ZZ9KContext *second;
  ZZ9KContext *survivor;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;

  memset(&first_library, 0, sizeof(first_library));
  memset(&second_library, 0, sizeof(second_library));
  init_transport(&first_transport, &first_library);
  init_transport(&second_transport, &second_library);
  zz9k_set_library_transport_for_test(&first_transport);
  if (zz9k_open(&first) != ZZ9K_STATUS_OK) return 1;
  zz9k_set_library_transport_for_test(&second_transport);
  if (zz9k_open(&second) != ZZ9K_STATUS_OK) return 2;

  if (close_first) {
    zz9k_close(first);
    survivor = second;
  } else {
    zz9k_close(second);
    survivor = first;
  }
  memset(&request, 0, sizeof(request));
  request.entry.opcode = ZZ9K_OP_PING;
  if (zz9k_call(survivor, &request, &reply, 1U) != ZZ9K_STATUS_OK) return 3;
  if (close_first) {
    if (first_library.closes != 1U || first_library.calls != 0U ||
        second_library.calls != 1U) return 4;
  } else {
    if (second_library.closes != 1U || second_library.calls != 0U ||
        first_library.calls != 1U) return 5;
  }
  zz9k_close(survivor);
  zz9k_set_library_transport_for_test(0);
  return 0;
}

static int test_completion_cancel_handoff(void)
{
  struct FakeLibrary library;
  ZZ9KLibraryTransport transport;
  ZZ9KContext *ctx;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  uint32_t request_id;

  memset(&library, 0, sizeof(library));
  init_transport(&transport, &library);
  zz9k_set_library_transport_for_test(&transport);
  if (zz9k_open(&ctx) != ZZ9K_STATUS_OK) return 1;
  memset(&request, 0, sizeof(request));
  request.entry.opcode = ZZ9K_OP_PING;
  if (zz9k_submit(ctx, &request, &request_id) != ZZ9K_STATUS_QUEUED) return 2;
  memset(&library.reply, 0, sizeof(library.reply));
  library.reply.request_id = request_id;
  library.reply.opcode = ZZ9K_OP_PING;
  library.reply.status = ZZ9K_STATUS_OK;
  library.reply_ready = 1;
  if (zz9k_poll(ctx, &reply) != ZZ9K_STATUS_OK ||
      reply.request_id != request_id) return 3;
  if (library.cancels != 1U || !library.cancel_saw_completed ||
      library.order_count != 2U || library.order[0] != 'P' ||
      library.order[1] != 'C') return 4;
  zz9k_close(ctx);
  zz9k_set_library_transport_for_test(0);
  return library.closes == 1U ? 0 : 5;
}

static int test_library_irq_refusal(void)
{
  struct FakeLibrary library;
  ZZ9KLibraryTransport transport;
  ZZ9KContext *ctx;
  ZZ9KCaps caps;

  memset(&library, 0, sizeof(library));
  library.capability_bits = ZZ9K_CAP_MAILBOX | ZZ9K_CAP_IRQ_COMPLETION;
  init_transport(&transport, &library);
  zz9k_set_library_transport_for_test(&transport);
  if (zz9k_open(&ctx) != ZZ9K_STATUS_OK) return 1;
  if (zz9k_query_caps(ctx, &caps) != ZZ9K_STATUS_OK ||
      (caps.capability_bits & ZZ9K_CAP_IRQ_COMPLETION) == 0U) return 2;
  if (zz9k_completion_irq_supported(ctx) ||
      zz9k_completion_irq_ack(ctx) != ZZ9K_STATUS_UNSUPPORTED ||
      zz9k_completion_irq_enable(ctx, 1) != ZZ9K_STATUS_UNSUPPORTED ||
      zz9k_arm_completion_irq(ctx) != ZZ9K_STATUS_UNSUPPORTED) return 3;
  zz9k_close(ctx);
  zz9k_set_library_transport_for_test(0);
  return library.closes == 1U ? 0 : 4;
}

static int test_static_proxy_requires_foreign_completion_revision(void)
{
  struct FakeLibrary too_old;
  struct FakeLibrary current;
  ZZ9KLibraryTransport transport;
  ZZ9KContext *ctx = 0;
  int failed = 0;

  memset(&too_old, 0, sizeof too_old);
  too_old.revision = 35U;
  init_transport(&transport, &too_old);
  zz9k_set_library_transport_for_test(&transport);
  if (zz9k_open(&ctx) != ZZ9K_STATUS_LIBRARY_TOO_OLD || ctx != 0 ||
      too_old.opens != 1U || too_old.closes != 1U)
    failed = 1;

  if (!failed) {
    too_old.revision = 34U;
    too_old.opens = 0U;
    too_old.closes = 0U;
    ctx = 0;
    if (zz9k_open(&ctx) != ZZ9K_STATUS_LIBRARY_TOO_OLD || ctx != 0 ||
        too_old.closes != 1U)
      failed = 2;
  }

  if (!failed) {
    memset(&current, 0, sizeof current);
    current.revision = 36U;
    init_transport(&transport, &current);
    zz9k_set_library_transport_for_test(&transport);
    ctx = 0;
    if (zz9k_open(&ctx) != ZZ9K_STATUS_OK || ctx == 0)
      failed = 3;
    else {
      zz9k_close(ctx);
      ctx = 0;
      if (current.closes != 1U)
        failed = 4;
    }
  }

  if (ctx)
    zz9k_close(ctx);
  zz9k_set_library_transport_for_test(0);
  return failed;
}

static int test_direct_fallback(void)
{
  struct TestMailbox mailbox;
  ZZ9KBoard board;
  ZZ9KContext *ctx;
  ZZ9KRequest request;
  uint32_t request_id;

  zz9k_set_library_transport_for_test(0);
  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(&request, 0, sizeof(request));
  request.entry.opcode = ZZ9K_OP_PING;
  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) return 1;
  if (zz9k_submit(ctx, &request, &request_id) != ZZ9K_STATUS_QUEUED ||
      request_id != 1U ||
      zz9k_get_be32(mailbox.request_ring[0].request_id) != 1U) return 2;
  zz9k_close(ctx);
  return 0;
}

int main(void)
{
  int result;

  result = test_context_base_survives_other_close(1);
  if (result) return 10 + result;
  result = test_context_base_survives_other_close(0);
  if (result) return 20 + result;
  result = test_completion_cancel_handoff();
  if (result) return 30 + result;
  result = test_library_irq_refusal();
  if (result) return 40 + result;
  result = test_direct_fallback();
  if (result) return 50 + result;
  result = test_static_proxy_requires_foreign_completion_revision();
  if (result) return 60 + result;
  printf("host library transport: 6 passed\n");
  return 0;
}
