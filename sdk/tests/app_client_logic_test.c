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
  uint32_t i;

  memset(ring, 0, sizeof(ring));
  for (i = 0U; i < sizeof(sent); i++) sent[i] = (uint8_t)i;
  expect_status("ring push", zz9k_app_ring_push(ring, sent), ZZ9K_STATUS_OK);
  expect_status("ring pop", zz9k_app_ring_pop(ring, received), ZZ9K_STATUS_OK);
  if (memcmp(sent, received, sizeof(sent)) != 0) {
    puts("ring payload mismatch");
    failures++;
  }
  expect_status("ring empty", zz9k_app_ring_pop(ring, received),
                ZZ9K_STATUS_BUSY);
  for (i = 0U; i < ZZ9K_APP_MESSAGE_CAPACITY; i++) {
    expect_status("ring fill", zz9k_app_ring_push(ring, sent),
                  ZZ9K_STATUS_OK);
  }
  expect_status("ring full", zz9k_app_ring_push(ring, sent),
                ZZ9K_STATUS_BUSY);
}

static void test_owner_state(void)
{
  ZZ9KAppOwner owner;
  uint32_t token_a = 1U;
  uint32_t token_b = 2U;

  zz9k_app_owner_init(&owner);
  expect_status("claim a", zz9k_app_owner_claim(&owner, &token_a),
                ZZ9K_STATUS_OK);
  expect_status("claim a twice", zz9k_app_owner_claim(&owner, &token_a),
                ZZ9K_STATUS_OK);
  expect_status("claim b busy", zz9k_app_owner_claim(&owner, &token_b),
                ZZ9K_STATUS_BUSY);
  expect_status("release b", zz9k_app_owner_release(&owner, &token_b),
                ZZ9K_STATUS_BAD_HANDLE);
  expect_status("release a", zz9k_app_owner_release(&owner, &token_a),
                ZZ9K_STATUS_OK);
  expect_status("claim b", zz9k_app_owner_claim(&owner, &token_b),
                ZZ9K_STATUS_OK);
}

int main(void)
{
  test_request_builders();
  test_status_parser();
  test_message_rings();
  test_owner_state();
  return failures != 0;
}
