/*
 * Native checks for zz9k.library ARM app client wire helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "zz9k/app.h"
#include <stdio.h>
#include <string.h>


static int failures;

static void expect_u32(const char *name, uint32_t actual, uint32_t expected)
{
  if (actual != expected) {
    printf("%s: got %lu expected %lu\n", name, (unsigned long)actual,
           (unsigned long)expected);
    failures++;
  }
}

static void expect_status(const char *name, int actual, int expected)
{
  if (actual != expected) {
    printf("%s: got %d expected %d\n", name, actual, expected);
    failures++;
  }
}

static void test_request_builders(void)
{
  ZZ9KRequest request;

  expect_status("load begin", zz9k_app_request_init(&request,
                                                      ZZ9K_APP_OP_LOAD_BEGIN),
                ZZ9K_STATUS_OK);
  expect_u32("load begin opcode", request.entry.opcode, ZZ9K_APP_OP_LOAD_BEGIN);
  expect_u32("load begin bytes", request.entry.payload_len, 0U);

  expect_status("chunk", zz9k_app_request_chunk(&request, 0x1234U, 4096U),
                ZZ9K_STATUS_OK);
  expect_u32("chunk opcode", request.entry.opcode, ZZ9K_APP_OP_LOAD_CHUNK);
  expect_u32("chunk bytes", request.entry.payload_len, 12U);
  expect_u32("chunk offset", zz9k_get_be32(request.entry.payload.inline_data),
             0x1234U);
  expect_u32("chunk length",
             zz9k_get_be32(request.entry.payload.inline_data + 4U), 4096U);
  expect_u32("chunk staging",
             zz9k_get_be32(request.entry.payload.inline_data + 8U),
             ZZ9K_APP_STAGING_OFFSET);
  expect_status("chunk zero", zz9k_app_request_chunk(&request, 0U, 0U),
                ZZ9K_STATUS_BAD_REQUEST);
  expect_status("chunk too large",
                zz9k_app_request_chunk(&request, 0U, ZZ9K_APP_CHUNK_MAX + 1U),
                ZZ9K_STATUS_BAD_REQUEST);

  expect_status("start", zz9k_app_request_start(&request, 2U),
                ZZ9K_STATUS_OK);
  expect_u32("start opcode", request.entry.opcode, ZZ9K_APP_OP_START);
  expect_u32("start argc", zz9k_get_be32(request.entry.payload.inline_data),
             2U);

  expect_status("stop", zz9k_app_request_stop(&request, 9U), ZZ9K_STATUS_OK);
  expect_u32("stop opcode", request.entry.opcode, ZZ9K_APP_OP_STOP);
  expect_u32("stop bytes", request.entry.payload_len, ZZ9K_APP_STOP_REQ_BYTES);
  expect_u32("stop generation",
             zz9k_get_be32(request.entry.payload.inline_data +
                           ZZ9K_APP_STOP_GENERATION),
             9U);
  expect_status("stop zero generation", zz9k_app_request_stop(&request, 0U),
                ZZ9K_STATUS_BAD_REQUEST);
}

static void test_slot_selectors(void)
{
  ZZ9KRequest first;
  ZZ9KRequest second;
  ZZ9KRequest third;

  expect_status("first selector request",
                zz9k_app_request_init(&first, ZZ9K_APP_OP_STATUS),
                ZZ9K_STATUS_OK);
  expect_status("first selector", zz9k_app_request_select(&first, 0U, 101U),
                ZZ9K_STATUS_OK);
  expect_status("second selector request",
                zz9k_app_request_init(&second, ZZ9K_APP_OP_STATUS),
                ZZ9K_STATUS_OK);
  expect_status("second selector",
                zz9k_app_request_select(&second, 1U, 202U), ZZ9K_STATUS_OK);
  expect_status("third selector request",
                zz9k_app_request_init(&third, ZZ9K_APP_OP_STATUS),
                ZZ9K_STATUS_OK);
  expect_status("third selector", zz9k_app_request_select(&third, 3U, 303U),
                ZZ9K_STATUS_OK);
  expect_u32("first selector bytes", first.entry.payload_len,
             ZZ9K_APP_MAILBOX_PAYLOAD_BYTES);
  expect_u32("second selector bytes", second.entry.payload_len,
             ZZ9K_APP_MAILBOX_PAYLOAD_BYTES);
  expect_u32("third selector bytes", third.entry.payload_len,
             ZZ9K_APP_MAILBOX_PAYLOAD_BYTES);
  expect_u32("first selector slot",
             zz9k_get_be32(first.entry.payload.inline_data + ZZ9K_APP_SEL_SLOT),
             0U);
  expect_u32("second selector slot",
             zz9k_get_be32(second.entry.payload.inline_data + ZZ9K_APP_SEL_SLOT),
             1U);
  expect_u32("third selector slot",
             zz9k_get_be32(third.entry.payload.inline_data + ZZ9K_APP_SEL_SLOT),
             3U);
  expect_u32("first selector generation",
             zz9k_get_be32(first.entry.payload.inline_data +
                            ZZ9K_APP_SEL_GENERATION), 101U);
  expect_u32("second selector generation",
             zz9k_get_be32(second.entry.payload.inline_data +
                            ZZ9K_APP_SEL_GENERATION), 202U);
  expect_u32("third selector generation",
             zz9k_get_be32(third.entry.payload.inline_data +
                            ZZ9K_APP_SEL_GENERATION), 303U);
  expect_u32("stale second generation",
             zz9k_app_generation_matches(202U, 999U), 0U);
  expect_u32("first selector survives stale second",
             zz9k_get_be32(first.entry.payload.inline_data +
                            ZZ9K_APP_SEL_GENERATION), 101U);
  expect_u32("third selector survives stale second",
             zz9k_get_be32(third.entry.payload.inline_data +
                            ZZ9K_APP_SEL_GENERATION), 303U);
}

static void test_slot_registry(void)
{
  ZZ9KAppSlotRegistry registry;
  void *previous;
  uint32_t first;
  uint32_t second;
  uint32_t third;

  memset(&registry, 0, sizeof(registry));
  expect_status("claim first", zz9k_app_slot_claim(&registry, 0U, &first),
                ZZ9K_STATUS_OK);
  expect_status("claim second", zz9k_app_slot_claim(&registry, 1U, &second),
                ZZ9K_STATUS_OK);
  expect_status("claim third", zz9k_app_slot_claim(&registry, 3U, &third),
                ZZ9K_STATUS_OK);
  expect_u32("first active", zz9k_app_slot_get(&registry, 0U) == &first, 1U);
  expect_u32("second active", zz9k_app_slot_get(&registry, 1U) == &second, 1U);
  expect_u32("third active", zz9k_app_slot_get(&registry, 3U) == &third, 1U);
  expect_status("close second",
                zz9k_app_slot_release(&registry, 1U, &second), ZZ9K_STATUS_OK);
  expect_u32("first lease remains", zz9k_app_slot_get(&registry, 0U) == &first,
             1U);
  expect_u32("second inactive", zz9k_app_slot_get(&registry, 1U) == 0, 1U);
  expect_u32("third lease remains", zz9k_app_slot_get(&registry, 3U) == &third,
             1U);

  memset(&registry, 0, sizeof(registry));
  expect_status("legacy first open",
                zz9k_app_slot_claim(&registry, 0U, &first), ZZ9K_STATUS_OK);
  expect_status("legacy second open",
                zz9k_app_slot_claim(&registry, 0U, &second), ZZ9K_STATUS_BUSY);
  expect_status("replace stale first",
                zz9k_app_slot_replace(&registry, 0U, &second, &previous),
                ZZ9K_STATUS_OK);
  expect_u32("replace returns stale handle", previous == &first, 1U);
  expect_u32("replacement becomes active",
             zz9k_app_slot_get(&registry, 0U) == &second, 1U);
}

static void test_status_parser(void)
{
  ZZ9KMailboxEntry reply;
  ZZ9KAppStatusInfo status;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_APP_OP_STATUS;
  reply.payload_len = ZZ9K_APP_STATUS_REPLY_BYTES;
  zz9k_put_be32(reply.payload.inline_data + 0U, ZZ9K_APP_STATE_RUNNING);
  zz9k_put_be32(reply.payload.inline_data + 4U, 7U);
  zz9k_put_be32(reply.payload.inline_data + 8U, ZZ9K_APP_FAULT_NONE);
  zz9k_put_be32(reply.payload.inline_data + 12U, 42U);
  zz9k_put_be32(reply.payload.inline_data + 16U, 3U);
  zz9k_put_be32(reply.payload.inline_data + 20U, 0x12345678U);
  expect_status("status parse", zz9k_app_parse_status(&reply, &status),
                ZZ9K_STATUS_OK);
  expect_u32("status state", status.state, ZZ9K_APP_STATE_RUNNING);
  expect_u32("status generation", status.generation, 7U);
  expect_u32("status exit", (uint32_t)status.exit_code, 42U);
  expect_u32("status pc", status.pc, 0x12345678U);
  reply.payload_len--;
  expect_status("short status", zz9k_app_parse_status(&reply, &status),
                ZZ9K_STATUS_BAD_REQUEST);
}

static void test_message_rings(void)
{
  uint8_t ring[ZZ9K_APP_MESSAGE_RING_BYTES];
  uint8_t sent[ZZ9K_APP_MESSAGE_BYTES];
  uint8_t received[ZZ9K_APP_MESSAGE_BYTES];
  uint32_t head;
  uint32_t tail;
  uint32_t i;
  uint32_t j;

  memset(ring, 0, sizeof(ring));
  for (i = 0U; i < 300U; i++) {
    for (j = 0U; j < sizeof(sent); j++) sent[j] = (uint8_t)(i + j);
    expect_status("ring push", zz9k_app_ring_push(ring, sent), ZZ9K_STATUS_OK);
    expect_status("ring producer indices",
                  zz9k_app_ring_indices(ring, &head, &tail), ZZ9K_STATUS_OK);
    expect_u32("ring producer head", head, i + 1U);
    expect_u32("ring producer tail", tail, i);
    expect_status("ring pop", zz9k_app_ring_pop(ring, received), ZZ9K_STATUS_OK);
    if (memcmp(sent, received, sizeof(sent)) != 0) {
      puts("ring payload mismatch");
      failures++;
    }
  }
  expect_status("ring consumer indices",
                zz9k_app_ring_indices(ring, &head, &tail), ZZ9K_STATUS_OK);
  expect_u32("ring consumer head", head, 300U);
  expect_u32("ring consumer tail", tail, 300U);
  expect_status("ring empty", zz9k_app_ring_pop(ring, received),
                ZZ9K_STATUS_BUSY);
  for (i = 0U; i < ZZ9K_APP_MESSAGE_CAPACITY; i++) {
    expect_status("ring fill", zz9k_app_ring_push(ring, sent),
                  ZZ9K_STATUS_OK);
  }
  expect_status("ring full", zz9k_app_ring_push(ring, sent),
                ZZ9K_STATUS_BUSY);
}

static void test_generation_fence(void)
{
  expect_u32("zero generation", zz9k_app_generation_matches(0U, 0U), 0U);
  expect_u32("matching generation", zz9k_app_generation_matches(9U, 9U), 1U);
  expect_u32("replaced generation", zz9k_app_generation_matches(9U, 10U), 0U);
}

int main(void)
{
  test_request_builders();
  test_status_parser();
  test_message_rings();
  test_slot_selectors();
  test_slot_registry();
  test_generation_fence();
  return failures != 0;
}
