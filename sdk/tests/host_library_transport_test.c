/*
 * Static host-client transport routing checks.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/host.h"
#include <stdio.h>
#include <string.h>

typedef struct ZZ9KLibraryTransportForTest {
  int (*open)(void *user, ZZ9KBoard *board);
  void (*close)(void *user);
  int (*call)(void *user, ZZ9KRequest *request, ZZ9KMailboxEntry *reply,
              uint32_t timeout_ticks);
  int (*submit)(void *user, ZZ9KRequest *request, uint32_t *request_id);
  int (*poll)(void *user, ZZ9KMailboxEntry *reply);
  void *user;
} ZZ9KLibraryTransportForTest;

void zz9k_set_library_transport_for_test(
    const ZZ9KLibraryTransportForTest *transport);

struct TestMailbox {
  ZZ9KMailboxDescriptor descriptor;
  ZZ9KMailboxWireEntry request_ring[4];
  ZZ9KMailboxWireEntry completion_ring[4];
};

struct FakeLibrary {
  uint32_t next_request_id;
  uint32_t static_request_id;
  uint32_t library_request_id;
  ZZ9KMailboxEntry static_reply;
  ZZ9KMailboxEntry library_reply;
  int static_ready;
  int library_ready;
  int opens;
  int closes;
  int calls;
  int submits;
  int polls;
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

static int fake_open(void *user, ZZ9KBoard *board)
{
  struct FakeLibrary *library = (struct FakeLibrary *)user;

  library->opens++;
  memset(board, 0, sizeof(*board));
  return ZZ9K_STATUS_OK;
}

static void fake_close(void *user)
{
  ((struct FakeLibrary *)user)->closes++;
}

static int fake_call(void *user, ZZ9KRequest *request, ZZ9KMailboxEntry *reply,
                     uint32_t timeout_ticks)
{
  struct FakeLibrary *library = (struct FakeLibrary *)user;

  (void)timeout_ticks;
  library->calls++;
  request->entry.request_id = ++library->next_request_id;
  memset(reply, 0, sizeof(*reply));
  reply->request_id = request->entry.request_id;
  reply->opcode = request->entry.opcode;
  reply->status = ZZ9K_STATUS_OK;
  return ZZ9K_STATUS_OK;
}

static int fake_submit(void *user, ZZ9KRequest *request, uint32_t *request_id)
{
  struct FakeLibrary *library = (struct FakeLibrary *)user;

  library->submits++;
  *request_id = ++library->next_request_id;
  request->entry.request_id = *request_id;
  library->static_request_id = *request_id;
  return ZZ9K_STATUS_QUEUED;
}

static int fake_poll(void *user, ZZ9KMailboxEntry *reply)
{
  struct FakeLibrary *library = (struct FakeLibrary *)user;

  library->polls++;
  if (!library->static_ready) {
    return ZZ9K_STATUS_BUSY;
  }
  *reply = library->static_reply;
  library->static_ready = 0;
  return ZZ9K_STATUS_OK;
}

static int fake_library_submit(struct FakeLibrary *library, uint32_t *request_id)
{
  *request_id = ++library->next_request_id;
  library->library_request_id = *request_id;
  return ZZ9K_STATUS_QUEUED;
}

static int fake_library_poll(struct FakeLibrary *library, ZZ9KMailboxEntry *reply)
{
  if (!library->library_ready) {
    return ZZ9K_STATUS_BUSY;
  }
  *reply = library->library_reply;
  library->library_ready = 0;
  return ZZ9K_STATUS_OK;
}

static int test_interleaved_library_routing(void)
{
  struct FakeLibrary library;
  ZZ9KLibraryTransportForTest transport;
  ZZ9KContext *tool;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  uint32_t tool_request_id;
  uint32_t library_request_id;

  memset(&library, 0, sizeof(library));
  memset(&transport, 0, sizeof(transport));
  transport.open = fake_open;
  transport.close = fake_close;
  transport.call = fake_call;
  transport.submit = fake_submit;
  transport.poll = fake_poll;
  transport.user = &library;
  zz9k_set_library_transport_for_test(&transport);

  if (zz9k_open(&tool) != ZZ9K_STATUS_OK || library.opens != 1) {
    zz9k_set_library_transport_for_test(0);
    return 1;
  }

  memset(&request, 0, sizeof(request));
  request.entry.opcode = ZZ9K_OP_APP_QUERY;
  if (zz9k_submit(tool, &request, &tool_request_id) != ZZ9K_STATUS_QUEUED) {
    zz9k_close(tool);
    zz9k_set_library_transport_for_test(0);
    return 2;
  }
  if (fake_library_submit(&library, &library_request_id) != ZZ9K_STATUS_QUEUED) {
    zz9k_close(tool);
    zz9k_set_library_transport_for_test(0);
    return 3;
  }

  memset(&library.library_reply, 0, sizeof(library.library_reply));
  library.library_reply.request_id = library_request_id;
  library.library_reply.opcode = ZZ9K_OP_APP_QUERY;
  library.library_reply.status = ZZ9K_STATUS_OK;
  library.library_ready = 1;
  memset(&library.static_reply, 0, sizeof(library.static_reply));
  library.static_reply.request_id = tool_request_id;
  library.static_reply.opcode = ZZ9K_OP_APP_QUERY;
  library.static_reply.status = ZZ9K_STATUS_OK;
  library.static_ready = 1;

  memset(&reply, 0, sizeof(reply));
  if (zz9k_poll(tool, &reply) != ZZ9K_STATUS_OK ||
      reply.request_id != tool_request_id || library.polls != 1) {
    zz9k_close(tool);
    zz9k_set_library_transport_for_test(0);
    return 4;
  }
  memset(&reply, 0, sizeof(reply));
  if (fake_library_poll(&library, &reply) != ZZ9K_STATUS_OK ||
      reply.request_id != library_request_id) {
    zz9k_close(tool);
    zz9k_set_library_transport_for_test(0);
    return 5;
  }

  memset(&request, 0, sizeof(request));
  request.entry.opcode = ZZ9K_OP_PING;
  memset(&reply, 0, sizeof(reply));
  if (zz9k_call(tool, &request, &reply, 1U) != ZZ9K_STATUS_OK ||
      reply.opcode != ZZ9K_OP_PING || library.calls != 1) {
    zz9k_close(tool);
    zz9k_set_library_transport_for_test(0);
    return 6;
  }

  zz9k_close(tool);
  zz9k_set_library_transport_for_test(0);
  return library.closes == 1 ? 0 : 7;
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
      ZZ9K_STATUS_OK) {
    return 1;
  }
  if (zz9k_submit(ctx, &request, &request_id) != ZZ9K_STATUS_QUEUED ||
      request_id != 1U ||
      zz9k_get_be32(mailbox.request_ring[0].request_id) != 1U) {
    zz9k_close(ctx);
    return 2;
  }
  zz9k_close(ctx);
  return 0;
}

int main(void)
{
  int result;

  result = test_interleaved_library_routing();
  if (result) {
    printf("test_interleaved_library_routing failed: %d\n", result);
    return result;
  }
  result = test_direct_fallback();
  if (result) {
    printf("test_direct_fallback failed: %d\n", result);
    return 10 + result;
  }
  printf("host library transport: 2 passed\n");
  return 0;
}
