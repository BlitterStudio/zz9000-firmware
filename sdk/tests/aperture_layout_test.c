/*
 * Zorro 2 aperture negotiation and HOST_WINDOW allocation checks.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/host.h"
#include "zz9k/reply.h"
#include "zz9k/request.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_SYNC_COOKIE_MASK 0x5aa55aa5UL
#define TEST_RING_ENTRIES 8U

struct TestMailbox {
  ZZ9KMailboxDescriptor descriptor;
  ZZ9KMailboxWireEntry request_ring[TEST_RING_ENTRIES];
  ZZ9KMailboxWireEntry completion_ring[TEST_RING_ENTRIES];
};

typedef struct LayoutCase {
  uint32_t aperture_size;
  uint32_t profile;
  uint32_t framebuffer_base;
  uint32_t framebuffer_size;
  uint32_t pip_base;
  uint32_t pip_size;
  uint32_t template_base;
  uint32_t template_size;
  uint32_t host_base;
  uint32_t host_size;
  uint32_t audio_base;
  uint32_t audio_size;
} LayoutCase;

static const LayoutCase layout_2m = {
  0x00200000UL,
  ZZ9K_APERTURE_PROFILE(ZZ9K_APERTURE_LAYOUT_GENERATION_1,
                        ZZ9K_APERTURE_FLAG_VALID |
                        ZZ9K_APERTURE_FLAG_ACKED |
                        ZZ9K_APERTURE_FLAG_HOST_WINDOW),
  0x00010000UL, 0x001c0000UL,
  0U, 0U,
  0x001d0000UL, 0x00010000UL,
  0x001e0000UL, 0x00010000UL,
  0x001f0000UL, 0x00010000UL
};

static const LayoutCase layout_4m = {
  0x00400000UL,
  ZZ9K_APERTURE_PROFILE(ZZ9K_APERTURE_LAYOUT_GENERATION_1,
                        ZZ9K_APERTURE_FLAG_VALID |
                        ZZ9K_APERTURE_FLAG_ACKED |
                        ZZ9K_APERTURE_FLAG_HOST_WINDOW |
                        ZZ9K_APERTURE_FLAG_PIP),
  0x00010000UL, 0x00388000UL,
  0x00398000UL, 0x00038000UL,
  0x003d0000UL, 0x00010000UL,
  0x003e0000UL, 0x00010000UL,
  0x003f0000UL, 0x00010000UL
};

static const LayoutCase layout_8m = {
  0x00800000UL,
  ZZ9K_APERTURE_PROFILE(ZZ9K_APERTURE_LAYOUT_GENERATION_1,
                        ZZ9K_APERTURE_FLAG_VALID |
                        ZZ9K_APERTURE_FLAG_ACKED |
                        ZZ9K_APERTURE_FLAG_HOST_WINDOW |
                        ZZ9K_APERTURE_FLAG_PIP),
  0x00010000UL, 0x00770000UL,
  0x00780000UL, 0x00040000UL,
  0x007c0000UL, 0x00010000UL,
  0x007d0000UL, 0x00020000UL,
  0x007f0000UL, 0x00010000UL
};

static void init_mailbox(struct TestMailbox *mailbox, uint32_t caps)
{
  memset(mailbox, 0, sizeof(*mailbox));
  zz9k_put_be32(mailbox->descriptor.magic, ZZ9K_ABI_MAGIC);
  zz9k_put_be16(mailbox->descriptor.abi_major, ZZ9K_ABI_VERSION_MAJOR);
  zz9k_put_be16(mailbox->descriptor.abi_minor, ZZ9K_ABI_VERSION_MINOR);
  zz9k_put_be32(mailbox->descriptor.descriptor_size,
                (uint32_t)sizeof(mailbox->descriptor));
  zz9k_put_be32(mailbox->descriptor.request_ring_offset,
                (uint32_t)offsetof(struct TestMailbox, request_ring));
  zz9k_put_be32(mailbox->descriptor.request_ring_entries, TEST_RING_ENTRIES);
  zz9k_put_be32(mailbox->descriptor.completion_ring_offset,
                (uint32_t)offsetof(struct TestMailbox, completion_ring));
  zz9k_put_be32(mailbox->descriptor.completion_ring_entries,
                TEST_RING_ENTRIES);
  zz9k_put_be32(mailbox->descriptor.capability_bits,
                ZZ9K_CAP_MAILBOX | caps);
}

static void prepare_completion(struct TestMailbox *mailbox, uint32_t index,
                               uint32_t request_id, uint16_t opcode,
                               uint16_t payload_len)
{
  ZZ9KMailboxWireEntry *reply = &mailbox->completion_ring[index];

  zz9k_put_be32(reply->request_id, request_id);
  zz9k_put_be16(reply->opcode, opcode);
  zz9k_put_be16(reply->status, ZZ9K_STATUS_OK);
  zz9k_put_be16(reply->payload_len, payload_len);
  zz9k_put_be32(reply->user_cookie,
                request_id ^ TEST_SYNC_COOKIE_MASK);
  zz9k_put_be32(mailbox->descriptor.completion_tail, index + 1U);
}

static void put_layout(ZZ9KMailboxWireEntry *reply, const LayoutCase *layout)
{
  uint8_t *payload = reply->payload;

  zz9k_put_be32(&payload[0], layout->profile);
  zz9k_put_be32(&payload[4], layout->aperture_size);
  zz9k_put_be32(&payload[8], layout->framebuffer_base);
  zz9k_put_be32(&payload[12], layout->framebuffer_size);
  zz9k_put_be32(&payload[16], layout->pip_base);
  zz9k_put_be32(&payload[20], layout->pip_size);
  zz9k_put_be32(&payload[24], layout->template_base);
  zz9k_put_be32(&payload[28], layout->template_size);
  zz9k_put_be32(&payload[32], layout->host_base);
  zz9k_put_be32(&payload[36], layout->host_size);
  zz9k_put_be32(&payload[40], layout->audio_base);
  zz9k_put_be32(&payload[44], layout->audio_size);
}

static void put_alloc(ZZ9KMailboxWireEntry *reply, uint32_t handle,
                      uint32_t arm_addr, uint32_t length, uint32_t flags)
{
  zz9k_put_be32(&reply->payload[0], handle);
  zz9k_put_be32(&reply->payload[4], arm_addr);
  zz9k_put_be32(&reply->payload[8], length);
  zz9k_put_be32(&reply->payload[12], flags);
}

static void put_caps(ZZ9KMailboxWireEntry *reply, uint32_t caps)
{
  zz9k_put_be32(&reply->payload[0], ZZ9K_ABI_MAGIC);
  zz9k_put_be16(&reply->payload[4], ZZ9K_ABI_VERSION_MAJOR);
  zz9k_put_be16(&reply->payload[6], ZZ9K_ABI_VERSION_MINOR);
  zz9k_put_be32(&reply->payload[8], ZZ9K_CAP_MAILBOX | caps);
  zz9k_put_be32(&reply->payload[12], 48U);
  zz9k_put_be32(&reply->payload[16], 32U);
  zz9k_put_be32(&reply->payload[28], TEST_RING_ENTRIES);
  zz9k_put_be32(&reply->payload[32], TEST_RING_ENTRIES);
}

static void prepare_layout_attach(struct TestMailbox *mailbox,
                                  const LayoutCase *layout,
                                  uint32_t refreshed_caps)
{
  prepare_completion(mailbox, 0U, 1U, ZZ9K_OP_QUERY_APERTURE_LAYOUT,
                     sizeof(ZZ9KApertureLayoutPayload));
  put_layout(&mailbox->completion_ring[0], layout);
  prepare_completion(mailbox, 1U, 2U, ZZ9K_OP_QUERY_CAPS,
                     sizeof(ZZ9KQueryCapsPayload));
  put_caps(&mailbox->completion_ring[1], refreshed_caps);
}

static void init_z2_board(ZZ9KBoard *board, uint32_t board_size)
{
  memset(board, 0, sizeof(*board));
  board->board_addr = 0x10000000UL;
  board->board_size = board_size;
  board->product = ZZ9K_PRODUCT_Z2;
  board->zorro_version = 2U;
}

static uint32_t host_arm_base(const LayoutCase *layout)
{
  return ZZ9K_ARM_MEMORY_START +
         (layout->host_base - ZZ9K_AMIGA_MEMORY_OFFSET);
}

static int expect_layout(const ZZ9KApertureLayout *actual,
                         const LayoutCase *expected)
{
  return actual->profile == expected->profile &&
         actual->aperture_size == expected->aperture_size &&
         actual->framebuffer_base == expected->framebuffer_base &&
         actual->framebuffer_size == expected->framebuffer_size &&
         actual->pip_base == expected->pip_base &&
         actual->pip_size == expected->pip_size &&
         actual->template_base == expected->template_base &&
         actual->template_size == expected->template_size &&
         actual->host_base == expected->host_base &&
         actual->host_size == expected->host_size &&
         actual->audio_base == expected->audio_base &&
         actual->audio_size == expected->audio_size;
}

static int test_request_and_reply_helpers(void)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  ZZ9KApertureLayout layout;
  ZZ9KDiagMemoryInfo diag;
  uint32_t i;

  if (zz9k_request_query_aperture_layout(&request) != ZZ9K_STATUS_OK ||
      request.entry.opcode != ZZ9K_OP_QUERY_APERTURE_LAYOUT ||
      request.entry.payload_len != 0U) return 1;
  if (zz9k_request_diag_memory(&request) != ZZ9K_STATUS_OK ||
      request.entry.opcode != ZZ9K_OP_DIAG_MEMORY ||
      request.entry.payload_len != 0U) return 2;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_QUERY_APERTURE_LAYOUT;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = sizeof(ZZ9KApertureLayoutPayload);
  for (i = 0U; i < 12U; i++) {
    zz9k_put_be32(&reply.payload.inline_data[i * 4U], i + 1U);
  }
  if (zz9k_reply_aperture_layout(&reply, &layout) != ZZ9K_STATUS_OK ||
      layout.profile != 1U || layout.audio_size != 12U) return 3;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_DIAG_MEMORY;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = sizeof(ZZ9KDiagMemoryPayload);
  for (i = 0U; i < 12U; i++) {
    zz9k_put_be32(&reply.payload.inline_data[i * 4U], 100U + i);
  }
  if (zz9k_reply_diag_memory(&reply, &diag) != ZZ9K_STATUS_OK ||
      diag.version != 100U || diag.reserved != 111U) return 4;
  return 0;
}

static int test_canonical_layout(const LayoutCase *expected)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KApertureLayout layout;

  init_mailbox(&mailbox, ZZ9K_CAP_APERTURE_LAYOUT |
                         ZZ9K_CAP_HOST_WINDOW_HEAP);
  init_z2_board(&board, expected->aperture_size);
  prepare_layout_attach(&mailbox, expected, ZZ9K_CAP_APERTURE_LAYOUT |
                                            ZZ9K_CAP_HOST_WINDOW_HEAP);
  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) return 1;
  if (zz9k_query_aperture_layout(ctx, &layout) != ZZ9K_STATUS_OK) return 2;
  if (!expect_layout(&layout, expected)) return 3;
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
          ZZ9K_OP_QUERY_APERTURE_LAYOUT ||
      zz9k_get_be16(mailbox.request_ring[1].opcode) !=
          ZZ9K_OP_QUERY_CAPS) return 4;
  zz9k_close(ctx);
  return 0;
}

static int query_rejected(const LayoutCase *wire_layout,
                          uint32_t board_size)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KApertureLayout layout;
  int status;

  init_mailbox(&mailbox, ZZ9K_CAP_APERTURE_LAYOUT |
                         ZZ9K_CAP_HOST_WINDOW_HEAP);
  init_z2_board(&board, board_size);
  prepare_completion(&mailbox, 0U, 1U, ZZ9K_OP_QUERY_APERTURE_LAYOUT,
                     sizeof(ZZ9KApertureLayoutPayload));
  put_layout(&mailbox.completion_ring[0], wire_layout);
  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) return 1;
  status = zz9k_query_aperture_layout(ctx, &layout);
  zz9k_close(ctx);
  return status != ZZ9K_STATUS_OK;
}

static int test_invalid_layouts_fail_closed(void)
{
  LayoutCase bad;
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;

  if (!query_rejected(&layout_4m, layout_2m.aperture_size)) return 1;

  /* Generation 2 is the live direct-ring carve; the first rejected
   * generation is now 3 (unknown future layout). */
  bad = layout_4m;
  bad.profile = ZZ9K_APERTURE_PROFILE(
      ZZ9K_APERTURE_LAYOUT_GENERATION_2 + 1U,
      ZZ9K_APERTURE_FLAG_VALID | ZZ9K_APERTURE_FLAG_ACKED |
      ZZ9K_APERTURE_FLAG_HOST_WINDOW | ZZ9K_APERTURE_FLAG_PIP);
  if (!query_rejected(&bad, bad.aperture_size)) return 2;

  /* A generation-2 profile on generation-1 heap geometry (the carve
   * is missing) is still a mismatch. */
  bad = layout_4m;
  bad.profile = ZZ9K_APERTURE_PROFILE(
      ZZ9K_APERTURE_LAYOUT_GENERATION_2,
      ZZ9K_APERTURE_FLAG_VALID | ZZ9K_APERTURE_FLAG_ACKED |
      ZZ9K_APERTURE_FLAG_HOST_WINDOW | ZZ9K_APERTURE_FLAG_PIP);
  if (!query_rejected(&bad, bad.aperture_size)) return 11;

  bad = layout_4m;
  bad.profile |= 1U << 4;
  if (!query_rejected(&bad, bad.aperture_size)) return 3;

  bad = layout_4m;
  bad.framebuffer_base = 0xfffffff0UL;
  bad.framebuffer_size = 0x1000U;
  if (!query_rejected(&bad, bad.aperture_size)) return 4;

  bad = layout_4m;
  bad.host_base--;
  if (!query_rejected(&bad, bad.aperture_size)) return 5;

  init_mailbox(&mailbox, ZZ9K_CAP_APERTURE_LAYOUT);
  init_z2_board(&board, 0U);
  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) ==
      ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 6;
  }
  init_mailbox(&mailbox, ZZ9K_CAP_APERTURE_LAYOUT);
  init_z2_board(&board, 0x00300000UL);
  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) ==
      ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 7;
  }
  return 0;
}

static int test_validated_2m_host_allocation(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KApertureLayout layout;
  ZZ9KSharedBuffer buffer;
  uint32_t arm_base = host_arm_base(&layout_2m);

  /* The descriptor snapshot predates the driver's ACK. The mandatory
   * QUERY_CAPS refresh below must observe HOST_WINDOW_HEAP before alloc. */
  init_mailbox(&mailbox, ZZ9K_CAP_APERTURE_LAYOUT);
  init_z2_board(&board, layout_2m.aperture_size);
  prepare_layout_attach(&mailbox, &layout_2m,
                        ZZ9K_CAP_APERTURE_LAYOUT |
                        ZZ9K_CAP_HOST_WINDOW_HEAP);
  prepare_completion(&mailbox, 2U, 3U, ZZ9K_OP_ALLOC_SHARED,
                     sizeof(ZZ9KSharedBufferInfoPayload));
  put_alloc(&mailbox.completion_ring[2], 7U, arm_base,
            layout_2m.host_size, ZZ9K_ALLOC_HOST_WINDOW);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) return 1;
  if (zz9k_query_aperture_layout(ctx, &layout) != ZZ9K_STATUS_OK) return 2;
  if (zz9k_alloc_shared(ctx, layout_2m.host_size, 64U,
                        ZZ9K_ALLOC_HOST_WINDOW, &buffer) != ZZ9K_STATUS_OK) {
    return 3;
  }
  if (buffer.handle != 7U || buffer.length != layout_2m.host_size) return 4;
  if ((uintptr_t)buffer.data !=
      (uintptr_t)board.board_addr + layout_2m.host_base) return 5;
  if (zz9k_get_be32(&mailbox.request_ring[2].payload[8]) !=
      ZZ9K_ALLOC_HOST_WINDOW) return 6;
  zz9k_close(ctx);
  return 0;
}

static int test_host_allocation_failure(uint32_t arm_addr, uint32_t length,
                                        uint32_t reply_flags)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KApertureLayout layout;
  ZZ9KSharedBuffer buffer;
  int status;

  init_mailbox(&mailbox, ZZ9K_CAP_APERTURE_LAYOUT |
                         ZZ9K_CAP_HOST_WINDOW_HEAP);
  init_z2_board(&board, layout_2m.aperture_size);
  prepare_layout_attach(&mailbox, &layout_2m,
                        ZZ9K_CAP_APERTURE_LAYOUT |
                        ZZ9K_CAP_HOST_WINDOW_HEAP);
  prepare_completion(&mailbox, 2U, 3U, ZZ9K_OP_ALLOC_SHARED,
                     sizeof(ZZ9KSharedBufferInfoPayload));
  put_alloc(&mailbox.completion_ring[2], 9U, arm_addr, length, reply_flags);
  prepare_completion(&mailbox, 3U, 4U, ZZ9K_OP_FREE_SHARED, 0U);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) return 1;
  if (zz9k_query_aperture_layout(ctx, &layout) != ZZ9K_STATUS_OK) return 2;
  status = zz9k_alloc_shared(ctx, 4096U, 64U, ZZ9K_ALLOC_HOST_WINDOW,
                             &buffer);
  if (status != ZZ9K_STATUS_INTERNAL_ERROR) return 3;
  if (buffer.handle != 0U || buffer.data != 0 || buffer.length != 0U) return 4;
  if (zz9k_get_be16(mailbox.request_ring[3].opcode) != ZZ9K_OP_FREE_SHARED ||
      zz9k_get_be32(&mailbox.request_ring[3].payload[0]) != 9U) return 5;
  zz9k_close(ctx);
  return 0;
}

static int test_host_allocation_boundaries_and_cleanup(void)
{
  uint32_t arm_base = host_arm_base(&layout_2m);
  int status;

  status = test_host_allocation_failure(arm_base, 4096U, 0U);
  if (status) return 10 + status;
  status = test_host_allocation_failure(arm_base - 1U, 4096U,
                                        ZZ9K_ALLOC_HOST_WINDOW);
  if (status) return 20 + status;
  status = test_host_allocation_failure(
      arm_base + layout_2m.host_size - 4095U, 4096U,
      ZZ9K_ALLOC_HOST_WINDOW);
  if (status) return 30 + status;
  status = test_host_allocation_failure(0xfffffff0UL, 4096U,
                                        ZZ9K_ALLOC_HOST_WINDOW);
  if (status) return 40 + status;
  status = test_host_allocation_failure(arm_base, 0U,
                                        ZZ9K_ALLOC_HOST_WINDOW);
  if (status) return 50 + status;
  status = test_host_allocation_failure(arm_base, 2048U,
                                        ZZ9K_ALLOC_HOST_WINDOW);
  if (status) return 60 + status;
  return 0;
}

static int test_late_ack_host_allocation_recovery(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KSharedBuffer buffer;
  LayoutCase unacked = layout_2m;
  uint32_t arm_base = host_arm_base(&layout_2m);

  unacked.profile &= ~ZZ9K_APERTURE_FLAG_ACKED;
  init_mailbox(&mailbox, ZZ9K_CAP_APERTURE_LAYOUT);
  init_z2_board(&board, layout_2m.aperture_size);
  prepare_layout_attach(&mailbox, &unacked, ZZ9K_CAP_APERTURE_LAYOUT);
  prepare_completion(&mailbox, 2U, 3U, ZZ9K_OP_QUERY_CAPS,
                     sizeof(ZZ9KQueryCapsPayload));
  put_caps(&mailbox.completion_ring[2],
           ZZ9K_CAP_APERTURE_LAYOUT | ZZ9K_CAP_HOST_WINDOW_HEAP);
  prepare_completion(&mailbox, 3U, 4U, ZZ9K_OP_QUERY_APERTURE_LAYOUT,
                     sizeof(ZZ9KApertureLayoutPayload));
  put_layout(&mailbox.completion_ring[3], &layout_2m);
  prepare_completion(&mailbox, 4U, 5U, ZZ9K_OP_ALLOC_SHARED,
                     sizeof(ZZ9KSharedBufferInfoPayload));
  put_alloc(&mailbox.completion_ring[4], 13U, arm_base, 4096U,
            ZZ9K_ALLOC_HOST_WINDOW);

  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) return 1;
  if (zz9k_alloc_shared(ctx, 4096U, 64U, ZZ9K_ALLOC_HOST_WINDOW,
                        &buffer) != ZZ9K_STATUS_OK) return 2;
  if (buffer.handle != 13U ||
      (uintptr_t)buffer.data != (uintptr_t)board.board_addr +
                                layout_2m.host_base) return 3;
  if (zz9k_get_be16(mailbox.request_ring[2].opcode) != ZZ9K_OP_QUERY_CAPS ||
      zz9k_get_be16(mailbox.request_ring[3].opcode) !=
          ZZ9K_OP_QUERY_APERTURE_LAYOUT ||
      zz9k_get_be16(mailbox.request_ring[4].opcode) !=
          ZZ9K_OP_ALLOC_SHARED) return 4;
  zz9k_close(ctx);
  return 0;
}

static int test_legacy_and_card_only_paths(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KSharedBuffer buffer;
  uint32_t legacy_arm = ZZ9K_ARM_MEMORY_START +
                        (0x003e0000UL - ZZ9K_AMIGA_MEMORY_OFFSET);

  init_mailbox(&mailbox, ZZ9K_CAP_HOST_WINDOW_HEAP);
  init_z2_board(&board, layout_4m.aperture_size);
  prepare_completion(&mailbox, 0U, 1U, ZZ9K_OP_ALLOC_SHARED,
                     sizeof(ZZ9KSharedBufferInfoPayload));
  put_alloc(&mailbox.completion_ring[0], 11U, legacy_arm, 4096U,
            ZZ9K_ALLOC_HOST_WINDOW);
  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) return 1;
  if (zz9k_alloc_shared(ctx, 4096U, 64U, ZZ9K_ALLOC_HOST_WINDOW,
                        &buffer) != ZZ9K_STATUS_OK) return 2;
  zz9k_close(ctx);

  init_mailbox(&mailbox, ZZ9K_CAP_HOST_WINDOW_HEAP);
  init_z2_board(&board, layout_2m.aperture_size);
  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) return 3;
  if (zz9k_alloc_shared(ctx, 4096U, 64U, ZZ9K_ALLOC_HOST_WINDOW,
                        &buffer) != ZZ9K_STATUS_UNSUPPORTED) return 4;
  if (zz9k_get_be32(mailbox.descriptor.request_tail) != 0U) return 5;
  zz9k_close(ctx);

  init_mailbox(&mailbox, ZZ9K_CAP_APERTURE_LAYOUT |
                         ZZ9K_CAP_HOST_WINDOW_HEAP);
  init_z2_board(&board, layout_2m.aperture_size);
  {
    LayoutCase unacked = layout_2m;
    unacked.profile &= ~ZZ9K_APERTURE_FLAG_ACKED;
    prepare_layout_attach(&mailbox, &unacked,
                          ZZ9K_CAP_APERTURE_LAYOUT |
                          ZZ9K_CAP_HOST_WINDOW_HEAP);
  }
  prepare_completion(&mailbox, 2U, 3U, ZZ9K_OP_QUERY_CAPS,
                     sizeof(ZZ9KQueryCapsPayload));
  put_caps(&mailbox.completion_ring[2], ZZ9K_CAP_APERTURE_LAYOUT);
  prepare_completion(&mailbox, 3U, 4U, ZZ9K_OP_ALLOC_SHARED,
                     sizeof(ZZ9KSharedBufferInfoPayload));
  put_alloc(&mailbox.completion_ring[3], 12U, 0xfffffff0UL, 4096U,
            ZZ9K_ALLOC_CARD_ONLY);
  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) return 6;
  if (zz9k_alloc_shared(ctx, 4096U, 64U, ZZ9K_ALLOC_HOST_WINDOW,
                        &buffer) != ZZ9K_STATUS_UNSUPPORTED) return 7;
  if (zz9k_get_be32(mailbox.descriptor.request_tail) != 3U) return 8;
  if (zz9k_alloc_shared(ctx, 4096U, 64U,
                        ZZ9K_ALLOC_HOST_WINDOW | ZZ9K_ALLOC_CARD_ONLY,
                        &buffer) != ZZ9K_STATUS_OK) return 9;
  if (buffer.handle != 12U || buffer.data != 0) return 10;
  if (zz9k_get_be32(&mailbox.request_ring[3].payload[8]) !=
      ZZ9K_ALLOC_CARD_ONLY) return 11;
  zz9k_close(ctx);
  return 0;
}

static int test_diag_memory_roundtrip(void)
{
  struct TestMailbox mailbox;
  ZZ9KContext *ctx;
  ZZ9KBoard board;
  ZZ9KDiagMemoryInfo diag;
  uint32_t i;

  init_mailbox(&mailbox, ZZ9K_CAP_DIAGNOSTICS);
  memset(&board, 0, sizeof(board));
  prepare_completion(&mailbox, 0U, 1U, ZZ9K_OP_DIAG_MEMORY,
                     sizeof(ZZ9KDiagMemoryPayload));
  for (i = 0U; i < 12U; i++) {
    zz9k_put_be32(&mailbox.completion_ring[0].payload[i * 4U], 10U + i);
  }
  if (zz9k_attach_mailbox(&ctx, &board, &mailbox.descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) return 1;
  if (zz9k_read_diag_memory(ctx, &diag) != ZZ9K_STATUS_OK) return 2;
  if (diag.version != 10U || diag.aperture_size != 12U ||
      diag.host_total != 16U || diag.allocator_invalid_slots != 20U ||
      diag.reserved != 21U) return 3;
  zz9k_close(ctx);
  return 0;
}

int main(void)
{
  int status;

  status = test_request_and_reply_helpers();
  if (status) return 10 + status;
  status = test_canonical_layout(&layout_2m);
  if (status) return 20 + status;
  status = test_canonical_layout(&layout_4m);
  if (status) return 30 + status;
  status = test_canonical_layout(&layout_8m);
  if (status) return 40 + status;
  status = test_invalid_layouts_fail_closed();
  if (status) return 50 + status;
  status = test_validated_2m_host_allocation();
  if (status) return 70 + status;
  status = test_host_allocation_boundaries_and_cleanup();
  if (status) return 90 + status;
  status = test_late_ack_host_allocation_recovery();
  if (status) return 130 + status;
  status = test_legacy_and_card_only_paths();
  if (status) return 170 + status;
  status = test_diag_memory_roundtrip();
  if (status) return 190 + status;

  printf("aperture_layout_test ok\n");
  return 0;
}
