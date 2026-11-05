/*
 * Shared wire helpers for the zz9k.library ARM app client.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "zz9k/app.h"
#include "zz9k/request.h"
#include <string.h>

static void zz9k_app_barrier(void)
{
#if defined(__GNUC__)
  __asm__ volatile("" : : : "memory");
#endif
}

int zz9k_app_generation_matches(uint32_t owned_generation,
                                uint32_t observed_generation)
{
  return owned_generation != 0U && owned_generation == observed_generation;
}

int zz9k_app_request_init(ZZ9KRequest *request, uint16_t opcode)
{
  if (!request || opcode < ZZ9K_APP_OP_QUERY || opcode > ZZ9K_APP_OP_STATUS) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  zz9k_request_init(request, opcode);
  return ZZ9K_STATUS_OK;
}

int zz9k_app_request_chunk(ZZ9KRequest *request, uint32_t file_offset,
                           uint32_t length)
{
  int result;

  if (length == 0U || length > ZZ9K_APP_CHUNK_MAX ||
      file_offset > ZZ9K_APP_IMAGE_MAX_BYTES - length) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  result = zz9k_app_request_init(request, ZZ9K_APP_OP_LOAD_CHUNK);
  if (result != ZZ9K_STATUS_OK) {
    return result;
  }
  request->entry.payload_len = 12U;
  zz9k_put_be32(request->entry.payload.inline_data, file_offset);
  zz9k_put_be32(request->entry.payload.inline_data + 4U, length);
  zz9k_put_be32(request->entry.payload.inline_data + 8U,
                ZZ9K_APP_STAGING_OFFSET);
  return ZZ9K_STATUS_OK;
}

int zz9k_app_request_start(ZZ9KRequest *request, uint32_t argc)
{
  int result;

  if (argc > ZZ9K_APP_ARGV_MAX) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  result = zz9k_app_request_init(request, ZZ9K_APP_OP_START);
  if (result != ZZ9K_STATUS_OK) {
    return result;
  }
  request->entry.payload_len = 4U;
  zz9k_put_be32(request->entry.payload.inline_data, argc);
  return ZZ9K_STATUS_OK;
}

int zz9k_app_parse_status(const ZZ9KMailboxEntry *reply,
                          ZZ9KAppStatusInfo *status)
{
  const uint8_t *payload;

  if (!reply || !status || reply->opcode != ZZ9K_APP_OP_STATUS ||
      reply->payload_len < ZZ9K_APP_STATUS_REPLY_BYTES) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  payload = reply->payload.inline_data;
  status->state = zz9k_get_be32(payload);
  status->generation = zz9k_get_be32(payload + 4U);
  status->fault_code = zz9k_get_be32(payload + 8U);
  status->exit_code = (int32_t)zz9k_get_be32(payload + 12U);
  status->flags = zz9k_get_be32(payload + 16U);
  status->pc = zz9k_get_be32(payload + 20U);
  status->sp = zz9k_get_be32(payload + 24U);
  status->lr = zz9k_get_be32(payload + 28U);
  status->far = zz9k_get_be32(payload + 32U);
  status->fsr = zz9k_get_be32(payload + 36U);
  status->cpsr = zz9k_get_be32(payload + 40U);
  status->flip_generation = zz9k_get_be32(payload + 44U);
  return ZZ9K_STATUS_OK;
}

static int zz9k_app_ring_aligned(const volatile uint8_t *ring)
{
  return ((uintptr_t)ring & (sizeof(uint32_t) - 1U)) == 0U;
}

static uint32_t zz9k_app_ring_load_be(const volatile uint8_t *address)
{
#if defined(__m68k__)
  return *(const volatile uint32_t *)(const void *)address;
#else
  return zz9k_get_be32((const void *)address);
#endif
}

static void zz9k_app_ring_store_be(volatile uint8_t *address, uint32_t value)
{
#if defined(__m68k__)
  *(volatile uint32_t *)(void *)address = value;
#else
  zz9k_put_be32((void *)address, value);
#endif
}

int zz9k_app_ring_indices(const volatile uint8_t *ring, uint32_t *head,
                          uint32_t *tail)
{
  if (!ring || !head || !tail || !zz9k_app_ring_aligned(ring)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  *head = zz9k_app_ring_load_be(ring);
  zz9k_app_barrier();
  *tail = zz9k_app_ring_load_be(ring + 4U);
  return ZZ9K_STATUS_OK;
}

int zz9k_app_ring_push(volatile uint8_t *ring, const uint8_t message[32])
{
  uint32_t head;
  uint32_t tail;
  uint32_t offset;
  uint32_t i;

  if (!ring || !message || !zz9k_app_ring_aligned(ring)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (zz9k_app_ring_indices(ring, &head, &tail) != ZZ9K_STATUS_OK) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (head - tail >= ZZ9K_APP_MESSAGE_CAPACITY) {
    return ZZ9K_STATUS_BUSY;
  }
  offset = ZZ9K_APP_MESSAGE_RING_HEADER_BYTES +
           (head % ZZ9K_APP_MESSAGE_CAPACITY) * ZZ9K_APP_MESSAGE_BYTES;
  for (i = 0U; i < ZZ9K_APP_MESSAGE_BYTES; i++) {
    ring[offset + i] = message[i];
  }
  zz9k_app_barrier();
  zz9k_app_ring_store_be(ring, head + 1U);
  return ZZ9K_STATUS_OK;
}

int zz9k_app_ring_pop(volatile uint8_t *ring, uint8_t message[32])
{
  uint32_t head;
  uint32_t tail;
  uint32_t offset;
  uint32_t i;

  if (!ring || !message || !zz9k_app_ring_aligned(ring)) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (zz9k_app_ring_indices(ring, &head, &tail) != ZZ9K_STATUS_OK) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }
  if (head == tail) {
    return ZZ9K_STATUS_BUSY;
  }
  offset = ZZ9K_APP_MESSAGE_RING_HEADER_BYTES +
           (tail % ZZ9K_APP_MESSAGE_CAPACITY) * ZZ9K_APP_MESSAGE_BYTES;
  for (i = 0U; i < ZZ9K_APP_MESSAGE_BYTES; i++) {
    message[i] = ring[offset + i];
  }
  zz9k_app_barrier();
  zz9k_app_ring_store_be(ring + 4U, tail + 1U);
  return ZZ9K_STATUS_OK;
}
