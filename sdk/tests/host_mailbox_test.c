/*
 * Host-side mailbox behavior checks.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200112L
#endif

#include "zz9k/host.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TEST_SYNC_COOKIE_MASK 0x5aa55aa5UL

struct TestMailbox {
  ZZ9KMailboxDescriptor descriptor;
  ZZ9KMailboxWireEntry request_ring[4];
  ZZ9KMailboxWireEntry completion_ring[4];
};

void zz9k_set_idle_hook_for_test(void (*hook)(void));
void zz9k_set_now_hook_for_test(uint32_t (*hook)(void));
void zz9k_set_offload_timeout_ms(ZZ9KContext *ctx, uint32_t timeout_ms);

static struct TestMailbox *idle_test_mailbox;
static uint32_t idle_test_now_ms;
static uint32_t idle_test_calls;

static void clear_test_env_u32(void)
{
#ifdef _WIN32
  _putenv_s("ZZ9K_TEST_ENV_U32", "");
#else
  unsetenv("ZZ9K_TEST_ENV_U32");
#endif
}

static void set_test_env_u32(const char *value)
{
#ifdef _WIN32
  _putenv_s("ZZ9K_TEST_ENV_U32", value);
#else
  setenv("ZZ9K_TEST_ENV_U32", value, 1);
#endif
}

static void init_mailbox(struct TestMailbox *mailbox)
{
  memset(mailbox, 0, sizeof(*mailbox));
  zz9k_put_be32(mailbox->descriptor.magic, ZZ9K_ABI_MAGIC);
  zz9k_put_be16(mailbox->descriptor.abi_major, ZZ9K_ABI_VERSION_MAJOR);
  zz9k_put_be16(mailbox->descriptor.abi_minor, ZZ9K_ABI_VERSION_MINOR);
  zz9k_put_be32(mailbox->descriptor.descriptor_size,
                (uint32_t)sizeof(mailbox->descriptor));
  zz9k_put_be32(mailbox->descriptor.request_ring_offset,
                (uint32_t)offsetof(struct TestMailbox, request_ring));
  zz9k_put_be32(mailbox->descriptor.request_ring_entries, 4);
  zz9k_put_be32(mailbox->descriptor.completion_ring_offset,
                (uint32_t)offsetof(struct TestMailbox, completion_ring));
  zz9k_put_be32(mailbox->descriptor.completion_ring_entries, 4);
  zz9k_put_be32(mailbox->descriptor.capability_bits,
                ZZ9K_CAP_MAILBOX | ZZ9K_CAP_IRQ_COMPLETION);
}

static void prepare_completion_at(struct TestMailbox *mailbox, uint32_t index,
                                  uint32_t request_id, uint16_t opcode,
                                  uint16_t status, uint16_t payload_len)
{
  zz9k_put_be32(mailbox->completion_ring[index].request_id, request_id);
  zz9k_put_be16(mailbox->completion_ring[index].opcode, opcode);
  zz9k_put_be16(mailbox->completion_ring[index].status, status);
  zz9k_put_be16(mailbox->completion_ring[index].payload_len, payload_len);
  zz9k_put_be32(mailbox->completion_ring[index].user_cookie,
                request_id ^ TEST_SYNC_COOKIE_MASK);
  zz9k_put_be32(mailbox->descriptor.completion_tail,
                (index + 1U) % 4U);
}

static void prepare_completion(struct TestMailbox *mailbox, uint32_t request_id,
                               uint16_t opcode, uint16_t status,
                               uint16_t payload_len)
{
  prepare_completion_at(mailbox, 0, request_id, opcode, status, payload_len);
}

static void complete_query_caps_from_idle_hook(void)
{
  uint32_t request_id;

  if (!idle_test_mailbox) {
    return;
  }
  if (zz9k_get_be32(idle_test_mailbox->descriptor.request_tail) == 0) {
    return;
  }
  if (zz9k_get_be32(idle_test_mailbox->descriptor.completion_tail) != 0) {
    return;
  }

  request_id = zz9k_get_be32(idle_test_mailbox->request_ring[0].request_id);
  prepare_completion(idle_test_mailbox, request_id, ZZ9K_OP_QUERY_CAPS,
                     ZZ9K_STATUS_OK, 40);
  zz9k_put_be32(idle_test_mailbox->completion_ring[0].user_cookie,
                zz9k_get_be32(idle_test_mailbox->request_ring[0].user_cookie));
  zz9k_put_be32(&idle_test_mailbox->completion_ring[0].payload[0],
                ZZ9K_ABI_MAGIC);
  zz9k_put_be16(&idle_test_mailbox->completion_ring[0].payload[4],
                ZZ9K_ABI_VERSION_MAJOR);
  zz9k_put_be16(&idle_test_mailbox->completion_ring[0].payload[6],
                ZZ9K_ABI_VERSION_MINOR);
  zz9k_put_be32(&idle_test_mailbox->completion_ring[0].payload[8],
                ZZ9K_CAP_MAILBOX);
  zz9k_put_be32(&idle_test_mailbox->completion_ring[0].payload[12], 48);
  zz9k_put_be32(&idle_test_mailbox->completion_ring[0].payload[16], 32);
}

static void complete_query_caps_after_stale_reply(void)
{
  uint32_t request_id;

  if (!idle_test_mailbox) {
    return;
  }
  if (zz9k_get_be32(idle_test_mailbox->descriptor.request_tail) == 0) {
    return;
  }
  if (zz9k_get_be32(idle_test_mailbox->descriptor.completion_head) != 1 ||
      zz9k_get_be32(idle_test_mailbox->descriptor.completion_tail) != 1) {
    return;
  }

  request_id = zz9k_get_be32(idle_test_mailbox->request_ring[0].request_id);
  prepare_completion_at(idle_test_mailbox, 1, request_id, ZZ9K_OP_QUERY_CAPS,
                        ZZ9K_STATUS_OK, 40);
  zz9k_put_be32(idle_test_mailbox->completion_ring[1].user_cookie,
                zz9k_get_be32(idle_test_mailbox->request_ring[0].user_cookie));
  zz9k_put_be32(&idle_test_mailbox->completion_ring[1].payload[0],
                ZZ9K_ABI_MAGIC);
  zz9k_put_be16(&idle_test_mailbox->completion_ring[1].payload[4],
                ZZ9K_ABI_VERSION_MAJOR);
  zz9k_put_be16(&idle_test_mailbox->completion_ring[1].payload[6],
                ZZ9K_ABI_VERSION_MINOR);
  zz9k_put_be32(&idle_test_mailbox->completion_ring[1].payload[8],
                ZZ9K_CAP_MAILBOX);
  zz9k_put_be32(&idle_test_mailbox->completion_ring[1].payload[12], 48);
  zz9k_put_be32(&idle_test_mailbox->completion_ring[1].payload[16], 32);
}

static uint32_t idle_test_now_hook(void)
{
  return idle_test_now_ms;
}

static void idle_advance_10ms_without_completion(void)
{
  idle_test_now_ms += 10U;
  idle_test_calls++;
}

static int test_submit_writes_request_without_doorbell(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KRequest request;
  uint16_t doorbell = 0;
  uint32_t request_id = 0;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(&request, 0, sizeof(request));
  request.entry.opcode = ZZ9K_OP_PING;
  request.entry.flags = ZZ9K_ENTRY_INLINE_PAYLOAD;
  request.entry.payload_len = 4;
  request.entry.payload.inline_data[0] = 'p';
  request.entry.payload.inline_data[1] = 'i';
  request.entry.payload.inline_data[2] = 'n';
  request.entry.payload.inline_data[3] = 'g';

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, &doorbell, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (zz9k_submit(ctx, &request, &request_id) != ZZ9K_STATUS_QUEUED) return 2;
  if (request_id == 0) return 3;
  if (doorbell != 0) return 4;
  if (zz9k_get_be32(mailbox.descriptor.request_tail) != 1) return 5;
  if (zz9k_get_be32(mailbox.request_ring[0].request_id) != request_id) return 6;
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) != ZZ9K_OP_PING) return 7;
  if (mailbox.request_ring[0].payload[0] != 'p') return 8;

  zz9k_close(ctx);
  return 0;
}

static int test_submit_rings_advertised_doorbell(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KRequest request;
  uint16_t doorbell = 0;
  uint32_t request_id = 0;

  init_mailbox(&mailbox);
  zz9k_put_be32(mailbox.descriptor.capability_bits,
                ZZ9K_CAP_MAILBOX | ZZ9K_CAP_DOORBELL);
  memset(&board, 0, sizeof(board));
  memset(&request, 0, sizeof(request));
  request.entry.opcode = ZZ9K_OP_PING;
  request.entry.flags = ZZ9K_ENTRY_INLINE_PAYLOAD;

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, &doorbell, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (zz9k_submit(ctx, &request, &request_id) != ZZ9K_STATUS_QUEUED) return 2;
  if (doorbell != 1) return 3;

  zz9k_close(ctx);
  return 0;
}

static int test_submit_only_writes_declared_payload_bytes(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KRequest request;
  uint32_t request_id = 0;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(&request, 0, sizeof(request));
  memset((void *)mailbox.request_ring[0].payload, 0x55,
         sizeof(mailbox.request_ring[0].payload));
  request.entry.opcode = ZZ9K_OP_PING;
  request.entry.flags = ZZ9K_ENTRY_INLINE_PAYLOAD;
  request.entry.payload_len = 4;
  request.entry.payload.inline_data[0] = 'p';
  request.entry.payload.inline_data[1] = 'i';
  request.entry.payload.inline_data[2] = 'n';
  request.entry.payload.inline_data[3] = 'g';

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (zz9k_submit(ctx, &request, &request_id) != ZZ9K_STATUS_QUEUED) return 2;
  if (mailbox.request_ring[0].payload[0] != 'p') return 3;
  if (mailbox.request_ring[0].payload[3] != 'g') return 4;
  if (mailbox.request_ring[0].payload[4] != 0x55) return 5;

  zz9k_close(ctx);
  return 0;
}

static int test_submit_batch_queues_until_request_ring_fills(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KRequest requests[4];
  uint32_t request_ids[4];
  uint32_t submitted = 99;
  uint32_t i;
  int status;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(requests, 0, sizeof(requests));
  memset(request_ids, 0xff, sizeof(request_ids));

  for (i = 0; i < 4U; i++) {
    requests[i].entry.opcode = ZZ9K_OP_PING;
    requests[i].entry.flags = ZZ9K_ENTRY_INLINE_PAYLOAD;
    requests[i].entry.payload_len = 1;
    requests[i].entry.payload.inline_data[0] = (uint8_t)('a' + i);
  }

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  status = zz9k_submit_batch(ctx, requests, 4, request_ids, &submitted);
  if (status != ZZ9K_STATUS_QUEUED) return 2;
  if (submitted != 3U) return 3;
  if (zz9k_get_be32(mailbox.descriptor.request_tail) != 3U) return 4;
  if (request_ids[0] == 0 || request_ids[1] == 0 || request_ids[2] == 0) {
    return 5;
  }
  if (request_ids[3] != 0) return 6;
  if (zz9k_get_be32(mailbox.request_ring[0].request_id) !=
      request_ids[0]) {
    return 7;
  }
  if (mailbox.request_ring[2].payload[0] != 'c') return 8;

  submitted = 99;
  status = zz9k_submit_batch(ctx, &requests[3], 1, &request_ids[3],
                             &submitted);
  if (status != ZZ9K_STATUS_BUSY) return 9;
  if (submitted != 0U) return 10;
  if (request_ids[3] != 0) return 11;

  zz9k_close(ctx);
  return 0;
}

static int test_poll_reads_completion_and_advances_head(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KMailboxEntry reply;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));

  zz9k_put_be32(mailbox.completion_ring[0].request_id, 42);
  zz9k_put_be16(mailbox.completion_ring[0].opcode, ZZ9K_OP_PING);
  zz9k_put_be16(mailbox.completion_ring[0].status, ZZ9K_STATUS_OK);
  zz9k_put_be32(mailbox.descriptor.completion_tail, 1);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&reply, 0, sizeof(reply));
  if (zz9k_poll(ctx, &reply) != ZZ9K_STATUS_OK) return 2;
  if (reply.request_id != 42) return 3;
  if (reply.opcode != ZZ9K_OP_PING) return 4;
  if (reply.status != ZZ9K_STATUS_OK) return 5;
  if (zz9k_get_be32(mailbox.descriptor.completion_head) != 1) return 6;
  if (zz9k_poll(ctx, &reply) != ZZ9K_STATUS_BUSY) return 7;

  zz9k_close(ctx);
  return 0;
}

static int test_poll_batch_reads_available_completions(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KMailboxEntry replies[3];
  uint32_t completed = 99;
  int status;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  prepare_completion_at(&mailbox, 0, 11, ZZ9K_OP_PING,
                        ZZ9K_STATUS_OK, 0);
  prepare_completion_at(&mailbox, 1, 12, ZZ9K_OP_PING,
                        ZZ9K_STATUS_OK, 0);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(replies, 0, sizeof(replies));
  status = zz9k_poll_batch(ctx, replies, 3, &completed);
  if (status != ZZ9K_STATUS_OK) return 2;
  if (completed != 2U) return 3;
  if (replies[0].request_id != 11U) return 4;
  if (replies[1].request_id != 12U) return 5;
  if (zz9k_get_be32(mailbox.descriptor.completion_head) != 2U) return 6;

  completed = 99;
  status = zz9k_poll_batch(ctx, replies, 3, &completed);
  if (status != ZZ9K_STATUS_BUSY) return 7;
  if (completed != 0U) return 8;

  zz9k_close(ctx);
  return 0;
}

static int test_poll_acks_advertised_irq_completion(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KMailboxEntry reply;
  uint16_t irq_ack = 0;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  prepare_completion(&mailbox, 42, ZZ9K_OP_PING, ZZ9K_STATUS_OK, 0);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, &irq_ack) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&reply, 0, sizeof(reply));
  if (zz9k_poll(ctx, &reply) != ZZ9K_STATUS_OK) return 2;
  if (irq_ack != 1) return 3;

  zz9k_close(ctx);
  return 0;
}

static int test_completion_irq_supported_tracks_capability(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }
  if (!zz9k_completion_irq_supported(ctx)) return 2;
  zz9k_close(ctx);

  zz9k_put_be32(mailbox.descriptor.capability_bits, ZZ9K_CAP_MAILBOX);
  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 3;
  }
  if (zz9k_completion_irq_supported(ctx)) return 4;

  zz9k_close(ctx);
  return 0;
}

static int test_shared_heap_board_visible_tracks_zorro_version(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;

  if (zz9k_shared_heap_board_visible(0)) return 1;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  board.zorro_version = 2U;
  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_shared_heap_board_visible(ctx)) return 3;
  zz9k_close(ctx);

  board.zorro_version = 3U;
  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 4;
  }
  if (!zz9k_shared_heap_board_visible(ctx)) return 5;

  zz9k_close(ctx);
  return 0;
}

static int test_query_caps_roundtrips_through_core_service(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KCaps caps;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  prepare_completion(&mailbox, 1, ZZ9K_OP_QUERY_CAPS, ZZ9K_STATUS_OK, 40);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], ZZ9K_ABI_MAGIC);
  zz9k_put_be16(&mailbox.completion_ring[0].payload[4],
                ZZ9K_ABI_VERSION_MAJOR);
  zz9k_put_be16(&mailbox.completion_ring[0].payload[6],
                ZZ9K_ABI_VERSION_MINOR);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8],
                ZZ9K_CAP_MAILBOX | ZZ9K_CAP_SHARED_ALLOC);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[12], 48);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16], 32);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[20], 0);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[24], 0x01020304);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&caps, 0, sizeof(caps));
  if (zz9k_query_caps(ctx, &caps) != ZZ9K_STATUS_OK) return 2;
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) != ZZ9K_OP_QUERY_CAPS) {
    return 3;
  }
  if (caps.magic != ZZ9K_ABI_MAGIC) return 4;
  if (caps.capability_bits != (ZZ9K_CAP_MAILBOX | ZZ9K_CAP_SHARED_ALLOC)) {
    return 5;
  }
  if (caps.max_shared_buffers != 32) return 6;
  if (caps.firmware_version != 0x01020304) return 7;

  zz9k_close(ctx);
  return 0;
}

static int test_query_service_builds_request_and_maps_reply(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KServiceInfo service;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  prepare_completion(&mailbox, 1, ZZ9K_OP_QUERY_SERVICE,
                     ZZ9K_STATUS_OK, 48);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0],
                ZZ9K_SERVICE_IMAGE);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4], 0x00020001UL);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8],
                ZZ9K_CAP_IMAGE_SCALE | ZZ9K_CAP_IMAGE_DECODE);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[12],
                ZZ9K_SERVICE_FLAG_FIRMWARE |
                ZZ9K_SERVICE_FLAG_IMAGE_JPEG_BASELINE |
                ZZ9K_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
                ZZ9K_SERVICE_FLAG_IMAGE_TILE_OUTPUT |
                ZZ9K_SERVICE_FLAG_IMAGE_FRAMEBUFFER_OUTPUT);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16],
                ZZ9K_SERVICE_IMAGE);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[20], 7);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[24], 48);
  memcpy(&mailbox.completion_ring[0].payload[28], "image", 5);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&service, 0, sizeof(service));
  if (zz9k_query_service(ctx, ZZ9K_SERVICE_IMAGE, &service) !=
      ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_QUERY_SERVICE) {
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) !=
      ZZ9K_SERVICE_IMAGE) {
    return 4;
  }
  if (service.service_id != ZZ9K_SERVICE_IMAGE) return 5;
  if (service.version != 0x00020001UL) return 6;
  if (service.capability_bits !=
      (ZZ9K_CAP_IMAGE_SCALE | ZZ9K_CAP_IMAGE_DECODE)) return 7;
  if (service.flags != (ZZ9K_SERVICE_FLAG_FIRMWARE |
                        ZZ9K_SERVICE_FLAG_IMAGE_JPEG_BASELINE |
                        ZZ9K_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
                        ZZ9K_SERVICE_FLAG_IMAGE_TILE_OUTPUT |
                        ZZ9K_SERVICE_FLAG_IMAGE_FRAMEBUFFER_OUTPUT)) return 8;
  if (service.opcode_base != ZZ9K_SERVICE_IMAGE) return 9;
  if (service.opcode_count != 7) return 10;
  if (strcmp(service.name, "image") != 0) return 11;

  zz9k_close(ctx);
  return 0;
}

static int test_ping_echoes_inline_payload(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  uint8_t request_payload[4];
  uint8_t reply_payload[8];
  uint32_t reply_len;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  request_payload[0] = 'z';
  request_payload[1] = 'z';
  request_payload[2] = '9';
  request_payload[3] = 'k';
  prepare_completion(&mailbox, 1, ZZ9K_OP_PING, ZZ9K_STATUS_OK, 4);
  memcpy((void *)mailbox.completion_ring[0].payload, request_payload, 4);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(reply_payload, 0, sizeof(reply_payload));
  reply_len = sizeof(reply_payload);
  if (zz9k_ping(ctx, request_payload, 4, reply_payload, &reply_len) !=
      ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) != ZZ9K_OP_PING) {
    return 3;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].payload_len) != 4) return 4;
  if (memcmp((void *)mailbox.request_ring[0].payload,
             request_payload, 4) != 0) {
    return 5;
  }
  if (reply_len != 4) return 6;
  if (memcmp(reply_payload, request_payload, 4) != 0) return 7;

  zz9k_close(ctx);
  return 0;
}

static int test_status_and_service_helpers_are_stable(void)
{
  if (strcmp(zz9k_status_name(ZZ9K_STATUS_OK), "ok") != 0) return 1;
  if (strcmp(zz9k_status_name(ZZ9K_STATUS_TIMEOUT), "timeout") != 0) {
    return 2;
  }
  if (strcmp(zz9k_status_name(0x12345678), "error") != 0) return 3;
  if (strcmp(zz9k_service_name(ZZ9K_SERVICE_CORE), "core") != 0) return 4;
  if (strcmp(zz9k_service_name(ZZ9K_SERVICE_IMAGE), "image") != 0) return 5;
  if (strcmp(zz9k_service_name(0x77770000UL), "unknown") != 0) return 6;
  if (zz9k_known_service_count() < 5) return 7;
  if (zz9k_known_service_id(0) != ZZ9K_SERVICE_CORE) return 8;
  if (zz9k_known_service_id(zz9k_known_service_count()) != 0xffffffffUL) {
    return 9;
  }
  if (!zz9k_service_advertised_by_caps(
          ZZ9K_SERVICE_CORE, ZZ9K_CAP_MAILBOX)) {
    return 10;
  }
  if (!zz9k_service_advertised_by_caps(
          ZZ9K_SERVICE_MEMORY, ZZ9K_CAP_SHARED_ALLOC)) {
    return 11;
  }
  if (!zz9k_service_advertised_by_caps(
          ZZ9K_SERVICE_SURFACE, ZZ9K_CAP_SURFACES)) {
    return 12;
  }
  if (!zz9k_service_advertised_by_caps(
          ZZ9K_SERVICE_IMAGE, ZZ9K_CAP_IMAGE_SCALE)) {
    return 13;
  }
  if (!zz9k_service_advertised_by_caps(
          ZZ9K_SERVICE_CRYPTO, ZZ9K_CAP_CRYPTO)) {
    return 14;
  }
  if (!zz9k_service_advertised_by_caps(
          ZZ9K_SERVICE_DIAG, ZZ9K_CAP_DIAGNOSTICS)) {
    return 15;
  }
  if (zz9k_service_advertised_by_caps(ZZ9K_SERVICE_GFX, 0U)) return 16;
  if (zz9k_service_advertised_by_caps(ZZ9K_SERVICE_AUDIO, 0U)) return 17;
  if (zz9k_service_advertised_by_caps(ZZ9K_SERVICE_CODEC, 0U)) return 18;
  if (zz9k_service_advertised_by_caps(ZZ9K_SERVICE_STORAGE, 0U)) return 19;
  if (zz9k_service_advertised_by_caps(ZZ9K_SERVICE_MODULE, 0U)) return 20;

  return 0;
}

static int test_alloc_shared_builds_request_and_maps_reply(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KSharedBuffer buffer;
  uintptr_t expected_ptr;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  board.board_addr = 0x10000000UL;
  prepare_completion(&mailbox, 1, ZZ9K_OP_ALLOC_SHARED, ZZ9K_STATUS_OK, 16);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 7);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_ARM_MEMORY_START + 0x2000);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8], 4096);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[12], 0);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&buffer, 0, sizeof(buffer));
  if (zz9k_alloc_shared(ctx, 4096, 64, 0, &buffer) != ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) != ZZ9K_OP_ALLOC_SHARED) {
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) != 4096) return 4;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[4]) != 64) return 5;
  if (buffer.handle != 7) return 6;
  if (buffer.length != 4096) return 7;

  expected_ptr = (uintptr_t)board.board_addr + ZZ9K_AMIGA_MEMORY_OFFSET +
                 0x2000;
  if ((uintptr_t)buffer.data != expected_ptr) return 8;

  zz9k_close(ctx);
  return 0;
}

static int test_free_shared_builds_request(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  prepare_completion(&mailbox, 1, ZZ9K_OP_FREE_SHARED, ZZ9K_STATUS_OK, 0);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (zz9k_free_shared(ctx, 7) != ZZ9K_STATUS_OK) return 2;
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) != ZZ9K_OP_FREE_SHARED) {
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) != 7) return 4;

  zz9k_close(ctx);
  return 0;
}

static int test_mem_fill_builds_request(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  prepare_completion(&mailbox, 1, ZZ9K_OP_MEM_FILL, ZZ9K_STATUS_OK, 0);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (zz9k_mem_fill(ctx, 7, 128, 16, 0xaa) != ZZ9K_STATUS_OK) return 2;
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) != ZZ9K_OP_MEM_FILL) {
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) != 7) return 4;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[4]) != 128) return 5;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[8]) != 16) return 6;
  if (mailbox.request_ring[0].payload[12] != 0xaa) return 7;

  zz9k_close(ctx);
  return 0;
}

static int test_mem_copy_builds_request(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  prepare_completion(&mailbox, 1, ZZ9K_OP_MEM_COPY, ZZ9K_STATUS_OK, 0);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (zz9k_mem_copy(ctx, 9, 32, 7, 16, 128) != ZZ9K_STATUS_OK) return 2;
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) != ZZ9K_OP_MEM_COPY) {
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) != 9) return 4;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[4]) != 32) return 5;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[8]) != 7) return 6;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[12]) != 16) return 7;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[16]) != 128) return 8;

  zz9k_close(ctx);
  return 0;
}

static int test_read_diag_roundtrips_through_diag_service(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KDiagInfo diag;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  prepare_completion(&mailbox, 1, ZZ9K_OP_DIAG_READ, ZZ9K_STATUS_OK, 48);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 123);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4], 4);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8], ZZ9K_STATUS_BUSY);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[12], 2);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16], 3);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[20], 0x00100000);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[24], 0x000f0000);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[28], 0x00080000);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[32], 0x3fb00000);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[36], 64);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[40], 5);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[44], 6);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&diag, 0, sizeof(diag));
  if (zz9k_read_diag(ctx, &diag) != ZZ9K_STATUS_OK) return 2;
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) != ZZ9K_OP_DIAG_READ) {
    return 3;
  }
  if (diag.requests_completed != 123) return 4;
  if (diag.requests_failed != 4) return 5;
  if (diag.last_status != ZZ9K_STATUS_BUSY) return 6;
  if (diag.pending_requests != 2) return 7;
  if (diag.shared_buffers_used != 3) return 8;
  if (diag.shared_heap_total != 0x00100000) return 9;
  if (diag.shared_heap_free != 0x000f0000) return 10;
  if (diag.shared_heap_largest_free != 0x00080000) return 11;
  if (diag.mailbox_arm_addr != 0x3fb00000) return 12;
  if (diag.mailbox_ring_entries != 64) return 13;
  if (diag.surfaces_used != 5) return 14;
  if (diag.allocator_invalid_slots != 6) return 15;

  zz9k_close(ctx);
  return 0;
}

static int test_map_framebuffer_surface_parses_zero_copy_surface(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KSurface surface;
  uintptr_t expected_ptr;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  board.board_addr = 0x10000000UL;
  prepare_completion(&mailbox, 1, ZZ9K_OP_MAP_FRAMEBUFFER_SURFACE,
                     ZZ9K_STATUS_OK, 48);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0],
                ZZ9K_SURFACE_HANDLE_FRAMEBUFFER);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_ARM_MEMORY_START + 0x00dff2f8);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8], 800);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[12], 600);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16], 3200);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[20],
                ZZ9K_SURFACE_FORMAT_BGRA8888);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[24],
                ZZ9K_SURFACE_FLAG_FRAMEBUFFER |
                    ZZ9K_SURFACE_FLAG_CPU_VISIBLE);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[28], 1920000);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&surface, 0, sizeof(surface));
  if (zz9k_map_framebuffer_surface(ctx, &surface) != ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_MAP_FRAMEBUFFER_SURFACE) {
    return 3;
  }
  if (surface.handle != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) return 4;
  if (surface.width != 800) return 5;
  if (surface.height != 600) return 6;
  if (surface.pitch != 3200) return 7;
  if (surface.format != ZZ9K_SURFACE_FORMAT_BGRA8888) return 8;
  if (surface.length != 1920000) return 9;
  if (surface.arm_addr != ZZ9K_ARM_MEMORY_START + 0x00dff2f8) return 10;

  expected_ptr = (uintptr_t)board.board_addr + ZZ9K_AMIGA_MEMORY_OFFSET +
                 0x00dff2f8;
  if ((uintptr_t)surface.data != expected_ptr) return 11;

  zz9k_close(ctx);
  return 0;
}

static int test_map_framebuffer_surface_allows_unmapped_zero_copy_surface(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KSurface surface;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  board.board_addr = 0x10000000UL;
  board.board_size = 0x00100000UL;
  prepare_completion(&mailbox, 1, ZZ9K_OP_MAP_FRAMEBUFFER_SURFACE,
                     ZZ9K_STATUS_OK, 48);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0],
                ZZ9K_SURFACE_HANDLE_FRAMEBUFFER);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_ARM_MEMORY_START + 0x00dff2f8);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8], 800);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[12], 600);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16], 3200);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[20],
                ZZ9K_SURFACE_FORMAT_BGRA8888);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[24],
                ZZ9K_SURFACE_FLAG_FRAMEBUFFER |
                    ZZ9K_SURFACE_FLAG_CPU_VISIBLE);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[28], 1920000);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&surface, 0, sizeof(surface));
  if (zz9k_map_framebuffer_surface(ctx, &surface) != ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_MAP_FRAMEBUFFER_SURFACE) {
    return 3;
  }
  if (surface.handle != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) return 4;
  if (surface.data != 0) return 5;
  if (surface.width != 800) return 6;
  if (surface.height != 600) return 7;
  if (surface.pitch != 3200) return 8;
  if (surface.format != ZZ9K_SURFACE_FORMAT_BGRA8888) return 9;
  if (surface.length != 1920000) return 10;
  if (surface.arm_addr != ZZ9K_ARM_MEMORY_START + 0x00dff2f8) return 11;

  zz9k_close(ctx);
  return 0;
}

static int test_alloc_surface_builds_request_and_maps_reply(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KSurface surface;
  uintptr_t expected_ptr;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  board.board_addr = 0x10000000UL;
  prepare_completion(&mailbox, 1, ZZ9K_OP_ALLOC_SURFACE, ZZ9K_STATUS_OK, 48);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 0x40000001UL);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_ARM_MEMORY_START + 0x00100000);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8], 320);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[12], 200);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16], 1280);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[20],
                ZZ9K_SURFACE_FORMAT_ARGB8888);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[24],
                ZZ9K_SURFACE_FLAG_CPU_VISIBLE);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[28], 256000);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&surface, 0, sizeof(surface));
  if (zz9k_alloc_surface(ctx, 320, 200, ZZ9K_SURFACE_FORMAT_ARGB8888, 0,
                         &surface) != ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_ALLOC_SURFACE) {
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) != 320) return 4;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[4]) != 200) return 5;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[8]) !=
      ZZ9K_SURFACE_FORMAT_ARGB8888) {
    return 6;
  }
  if (surface.handle != 0x40000001UL) return 7;
  if (surface.pitch != 1280) return 8;
  if (surface.length != 256000) return 9;
  if (surface.arm_addr != ZZ9K_ARM_MEMORY_START + 0x00100000) return 10;

  expected_ptr = (uintptr_t)board.board_addr + ZZ9K_AMIGA_MEMORY_OFFSET +
                 0x00100000;
  if ((uintptr_t)surface.data != expected_ptr) return 11;

  zz9k_close(ctx);
  return 0;
}

static int test_alloc_surface_ex_builds_request_with_pitch(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KSurface surface;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  board.board_addr = 0x10000000UL;
  prepare_completion(&mailbox, 1, ZZ9K_OP_ALLOC_SURFACE, ZZ9K_STATUS_OK, 48);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 0x40000002UL);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_ARM_MEMORY_START + 0x00140000);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8], 200);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[12], 100);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16], 1024);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[20],
                ZZ9K_SURFACE_FORMAT_RGB565);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[24],
                ZZ9K_SURFACE_FLAG_CPU_VISIBLE);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[28], 102400);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&surface, 0, sizeof(surface));
  if (zz9k_alloc_surface_ex(ctx, 200, 100, ZZ9K_SURFACE_FORMAT_RGB565, 0,
                            1024, &surface) != ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_ALLOC_SURFACE) {
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) != 200) return 4;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[4]) != 100) return 5;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[8]) !=
      ZZ9K_SURFACE_FORMAT_RGB565) {
    return 6;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[16]) != 1024) return 7;
  if (surface.handle != 0x40000002UL) return 8;
  if (surface.pitch != 1024) return 9;
  if (surface.length != 102400) return 10;

  zz9k_close(ctx);
  return 0;
}

static int test_alloc_surface_rejects_unmapped_arm_address(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KSurface surface;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  board.board_addr = 0x10000000UL;
  prepare_completion(&mailbox, 1, ZZ9K_OP_ALLOC_SURFACE, ZZ9K_STATUS_OK, 48);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 0x40000003UL);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4], 0x3fb00000UL);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8], 16);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[12], 16);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16], 64);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[20],
                ZZ9K_SURFACE_FORMAT_ARGB8888);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[24],
                ZZ9K_SURFACE_FLAG_CPU_VISIBLE);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[28], 1024);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&surface, 0, sizeof(surface));
  if (zz9k_alloc_surface(ctx, 16, 16, ZZ9K_SURFACE_FORMAT_ARGB8888, 0,
                         &surface) != ZZ9K_STATUS_INTERNAL_ERROR) {
    return 2;
  }
  if (surface.data != 0) return 3;

  zz9k_close(ctx);
  return 0;
}

static int test_alloc_surface_maps_legacy_io_window_address(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KSurface surface;
  uintptr_t expected_ptr;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  board.board_addr = 0x10000000UL;
  board.board_size = 0x00010000UL;
  prepare_completion(&mailbox, 1, ZZ9K_OP_ALLOC_SURFACE, ZZ9K_STATUS_OK, 48);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 0x40000005UL);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_SDK_MAILBOX_ARM_ADDRESS);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8], 16);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[12], 16);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16], 64);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[20],
                ZZ9K_SURFACE_FORMAT_ARGB8888);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[24],
                ZZ9K_SURFACE_FLAG_CPU_VISIBLE);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[28], 1024);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&surface, 0, sizeof(surface));
  if (zz9k_alloc_surface(ctx, 16, 16, ZZ9K_SURFACE_FORMAT_ARGB8888, 0,
                         &surface) != ZZ9K_STATUS_OK) {
    return 2;
  }
  if (surface.handle != 0x40000005UL) return 3;
  if (surface.arm_addr != ZZ9K_SDK_MAILBOX_ARM_ADDRESS) return 4;

  expected_ptr = (uintptr_t)board.board_addr + ZZ9K_SDK_MAILBOX_BOARD_OFFSET;
  if ((uintptr_t)surface.data != expected_ptr) return 5;

  zz9k_close(ctx);
  return 0;
}

static int test_alloc_surface_rejects_address_outside_board_window(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KSurface surface;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  board.board_addr = 0x10000000UL;
  board.board_size = 0x00200000UL;
  prepare_completion(&mailbox, 1, ZZ9K_OP_ALLOC_SURFACE, ZZ9K_STATUS_OK, 48);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 0x40000004UL);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_ARM_MEMORY_START + 0x00200000UL);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8], 16);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[12], 16);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16], 64);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[20],
                ZZ9K_SURFACE_FORMAT_ARGB8888);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[24],
                ZZ9K_SURFACE_FLAG_CPU_VISIBLE);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[28], 1024);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&surface, 0, sizeof(surface));
  if (zz9k_alloc_surface(ctx, 16, 16, ZZ9K_SURFACE_FORMAT_ARGB8888, 0,
                         &surface) != ZZ9K_STATUS_INTERNAL_ERROR) {
    return 2;
  }
  if (surface.data != 0) return 3;

  zz9k_close(ctx);
  return 0;
}

static int test_alloc_surface_allows_arm_local_unmapped_surface(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KSurface surface;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  board.board_addr = 0x10000000UL;
  board.board_size = 0x00200000UL;
  prepare_completion(&mailbox, 1, ZZ9K_OP_ALLOC_SURFACE, ZZ9K_STATUS_OK, 48);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 0x40000006UL);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4], 0x03400000UL);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8], 1280);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[12], 720);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16], 5120);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[20],
                ZZ9K_SURFACE_FORMAT_BGRA8888);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[24],
                ZZ9K_SURFACE_FLAG_ARM_LOCAL);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[28], 3686400);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&surface, 0, sizeof(surface));
  if (zz9k_alloc_surface_ex(ctx, 1280, 720, ZZ9K_SURFACE_FORMAT_BGRA8888,
                            ZZ9K_SURFACE_FLAG_ARM_LOCAL, 5120,
                            &surface) != ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[12]) !=
      ZZ9K_SURFACE_FLAG_ARM_LOCAL) {
    return 3;
  }
  if (surface.handle != 0x40000006UL) return 4;
  if (surface.arm_addr != 0x03400000UL) return 5;
  if (surface.flags != ZZ9K_SURFACE_FLAG_ARM_LOCAL) return 6;
  if (surface.data != 0) return 7;

  zz9k_close(ctx);
  return 0;
}

static int test_free_surface_builds_request(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  prepare_completion(&mailbox, 1, ZZ9K_OP_FREE_SURFACE, ZZ9K_STATUS_OK, 0);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (zz9k_free_surface(ctx, 0x40000001UL) != ZZ9K_STATUS_OK) return 2;
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_FREE_SURFACE) {
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) !=
      0x40000001UL) {
    return 4;
  }

  zz9k_close(ctx);
  return 0;
}

static int test_scale_image_builds_request(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KScaleImageDesc desc;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(&desc, 0, sizeof(desc));
  desc.src_surface = 0x40000001UL;
  desc.dst_surface = ZZ9K_SURFACE_HANDLE_FRAMEBUFFER;
  desc.src_x = 1;
  desc.src_y = 2;
  desc.src_w = 160;
  desc.src_h = 100;
  desc.dst_x = 10;
  desc.dst_y = 20;
  desc.dst_w = 320;
  desc.dst_h = 200;
  desc.filter = ZZ9K_SCALE_NEAREST;
  desc.flags = 0;
  prepare_completion(&mailbox, 1, ZZ9K_OP_SCALE_IMAGE, ZZ9K_STATUS_OK, 0);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (zz9k_scale_image(ctx, &desc) != ZZ9K_STATUS_OK) return 2;
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) != ZZ9K_OP_SCALE_IMAGE) {
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) !=
      0x40000001UL) {
    return 4;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[4]) !=
      ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) {
    return 5;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[8]) != 1) return 6;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[12]) != 2) return 7;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[16]) != 160) return 8;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[20]) != 100) return 9;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[24]) != 10) return 10;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[28]) != 20) return 11;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[32]) != 320) return 12;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[36]) != 200) return 13;

  zz9k_close(ctx);
  return 0;
}

static int test_fill_surface_builds_request(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KSurfaceFillDesc desc;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(&desc, 0, sizeof(desc));
  desc.surface = ZZ9K_SURFACE_HANDLE_FRAMEBUFFER;
  desc.x = 10;
  desc.y = 20;
  desc.width = 30;
  desc.height = 40;
  desc.color = 0xff112233UL;
  desc.flags = 0;
  prepare_completion(&mailbox, 1, ZZ9K_OP_FILL_SURFACE, ZZ9K_STATUS_OK, 0);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (zz9k_fill_surface(ctx, &desc) != ZZ9K_STATUS_OK) return 2;
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_FILL_SURFACE) {
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) !=
      ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) {
    return 4;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[4]) != 10) return 5;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[12]) != 30) return 6;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[20]) !=
      0xff112233UL) {
    return 7;
  }

  zz9k_close(ctx);
  return 0;
}

static int test_query_palette_builds_request(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KPaletteQueryDesc desc;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(&desc, 0, sizeof(desc));
  desc.surface = ZZ9K_SURFACE_HANDLE_FRAMEBUFFER;
  desc.start = 0U;
  desc.count = (uint32_t)ZZ9K_PALETTE_MAX_ENTRIES;
  desc.dst_handle = 0x40000009UL;
  desc.dst_offset = 0x40U;
  desc.flags = 0U;
  prepare_completion(&mailbox, 1, ZZ9K_OP_QUERY_PALETTE, ZZ9K_STATUS_OK, 0);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (zz9k_query_palette(ctx, &desc) != ZZ9K_STATUS_OK) return 2;
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_QUERY_PALETTE) {
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) !=
      ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) {
    return 4;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[4]) != 0U) return 5;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[8]) !=
      (uint32_t)ZZ9K_PALETTE_MAX_ENTRIES) {
    return 6;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[12]) != 0x40000009UL) {
    return 7;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[16]) != 0x40U) return 8;

  zz9k_close(ctx);
  return 0;
}

/* Firmware that predates the op answers UNSUPPORTED; the call must surface
 * that verbatim rather than reporting a transport failure, because that is
 * the status callers gate the feature on. */
static int test_query_palette_reports_unsupported(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KPaletteQueryDesc desc;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(&desc, 0, sizeof(desc));
  desc.surface = ZZ9K_SURFACE_HANDLE_FRAMEBUFFER;
  desc.start = 0U;
  desc.count = 16U;
  desc.dst_handle = 0x40000009UL;
  prepare_completion(&mailbox, 1, ZZ9K_OP_QUERY_PALETTE,
                     ZZ9K_STATUS_UNSUPPORTED, 0);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }
  if (zz9k_query_palette(ctx, &desc) != ZZ9K_STATUS_UNSUPPORTED) return 2;

  /* A rejected descriptor must not reach the ring at all. */
  desc.count = 0U;
  if (zz9k_query_palette(ctx, &desc) != ZZ9K_STATUS_BAD_REQUEST) return 3;
  if (zz9k_query_palette(NULL, &desc) != ZZ9K_STATUS_BAD_REQUEST) return 4;

  zz9k_close(ctx);
  return 0;
}

static int test_copy_surface_builds_request(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KSurfaceCopyDesc desc;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(&desc, 0, sizeof(desc));
  desc.src_surface = 0x40000001UL;
  desc.dst_surface = ZZ9K_SURFACE_HANDLE_FRAMEBUFFER;
  desc.src_x = 1;
  desc.src_y = 2;
  desc.dst_x = 3;
  desc.dst_y = 4;
  desc.width = 5;
  desc.height = 6;
  desc.flags = 0;
  prepare_completion(&mailbox, 1, ZZ9K_OP_COPY_SURFACE, ZZ9K_STATUS_OK, 0);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (zz9k_copy_surface(ctx, &desc) != ZZ9K_STATUS_OK) return 2;
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_COPY_SURFACE) {
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) !=
      0x40000001UL) {
    return 4;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[4]) !=
      ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) {
    return 5;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[12]) != 2) return 6;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[20]) != 4) return 7;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[24]) != 5) return 8;
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[28]) != 6) return 9;

  zz9k_close(ctx);
  return 0;
}

static int test_decode_image_builds_request_and_maps_result(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KImageDecodeDesc desc;
  ZZ9KImageDecodeResult result;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(&desc, 0, sizeof(desc));
  desc.src_handle = 0x40000010UL;
  desc.src_offset = 12;
  desc.src_length = 4096;
  desc.dst_surface = 0x40000020UL;
  desc.dst_x = 3;
  desc.dst_y = 4;
  desc.dst_width = 160;
  desc.dst_height = 100;
  desc.output_format = ZZ9K_SURFACE_FORMAT_RGB565;
  desc.flags = ZZ9K_IMAGE_DECODE_FLAG_FIT |
               ZZ9K_IMAGE_DECODE_FLAG_PRESERVE_ASPECT;
  prepare_completion(&mailbox, 1, ZZ9K_OP_DECODE_PNG, ZZ9K_STATUS_OK,
                     sizeof(ZZ9KImageDecodeResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 320);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4], 200);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8],
                ZZ9K_SURFACE_FORMAT_RGB565);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[12],
                ZZ9K_IMAGE_DECODE_RESULT_ALPHA);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16], 32000);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&result, 0, sizeof(result));
  if (zz9k_decode_image(ctx, ZZ9K_OP_DECODE_PNG, &desc, &result) !=
      ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) != ZZ9K_OP_DECODE_PNG) {
    zz9k_close(ctx);
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) !=
      0x40000010UL) {
    zz9k_close(ctx);
    return 4;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[12]) !=
      0x40000020UL) {
    zz9k_close(ctx);
    return 5;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[32]) !=
      ZZ9K_SURFACE_FORMAT_RGB565) {
    zz9k_close(ctx);
    return 6;
  }
  if (result.width != 320 || result.height != 200 ||
      result.output_format != ZZ9K_SURFACE_FORMAT_RGB565 ||
      result.bytes_written != 32000) {
    zz9k_close(ctx);
    return 7;
  }
  if (result.flags != ZZ9K_IMAGE_DECODE_RESULT_ALPHA) {
    zz9k_close(ctx);
    return 8;
  }

  zz9k_close(ctx);
  return 0;
}

static int test_image_session_helpers_roundtrip_results(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KImageSessionBeginDesc begin;
  ZZ9KImageSessionFeedDesc feed;
  ZZ9KImageSessionResult result;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));

  prepare_completion_at(&mailbox, 0, 1, ZZ9K_OP_IMAGE_SESSION_BEGIN,
                        ZZ9K_STATUS_OK,
                        sizeof(ZZ9KImageSessionResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 9U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16],
                ZZ9K_SURFACE_FORMAT_BGRA8888);

  prepare_completion_at(&mailbox, 1, 2, ZZ9K_OP_IMAGE_SESSION_FEED,
                        ZZ9K_STATUS_OK,
                        sizeof(ZZ9KImageSessionResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[1].payload[0], 9U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[4],
                ZZ9K_IMAGE_SESSION_STATE_TILE_READY);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[8], 640U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[12], 480U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[16],
                ZZ9K_SURFACE_FORMAT_BGRA8888);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[24], 32U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[28], 640U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[32], 16U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[36], 4096U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[40], 40960U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[44],
                ZZ9K_IMAGE_SESSION_RESULT_PARTIAL);

  prepare_completion_at(&mailbox, 2, 3, ZZ9K_OP_IMAGE_SESSION_CLOSE,
                        ZZ9K_STATUS_OK, 0);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&begin, 0, sizeof(begin));
  begin.codec = ZZ9K_IMAGE_CODEC_JPEG;
  begin.output_mode = ZZ9K_IMAGE_OUTPUT_TILE_BUFFER;
  begin.output_format = ZZ9K_SURFACE_FORMAT_BGRA8888;
  begin.tile_handle = 0x40000001UL;
  begin.tile_stride = 2560U;
  begin.tile_rows = 16U;
  memset(&result, 0, sizeof(result));
  if (zz9k_image_session_begin(ctx, &begin, &result) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_IMAGE_SESSION_BEGIN) {
    zz9k_close(ctx);
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) !=
      ZZ9K_IMAGE_CODEC_JPEG) {
    zz9k_close(ctx);
    return 4;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[32]) !=
      0x40000001UL) {
    zz9k_close(ctx);
    return 5;
  }
  if (result.session != 9U ||
      result.state != ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT) {
    zz9k_close(ctx);
    return 6;
  }

  memset(&feed, 0, sizeof(feed));
  feed.session = result.session;
  feed.src_handle = 0x40000002UL;
  feed.src_offset = 128U;
  feed.src_length = 4096U;
  memset(&result, 0, sizeof(result));
  if (zz9k_image_session_feed(ctx, &feed, &result) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 7;
  }
  if (zz9k_get_be16(mailbox.request_ring[1].opcode) !=
      ZZ9K_OP_IMAGE_SESSION_FEED) {
    zz9k_close(ctx);
    return 8;
  }
  if (zz9k_get_be32(&mailbox.request_ring[1].payload[0]) != 9U ||
      zz9k_get_be32(&mailbox.request_ring[1].payload[8]) != 128U) {
    zz9k_close(ctx);
    return 9;
  }
  if (result.state != ZZ9K_IMAGE_SESSION_STATE_TILE_READY ||
      result.tile_y != 32U || result.tile_height != 16U ||
      result.bytes_written != 40960U) {
    zz9k_close(ctx);
    return 10;
  }

  if (zz9k_image_session_close(ctx, 9U, 0U) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 11;
  }
  if (zz9k_get_be16(mailbox.request_ring[2].opcode) !=
      ZZ9K_OP_IMAGE_SESSION_CLOSE) {
    zz9k_close(ctx);
    return 12;
  }
  if (zz9k_get_be32(&mailbox.request_ring[2].payload[0]) != 9U) {
    zz9k_close(ctx);
    return 13;
  }

  zz9k_close(ctx);
  return 0;
}

static int test_crypto_hash_builds_request_and_maps_result(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KCryptoHashDesc desc;
  ZZ9KCryptoResult result;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(&desc, 0, sizeof(desc));
  desc.algorithm = ZZ9K_CRYPTO_HASH_SHA256;
  desc.flags = ZZ9K_CRYPTO_HASH_FLAG_HMAC;
  desc.src_handle = 0x40000030UL;
  desc.src_offset = 5;
  desc.src_length = 1024;
  desc.key_handle = 0x40000031UL;
  desc.key_offset = 7;
  desc.key_length = 32;
  desc.dst_handle = 0x40000032UL;
  desc.dst_offset = 9;
  prepare_completion(&mailbox, 1, ZZ9K_OP_CRYPTO_HASH, ZZ9K_STATUS_OK,
                     sizeof(ZZ9KCryptoResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 32);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_CRYPTO_HASH_SHA256);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8], 0x01020304UL);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&result, 0, sizeof(result));
  if (zz9k_crypto_hash(ctx, &desc, &result) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_CRYPTO_HASH) {
    zz9k_close(ctx);
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) !=
      0x40000030UL) {
    zz9k_close(ctx);
    return 4;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[12]) !=
      0x40000032UL) {
    zz9k_close(ctx);
    return 5;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[32]) !=
      ZZ9K_CRYPTO_HASH_SHA256) {
    zz9k_close(ctx);
    return 6;
  }
  if (result.bytes_written != 32 ||
      result.algorithm != ZZ9K_CRYPTO_HASH_SHA256 ||
      result.flags != 0x01020304UL) {
    zz9k_close(ctx);
    return 7;
  }

  zz9k_close(ctx);
  return 0;
}

static int test_crypto_hash_batch_maps_results_by_cookie(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KCryptoHashDesc descs[2];
  ZZ9KCryptoResult results[2];

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(descs, 0, sizeof(descs));
  memset(results, 0xff, sizeof(results));

  descs[0].algorithm = ZZ9K_CRYPTO_HASH_SHA256;
  descs[0].flags = ZZ9K_CRYPTO_HASH_FLAG_HMAC;
  descs[0].src_handle = 0x40000040UL;
  descs[0].src_offset = 0x1000UL;
  descs[0].src_length = 0x4000UL;
  descs[0].key_handle = 0x40000041UL;
  descs[0].key_length = 64U;
  descs[0].dst_handle = 0x40000042UL;
  descs[0].dst_offset = 0U;

  descs[1] = descs[0];
  descs[1].src_offset = 0x5000UL;
  descs[1].dst_offset = 32U;

  prepare_completion_at(&mailbox, 0, 2, ZZ9K_OP_CRYPTO_HASH,
                        ZZ9K_STATUS_OK, sizeof(ZZ9KCryptoResultPayload));
  zz9k_put_be32(mailbox.completion_ring[0].user_cookie, 1U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 32U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_CRYPTO_HASH_SHA256);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8],
                ZZ9K_CRYPTO_HASH_FLAG_HMAC);

  prepare_completion_at(&mailbox, 1, 1, ZZ9K_OP_CRYPTO_HASH,
                        ZZ9K_STATUS_OK, sizeof(ZZ9KCryptoResultPayload));
  zz9k_put_be32(mailbox.completion_ring[1].user_cookie, 0U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[0], 32U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[4],
                ZZ9K_CRYPTO_HASH_SHA256);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[8],
                ZZ9K_CRYPTO_HASH_FLAG_HMAC);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (zz9k_crypto_hash_batch(ctx, descs, results, 2U, 2U,
                             ZZ9K_DEFAULT_TIMEOUT_TICKS) !=
      ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_CRYPTO_HASH) {
    zz9k_close(ctx);
    return 3;
  }
  if (zz9k_get_be32(mailbox.request_ring[0].user_cookie) != 0U ||
      zz9k_get_be32(mailbox.request_ring[1].user_cookie) != 1U) {
    zz9k_close(ctx);
    return 4;
  }
  if (zz9k_get_be32(&mailbox.request_ring[1].payload[4]) != 0x5000UL ||
      zz9k_get_be32(&mailbox.request_ring[1].payload[16]) != 32U) {
    zz9k_close(ctx);
    return 5;
  }
  if (results[0].bytes_written != 32U ||
      results[0].algorithm != ZZ9K_CRYPTO_HASH_SHA256 ||
      results[0].flags != ZZ9K_CRYPTO_HASH_FLAG_HMAC) {
    zz9k_close(ctx);
    return 6;
  }
  if (results[1].bytes_written != 32U ||
      results[1].algorithm != ZZ9K_CRYPTO_HASH_SHA256 ||
      results[1].flags != ZZ9K_CRYPTO_HASH_FLAG_HMAC) {
    zz9k_close(ctx);
    return 7;
  }

  zz9k_close(ctx);
  return 0;
}

static int test_crypto_stream_builds_request_and_maps_result(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KCryptoStreamDesc desc;
  ZZ9KCryptoResult result;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(&desc, 0, sizeof(desc));
  desc.algorithm = ZZ9K_CRYPTO_STREAM_CHACHA20;
  desc.src_handle = 0x40000070UL;
  desc.src_offset = 11U;
  desc.src_length = 4096U;
  desc.dst_handle = 0x40000071UL;
  desc.dst_offset = 13U;
  desc.key_handle = 0x40000072UL;
  desc.key_offset = 17U;
  desc.nonce_handle = 0x40000073UL;
  desc.nonce_offset = 19U;
  desc.counter = 23U;

  prepare_completion(&mailbox, 1, ZZ9K_OP_CRYPTO_STREAM, ZZ9K_STATUS_OK,
                     sizeof(ZZ9KCryptoResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], desc.src_length);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_CRYPTO_STREAM_CHACHA20);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&result, 0, sizeof(result));
  if (zz9k_crypto_stream(ctx, &desc, &result) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_CRYPTO_STREAM) {
    zz9k_close(ctx);
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) !=
      0x40000070UL) {
    zz9k_close(ctx);
    return 4;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[20]) !=
      0x40000072UL) {
    zz9k_close(ctx);
    return 5;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[36]) != 23U) {
    zz9k_close(ctx);
    return 6;
  }
  if (result.bytes_written != desc.src_length ||
      result.algorithm != ZZ9K_CRYPTO_STREAM_CHACHA20) {
    zz9k_close(ctx);
    return 7;
  }

  zz9k_close(ctx);
  return 0;
}

static int test_crypto_stream_batch_maps_results_by_cookie(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KCryptoStreamDesc descs[2];
  ZZ9KCryptoResult results[2];

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(descs, 0, sizeof(descs));
  memset(results, 0xff, sizeof(results));

  descs[0].algorithm = ZZ9K_CRYPTO_STREAM_CHACHA20;
  descs[0].src_handle = 0x40000090UL;
  descs[0].src_offset = 0x1000UL;
  descs[0].src_length = 0x4000UL;
  descs[0].dst_handle = 0x40000091UL;
  descs[0].dst_offset = 0U;
  descs[0].key_handle = 0x40000092UL;
  descs[0].nonce_handle = 0x40000093UL;
  descs[0].counter = 3U;

  descs[1] = descs[0];
  descs[1].src_offset = 0x5000UL;
  descs[1].dst_offset = 0x4000UL;
  descs[1].counter = 4U;

  prepare_completion_at(&mailbox, 0, 2, ZZ9K_OP_CRYPTO_STREAM,
                        ZZ9K_STATUS_OK, sizeof(ZZ9KCryptoResultPayload));
  zz9k_put_be32(mailbox.completion_ring[0].user_cookie, 1U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 0x4000U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_CRYPTO_STREAM_CHACHA20);

  prepare_completion_at(&mailbox, 1, 1, ZZ9K_OP_CRYPTO_STREAM,
                        ZZ9K_STATUS_OK, sizeof(ZZ9KCryptoResultPayload));
  zz9k_put_be32(mailbox.completion_ring[1].user_cookie, 0U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[0], 0x4000U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[4],
                ZZ9K_CRYPTO_STREAM_CHACHA20);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (zz9k_crypto_stream_batch(ctx, descs, results, 2U, 2U,
                               ZZ9K_DEFAULT_TIMEOUT_TICKS) !=
      ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_CRYPTO_STREAM) {
    zz9k_close(ctx);
    return 3;
  }
  if (zz9k_get_be32(mailbox.request_ring[0].user_cookie) != 0U ||
      zz9k_get_be32(mailbox.request_ring[1].user_cookie) != 1U) {
    zz9k_close(ctx);
    return 4;
  }
  if (zz9k_get_be32(&mailbox.request_ring[1].payload[4]) != 0x5000UL ||
      zz9k_get_be32(&mailbox.request_ring[1].payload[16]) != 0x4000UL ||
      zz9k_get_be32(&mailbox.request_ring[1].payload[36]) != 4U) {
    zz9k_close(ctx);
    return 5;
  }
  if (results[0].bytes_written != 0x4000U ||
      results[0].algorithm != ZZ9K_CRYPTO_STREAM_CHACHA20 ||
      results[0].flags != 0U) {
    zz9k_close(ctx);
    return 6;
  }
  if (results[1].bytes_written != 0x4000U ||
      results[1].algorithm != ZZ9K_CRYPTO_STREAM_CHACHA20 ||
      results[1].flags != 0U) {
    zz9k_close(ctx);
    return 7;
  }

  zz9k_close(ctx);
  return 0;
}

static int test_crypto_aead_builds_request_and_maps_result(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KCryptoAeadDesc desc;
  ZZ9KCryptoResult result;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(&desc, 0, sizeof(desc));
  desc.src_handle = 0x400000a0UL;
  desc.src_offset = 0x1000U;
  desc.src_length = 114U;
  desc.dst_handle = 0x400000a1UL;
  desc.dst_offset = 0x2000U;
  desc.aad_handle = 0x400000a2UL;
  desc.aad_offset = 0x40U;
  desc.aad_length = 12U;
  desc.key_handle = 0x400000a3UL;
  desc.key_offset = 0x80U;
  desc.nonce_handle = 0x400000a4UL;
  desc.flags = 0U;

  prepare_completion(&mailbox, 1, ZZ9K_OP_CRYPTO_AEAD, ZZ9K_STATUS_OK,
                     sizeof(ZZ9KCryptoResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0],
                desc.src_length + 16U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_CRYPTO_AEAD_CHACHA20_POLY1305);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8], desc.flags);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&result, 0, sizeof(result));
  if (zz9k_crypto_aead(ctx, &desc, &result) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_CRYPTO_AEAD) {
    zz9k_close(ctx);
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) !=
      0x400000a0UL) {
    zz9k_close(ctx);
    return 4;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[20]) !=
      0x400000a2UL) {
    zz9k_close(ctx);
    return 5;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[40]) !=
      0x400000a4UL) {
    zz9k_close(ctx);
    return 6;
  }
  if (result.bytes_written != desc.src_length + 16U ||
      result.algorithm != ZZ9K_CRYPTO_AEAD_CHACHA20_POLY1305 ||
      result.flags != 0U) {
    zz9k_close(ctx);
    return 7;
  }

  zz9k_close(ctx);
  return 0;
}

static int test_crypto_aead_batch_maps_results_by_cookie(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KCryptoAeadDesc descs[2];
  ZZ9KCryptoResult results[2];

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(descs, 0, sizeof(descs));
  memset(results, 0xff, sizeof(results));

  descs[0].src_handle = 0x400000b0UL;
  descs[0].src_offset = 0x1000UL;
  descs[0].src_length = 0x4000UL;
  descs[0].dst_handle = 0x400000b1UL;
  descs[0].dst_offset = 0U;
  descs[0].aad_handle = 0x400000b2UL;
  descs[0].aad_length = 12U;
  descs[0].key_handle = 0x400000b3UL;
  descs[0].nonce_handle = 0x400000b4UL;

  descs[1] = descs[0];
  descs[1].src_offset = 0x5000UL;
  descs[1].dst_offset = 0x4010UL;

  prepare_completion_at(&mailbox, 0, 2, ZZ9K_OP_CRYPTO_AEAD,
                        ZZ9K_STATUS_OK, sizeof(ZZ9KCryptoResultPayload));
  zz9k_put_be32(mailbox.completion_ring[0].user_cookie, 1U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0],
                0x4000U + 16U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_CRYPTO_AEAD_CHACHA20_POLY1305);

  prepare_completion_at(&mailbox, 1, 1, ZZ9K_OP_CRYPTO_AEAD,
                        ZZ9K_STATUS_OK, sizeof(ZZ9KCryptoResultPayload));
  zz9k_put_be32(mailbox.completion_ring[1].user_cookie, 0U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[0],
                0x4000U + 16U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[4],
                ZZ9K_CRYPTO_AEAD_CHACHA20_POLY1305);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (zz9k_crypto_aead_batch(ctx, descs, results, 2U, 2U,
                             ZZ9K_DEFAULT_TIMEOUT_TICKS) !=
      ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_CRYPTO_AEAD) {
    zz9k_close(ctx);
    return 3;
  }
  if (zz9k_get_be32(mailbox.request_ring[0].user_cookie) != 0U ||
      zz9k_get_be32(mailbox.request_ring[1].user_cookie) != 1U) {
    zz9k_close(ctx);
    return 4;
  }
  if (zz9k_get_be32(&mailbox.request_ring[1].payload[4]) != 0x5000UL ||
      zz9k_get_be32(&mailbox.request_ring[1].payload[16]) != 0x4010UL) {
    zz9k_close(ctx);
    return 5;
  }
  if (results[0].bytes_written != 0x4000U + 16U ||
      results[0].algorithm != ZZ9K_CRYPTO_AEAD_CHACHA20_POLY1305 ||
      results[0].flags != 0U) {
    zz9k_close(ctx);
    return 6;
  }
  if (results[1].bytes_written != 0x4000U + 16U ||
      results[1].algorithm != ZZ9K_CRYPTO_AEAD_CHACHA20_POLY1305 ||
      results[1].flags != 0U) {
    zz9k_close(ctx);
    return 7;
  }

  zz9k_close(ctx);
  return 0;
}

static int test_decompress_builds_request_and_maps_result(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KDecompressDesc desc;
  ZZ9KDecompressResult result;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(&desc, 0, sizeof(desc));
  desc.src_handle = 0x40000060UL;
  desc.src_offset = 11U;
  desc.src_length = 1024U;
  desc.dst_handle = 0x40000061UL;
  desc.dst_offset = 17U;
  desc.dst_capacity = 4096U;
  desc.algorithm = ZZ9K_COMPRESSION_DEFLATE_RAW;
  desc.flags = ZZ9K_DECOMPRESS_FLAG_EXPECT_END;

  prepare_completion(&mailbox, 1, ZZ9K_OP_DECOMPRESS, ZZ9K_STATUS_OK,
                     sizeof(ZZ9KDecompressResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 1024U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4], 4096U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8], 0x89abcdefUL);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[12],
                ZZ9K_COMPRESSION_DEFLATE_RAW);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16],
                ZZ9K_DECOMPRESS_RESULT_STREAM_END);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&result, 0, sizeof(result));
  if (zz9k_decompress(ctx, &desc, &result) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_DECOMPRESS) {
    zz9k_close(ctx);
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) !=
      0x40000060UL) {
    zz9k_close(ctx);
    return 4;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[20]) != 4096U) {
    zz9k_close(ctx);
    return 5;
  }
  if (result.bytes_consumed != 1024U ||
      result.bytes_written != 4096U ||
      result.checksum != 0x89abcdefUL ||
      result.algorithm != ZZ9K_COMPRESSION_DEFLATE_RAW ||
      result.flags != ZZ9K_DECOMPRESS_RESULT_STREAM_END) {
    zz9k_close(ctx);
    return 6;
  }

  zz9k_close(ctx);
  return 0;
}

static int test_decompress_batch_maps_result(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KDecompressBatchDesc desc;
  ZZ9KDecompressBatchResult result;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(&desc, 0, sizeof(desc));
  desc.arena_handle = 0x77U;
  desc.arena_offset = 0U;
  desc.arena_length = 0x1000U;

  prepare_completion(&mailbox, 1, ZZ9K_OP_DECOMPRESS_BATCH, ZZ9K_STATUS_OK,
                     sizeof(ZZ9KDecompressBatchResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 3U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4], 2U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8], 1U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[12], 0U);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&result, 0, sizeof(result));
  if (zz9k_decompress_batch(ctx, &desc, &result) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_DECOMPRESS_BATCH) {
    zz9k_close(ctx);
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) != 0x77U) {
    zz9k_close(ctx);
    return 4;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[8]) != 0x1000U) {
    zz9k_close(ctx);
    return 5;
  }
  if (result.members_total != 3U || result.members_ok != 2U ||
      result.members_failed != 1U) {
    zz9k_close(ctx);
    return 6;
  }

  if (zz9k_decompress_batch(0, &desc, &result) != ZZ9K_STATUS_BAD_REQUEST) {
    zz9k_close(ctx);
    return 7;
  }
  if (zz9k_decompress_batch(ctx, 0, &result) != ZZ9K_STATUS_BAD_REQUEST) {
    zz9k_close(ctx);
    return 8;
  }
  if (zz9k_decompress_batch(ctx, &desc, 0) != ZZ9K_STATUS_BAD_REQUEST) {
    zz9k_close(ctx);
    return 9;
  }

  zz9k_close(ctx);
  return 0;
}

static int test_decompress_stream_helpers_roundtrip_results(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KDecompressStreamBeginDesc begin_desc;
  ZZ9KDecompressStreamFeedDesc feed_desc;
  ZZ9KDecompressStreamReadDesc read_desc;
  ZZ9KDecompressStreamResult result;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(&begin_desc, 0, sizeof(begin_desc));
  begin_desc.src_handle = 0x40000070UL;
  begin_desc.src_offset = 13U;
  begin_desc.src_length = 2048U;
  begin_desc.output_limit = 8192U;
  begin_desc.algorithm = ZZ9K_COMPRESSION_LZMA_ALONE;
  begin_desc.flags = ZZ9K_DECOMPRESS_FLAG_EXPECT_END;

  prepare_completion(&mailbox, 1, ZZ9K_OP_DECOMPRESS_STREAM_BEGIN,
                     ZZ9K_STATUS_OK,
                     sizeof(ZZ9KDecompressStreamResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 5U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4], 0U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8], 0U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[12], 0U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16],
                ZZ9K_COMPRESSION_LZMA_ALONE);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[20], 0U);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&result, 0, sizeof(result));
  if (zz9k_decompress_stream_begin(ctx, &begin_desc, &result) !=
      ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_DECOMPRESS_STREAM_BEGIN) {
    zz9k_close(ctx);
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) !=
      0x40000070UL) {
    zz9k_close(ctx);
    return 4;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[12]) != 8192U ||
      result.session != 5U ||
      result.algorithm != ZZ9K_COMPRESSION_LZMA_ALONE) {
    zz9k_close(ctx);
    return 5;
  }

  prepare_completion_at(&mailbox, 1, 2, ZZ9K_OP_DECOMPRESS_STREAM_FEED,
                        ZZ9K_STATUS_OK,
                        sizeof(ZZ9KDecompressStreamResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[1].payload[0], 5U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[16],
                ZZ9K_COMPRESSION_LZMA_ALONE);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[20],
                ZZ9K_DECOMPRESS_RESULT_NEED_INPUT);

  memset(&feed_desc, 0, sizeof(feed_desc));
  feed_desc.session = result.session;
  feed_desc.src_handle = 0x40000072UL;
  feed_desc.src_offset = 23U;
  feed_desc.src_length = 1024U;
  feed_desc.flags = ZZ9K_DECOMPRESS_STREAM_FEED_EOF;
  memset(&result, 0, sizeof(result));
  if (zz9k_decompress_stream_feed(ctx, &feed_desc, &result) !=
      ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 12;
  }
  if (zz9k_get_be16(mailbox.request_ring[1].opcode) !=
      ZZ9K_OP_DECOMPRESS_STREAM_FEED) {
    zz9k_close(ctx);
    return 13;
  }
  if (zz9k_get_be32(&mailbox.request_ring[1].payload[0]) != 5U ||
      zz9k_get_be32(&mailbox.request_ring[1].payload[4]) !=
          0x40000072UL ||
      zz9k_get_be32(&mailbox.request_ring[1].payload[12]) != 1024U ||
      zz9k_get_be32(&mailbox.request_ring[1].payload[16]) !=
          ZZ9K_DECOMPRESS_STREAM_FEED_EOF ||
      result.flags != ZZ9K_DECOMPRESS_RESULT_NEED_INPUT) {
    zz9k_close(ctx);
    return 14;
  }

  prepare_completion_at(&mailbox, 2, 3, ZZ9K_OP_DECOMPRESS_STREAM_READ,
                     ZZ9K_STATUS_OK,
                     sizeof(ZZ9KDecompressStreamResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[2].payload[0], 5U);
  zz9k_put_be32(&mailbox.completion_ring[2].payload[4], 2048U);
  zz9k_put_be32(&mailbox.completion_ring[2].payload[8], 8192U);
  zz9k_put_be32(&mailbox.completion_ring[2].payload[12], 0x44556677UL);
  zz9k_put_be32(&mailbox.completion_ring[2].payload[16],
                ZZ9K_COMPRESSION_LZMA_ALONE);
  zz9k_put_be32(&mailbox.completion_ring[2].payload[20],
                ZZ9K_DECOMPRESS_RESULT_STREAM_END |
                ZZ9K_DECOMPRESS_RESULT_CHECKSUM_VALID);

  memset(&read_desc, 0, sizeof(read_desc));
  read_desc.session = result.session;
  read_desc.dst_handle = 0x40000071UL;
  read_desc.dst_offset = 19U;
  read_desc.dst_capacity = 32768U;
  memset(&result, 0, sizeof(result));
  if (zz9k_decompress_stream_read(ctx, &read_desc, &result) !=
      ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 6;
  }
  if (zz9k_get_be16(mailbox.request_ring[1].opcode) !=
      ZZ9K_OP_DECOMPRESS_STREAM_FEED) {
    zz9k_close(ctx);
    return 15;
  }
  if (zz9k_get_be16(mailbox.request_ring[2].opcode) !=
      ZZ9K_OP_DECOMPRESS_STREAM_READ) {
    zz9k_close(ctx);
    return 7;
  }
  if (zz9k_get_be32(&mailbox.request_ring[2].payload[0]) != 5U ||
      zz9k_get_be32(&mailbox.request_ring[2].payload[4]) !=
          0x40000071UL ||
      zz9k_get_be32(&mailbox.request_ring[2].payload[12]) != 32768U ||
      result.bytes_written != 8192U ||
      result.checksum != 0x44556677UL ||
      result.flags != (ZZ9K_DECOMPRESS_RESULT_STREAM_END |
                       ZZ9K_DECOMPRESS_RESULT_CHECKSUM_VALID)) {
    zz9k_close(ctx);
    return 8;
  }

  zz9k_put_be32(mailbox.descriptor.request_head, 3U);
  prepare_completion_at(&mailbox, 3, 4, ZZ9K_OP_DECOMPRESS_STREAM_CLOSE,
                        ZZ9K_STATUS_OK, 0U);
  zz9k_put_be32(mailbox.descriptor.completion_tail, 0U);
  if (zz9k_decompress_stream_close(ctx, 5U, 0U) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 9;
  }
  if (zz9k_get_be16(mailbox.request_ring[3].opcode) !=
      ZZ9K_OP_DECOMPRESS_STREAM_CLOSE) {
    zz9k_close(ctx);
    return 10;
  }
  if (zz9k_get_be32(&mailbox.request_ring[3].payload[0]) != 5U) {
    zz9k_close(ctx);
    return 11;
  }

  zz9k_close(ctx);
  return 0;
}

static int test_audio_stream_helpers_roundtrip_results(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KAudioStreamBeginDesc begin;
  ZZ9KAudioStreamFeedDesc feed;
  ZZ9KAudioStreamResult result;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(&begin, 0, sizeof(begin));
  begin.mp3_ring_handle = 0x40000080UL;
  begin.mp3_ring_capacity = 524288U;
  begin.pcm_ring_handle = 0x40000081UL;
  begin.pcm_ring_capacity = 1048576U;
  begin.output_format = ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE;
  begin.low_water_bytes = 32768U;
  begin.high_water_bytes = 65536U;

  prepare_completion(&mailbox, 1, ZZ9K_OP_AUDIO_STREAM_BEGIN,
                     ZZ9K_STATUS_OK,
                     sizeof(ZZ9KAudioStreamResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 11U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_AUDIO_STREAM_STATE_STREAMING);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16],
                ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&result, 0, sizeof(result));
  if (zz9k_audio_stream_begin(ctx, &begin, &result) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_AUDIO_STREAM_BEGIN) {
    zz9k_close(ctx);
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) !=
          0x40000080UL ||
      zz9k_get_be32(&mailbox.request_ring[0].payload[12]) != 1048576U ||
      result.session != 11U ||
      result.state != ZZ9K_AUDIO_STREAM_STATE_STREAMING) {
    zz9k_close(ctx);
    return 4;
  }

  prepare_completion_at(&mailbox, 1, 2, ZZ9K_OP_AUDIO_STREAM_FEED,
                        ZZ9K_STATUS_OK,
                        sizeof(ZZ9KAudioStreamResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[1].payload[0], 11U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[4],
                ZZ9K_AUDIO_STREAM_STATE_STREAMING);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[8], 44100U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[12], 2U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[16],
                ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[20], 4096U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[24], 8192U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[36], 4096U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[40], 8192U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[44],
                ZZ9K_AUDIO_STREAM_RESULT_PCM_READY);

  memset(&feed, 0, sizeof(feed));
  feed.session = 11U;
  feed.src_handle = 0x40000082UL;
  feed.src_length = 4096U;
  feed.flags = ZZ9K_AUDIO_STREAM_FEED_EOF;
  memset(&result, 0, sizeof(result));
  if (zz9k_audio_stream_feed(ctx, &feed, &result) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 5;
  }
  if (zz9k_get_be16(mailbox.request_ring[1].opcode) !=
      ZZ9K_OP_AUDIO_STREAM_FEED) {
    zz9k_close(ctx);
    return 6;
  }
  if (zz9k_get_be32(&mailbox.request_ring[1].payload[0]) != 11U ||
      zz9k_get_be32(&mailbox.request_ring[1].payload[4]) !=
          0x40000082UL ||
      zz9k_get_be32(&mailbox.request_ring[1].payload[16]) !=
          ZZ9K_AUDIO_STREAM_FEED_EOF ||
      result.bytes_produced != 8192U ||
      result.flags != ZZ9K_AUDIO_STREAM_RESULT_PCM_READY) {
    zz9k_close(ctx);
    return 7;
  }

  prepare_completion_at(&mailbox, 2, 3, ZZ9K_OP_AUDIO_STREAM_READ,
                        ZZ9K_STATUS_OK,
                        sizeof(ZZ9KAudioStreamResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[2].payload[0], 11U);
  zz9k_put_be32(&mailbox.completion_ring[2].payload[4],
                ZZ9K_AUDIO_STREAM_STATE_DONE);
  zz9k_put_be32(&mailbox.completion_ring[2].payload[16],
                ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE);
  zz9k_put_be32(&mailbox.completion_ring[2].payload[28], 8192U);
  zz9k_put_be32(&mailbox.completion_ring[2].payload[44],
                ZZ9K_AUDIO_STREAM_RESULT_DONE);
  if (zz9k_audio_stream_read(ctx, 11U, 8192U, 0U, &result) !=
      ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 8;
  }
  if (zz9k_get_be16(mailbox.request_ring[2].opcode) !=
      ZZ9K_OP_AUDIO_STREAM_READ ||
      zz9k_get_be32(&mailbox.request_ring[2].payload[4]) != 8192U ||
      result.state != ZZ9K_AUDIO_STREAM_STATE_DONE) {
    zz9k_close(ctx);
    return 9;
  }

  prepare_completion_at(&mailbox, 3, 4, ZZ9K_OP_AUDIO_STREAM_CLOSE,
                        ZZ9K_STATUS_OK,
                        sizeof(ZZ9KAudioStreamResultPayload));
  zz9k_put_be32(mailbox.descriptor.request_head, 1U);
  zz9k_put_be32(&mailbox.completion_ring[3].payload[0], 11U);
  zz9k_put_be32(&mailbox.completion_ring[3].payload[4],
                ZZ9K_AUDIO_STREAM_STATE_DONE);
  zz9k_put_be32(&mailbox.completion_ring[3].payload[16],
                ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE);
  if (zz9k_audio_stream_close(ctx, 11U, 0U, &result) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 10;
  }
  if (zz9k_get_be16(mailbox.request_ring[3].opcode) !=
      ZZ9K_OP_AUDIO_STREAM_CLOSE ||
      zz9k_get_be32(&mailbox.request_ring[3].payload[0]) != 11U) {
    zz9k_close(ctx);
    return 11;
  }

  zz9k_close(ctx);
  return 0;
}

static int test_audio_stream_play_stop_roundtrip_results(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KAudioStreamResult result;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));

  prepare_completion(&mailbox, 1, ZZ9K_OP_AUDIO_STREAM_PLAY,
                     ZZ9K_STATUS_OK,
                     sizeof(ZZ9KAudioStreamResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 11U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_AUDIO_STREAM_STATE_STREAMING);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16],
                ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  memset(&result, 0, sizeof(result));
  if (zz9k_audio_stream_play(ctx, 11U, 0U, &result) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_AUDIO_STREAM_PLAY) {
    zz9k_close(ctx);
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[0]) != 11U ||
      result.session != 11U ||
      result.state != ZZ9K_AUDIO_STREAM_STATE_STREAMING) {
    zz9k_close(ctx);
    return 4;
  }

  prepare_completion_at(&mailbox, 1, 2, ZZ9K_OP_AUDIO_STREAM_STOP,
                        ZZ9K_STATUS_OK,
                        sizeof(ZZ9KAudioStreamResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[1].payload[0], 11U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[4],
                ZZ9K_AUDIO_STREAM_STATE_STREAMING);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[16],
                ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE);
  memset(&result, 0, sizeof(result));
  if (zz9k_audio_stream_stop(ctx, 11U, 0U, &result) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 5;
  }
  if (zz9k_get_be16(mailbox.request_ring[1].opcode) !=
      ZZ9K_OP_AUDIO_STREAM_STOP) {
    zz9k_close(ctx);
    return 6;
  }
  if (zz9k_get_be32(&mailbox.request_ring[1].payload[0]) != 11U ||
      result.session != 11U) {
    zz9k_close(ctx);
    return 7;
  }

  zz9k_close(ctx);
  return 0;
}

static int test_sync_call_yields_bus_before_polling_completion(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KCaps caps;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  idle_test_mailbox = &mailbox;
  zz9k_set_idle_hook_for_test(complete_query_caps_from_idle_hook);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    zz9k_set_idle_hook_for_test(0);
    idle_test_mailbox = 0;
    return 1;
  }

  memset(&caps, 0, sizeof(caps));
  if (zz9k_query_caps(ctx, &caps) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    zz9k_set_idle_hook_for_test(0);
    idle_test_mailbox = 0;
    return 2;
  }
  if (caps.capability_bits != ZZ9K_CAP_MAILBOX) {
    zz9k_close(ctx);
    zz9k_set_idle_hook_for_test(0);
    idle_test_mailbox = 0;
    return 3;
  }

  zz9k_close(ctx);
  zz9k_set_idle_hook_for_test(0);
  idle_test_mailbox = 0;
  return 0;
}

static int test_sync_call_skips_stale_completion_until_matching_reply(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KCaps caps;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  prepare_completion(&mailbox, 0xdeadbeefUL, ZZ9K_OP_PING, ZZ9K_STATUS_OK, 0);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  idle_test_mailbox = &mailbox;
  zz9k_set_idle_hook_for_test(complete_query_caps_after_stale_reply);
  memset(&caps, 0, sizeof(caps));
  if (zz9k_query_caps(ctx, &caps) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    zz9k_set_idle_hook_for_test(0);
    idle_test_mailbox = 0;
    return 2;
  }
  zz9k_set_idle_hook_for_test(0);
  idle_test_mailbox = 0;

  if (caps.magic != ZZ9K_ABI_MAGIC) {
    zz9k_close(ctx);
    return 3;
  }
  if (zz9k_get_be32(mailbox.descriptor.completion_head) != 2) {
    zz9k_close(ctx);
    return 4;
  }

  zz9k_close(ctx);
  return 0;
}

static int test_sync_call_rejects_same_id_wrong_opcode_completion(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KCaps caps;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  prepare_completion(&mailbox, 1U, ZZ9K_OP_PING, ZZ9K_STATUS_OK, 0);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  idle_test_mailbox = &mailbox;
  zz9k_set_idle_hook_for_test(complete_query_caps_after_stale_reply);
  memset(&caps, 0, sizeof(caps));
  if (zz9k_query_caps(ctx, &caps) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    zz9k_set_idle_hook_for_test(0);
    idle_test_mailbox = 0;
    return 2;
  }
  zz9k_set_idle_hook_for_test(0);
  idle_test_mailbox = 0;

  if (caps.magic != ZZ9K_ABI_MAGIC) {
    zz9k_close(ctx);
    return 3;
  }
  if (zz9k_get_be32(mailbox.descriptor.completion_head) != 2U) {
    zz9k_close(ctx);
    return 4;
  }

  zz9k_close(ctx);
  return 0;
}

static int test_sync_call_rejects_same_id_opcode_wrong_cookie_completion(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KCaps caps;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  prepare_completion(&mailbox, 1U, ZZ9K_OP_QUERY_CAPS, ZZ9K_STATUS_OK, 40);
  zz9k_put_be32(mailbox.completion_ring[0].user_cookie, 0x12345678UL);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  idle_test_mailbox = &mailbox;
  zz9k_set_idle_hook_for_test(complete_query_caps_after_stale_reply);
  memset(&caps, 0, sizeof(caps));
  if (zz9k_query_caps(ctx, &caps) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    zz9k_set_idle_hook_for_test(0);
    idle_test_mailbox = 0;
    return 2;
  }
  zz9k_set_idle_hook_for_test(0);
  idle_test_mailbox = 0;

  if (caps.magic != ZZ9K_ABI_MAGIC) {
    zz9k_close(ctx);
    return 3;
  }
  if (zz9k_get_be32(mailbox.descriptor.completion_head) != 2U) {
    zz9k_close(ctx);
    return 4;
  }

  zz9k_close(ctx);
  return 0;
}

static int test_sync_call_times_out_without_completion(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(&request, 0, sizeof(request));
  memset(&reply, 0, sizeof(reply));
  request.entry.opcode = ZZ9K_OP_PING;

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (zz9k_call(ctx, &request, &reply, 2) != ZZ9K_STATUS_TIMEOUT) return 2;

  zz9k_close(ctx);
  return 0;
}

static int test_sync_call_offload_timeout_uses_wall_clock_budget(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;

  init_mailbox(&mailbox);
  memset(&board, 0, sizeof(board));
  memset(&request, 0, sizeof(request));
  memset(&reply, 0, sizeof(reply));
  request.entry.opcode = ZZ9K_OP_PING;

  idle_test_now_ms = 0U;
  idle_test_calls = 0U;
  zz9k_set_now_hook_for_test(idle_test_now_hook);
  zz9k_set_idle_hook_for_test(idle_advance_10ms_without_completion);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    zz9k_set_now_hook_for_test(0);
    zz9k_set_idle_hook_for_test(0);
    return 1;
  }

  zz9k_set_offload_timeout_ms(ctx, 30U);
  if (zz9k_call(ctx, &request, &reply, 2) != ZZ9K_STATUS_TIMEOUT) {
    zz9k_close(ctx);
    zz9k_set_now_hook_for_test(0);
    zz9k_set_idle_hook_for_test(0);
    return 2;
  }
  if (idle_test_calls <= 2U || idle_test_now_ms < 30U) {
    zz9k_close(ctx);
    zz9k_set_now_hook_for_test(0);
    zz9k_set_idle_hook_for_test(0);
    return 3;
  }
  if (idle_test_calls > 8U) {
    zz9k_close(ctx);
    zz9k_set_now_hook_for_test(0);
    zz9k_set_idle_hook_for_test(0);
    return 4;
  }

  zz9k_close(ctx);
  zz9k_set_now_hook_for_test(0);
  zz9k_set_idle_hook_for_test(0);
  return 0;
}

static int test_env_u32_reads_positive_decimal_or_fallback(void)
{
  clear_test_env_u32();
  if (zz9k_env_u32("ZZ9K_TEST_ENV_U32", 42U) != 42U) return 1;

  set_test_env_u32("1536");
  if (zz9k_env_u32("ZZ9K_TEST_ENV_U32", 42U) != 1536U) return 2;

  set_test_env_u32("0");
  if (zz9k_env_u32("ZZ9K_TEST_ENV_U32", 42U) != 42U) return 3;

  set_test_env_u32("junk");
  if (zz9k_env_u32("ZZ9K_TEST_ENV_U32", 42U) != 42U) return 4;

  set_test_env_u32("1m");
  if (zz9k_env_u32("ZZ9K_TEST_ENV_U32", 42U) != 42U) return 5;

  set_test_env_u32("1536 ");
  if (zz9k_env_u32("ZZ9K_TEST_ENV_U32", 42U) != 42U) return 6;

  clear_test_env_u32();
  return 0;
}

int main(void)
{
  int result;

  result = test_submit_writes_request_without_doorbell();
  if (result) return 10 + result;

  result = test_submit_rings_advertised_doorbell();
  if (result) return 15 + result;

  result = test_submit_only_writes_declared_payload_bytes();
  if (result) return 20 + result;

  result = test_submit_batch_queues_until_request_ring_fills();
  if (result) return 25 + result;

  result = test_poll_reads_completion_and_advances_head();
  if (result) return 30 + result;

  result = test_poll_batch_reads_available_completions();
  if (result) return 35 + result;

  result = test_poll_acks_advertised_irq_completion();
  if (result) return 40 + result;

  result = test_completion_irq_supported_tracks_capability();
  if (result) return 45 + result;

  result = test_shared_heap_board_visible_tracks_zorro_version();
  if (result) return 47 + result;

  result = test_query_caps_roundtrips_through_core_service();
  if (result) return 50 + result;

  result = test_query_service_builds_request_and_maps_reply();
  if (result) return 60 + result;

  result = test_ping_echoes_inline_payload();
  if (result) return 63 + result;

  result = test_status_and_service_helpers_are_stable();
  if (result) return 65 + result;

  result = test_alloc_shared_builds_request_and_maps_reply();
  if (result) return 70 + result;

  result = test_free_shared_builds_request();
  if (result) return 90 + result;

  result = test_mem_fill_builds_request();
  if (result) return 110 + result;

  result = test_mem_copy_builds_request();
  if (result) return 130 + result;

  result = test_read_diag_roundtrips_through_diag_service();
  if (result) return 150 + result;

  result = test_map_framebuffer_surface_parses_zero_copy_surface();
  if (result) return 180 + result;

  result = test_map_framebuffer_surface_allows_unmapped_zero_copy_surface();
  if (result) return 190 + result;

  result = test_alloc_surface_builds_request_and_maps_reply();
  if (result) return 210 + result;

  result = test_alloc_surface_ex_builds_request_with_pitch();
  if (result) return 230 + result;

  result = test_alloc_surface_rejects_unmapped_arm_address();
  if (result) return 250 + result;

  result = test_alloc_surface_maps_legacy_io_window_address();
  if (result) return 260 + result;

  result = test_alloc_surface_rejects_address_outside_board_window();
  if (result) return 270 + result;

  result = test_alloc_surface_allows_arm_local_unmapped_surface();
  if (result) return 280 + result;

  result = test_free_surface_builds_request();
  if (result) return 290 + result;

  result = test_scale_image_builds_request();
  if (result) return 320 + result;

  result = test_fill_surface_builds_request();
  if (result) return 322 + result;

  result = test_query_palette_builds_request();
  if (result) return 375 + result;

  result = test_query_palette_reports_unsupported();
  if (result) return 380 + result;

  result = test_copy_surface_builds_request();
  if (result) return 324 + result;

  result = test_decode_image_builds_request_and_maps_result();
  if (result) return 325 + result;

  result = test_image_session_helpers_roundtrip_results();
  if (result) return 326 + result;

  result = test_crypto_hash_builds_request_and_maps_result();
  if (result) return 330 + result;

  result = test_crypto_hash_batch_maps_results_by_cookie();
  if (result) return 335 + result;

  result = test_crypto_stream_builds_request_and_maps_result();
  if (result) return 337 + result;

  result = test_crypto_stream_batch_maps_results_by_cookie();
  if (result) return 338 + result;

  result = test_crypto_aead_builds_request_and_maps_result();
  if (result) return 339 + result;

  result = test_crypto_aead_batch_maps_results_by_cookie();
  if (result) return 340 + result;

  result = test_decompress_builds_request_and_maps_result();
  if (result) return 342 + result;

  result = test_decompress_stream_helpers_roundtrip_results();
  if (result) return 343 + result;

  result = test_audio_stream_helpers_roundtrip_results();
  if (result) return 344 + result;

  result = test_audio_stream_play_stop_roundtrip_results();
  if (result) return 370 + result;

  result = test_sync_call_yields_bus_before_polling_completion();
  if (result) return 345 + result;

  result = test_sync_call_skips_stale_completion_until_matching_reply();
  if (result) return 350 + result;

  result = test_sync_call_rejects_same_id_wrong_opcode_completion();
  if (result) return 353 + result;

  result = test_sync_call_rejects_same_id_opcode_wrong_cookie_completion();
  if (result) return 354 + result;

  result = test_sync_call_times_out_without_completion();
  if (result) return 355 + result;

  result = test_sync_call_offload_timeout_uses_wall_clock_budget();
  if (result) return 358 + result;

  result = test_env_u32_reads_positive_decimal_or_fallback();
  if (result) return 356 + result;

  result = test_decompress_batch_maps_result();
  if (result) return 365 + result;

  return 0;
}
