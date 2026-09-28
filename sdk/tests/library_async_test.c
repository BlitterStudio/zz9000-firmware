/*
 * zz9k.library facade behavior checks.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/library.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TEST_SYNC_COOKIE_MASK 0x5aa55aa5UL

struct TestMailbox {
  ZZ9KMailboxDescriptor descriptor;
  ZZ9KMailboxWireEntry request_ring[4];
  ZZ9KMailboxWireEntry completion_ring[4];
};

struct CallbackState {
  int called;
  int status;
  uint32_t request_id;
};

struct EventWaitState {
  struct TestMailbox *mailbox;
  uint32_t request_id;
  uint32_t wait_calls;
  uint32_t fail_after_calls;
};

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

static void prepare_completion(struct TestMailbox *mailbox, uint32_t request_id,
                               uint16_t opcode, uint16_t status,
                               uint16_t payload_len)
{
  zz9k_put_be32(mailbox->completion_ring[0].request_id, request_id);
  zz9k_put_be16(mailbox->completion_ring[0].opcode, opcode);
  zz9k_put_be16(mailbox->completion_ring[0].status, status);
  zz9k_put_be16(mailbox->completion_ring[0].payload_len, payload_len);
  zz9k_put_be32(mailbox->completion_ring[0].user_cookie,
                request_id ^ TEST_SYNC_COOKIE_MASK);
  zz9k_put_be32(mailbox->descriptor.completion_tail, 1);
}

static void prepare_completion_at(struct TestMailbox *mailbox,
                                  uint32_t index, uint32_t request_id,
                                  uint16_t opcode, uint16_t status,
                                  uint16_t payload_len)
{
  zz9k_put_be32(mailbox->completion_ring[index].request_id, request_id);
  zz9k_put_be16(mailbox->completion_ring[index].opcode, opcode);
  zz9k_put_be16(mailbox->completion_ring[index].status, status);
  zz9k_put_be16(mailbox->completion_ring[index].payload_len, payload_len);
  zz9k_put_be32(mailbox->completion_ring[index].user_cookie,
                request_id ^ TEST_SYNC_COOKIE_MASK);
  zz9k_put_be32(mailbox->descriptor.completion_tail, index + 1U);
}

static void async_callback(ZZ9KAsyncRequest *async, void *user_data)
{
  struct CallbackState *state = (struct CallbackState *)user_data;

  state->called++;
  state->status = async->status;
  state->request_id = async->request_id;
}

static int prepare_completion_on_event(ZZ9KLibrary *library,
                                       void *user_data)
{
  struct EventWaitState *state;

  (void)library;
  state = (struct EventWaitState *)user_data;
  state->wait_calls++;
  if (state->fail_after_calls &&
      state->wait_calls >= state->fail_after_calls) {
    return ZZ9K_STATUS_TIMEOUT;
  }

  prepare_completion(state->mailbox, state->request_id, ZZ9K_OP_PING,
                     ZZ9K_STATUS_OK, 4);
  memcpy((void *)state->mailbox->completion_ring[0].payload, "wake", 4);
  return ZZ9K_STATUS_OK;
}

static uint32_t count_pending(const ZZ9KLibrary *library)
{
  const ZZ9KAsyncRequest *async;
  uint32_t count;

  count = 0;
  async = library->pending_head;
  while (async) {
    count++;
    async = async->next;
  }

  return count;
}

static int attach_library(ZZ9KLibrary *library, struct TestMailbox *mailbox)
{
  ZZ9KContext *ctx;
  ZZ9KBoard board;

  memset(&board, 0, sizeof(board));
  if (zz9k_attach_mailbox(&ctx, &board, &mailbox->descriptor, 0, 0) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (ZZ9KAttachContext(library, ctx, 1) != ZZ9K_STATUS_OK) {
    zz9k_close(ctx);
    return 2;
  }

  return 0;
}

static int test_async_batch_queues_until_request_ring_fills(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KAsyncRequest asyncs[4];
  ZZ9KRequest requests[4];
  uint32_t queued = 99;
  uint32_t i;
  int status;

  init_mailbox(&mailbox);
  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  memset(asyncs, 0, sizeof(asyncs));
  memset(requests, 0, sizeof(requests));
  for (i = 0; i < 4U; i++) {
    requests[i].entry.opcode = ZZ9K_OP_PING;
    requests[i].entry.flags = ZZ9K_ENTRY_INLINE_PAYLOAD;
    requests[i].entry.payload_len = 1;
    requests[i].entry.payload.inline_data[0] = (uint8_t)('a' + i);
  }

  status = ZZ9KCallAsyncBatch(&library, asyncs, requests, 4, 0, 0,
                              &queued);
  if (status != ZZ9K_STATUS_QUEUED) return 2;
  if (queued != 3U) return 3;
  if (count_pending(&library) != 3U) return 4;
  if (zz9k_get_be32(mailbox.descriptor.request_tail) != 3U) return 5;
  if (!asyncs[0].queued || !asyncs[1].queued || !asyncs[2].queued) {
    return 6;
  }
  if (asyncs[3].queued || asyncs[3].request_id != 0) return 7;
  if (mailbox.request_ring[2].payload[0] != 'c') return 8;

  queued = 99;
  status = ZZ9KCallAsyncBatch(&library, &asyncs[3], &requests[3], 1,
                              0, 0, &queued);
  if (status != ZZ9K_STATUS_BUSY) return 9;
  if (queued != 0U) return 10;

  ZZ9KClose(&library);
  return 0;
}

static int test_query_caps_uses_library_context(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KCaps caps;

  init_mailbox(&mailbox);
  prepare_completion(&mailbox, 1, ZZ9K_OP_QUERY_CAPS, ZZ9K_STATUS_OK, 36);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], ZZ9K_ABI_MAGIC);
  zz9k_put_be16(&mailbox.completion_ring[0].payload[4], ZZ9K_ABI_VERSION_MAJOR);
  zz9k_put_be16(&mailbox.completion_ring[0].payload[6], ZZ9K_ABI_VERSION_MINOR);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8],
                ZZ9K_CAP_MAILBOX | ZZ9K_CAP_SHARED_ALLOC);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[12], 48);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16], 32);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[20], 17);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[28], 64);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[32], 64);

  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  memset(&caps, 0, sizeof(caps));
  if (ZZ9KQueryCaps(&library, &caps) != ZZ9K_STATUS_OK) return 2;
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_QUERY_CAPS) {
    return 3;
  }
  if (caps.max_shared_buffers != 32) return 4;
  if (caps.max_surfaces != 17) return 5;

  ZZ9KClose(&library);
  return 0;
}

static int test_query_service_uses_library_context(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KServiceInfo service;

  init_mailbox(&mailbox);
  prepare_completion(&mailbox, 1, ZZ9K_OP_QUERY_SERVICE,
                     ZZ9K_STATUS_OK, 48);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0],
                ZZ9K_SERVICE_MEMORY);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4], 0x00020000UL);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8],
                ZZ9K_CAP_SHARED_ALLOC | ZZ9K_CAP_MEMORY_OPS);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[12],
                ZZ9K_SERVICE_FLAG_FIRMWARE);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16],
                ZZ9K_SERVICE_MEMORY);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[20], 4);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[24], 48);
  memcpy(&mailbox.completion_ring[0].payload[28], "memory", 6);

  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  memset(&service, 0, sizeof(service));
  if (ZZ9KQueryService(&library, ZZ9K_SERVICE_MEMORY, &service) !=
      ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_QUERY_SERVICE) {
    return 3;
  }
  if (service.service_id != ZZ9K_SERVICE_MEMORY) return 4;
  if (strcmp(service.name, "memory") != 0) return 5;

  ZZ9KClose(&library);
  return 0;
}

static int test_ping_uses_library_context(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  uint8_t request_payload[4];
  uint8_t reply_payload[8];
  uint32_t reply_len;

  init_mailbox(&mailbox);
  request_payload[0] = 'p';
  request_payload[1] = 'i';
  request_payload[2] = 'n';
  request_payload[3] = 'g';
  prepare_completion(&mailbox, 1, ZZ9K_OP_PING, ZZ9K_STATUS_OK, 4);
  memcpy((void *)mailbox.completion_ring[0].payload, request_payload, 4);

  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  memset(reply_payload, 0, sizeof(reply_payload));
  reply_len = sizeof(reply_payload);
  if (ZZ9KPing(&library, request_payload, 4, reply_payload, &reply_len) !=
      ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) != ZZ9K_OP_PING) {
    return 3;
  }
  if (reply_len != 4) return 4;
  if (memcmp(reply_payload, request_payload, 4) != 0) return 5;

  ZZ9KClose(&library);
  return 0;
}

static int test_decode_image_uses_library_context(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KImageDecodeDesc desc;
  ZZ9KImageDecodeResult result;

  init_mailbox(&mailbox);
  memset(&desc, 0, sizeof(desc));
  desc.src_handle = 0x40000040UL;
  desc.src_length = 2048;
  desc.dst_surface = 0x40000041UL;
  desc.dst_width = 64;
  desc.dst_height = 64;
  desc.output_format = ZZ9K_SURFACE_FORMAT_ARGB8888;
  prepare_completion(&mailbox, 1, ZZ9K_OP_DECODE_JPEG, ZZ9K_STATUS_OK,
                     sizeof(ZZ9KImageDecodeResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 640);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4], 480);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8],
                ZZ9K_SURFACE_FORMAT_ARGB8888);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16], 16384);

  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  memset(&result, 0, sizeof(result));
  if (ZZ9KDecodeImage(&library, ZZ9K_OP_DECODE_JPEG, &desc, &result) !=
      ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_DECODE_JPEG) {
    return 3;
  }
  if (result.width != 640 || result.height != 480) return 4;
  if (result.output_format != ZZ9K_SURFACE_FORMAT_ARGB8888 ||
      result.bytes_written != 16384) {
    return 5;
  }

  ZZ9KClose(&library);
  return 0;
}

static int test_decode_codec_wrappers_use_fixed_opcodes(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KImageDecodeDesc desc;
  ZZ9KImageDecodeResult result;

  init_mailbox(&mailbox);
  memset(&desc, 0, sizeof(desc));
  desc.src_handle = 0x40000100UL;
  desc.src_length = 4096U;
  desc.dst_surface = 0x40000101UL;
  desc.dst_width = 128U;
  desc.dst_height = 64U;
  desc.output_format = ZZ9K_SURFACE_FORMAT_BGRA8888;

  prepare_completion_at(&mailbox, 0, 1, ZZ9K_OP_DECODE_JPEG,
                        ZZ9K_STATUS_OK,
                        sizeof(ZZ9KImageDecodeResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 128U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4], 64U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[8],
                ZZ9K_SURFACE_FORMAT_BGRA8888);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16], 32768U);

  prepare_completion_at(&mailbox, 1, 2, ZZ9K_OP_DECODE_PNG,
                        ZZ9K_STATUS_OK,
                        sizeof(ZZ9KImageDecodeResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[1].payload[0], 128U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[4], 64U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[8],
                ZZ9K_SURFACE_FORMAT_BGRA8888);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[16], 32768U);

  prepare_completion_at(&mailbox, 2, 3, ZZ9K_OP_DECODE_GIF,
                        ZZ9K_STATUS_OK,
                        sizeof(ZZ9KImageDecodeResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[2].payload[0], 128U);
  zz9k_put_be32(&mailbox.completion_ring[2].payload[4], 64U);
  zz9k_put_be32(&mailbox.completion_ring[2].payload[8],
                ZZ9K_SURFACE_FORMAT_BGRA8888);
  zz9k_put_be32(&mailbox.completion_ring[2].payload[16], 32768U);

  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  memset(&result, 0, sizeof(result));
  if (ZZ9KDecodeJpeg(&library, &desc, &result) != ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_DECODE_JPEG) {
    return 3;
  }
  if (result.bytes_written != 32768U) return 4;

  memset(&result, 0, sizeof(result));
  if (ZZ9KDecodePng(&library, &desc, &result) != ZZ9K_STATUS_OK) {
    return 5;
  }
  if (zz9k_get_be16(mailbox.request_ring[1].opcode) !=
      ZZ9K_OP_DECODE_PNG) {
    return 6;
  }

  memset(&result, 0, sizeof(result));
  if (ZZ9KDecodeGif(&library, &desc, &result) != ZZ9K_STATUS_OK) {
    return 7;
  }
  if (zz9k_get_be16(mailbox.request_ring[2].opcode) !=
      ZZ9K_OP_DECODE_GIF) {
    return 8;
  }

  ZZ9KClose(&library);
  return 0;
}

static int test_image_session_uses_library_context(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KImageSessionBeginDesc begin;
  ZZ9KImageSessionFeedDesc feed;
  ZZ9KImageSessionResult result;

  init_mailbox(&mailbox);
  prepare_completion_at(&mailbox, 0, 1, ZZ9K_OP_IMAGE_SESSION_BEGIN,
                        ZZ9K_STATUS_OK,
                        sizeof(ZZ9KImageSessionResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 7U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[16],
                ZZ9K_SURFACE_FORMAT_BGRA8888);

  prepare_completion_at(&mailbox, 1, 2, ZZ9K_OP_IMAGE_SESSION_FEED,
                        ZZ9K_STATUS_OK,
                        sizeof(ZZ9KImageSessionResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[1].payload[0], 7U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[4],
                ZZ9K_IMAGE_SESSION_STATE_COMPLETE);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[8], 320U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[12], 200U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[16],
                ZZ9K_SURFACE_FORMAT_BGRA8888);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[36], 2048U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[40], 256000U);

  prepare_completion_at(&mailbox, 2, 3, ZZ9K_OP_IMAGE_SESSION_CLOSE,
                        ZZ9K_STATUS_OK, 0);

  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  memset(&begin, 0, sizeof(begin));
  begin.codec = ZZ9K_IMAGE_CODEC_JPEG;
  begin.output_mode = ZZ9K_IMAGE_OUTPUT_FRAMEBUFFER;
  begin.dst_surface = ZZ9K_SURFACE_HANDLE_FRAMEBUFFER;
  begin.dst_width = 320U;
  begin.dst_height = 200U;
  begin.output_format = ZZ9K_SURFACE_FORMAT_BGRA8888;
  memset(&result, 0, sizeof(result));
  if (ZZ9KImageSessionBegin(&library, &begin, &result) !=
      ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_IMAGE_SESSION_BEGIN) {
    return 3;
  }
  if (result.session != 7U ||
      result.state != ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT) {
    return 4;
  }

  memset(&feed, 0, sizeof(feed));
  feed.session = 7U;
  feed.src_handle = 0x40000002UL;
  feed.src_length = 2048U;
  feed.flags = ZZ9K_IMAGE_SESSION_FEED_EOF;
  memset(&result, 0, sizeof(result));
  if (ZZ9KImageSessionFeed(&library, &feed, &result) != ZZ9K_STATUS_OK) {
    return 5;
  }
  if (zz9k_get_be16(mailbox.request_ring[1].opcode) !=
      ZZ9K_OP_IMAGE_SESSION_FEED) {
    return 6;
  }
  if (result.state != ZZ9K_IMAGE_SESSION_STATE_COMPLETE ||
      result.bytes_consumed != 2048U ||
      result.bytes_written != 256000U) {
    return 7;
  }

  if (ZZ9KImageSessionClose(&library, 7U, 0U) != ZZ9K_STATUS_OK) {
    return 8;
  }
  if (zz9k_get_be16(mailbox.request_ring[2].opcode) !=
      ZZ9K_OP_IMAGE_SESSION_CLOSE) {
    return 9;
  }

  ZZ9KClose(&library);
  return 0;
}

static int test_surface_ops_use_library_context(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KSurfaceFillDesc fill;
  ZZ9KSurfaceCopyDesc copy;

  init_mailbox(&mailbox);
  memset(&fill, 0, sizeof(fill));
  fill.surface = ZZ9K_SURFACE_HANDLE_FRAMEBUFFER;
  fill.x = 1U;
  fill.y = 2U;
  fill.width = 3U;
  fill.height = 4U;
  fill.color = 0xff445566UL;
  prepare_completion(&mailbox, 1, ZZ9K_OP_FILL_SURFACE,
                     ZZ9K_STATUS_OK, 0);

  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  if (ZZ9KFillSurface(&library, &fill) != ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_FILL_SURFACE) {
    return 3;
  }
  if (zz9k_get_be32(&mailbox.request_ring[0].payload[20]) !=
      0xff445566UL) {
    return 4;
  }

  memset(&copy, 0, sizeof(copy));
  copy.src_surface = 0x40000001UL;
  copy.dst_surface = ZZ9K_SURFACE_HANDLE_FRAMEBUFFER;
  copy.width = 8U;
  copy.height = 9U;
  prepare_completion_at(&mailbox, 1, 2, ZZ9K_OP_COPY_SURFACE,
                        ZZ9K_STATUS_OK, 0);

  if (ZZ9KCopySurface(&library, &copy) != ZZ9K_STATUS_OK) {
    return 5;
  }
  if (zz9k_get_be16(mailbox.request_ring[1].opcode) !=
      ZZ9K_OP_COPY_SURFACE) {
    return 6;
  }
  if (zz9k_get_be32(&mailbox.request_ring[1].payload[24]) != 8U) {
    return 7;
  }

  ZZ9KClose(&library);
  return 0;
}

static int test_crypto_hash_uses_library_context(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KCryptoHashDesc desc;
  ZZ9KCryptoResult result;

  init_mailbox(&mailbox);
  memset(&desc, 0, sizeof(desc));
  desc.algorithm = ZZ9K_CRYPTO_HASH_SHA512;
  desc.src_handle = 0x40000050UL;
  desc.src_length = 512;
  desc.dst_handle = 0x40000051UL;
  prepare_completion(&mailbox, 1, ZZ9K_OP_CRYPTO_HASH, ZZ9K_STATUS_OK,
                     sizeof(ZZ9KCryptoResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 64);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_CRYPTO_HASH_SHA512);

  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  memset(&result, 0, sizeof(result));
  if (ZZ9KCryptoHash(&library, &desc, &result) != ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_CRYPTO_HASH) {
    return 3;
  }
  if (result.bytes_written != 64 ||
      result.algorithm != ZZ9K_CRYPTO_HASH_SHA512) {
    return 4;
  }

  ZZ9KClose(&library);
  return 0;
}

static int test_crypto_hash_batch_uses_library_context(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KCryptoHashDesc descs[2];
  ZZ9KCryptoResult results[2];

  init_mailbox(&mailbox);
  memset(descs, 0, sizeof(descs));
  descs[0].algorithm = ZZ9K_CRYPTO_HASH_SHA256;
  descs[0].flags = ZZ9K_CRYPTO_HASH_FLAG_HMAC;
  descs[0].src_handle = 0x40000060UL;
  descs[0].src_length = 0x4000UL;
  descs[0].key_handle = 0x40000061UL;
  descs[0].key_length = 64U;
  descs[0].dst_handle = 0x40000062UL;
  descs[1] = descs[0];
  descs[1].src_offset = 0x4000UL;
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

  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  memset(results, 0, sizeof(results));
  if (ZZ9KCryptoHashBatch(&library, descs, results, 2U, 2U,
                          ZZ9K_DEFAULT_TIMEOUT_TICKS) != ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_CRYPTO_HASH) {
    return 3;
  }
  if (zz9k_get_be32(mailbox.request_ring[1].user_cookie) != 1U) {
    return 4;
  }
  if (results[0].bytes_written != 32U ||
      results[1].bytes_written != 32U) {
    return 5;
  }

  ZZ9KClose(&library);
  return 0;
}

static int test_crypto_stream_uses_library_context(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KCryptoStreamDesc desc;
  ZZ9KCryptoResult result;

  init_mailbox(&mailbox);
  memset(&desc, 0, sizeof(desc));
  desc.algorithm = ZZ9K_CRYPTO_STREAM_CHACHA20;
  desc.src_handle = 0x40000080UL;
  desc.src_length = 1024U;
  desc.dst_handle = 0x40000081UL;
  desc.key_handle = 0x40000082UL;
  desc.nonce_handle = 0x40000083UL;
  prepare_completion(&mailbox, 1, ZZ9K_OP_CRYPTO_STREAM, ZZ9K_STATUS_OK,
                     sizeof(ZZ9KCryptoResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], desc.src_length);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_CRYPTO_STREAM_CHACHA20);

  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  memset(&result, 0, sizeof(result));
  if (ZZ9KCryptoStream(&library, &desc, &result) != ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_CRYPTO_STREAM) {
    return 3;
  }
  if (result.bytes_written != desc.src_length ||
      result.algorithm != ZZ9K_CRYPTO_STREAM_CHACHA20) {
    return 4;
  }

  ZZ9KClose(&library);
  return 0;
}

static int test_crypto_stream_batch_uses_library_context(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KCryptoStreamDesc descs[2];
  ZZ9KCryptoResult results[2];

  init_mailbox(&mailbox);
  memset(descs, 0, sizeof(descs));
  descs[0].algorithm = ZZ9K_CRYPTO_STREAM_CHACHA20;
  descs[0].src_handle = 0x40000090UL;
  descs[0].src_length = 0x4000UL;
  descs[0].dst_handle = 0x40000091UL;
  descs[0].key_handle = 0x40000092UL;
  descs[0].nonce_handle = 0x40000093UL;
  descs[0].counter = 1U;
  descs[1] = descs[0];
  descs[1].src_offset = 0x4000UL;
  descs[1].dst_offset = 0x4000UL;
  descs[1].counter = 2U;

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

  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  memset(results, 0, sizeof(results));
  if (ZZ9KCryptoStreamBatch(&library, descs, results, 2U, 2U,
                            ZZ9K_DEFAULT_TIMEOUT_TICKS) != ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_CRYPTO_STREAM) {
    return 3;
  }
  if (zz9k_get_be32(mailbox.request_ring[1].user_cookie) != 1U) {
    return 4;
  }
  if (results[0].bytes_written != 0x4000U ||
      results[1].bytes_written != 0x4000U ||
      results[0].algorithm != ZZ9K_CRYPTO_STREAM_CHACHA20 ||
      results[1].algorithm != ZZ9K_CRYPTO_STREAM_CHACHA20) {
    return 5;
  }

  ZZ9KClose(&library);
  return 0;
}

static int test_crypto_aead_batch_uses_library_context(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KCryptoAeadDesc descs[2];
  ZZ9KCryptoResult results[2];

  init_mailbox(&mailbox);
  memset(descs, 0, sizeof(descs));
  descs[0].src_handle = 0x400000a0UL;
  descs[0].src_length = 0x4000UL;
  descs[0].dst_handle = 0x400000a1UL;
  descs[0].aad_handle = 0x400000a2UL;
  descs[0].aad_length = 12U;
  descs[0].key_handle = 0x400000a3UL;
  descs[0].nonce_handle = 0x400000a4UL;
  descs[1] = descs[0];
  descs[1].src_offset = 0x4000UL;
  descs[1].dst_offset = 0x4010UL;

  prepare_completion_at(&mailbox, 0, 2, ZZ9K_OP_CRYPTO_AEAD,
                        ZZ9K_STATUS_OK, sizeof(ZZ9KCryptoResultPayload));
  zz9k_put_be32(mailbox.completion_ring[0].user_cookie, 1U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0], 0x4010U);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_CRYPTO_AEAD_CHACHA20_POLY1305);
  prepare_completion_at(&mailbox, 1, 1, ZZ9K_OP_CRYPTO_AEAD,
                        ZZ9K_STATUS_OK, sizeof(ZZ9KCryptoResultPayload));
  zz9k_put_be32(mailbox.completion_ring[1].user_cookie, 0U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[0], 0x4010U);
  zz9k_put_be32(&mailbox.completion_ring[1].payload[4],
                ZZ9K_CRYPTO_AEAD_CHACHA20_POLY1305);

  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  memset(results, 0, sizeof(results));
  if (ZZ9KCryptoAeadBatch(&library, descs, results, 2U, 2U,
                          ZZ9K_DEFAULT_TIMEOUT_TICKS) != ZZ9K_STATUS_OK) {
    return 2;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) !=
      ZZ9K_OP_CRYPTO_AEAD) {
    return 3;
  }
  if (zz9k_get_be32(mailbox.request_ring[1].user_cookie) != 1U) {
    return 4;
  }
  if (results[0].bytes_written != 0x4010U ||
      results[1].bytes_written != 0x4010U ||
      results[0].algorithm != ZZ9K_CRYPTO_AEAD_CHACHA20_POLY1305 ||
      results[1].algorithm != ZZ9K_CRYPTO_AEAD_CHACHA20_POLY1305) {
    return 5;
  }

  ZZ9KClose(&library);
  return 0;
}

static int test_crypto_kx_uses_library_context(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KCryptoKxDesc desc;
  ZZ9KCryptoResult result;

  init_mailbox(&mailbox);
  if (!zz9k_crypto_build_x25519_desc(&desc, 0x40000090UL, 0U,
                                     0x40000091UL, 0U,
                                     0x40000092UL, 0U)) {
    return 1;
  }
  prepare_completion(&mailbox, 1, ZZ9K_OP_CRYPTO_KX, ZZ9K_STATUS_OK,
                     sizeof(ZZ9KCryptoResultPayload));
  zz9k_put_be32(&mailbox.completion_ring[0].payload[0],
                ZZ9K_CRYPTO_X25519_SHARED_BYTES);
  zz9k_put_be32(&mailbox.completion_ring[0].payload[4],
                ZZ9K_CRYPTO_KX_X25519);

  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 2;

  memset(&result, 0, sizeof(result));
  if (ZZ9KCryptoKeyExchange(&library, &desc, &result) != ZZ9K_STATUS_OK) {
    return 3;
  }
  if (zz9k_get_be16(mailbox.request_ring[0].opcode) != ZZ9K_OP_CRYPTO_KX) {
    return 4;
  }
  if (result.bytes_written != ZZ9K_CRYPTO_X25519_SHARED_BYTES ||
      result.algorithm != ZZ9K_CRYPTO_KX_X25519) {
    return 5;
  }

  ZZ9KClose(&library);
  return 0;
}

static int test_async_call_completes_matching_request(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KAsyncRequest async;
  ZZ9KRequest request;
  struct CallbackState callback_state;
  uint32_t completed = 0;

  init_mailbox(&mailbox);
  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  memset(&request, 0, sizeof(request));
  request.entry.opcode = ZZ9K_OP_PING;
  request.entry.flags = ZZ9K_ENTRY_INLINE_PAYLOAD;
  request.entry.payload_len = 4;
  request.entry.payload.inline_data[0] = 'p';
  request.entry.payload.inline_data[1] = 'i';
  request.entry.payload.inline_data[2] = 'n';
  request.entry.payload.inline_data[3] = 'g';
  memset(&async, 0, sizeof(async));
  memset(&callback_state, 0, sizeof(callback_state));

  if (ZZ9KCallAsync(&library, &async, &request, async_callback,
                    &callback_state) != ZZ9K_STATUS_QUEUED) {
    return 2;
  }
  if (async.request_id == 0 || async.completed) return 3;
  if (zz9k_get_be32(mailbox.descriptor.request_tail) != 1) return 4;

  prepare_completion(&mailbox, async.request_id, ZZ9K_OP_PING,
                     ZZ9K_STATUS_OK, 4);
  mailbox.completion_ring[0].payload[0] = 'p';
  mailbox.completion_ring[0].payload[1] = 'o';
  mailbox.completion_ring[0].payload[2] = 'n';
  mailbox.completion_ring[0].payload[3] = 'g';

  if (ZZ9KPoll(&library, 4, &completed) != ZZ9K_STATUS_OK) return 5;
  if (completed != 1) return 6;
  if (!async.completed || async.status != ZZ9K_STATUS_OK) return 7;
  if (async.reply.payload.inline_data[1] != 'o') return 8;
  if (callback_state.called != 1) return 9;
  if (callback_state.request_id != async.request_id) return 10;
  if (zz9k_get_be32(mailbox.descriptor.completion_head) != 1) return 11;

  ZZ9KClose(&library);
  return 0;
}

static int test_cancel_async_completes_locally_and_ignores_late_reply(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KAsyncRequest async;
  ZZ9KRequest request;
  struct CallbackState callback_state;
  uint32_t completed = 99;
  uint32_t request_id;
  int status;

  init_mailbox(&mailbox);
  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  memset(&request, 0, sizeof(request));
  request.entry.opcode = ZZ9K_OP_PING;
  request.entry.flags = ZZ9K_ENTRY_INLINE_PAYLOAD;
  request.entry.payload_len = 4;
  memcpy(request.entry.payload.inline_data, "stop", 4);
  memset(&async, 0, sizeof(async));
  memset(&callback_state, 0, sizeof(callback_state));

  status = ZZ9KCallAsync(&library, &async, &request, async_callback,
                         &callback_state);
  if (status != ZZ9K_STATUS_QUEUED) return 2;
  request_id = async.request_id;
  if (count_pending(&library) != 1U) return 3;

  status = ZZ9KCancelAsync(&library, &async);
  if (status != ZZ9K_STATUS_OK) return 4;
  if (count_pending(&library) != 0U) return 5;
  if (!async.completed || async.queued) return 6;
  if (async.status != ZZ9K_STATUS_CANCELLED) return 7;
  if (async.request_id != request_id) return 8;
  if (callback_state.called != 1) return 9;
  if (callback_state.status != ZZ9K_STATUS_CANCELLED) return 10;

  prepare_completion(&mailbox, request_id, ZZ9K_OP_PING, ZZ9K_STATUS_OK, 4);
  memcpy((void *)mailbox.completion_ring[0].payload, "stop", 4);

  status = ZZ9KPoll(&library, 4, &completed);
  if (status != ZZ9K_STATUS_OK) return 11;
  if (completed != 1U) return 12;
  if (callback_state.called != 1) return 13;
  if (zz9k_get_be32(mailbox.descriptor.completion_head) != 1U) return 14;

  ZZ9KClose(&library);
  return 0;
}

static int test_wait_async_times_out_without_consuming_request(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KAsyncRequest async;
  ZZ9KRequest request;
  uint32_t polls = 99;
  int status;

  init_mailbox(&mailbox);
  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  memset(&request, 0, sizeof(request));
  request.entry.opcode = ZZ9K_OP_PING;
  request.entry.flags = ZZ9K_ENTRY_INLINE_PAYLOAD;
  request.entry.payload_len = 4;
  memcpy(request.entry.payload.inline_data, "wait", 4);
  memset(&async, 0, sizeof(async));

  status = ZZ9KCallAsync(&library, &async, &request, 0, 0);
  if (status != ZZ9K_STATUS_QUEUED) return 2;

  status = ZZ9KWaitAsync(&library, &async, 3, &polls);
  if (status != ZZ9K_STATUS_TIMEOUT) return 3;
  if (polls != 3U) return 4;
  if (!async.queued || async.completed) return 5;
  if (count_pending(&library) != 1U) return 6;

  ZZ9KClose(&library);
  return 0;
}

static int test_wait_async_uses_event_waiter_between_polls(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KAsyncRequest async;
  ZZ9KRequest request;
  struct EventWaitState event_state;
  uint32_t polls = 99;
  int status;

  init_mailbox(&mailbox);
  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  memset(&request, 0, sizeof(request));
  request.entry.opcode = ZZ9K_OP_PING;
  request.entry.flags = ZZ9K_ENTRY_INLINE_PAYLOAD;
  request.entry.payload_len = 4;
  memcpy(request.entry.payload.inline_data, "wait", 4);
  memset(&async, 0, sizeof(async));

  status = ZZ9KCallAsync(&library, &async, &request, 0, 0);
  if (status != ZZ9K_STATUS_QUEUED) return 2;

  memset(&event_state, 0, sizeof(event_state));
  event_state.mailbox = &mailbox;
  event_state.request_id = async.request_id;
  status = ZZ9KSetEventWaiter(&library, prepare_completion_on_event,
                              &event_state);
  if (status != ZZ9K_STATUS_OK) return 3;

  status = ZZ9KWaitAsync(&library, &async, 4, &polls);
  if (status != ZZ9K_STATUS_OK) return 4;
  if (polls != 2U) return 5;
  if (event_state.wait_calls != 1U) return 6;
  if (!async.completed || async.queued) return 7;
  if (memcmp(async.reply.payload.inline_data, "wake", 4) != 0) {
    return 8;
  }
  if (zz9k_get_be32(mailbox.descriptor.completion_head) != 1U) return 9;

  ZZ9KClose(&library);
  return 0;
}

static int test_wait_async_returns_event_waiter_error(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KAsyncRequest async;
  ZZ9KRequest request;
  struct EventWaitState event_state;
  uint32_t polls = 99;
  int status;

  init_mailbox(&mailbox);
  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  memset(&request, 0, sizeof(request));
  request.entry.opcode = ZZ9K_OP_PING;
  request.entry.flags = ZZ9K_ENTRY_INLINE_PAYLOAD;
  request.entry.payload_len = 4;
  memcpy(request.entry.payload.inline_data, "wait", 4);
  memset(&async, 0, sizeof(async));

  status = ZZ9KCallAsync(&library, &async, &request, 0, 0);
  if (status != ZZ9K_STATUS_QUEUED) return 2;

  memset(&event_state, 0, sizeof(event_state));
  event_state.mailbox = &mailbox;
  event_state.request_id = async.request_id;
  event_state.fail_after_calls = 1U;
  status = ZZ9KSetEventWaiter(&library, prepare_completion_on_event,
                              &event_state);
  if (status != ZZ9K_STATUS_OK) return 3;

  status = ZZ9KWaitAsync(&library, &async, 4, &polls);
  if (status != ZZ9K_STATUS_TIMEOUT) return 4;
  if (polls != 1U) return 5;
  if (event_state.wait_calls != 1U) return 6;
  if (!async.queued || async.completed) return 7;
  if (count_pending(&library) != 1U) return 8;

  ZZ9KClose(&library);
  return 0;
}

static int test_wait_async_ignores_other_completion_until_target_finishes(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KAsyncRequest asyncs[2];
  ZZ9KRequest requests[2];
  struct CallbackState callback_state;
  uint32_t polls = 99;
  int status;

  init_mailbox(&mailbox);
  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  memset(requests, 0, sizeof(requests));
  requests[0].entry.opcode = ZZ9K_OP_PING;
  requests[0].entry.flags = ZZ9K_ENTRY_INLINE_PAYLOAD;
  requests[0].entry.payload_len = 4;
  memcpy(requests[0].entry.payload.inline_data, "one!", 4);
  requests[1].entry.opcode = ZZ9K_OP_PING;
  requests[1].entry.flags = ZZ9K_ENTRY_INLINE_PAYLOAD;
  requests[1].entry.payload_len = 4;
  memcpy(requests[1].entry.payload.inline_data, "two!", 4);
  memset(asyncs, 0, sizeof(asyncs));
  memset(&callback_state, 0, sizeof(callback_state));

  status = ZZ9KCallAsync(&library, &asyncs[0], &requests[0],
                         async_callback, &callback_state);
  if (status != ZZ9K_STATUS_QUEUED) return 2;
  status = ZZ9KCallAsync(&library, &asyncs[1], &requests[1], 0, 0);
  if (status != ZZ9K_STATUS_QUEUED) return 3;

  prepare_completion_at(&mailbox, 0, asyncs[1].request_id, ZZ9K_OP_PING,
                        ZZ9K_STATUS_OK, 4);
  memcpy((void *)mailbox.completion_ring[0].payload, "two!", 4);
  prepare_completion_at(&mailbox, 1, asyncs[0].request_id, ZZ9K_OP_PING,
                        ZZ9K_STATUS_OK, 4);
  memcpy((void *)mailbox.completion_ring[1].payload, "one!", 4);

  status = ZZ9KWaitAsync(&library, &asyncs[0], 4, &polls);
  if (status != ZZ9K_STATUS_OK) return 4;
  if (polls != 2U) return 5;
  if (!asyncs[0].completed || asyncs[0].queued) return 6;
  if (!asyncs[1].completed || asyncs[1].queued) return 7;
  if (callback_state.called != 1) return 8;
  if (callback_state.request_id != asyncs[0].request_id) return 9;
  if (memcmp(asyncs[0].reply.payload.inline_data, "one!", 4) != 0) {
    return 10;
  }
  if (memcmp(asyncs[1].reply.payload.inline_data, "two!", 4) != 0) {
    return 11;
  }
  if (zz9k_get_be32(mailbox.descriptor.completion_head) != 2U) return 12;

  ZZ9KClose(&library);
  return 0;
}

static void build_batch_ping(ZZ9KRequest *request, char tag)
{
  memset(request, 0, sizeof(*request));
  request->entry.opcode = ZZ9K_OP_PING;
  request->entry.flags = ZZ9K_ENTRY_INLINE_PAYLOAD;
  request->entry.payload_len = 4;
  request->entry.payload.inline_data[0] = 'b';
  request->entry.payload.inline_data[1] = tag;
  request->entry.payload.inline_data[2] = '!';
  request->entry.payload.inline_data[3] = '!';
}

static int test_wait_async_batch_completes_all_requests(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KAsyncRequest asyncs[3];
  ZZ9KRequest requests[3];
  uint32_t completed = 99;
  uint32_t polls = 99;
  uint32_t queued = 99;
  int status;

  init_mailbox(&mailbox);
  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  build_batch_ping(&requests[0], '0');
  build_batch_ping(&requests[1], '1');
  build_batch_ping(&requests[2], '2');
  memset(asyncs, 0, sizeof(asyncs));

  status = ZZ9KCallAsyncBatch(&library, asyncs, requests, 3, 0, 0,
                              &queued);
  if (status != ZZ9K_STATUS_QUEUED || queued != 3U) return 2;

  prepare_completion_at(&mailbox, 0, asyncs[2].request_id, ZZ9K_OP_PING,
                        ZZ9K_STATUS_OK, 4);
  memcpy((void *)mailbox.completion_ring[0].payload, "b2!!", 4);
  prepare_completion_at(&mailbox, 1, asyncs[0].request_id, ZZ9K_OP_PING,
                        ZZ9K_STATUS_OK, 4);
  memcpy((void *)mailbox.completion_ring[1].payload, "b0!!", 4);
  prepare_completion_at(&mailbox, 2, asyncs[1].request_id, ZZ9K_OP_PING,
                        ZZ9K_STATUS_OK, 4);
  memcpy((void *)mailbox.completion_ring[2].payload, "b1!!", 4);

  status = ZZ9KWaitAsyncBatch(&library, asyncs, 3, 4, &completed, &polls);
  if (status != ZZ9K_STATUS_OK) return 3;
  if (completed != 3U) return 4;
  if (polls != 3U) return 5;
  if (count_pending(&library) != 0U) return 6;
  if (!asyncs[0].completed || !asyncs[1].completed ||
      !asyncs[2].completed) {
    return 7;
  }
  if (asyncs[0].queued || asyncs[1].queued || asyncs[2].queued) return 8;
  if (memcmp(asyncs[0].reply.payload.inline_data, "b0!!", 4) != 0) {
    return 9;
  }
  if (memcmp(asyncs[1].reply.payload.inline_data, "b1!!", 4) != 0) {
    return 10;
  }
  if (memcmp(asyncs[2].reply.payload.inline_data, "b2!!", 4) != 0) {
    return 11;
  }

  ZZ9KClose(&library);
  return 0;
}

static int test_wait_async_batch_times_out_with_partial_completion(void)
{
  struct TestMailbox mailbox;
  ZZ9KLibrary library;
  ZZ9KAsyncRequest asyncs[3];
  ZZ9KRequest requests[3];
  uint32_t completed = 99;
  uint32_t polls = 99;
  uint32_t queued = 99;
  int status;

  init_mailbox(&mailbox);
  ZZ9KInit(&library);
  if (attach_library(&library, &mailbox) != 0) return 1;

  build_batch_ping(&requests[0], '0');
  build_batch_ping(&requests[1], '1');
  build_batch_ping(&requests[2], '2');
  memset(asyncs, 0, sizeof(asyncs));

  status = ZZ9KCallAsyncBatch(&library, asyncs, requests, 3, 0, 0,
                              &queued);
  if (status != ZZ9K_STATUS_QUEUED || queued != 3U) return 2;

  prepare_completion_at(&mailbox, 0, asyncs[0].request_id, ZZ9K_OP_PING,
                        ZZ9K_STATUS_OK, 4);
  memcpy((void *)mailbox.completion_ring[0].payload, "b0!!", 4);

  status = ZZ9KWaitAsyncBatch(&library, asyncs, 3, 2, &completed, &polls);
  if (status != ZZ9K_STATUS_TIMEOUT) return 3;
  if (completed != 1U) return 4;
  if (polls != 2U) return 5;
  if (!asyncs[0].completed || asyncs[0].queued) return 6;
  if (asyncs[1].completed || !asyncs[1].queued) return 7;
  if (asyncs[2].completed || !asyncs[2].queued) return 8;
  if (count_pending(&library) != 2U) return 9;

  ZZ9KClose(&library);
  return 0;
}

int main(void)
{
  int result;

  result = test_query_caps_uses_library_context();
  if (result) return 10 + result;

  result = test_query_service_uses_library_context();
  if (result) return 20 + result;

  result = test_ping_uses_library_context();
  if (result) return 25 + result;

  result = test_decode_image_uses_library_context();
  if (result) return 26 + result;

  result = test_decode_codec_wrappers_use_fixed_opcodes();
  if (result) return 260 + result;

  result = test_image_session_uses_library_context();
  if (result) return 261 + result;

  result = test_surface_ops_use_library_context();
  if (result) return 265 + result;

  result = test_crypto_hash_uses_library_context();
  if (result) return 27 + result;

  result = test_crypto_hash_batch_uses_library_context();
  if (result) return 28 + result;

  result = test_crypto_stream_uses_library_context();
  if (result) return 29 + result;

  result = test_crypto_stream_batch_uses_library_context();
  if (result) return 30 + result;

  result = test_crypto_aead_batch_uses_library_context();
  if (result) return 31 + result;

  result = test_crypto_kx_uses_library_context();
  if (result) return 300 + result;

  result = test_async_batch_queues_until_request_ring_fills();
  if (result) return 32 + result;

  result = test_async_call_completes_matching_request();
  if (result) return 33 + result;

  result = test_cancel_async_completes_locally_and_ignores_late_reply();
  if (result) return 50 + result;

  result = test_wait_async_times_out_without_consuming_request();
  if (result) return 70 + result;

  result = test_wait_async_uses_event_waiter_between_polls();
  if (result) return 80 + result;

  result = test_wait_async_returns_event_waiter_error();
  if (result) return 85 + result;

  result = test_wait_async_ignores_other_completion_until_target_finishes();
  if (result) return 90 + result;

  result = test_wait_async_batch_completes_all_requests();
  if (result) return 110 + result;

  result = test_wait_async_batch_times_out_with_partial_completion();
  if (result) return 130 + result;

  return 0;
}
