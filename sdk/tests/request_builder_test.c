/*
 * Typed ZZ9KRequest builder checks.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/crypto.h"
#include "zz9k/request.h"
#include <string.h>

static int test_ping_builder_clears_and_copies_inline_payload(void)
{
  ZZ9KRequest request;
  uint8_t payload[4] = {'p', 'i', 'n', 'g'};

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_ping(&request, payload, 4U) != ZZ9K_STATUS_OK) {
    return 1;
  }

  if (request.entry.opcode != ZZ9K_OP_PING) return 2;
  if (request.entry.flags != ZZ9K_ENTRY_INLINE_PAYLOAD) return 3;
  if (request.entry.payload_len != 4U) return 4;
  if (memcmp(request.entry.payload.inline_data, payload, 4U) != 0) {
    return 5;
  }
  if (request.entry.payload.inline_data[4] != 0) return 6;
  if (request.entry.request_id != 0 || request.entry.user_cookie != 0) {
    return 7;
  }

  if (zz9k_request_ping(&request, 0, 1U) != ZZ9K_STATUS_BAD_REQUEST) {
    return 8;
  }

  return 0;
}

static int test_query_service_builder_uses_big_endian_payload(void)
{
  ZZ9KRequest request;
  const ZZ9KQueryServicePayload *payload;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_query_service(&request, ZZ9K_SERVICE_CRYPTO) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (request.entry.opcode != ZZ9K_OP_QUERY_SERVICE) return 2;
  if (request.entry.flags != ZZ9K_ENTRY_INLINE_PAYLOAD) return 3;
  if (request.entry.payload_len != sizeof(ZZ9KQueryServicePayload)) {
    return 4;
  }
  payload = (const ZZ9KQueryServicePayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(payload->service_id) != ZZ9K_SERVICE_CRYPTO) return 5;

  return 0;
}

static int test_memory_builders_encode_handles_and_lengths(void)
{
  ZZ9KRequest request;
  const ZZ9KAllocSharedPayload *alloc_payload;
  const ZZ9KMemFillPayload *fill_payload;
  const ZZ9KMemCopyPayload *copy_payload;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_alloc_shared(&request, 4096U, 64U, 0x12U) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }
  if (request.entry.opcode != ZZ9K_OP_ALLOC_SHARED) return 2;
  if (request.entry.payload_len != sizeof(ZZ9KAllocSharedPayload)) return 3;
  alloc_payload =
      (const ZZ9KAllocSharedPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(alloc_payload->length) != 4096U) return 4;
  if (zz9k_get_be32(alloc_payload->alignment) != 64U) return 5;
  if (zz9k_get_be32(alloc_payload->flags) != 0x12U) return 6;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_mem_fill(&request, 7U, 8U, 9U, 0xabU) !=
      ZZ9K_STATUS_OK) {
    return 7;
  }
  if (request.entry.opcode != ZZ9K_OP_MEM_FILL) return 8;
  fill_payload = (const ZZ9KMemFillPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(fill_payload->handle) != 7U) return 9;
  if (zz9k_get_be32(fill_payload->offset) != 8U) return 10;
  if (zz9k_get_be32(fill_payload->length) != 9U) return 11;
  if (fill_payload->value != 0xabU) return 12;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_mem_copy(&request, 10U, 11U, 12U, 13U, 14U) !=
      ZZ9K_STATUS_OK) {
    return 13;
  }
  if (request.entry.opcode != ZZ9K_OP_MEM_COPY) return 14;
  copy_payload = (const ZZ9KMemCopyPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(copy_payload->dst_handle) != 10U) return 15;
  if (zz9k_get_be32(copy_payload->dst_offset) != 11U) return 16;
  if (zz9k_get_be32(copy_payload->src_handle) != 12U) return 17;
  if (zz9k_get_be32(copy_payload->src_offset) != 13U) return 18;
  if (zz9k_get_be32(copy_payload->length) != 14U) return 19;

  if (zz9k_request_alloc_shared(&request, 0U, 64U, 0U) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 20;
  }
  if (zz9k_request_mem_fill(&request, ZZ9K_INVALID_HANDLE, 0U, 1U, 0U) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 21;
  }

  return 0;
}

static int test_surface_and_scale_builders_encode_descriptors(void)
{
  ZZ9KRequest request;
  ZZ9KScaleImageDesc desc;
  ZZ9KScaleImageClippedDesc clipped_desc;
  ZZ9KSurfaceFillDesc fill_desc;
  ZZ9KSurfaceCopyDesc copy_desc;
  const ZZ9KAllocSurfacePayload *surface_payload;
  const ZZ9KFreeSurfacePayload *free_payload;
  const ZZ9KScaleImagePayload *scale_payload;
  const ZZ9KScaleImageClippedPayload *clipped_payload;
  const ZZ9KSurfaceFillPayload *fill_payload;
  const ZZ9KSurfaceCopyPayload *copy_payload;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_alloc_surface_ex(&request, 320U, 200U,
                                    ZZ9K_SURFACE_FORMAT_ARGB8888, 3U,
                                    1280U) != ZZ9K_STATUS_OK) {
    return 1;
  }
  if (request.entry.opcode != ZZ9K_OP_ALLOC_SURFACE) return 2;
  surface_payload =
      (const ZZ9KAllocSurfacePayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(surface_payload->width) != 320U) return 3;
  if (zz9k_get_be32(surface_payload->height) != 200U) return 4;
  if (zz9k_get_be32(surface_payload->format) !=
      ZZ9K_SURFACE_FORMAT_ARGB8888) {
    return 5;
  }
  if (zz9k_get_be32(surface_payload->flags) != 3U) return 6;
  if (zz9k_get_be32(surface_payload->pitch) != 1280U) return 7;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_free_surface(&request, 0x40000001UL) !=
      ZZ9K_STATUS_OK) {
    return 8;
  }
  if (request.entry.opcode != ZZ9K_OP_FREE_SURFACE) return 9;
  free_payload =
      (const ZZ9KFreeSurfacePayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(free_payload->handle) != 0x40000001UL) return 10;
  if (zz9k_request_free_surface(&request, ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 11;
  }

  memset(&desc, 0, sizeof(desc));
  desc.src_surface = 1U;
  desc.dst_surface = 2U;
  desc.src_x = 3U;
  desc.src_y = 4U;
  desc.src_w = 5U;
  desc.src_h = 6U;
  desc.dst_x = 7U;
  desc.dst_y = 8U;
  desc.dst_w = 9U;
  desc.dst_h = 10U;
  desc.filter = ZZ9K_SCALE_BILINEAR;
  desc.flags = 0x20U;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_scale_image(&request, &desc) != ZZ9K_STATUS_OK) {
    return 12;
  }
  if (request.entry.opcode != ZZ9K_OP_SCALE_IMAGE) return 13;
  scale_payload =
      (const ZZ9KScaleImagePayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(scale_payload->src_surface) != 1U) return 14;
  if (zz9k_get_be32(scale_payload->dst_surface) != 2U) return 15;
  if (zz9k_get_be32(scale_payload->src_w) != 5U) return 16;
  if (zz9k_get_be32(scale_payload->dst_h) != 10U) return 17;
  if (zz9k_get_be32(scale_payload->filter) != ZZ9K_SCALE_BILINEAR) {
    return 18;
  }
  if (zz9k_get_be32(scale_payload->flags) != 0x20U) return 19;

  desc.dst_w = 0U;
  if (zz9k_request_scale_image(&request, &desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 20;
  }

  memset(&clipped_desc, 0, sizeof(clipped_desc));
  clipped_desc.src_surface = 0x40000011UL;
  clipped_desc.dst_surface = ZZ9K_SURFACE_HANDLE_FRAMEBUFFER;
  clipped_desc.src_x = 1U;
  clipped_desc.src_y = 2U;
  clipped_desc.src_w = 320U;
  clipped_desc.src_h = 200U;
  clipped_desc.dst_x = 10U;
  clipped_desc.dst_y = 20U;
  clipped_desc.dst_w = 640U;
  clipped_desc.dst_h = 400U;
  clipped_desc.clip_x = 12U;
  clipped_desc.clip_y = 24U;
  clipped_desc.clip_w = 128U;
  clipped_desc.clip_h = 64U;
  clipped_desc.filter = ZZ9K_SCALE_BILINEAR;
  clipped_desc.flags = 0x100U;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_scale_image_clipped(&request, &clipped_desc) !=
      ZZ9K_STATUS_OK) {
    return 41;
  }
  if (request.entry.opcode != ZZ9K_OP_SCALE_IMAGE_CLIPPED) return 42;
  if (request.entry.payload_len != sizeof(ZZ9KScaleImageClippedPayload)) {
    return 43;
  }
  clipped_payload =
      (const ZZ9KScaleImageClippedPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(clipped_payload->src_surface) != 0x40000011UL) {
    return 44;
  }
  if (zz9k_get_be32(clipped_payload->dst_surface) !=
      ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) {
    return 45;
  }
  if (zz9k_get_be16(clipped_payload->src_x) != 1U) return 46;
  if (zz9k_get_be16(clipped_payload->src_h) != 200U) return 47;
  if (zz9k_get_be16(clipped_payload->dst_y) != 20U) return 48;
  if (zz9k_get_be16(clipped_payload->dst_w) != 640U) return 49;
  if (zz9k_get_be16(clipped_payload->clip_x) != 12U) return 50;
  if (zz9k_get_be16(clipped_payload->clip_h) != 64U) return 51;
  if (zz9k_get_be32(clipped_payload->filter) != ZZ9K_SCALE_BILINEAR) {
    return 52;
  }
  if (zz9k_get_be32(clipped_payload->flags) != 0x100U) return 53;

  clipped_desc.clip_w = 0U;
  if (zz9k_request_scale_image_clipped(&request, &clipped_desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 54;
  }
  clipped_desc.clip_w = 128U;
  clipped_desc.dst_w = 70000U;
  if (zz9k_request_scale_image_clipped(&request, &clipped_desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 55;
  }

  memset(&fill_desc, 0, sizeof(fill_desc));
  fill_desc.surface = ZZ9K_SURFACE_HANDLE_FRAMEBUFFER;
  fill_desc.x = 11U;
  fill_desc.y = 12U;
  fill_desc.width = 13U;
  fill_desc.height = 14U;
  fill_desc.color = 0xff336699UL;
  fill_desc.flags = 0x40U;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_fill_surface(&request, &fill_desc) !=
      ZZ9K_STATUS_OK) {
    return 21;
  }
  if (request.entry.opcode != ZZ9K_OP_FILL_SURFACE) return 22;
  if (request.entry.payload_len != sizeof(ZZ9KSurfaceFillPayload)) {
    return 23;
  }
  fill_payload =
      (const ZZ9KSurfaceFillPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(fill_payload->surface) !=
      ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) {
    return 24;
  }
  if (zz9k_get_be32(fill_payload->x) != 11U) return 25;
  if (zz9k_get_be32(fill_payload->height) != 14U) return 26;
  if (zz9k_get_be32(fill_payload->color) != 0xff336699UL) return 27;
  if (zz9k_get_be32(fill_payload->flags) != 0x40U) return 28;

  fill_desc.width = 0U;
  if (zz9k_request_fill_surface(&request, &fill_desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 29;
  }

  memset(&copy_desc, 0, sizeof(copy_desc));
  copy_desc.src_surface = 0x40000002UL;
  copy_desc.dst_surface = ZZ9K_SURFACE_HANDLE_FRAMEBUFFER;
  copy_desc.src_x = 1U;
  copy_desc.src_y = 2U;
  copy_desc.dst_x = 3U;
  copy_desc.dst_y = 4U;
  copy_desc.width = 5U;
  copy_desc.height = 6U;
  copy_desc.flags = 0x80U;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_copy_surface(&request, &copy_desc) !=
      ZZ9K_STATUS_OK) {
    return 30;
  }
  if (request.entry.opcode != ZZ9K_OP_COPY_SURFACE) return 31;
  if (request.entry.payload_len != sizeof(ZZ9KSurfaceCopyPayload)) {
    return 32;
  }
  copy_payload =
      (const ZZ9KSurfaceCopyPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(copy_payload->src_surface) != 0x40000002UL) return 33;
  if (zz9k_get_be32(copy_payload->dst_surface) !=
      ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) {
    return 34;
  }
  if (zz9k_get_be32(copy_payload->src_y) != 2U) return 35;
  if (zz9k_get_be32(copy_payload->dst_x) != 3U) return 36;
  if (zz9k_get_be32(copy_payload->width) != 5U) return 37;
  if (zz9k_get_be32(copy_payload->height) != 6U) return 38;
  if (zz9k_get_be32(copy_payload->flags) != 0x80U) return 39;

  copy_desc.height = 0U;
  if (zz9k_request_copy_surface(&request, &copy_desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 40;
  }

  return 0;
}

static int test_image_decode_builders_encode_descriptors(void)
{
  ZZ9KRequest request;
  ZZ9KImageDecodeDesc desc;
  const ZZ9KImageDecodePayload *payload;

  memset(&desc, 0, sizeof(desc));
  desc.src_handle = 0x40000001UL;
  desc.src_offset = 128U;
  desc.src_length = 4096U;
  desc.dst_surface = ZZ9K_SURFACE_HANDLE_FRAMEBUFFER;
  desc.dst_x = 10U;
  desc.dst_y = 20U;
  desc.dst_width = 320U;
  desc.dst_height = 200U;
  desc.output_format = ZZ9K_SURFACE_FORMAT_ARGB8888;
  desc.flags = ZZ9K_IMAGE_DECODE_FLAG_DITHER;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_decode_jpeg(&request, &desc) != ZZ9K_STATUS_OK) {
    return 1;
  }
  if (request.entry.opcode != ZZ9K_OP_DECODE_JPEG) return 2;
  if (request.entry.payload_len != sizeof(ZZ9KImageDecodePayload)) return 3;
  payload = (const ZZ9KImageDecodePayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(payload->src_handle) != 0x40000001UL) return 4;
  if (zz9k_get_be32(payload->src_offset) != 128U) return 5;
  if (zz9k_get_be32(payload->src_length) != 4096U) return 6;
  if (zz9k_get_be32(payload->dst_surface) !=
      ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) {
    return 7;
  }
  if (zz9k_get_be32(payload->dst_width) != 320U) return 8;
  if (zz9k_get_be32(payload->dst_height) != 200U) return 9;
  if (zz9k_get_be32(payload->output_format) !=
      ZZ9K_SURFACE_FORMAT_ARGB8888) {
    return 10;
  }
  if (zz9k_get_be32(payload->flags) != ZZ9K_IMAGE_DECODE_FLAG_DITHER) {
    return 11;
  }

  if (zz9k_request_decode_png(&request, &desc) != ZZ9K_STATUS_OK) {
    return 12;
  }
  if (request.entry.opcode != ZZ9K_OP_DECODE_PNG) return 13;
  if (zz9k_request_decode_gif(&request, &desc) != ZZ9K_STATUS_OK) {
    return 14;
  }
  if (request.entry.opcode != ZZ9K_OP_DECODE_GIF) return 15;

  desc.src_length = 0U;
  if (zz9k_request_decode_jpeg(&request, &desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 16;
  }

  return 0;
}

static int test_crypto_hash_builder_encodes_descriptor(void)
{
  ZZ9KRequest request;
  ZZ9KCryptoHashDesc desc;
  const ZZ9KCryptoHashPayload *payload;

  memset(&desc, 0, sizeof(desc));
  desc.src_handle = 0x40000011UL;
  desc.src_offset = 64U;
  desc.src_length = 512U;
  desc.dst_handle = 0x40000012UL;
  desc.dst_offset = 96U;
  desc.key_handle = 0x40000013UL;
  desc.key_offset = 32U;
  desc.key_length = 48U;
  desc.algorithm = ZZ9K_CRYPTO_HASH_SHA256;
  desc.flags = ZZ9K_CRYPTO_HASH_FLAG_HMAC;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_crypto_hash(&request, &desc) != ZZ9K_STATUS_OK) {
    return 1;
  }

  if (request.entry.opcode != ZZ9K_OP_CRYPTO_HASH) return 2;
  if (request.entry.payload_len != sizeof(ZZ9KCryptoHashPayload)) return 3;
  payload = (const ZZ9KCryptoHashPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(payload->src_handle) != 0x40000011UL) return 4;
  if (zz9k_get_be32(payload->src_offset) != 64U) return 5;
  if (zz9k_get_be32(payload->src_length) != 512U) return 6;
  if (zz9k_get_be32(payload->dst_handle) != 0x40000012UL) return 7;
  if (zz9k_get_be32(payload->dst_offset) != 96U) return 8;
  if (zz9k_get_be32(payload->key_handle) != 0x40000013UL) return 9;
  if (zz9k_get_be32(payload->key_offset) != 32U) return 10;
  if (zz9k_get_be32(payload->key_length) != 48U) return 11;
  if (zz9k_get_be32(payload->algorithm) != ZZ9K_CRYPTO_HASH_SHA256) {
    return 12;
  }
  if (zz9k_get_be32(payload->flags) != ZZ9K_CRYPTO_HASH_FLAG_HMAC) {
    return 13;
  }

  memset(&desc, 0, sizeof(desc));
  desc.src_handle = 0x40000014UL;
  desc.src_length = 256U;
  desc.dst_handle = 0x40000015UL;
  desc.key_handle = 0x40000016UL;
  desc.key_length = 32U;
  desc.algorithm = ZZ9K_CRYPTO_HASH_POLY1305;
  if (zz9k_request_crypto_hash(&request, &desc) != ZZ9K_STATUS_OK) {
    return 14;
  }
  payload = (const ZZ9KCryptoHashPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(payload->algorithm) != ZZ9K_CRYPTO_HASH_POLY1305) {
    return 15;
  }
  if (zz9k_get_be32(payload->key_length) != 32U) return 16;
  if (zz9k_get_be32(payload->flags) != 0U) return 17;

  desc.key_handle = ZZ9K_INVALID_HANDLE;
  desc.key_length = 0U;
  if (zz9k_request_crypto_hash(&request, &desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 18;
  }

  desc.flags = 0U;
  desc.key_handle = ZZ9K_INVALID_HANDLE;
  desc.key_length = 0U;
  desc.algorithm = ZZ9K_CRYPTO_HASH_SHA256;
  if (zz9k_request_crypto_hash(&request, &desc) != ZZ9K_STATUS_OK) {
    return 19;
  }

  desc.src_length = 0U;
  if (zz9k_request_crypto_hash(&request, &desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 20;
  }

  return 0;
}

static int test_audio_decode_builder_encodes_descriptor(void)
{
  ZZ9KRequest request;
  ZZ9KAudioDecodeDesc desc;
  const ZZ9KAudioDecodePayload *payload;

  memset(&desc, 0, sizeof(desc));
  desc.src_handle = 0x40000100UL;
  desc.src_offset = 12U;
  desc.src_length = 3456U;
  desc.dst_handle = 0x40000110UL;
  desc.dst_offset = 24U;
  desc.dst_capacity = 8192U;
  desc.output_hz = 48000U;
  desc.output_channels = 2U;
  desc.output_format = ZZ9K_AUDIO_SAMPLE_FORMAT_S16LE;
  desc.flags = ZZ9K_AUDIO_DECODE_FLAG_EXPECT_END;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_decode_mp3(&request, &desc) != ZZ9K_STATUS_OK) {
    return 1;
  }

  if (request.entry.opcode != ZZ9K_OP_DECODE_MP3) return 2;
  if (request.entry.payload_len != sizeof(ZZ9KAudioDecodePayload)) return 3;
  payload = (const ZZ9KAudioDecodePayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(payload->src_handle) != 0x40000100UL) return 4;
  if (zz9k_get_be32(payload->src_offset) != 12U) return 5;
  if (zz9k_get_be32(payload->src_length) != 3456U) return 6;
  if (zz9k_get_be32(payload->dst_handle) != 0x40000110UL) return 7;
  if (zz9k_get_be32(payload->dst_offset) != 24U) return 8;
  if (zz9k_get_be32(payload->dst_capacity) != 8192U) return 9;
  if (zz9k_get_be32(payload->output_hz) != 48000U) return 10;
  if (zz9k_get_be32(payload->output_channels) != 2U) return 11;
  if (zz9k_get_be32(payload->output_format) !=
      ZZ9K_AUDIO_SAMPLE_FORMAT_S16LE) {
    return 12;
  }
  if (zz9k_get_be32(payload->flags) != ZZ9K_AUDIO_DECODE_FLAG_EXPECT_END) {
    return 13;
  }

  desc.src_length = 0U;
  if (zz9k_request_decode_mp3(&request, &desc) != ZZ9K_STATUS_BAD_REQUEST) {
    return 14;
  }

  desc.src_length = 1U;
  desc.output_format = 0xffffffffUL;
  if (zz9k_request_decode_mp3(&request, &desc) != ZZ9K_STATUS_BAD_REQUEST) {
    return 15;
  }

  return 0;
}

static int test_audio_stream_builders_encode_descriptors(void)
{
  ZZ9KAudioStreamBeginDesc begin;
  ZZ9KAudioStreamFeedDesc feed;
  ZZ9KRequest request;
  const ZZ9KAudioStreamBeginPayload *begin_payload;
  const ZZ9KAudioStreamFeedPayload *feed_payload;
  const ZZ9KAudioStreamReadPayload *read_payload;
  const ZZ9KAudioStreamClosePayload *close_payload;
  const ZZ9KAudioStreamPlayPayload *play_payload;
  const ZZ9KAudioStreamStopPayload *stop_payload;

  memset(&begin, 0, sizeof(begin));
  begin.mp3_ring_handle = 0x40000031UL;
  begin.mp3_ring_capacity = 524288U;
  begin.pcm_ring_handle = 0x40000032UL;
  begin.pcm_ring_capacity = 1048576U;
  begin.output_format = ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE;
  begin.low_water_bytes = 32768U;
  begin.high_water_bytes = 65536U;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_audio_stream_begin(&request, &begin) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }
  if (request.entry.opcode != ZZ9K_OP_AUDIO_STREAM_BEGIN) return 2;
  if (request.entry.payload_len != sizeof(ZZ9KAudioStreamBeginPayload)) {
    return 3;
  }
  begin_payload =
      (const ZZ9KAudioStreamBeginPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(begin_payload->mp3_ring_handle) !=
      begin.mp3_ring_handle) {
    return 4;
  }
  if (zz9k_get_be32(begin_payload->pcm_ring_capacity) !=
      begin.pcm_ring_capacity) {
    return 5;
  }
  if (zz9k_get_be32(begin_payload->output_format) !=
      ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE) {
    return 6;
  }

  memset(&feed, 0, sizeof(feed));
  feed.session = 7U;
  feed.src_handle = 0x40000033UL;
  feed.src_offset = 128U;
  feed.src_length = 4096U;
  feed.flags = ZZ9K_AUDIO_STREAM_FEED_EOF;
  if (zz9k_request_audio_stream_feed(&request, &feed) != ZZ9K_STATUS_OK) {
    return 7;
  }
  if (request.entry.opcode != ZZ9K_OP_AUDIO_STREAM_FEED) return 8;
  feed_payload =
      (const ZZ9KAudioStreamFeedPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(feed_payload->session) != 7U) return 9;
  if (zz9k_get_be32(feed_payload->flags) != ZZ9K_AUDIO_STREAM_FEED_EOF) {
    return 10;
  }

  memset(&feed, 0, sizeof(feed));
  feed.session = 7U;
  feed.flags = ZZ9K_AUDIO_STREAM_FEED_DRAIN;
  if (zz9k_request_audio_stream_feed(&request, &feed) != ZZ9K_STATUS_OK) {
    return 40;
  }
  feed_payload =
      (const ZZ9KAudioStreamFeedPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(feed_payload->flags) != ZZ9K_AUDIO_STREAM_FEED_DRAIN) {
    return 41;
  }
  feed.flags = ZZ9K_AUDIO_STREAM_FEED_EOF | ZZ9K_AUDIO_STREAM_FEED_DRAIN;
  if (zz9k_request_audio_stream_feed(&request, &feed) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 42;
  }

  if (zz9k_request_audio_stream_read(&request, 7U, 8192U, 0U) !=
      ZZ9K_STATUS_OK) {
    return 11;
  }
  if (request.entry.opcode != ZZ9K_OP_AUDIO_STREAM_READ) return 12;
  read_payload =
      (const ZZ9KAudioStreamReadPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(read_payload->pcm_read) != 8192U) return 13;

  if (zz9k_request_audio_stream_close(&request, 7U, 0U) !=
      ZZ9K_STATUS_OK) {
    return 14;
  }
  if (request.entry.opcode != ZZ9K_OP_AUDIO_STREAM_CLOSE) return 15;
  close_payload =
      (const ZZ9KAudioStreamClosePayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(close_payload->session) != 7U) return 16;

  if (zz9k_request_audio_stream_play(&request, 7U, 0U) !=
      ZZ9K_STATUS_OK) {
    return 18;
  }
  if (request.entry.opcode != ZZ9K_OP_AUDIO_STREAM_PLAY) return 19;
  if (request.entry.payload_len != sizeof(ZZ9KAudioStreamPlayPayload)) {
    return 20;
  }
  play_payload =
      (const ZZ9KAudioStreamPlayPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(play_payload->session) != 7U) return 21;

  if (zz9k_request_audio_stream_stop(&request, 7U, 0U) !=
      ZZ9K_STATUS_OK) {
    return 22;
  }
  if (request.entry.opcode != ZZ9K_OP_AUDIO_STREAM_STOP) return 23;
  if (request.entry.payload_len != sizeof(ZZ9KAudioStreamStopPayload)) {
    return 24;
  }
  stop_payload =
      (const ZZ9KAudioStreamStopPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(stop_payload->session) != 7U) return 25;

  if (zz9k_request_audio_stream_read(&request, 0U, 0U, 0U) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 17;
  }
  if (zz9k_request_audio_stream_play(&request, 0U, 0U) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 26;
  }
  if (zz9k_request_audio_stream_stop(&request, 0U, 0U) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 27;
  }

  return 0;
}

static int test_crypto_stream_builder_encodes_descriptor(void)
{
  ZZ9KRequest request;
  ZZ9KCryptoStreamDesc desc;
  const ZZ9KCryptoStreamPayload *payload;

  memset(&desc, 0, sizeof(desc));
  desc.src_handle = 0x40000021UL;
  desc.src_offset = 128U;
  desc.src_length = 1024U;
  desc.dst_handle = 0x40000022UL;
  desc.dst_offset = 256U;
  desc.key_handle = 0x40000023UL;
  desc.key_offset = 16U;
  desc.nonce_handle = 0x40000024UL;
  desc.nonce_offset = 8U;
  desc.counter = 7U;
  desc.algorithm = ZZ9K_CRYPTO_STREAM_CHACHA20;
  desc.flags = 0U;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_crypto_stream(&request, &desc) != ZZ9K_STATUS_OK) {
    return 1;
  }

  if (request.entry.opcode != ZZ9K_OP_CRYPTO_STREAM) return 2;
  if (request.entry.payload_len != sizeof(ZZ9KCryptoStreamPayload)) return 3;
  payload = (const ZZ9KCryptoStreamPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(payload->src_handle) != 0x40000021UL) return 4;
  if (zz9k_get_be32(payload->src_offset) != 128U) return 5;
  if (zz9k_get_be32(payload->src_length) != 1024U) return 6;
  if (zz9k_get_be32(payload->dst_handle) != 0x40000022UL) return 7;
  if (zz9k_get_be32(payload->dst_offset) != 256U) return 8;
  if (zz9k_get_be32(payload->key_handle) != 0x40000023UL) return 9;
  if (zz9k_get_be32(payload->key_offset) != 16U) return 10;
  if (zz9k_get_be32(payload->nonce_handle) != 0x40000024UL) return 11;
  if (zz9k_get_be32(payload->nonce_offset) != 8U) return 12;
  if (zz9k_get_be32(payload->counter) != 7U) return 13;
  if (zz9k_get_be32(payload->algorithm) != ZZ9K_CRYPTO_STREAM_CHACHA20) {
    return 14;
  }
  if (zz9k_get_be32(payload->flags) != 0U) return 15;

  desc.src_length = 0U;
  if (zz9k_request_crypto_stream(&request, &desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 16;
  }

  return 0;
}

static int test_crypto_aead_builder_encodes_descriptor(void)
{
  ZZ9KRequest request;
  ZZ9KCryptoAeadDesc desc;
  const ZZ9KCryptoAeadPayload *payload;

  memset(&desc, 0, sizeof(desc));
  desc.src_handle = 0x40000031UL;
  desc.src_offset = 64U;
  desc.src_length = 114U;
  desc.dst_handle = 0x40000032UL;
  desc.dst_offset = 128U;
  desc.aad_handle = 0x40000033UL;
  desc.aad_offset = 16U;
  desc.aad_length = 12U;
  desc.key_handle = 0x40000034UL;
  desc.key_offset = 32U;
  desc.nonce_handle = 0x40000035UL;
  desc.flags = ZZ9K_CRYPTO_AEAD_FLAG_DECRYPT;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_crypto_aead(&request, &desc) != ZZ9K_STATUS_OK) {
    return 1;
  }

  if (request.entry.opcode != ZZ9K_OP_CRYPTO_AEAD) return 2;
  if (request.entry.payload_len != sizeof(ZZ9KCryptoAeadPayload)) return 3;
  payload = (const ZZ9KCryptoAeadPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(payload->src_handle) != 0x40000031UL) return 4;
  if (zz9k_get_be32(payload->src_offset) != 64U) return 5;
  if (zz9k_get_be32(payload->src_length) != 114U) return 6;
  if (zz9k_get_be32(payload->dst_handle) != 0x40000032UL) return 7;
  if (zz9k_get_be32(payload->dst_offset) != 128U) return 8;
  if (zz9k_get_be32(payload->aad_handle) != 0x40000033UL) return 9;
  if (zz9k_get_be32(payload->aad_offset) != 16U) return 10;
  if (zz9k_get_be32(payload->aad_length) != 12U) return 11;
  if (zz9k_get_be32(payload->key_handle) != 0x40000034UL) return 12;
  if (zz9k_get_be32(payload->key_offset) != 32U) return 13;
  if (zz9k_get_be32(payload->nonce_handle) != 0x40000035UL) return 14;
  if (zz9k_get_be32(payload->flags) != ZZ9K_CRYPTO_AEAD_FLAG_DECRYPT) {
    return 15;
  }

  desc.flags = 0x80000000UL;
  if (zz9k_request_crypto_aead(&request, &desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 16;
  }

  desc.flags = 0U;
  desc.aad_handle = ZZ9K_INVALID_HANDLE;
  desc.aad_length = 0U;
  if (zz9k_request_crypto_aead(&request, &desc) != ZZ9K_STATUS_OK) {
    return 17;
  }

  desc.src_length = 0U;
  if (zz9k_request_crypto_aead(&request, &desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 18;
  }

  return 0;
}

static int test_crypto_aes_gcm_builder_encodes_descriptor(void)
{
  ZZ9KRequest request;
  ZZ9KCryptoAeadDesc desc;
  const ZZ9KCryptoAeadPayload *payload;

  /* AES-256-GCM decrypt: the algorithm rides in the flags field and the AEAD
   * request builder must accept (not reject) those algorithm bits. */
  if (!zz9k_crypto_build_aes_gcm_desc(&desc, 0x40000041UL, 0U, 256U,
                                      0x40000042UL, 0U, 0U, 0U, 0U,
                                      0x40000043UL, 0U,
                                      ZZ9K_CRYPTO_AES256_KEY_BYTES,
                                      0x40000044UL,
                                      ZZ9K_CRYPTO_AEAD_FLAG_DECRYPT)) {
    return 1;
  }
  if (zz9k_request_crypto_aead(&request, &desc) != ZZ9K_STATUS_OK) return 2;
  if (request.entry.opcode != ZZ9K_OP_CRYPTO_AEAD) return 3;
  payload = (const ZZ9KCryptoAeadPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(payload->key_handle) != 0x40000043UL) return 4;
  if (zz9k_get_be32(payload->nonce_handle) != 0x40000044UL) return 5;
  {
    uint32_t flags = zz9k_get_be32(payload->flags);
    if (ZZ9K_CRYPTO_AEAD_FLAG_GET_ALG(flags) != ZZ9K_CRYPTO_AEAD_AES256_GCM) {
      return 6;
    }
    if ((flags & ZZ9K_CRYPTO_AEAD_FLAG_DECRYPT) == 0U) return 7;
  }

  /* AES-128-GCM encrypt encodes the AES128 algorithm. */
  if (!zz9k_crypto_build_aes_gcm_desc(&desc, 0x40000041UL, 0U, 16U,
                                      0x40000042UL, 0U, 0U, 0U, 0U,
                                      0x40000043UL, 0U,
                                      ZZ9K_CRYPTO_AES128_KEY_BYTES,
                                      0x40000044UL, 0U)) {
    return 8;
  }
  if (zz9k_request_crypto_aead(&request, &desc) != ZZ9K_STATUS_OK) return 9;
  payload = (const ZZ9KCryptoAeadPayload *)request.entry.payload.inline_data;
  if (ZZ9K_CRYPTO_AEAD_FLAG_GET_ALG(zz9k_get_be32(payload->flags)) !=
      ZZ9K_CRYPTO_AEAD_AES128_GCM) {
    return 10;
  }
  return 0;
}

static int test_decompress_builder_encodes_descriptor(void)
{
  ZZ9KRequest request;
  ZZ9KDecompressDesc desc;
  ZZ9KDecompressTestDesc test_desc;
  ZZ9KDecompressStreamBeginDesc stream_begin;
  ZZ9KDecompressStreamReadDesc stream_read;
  ZZ9KDecompressStreamFeedDesc stream_feed;
  const ZZ9KDecompressPayload *payload;
  const ZZ9KDecompressTestPayload *test_payload;
  const ZZ9KDecompressStreamBeginPayload *stream_begin_payload;
  const ZZ9KDecompressStreamReadPayload *stream_read_payload;
  const ZZ9KDecompressStreamFeedPayload *stream_feed_payload;
  const ZZ9KDecompressStreamClosePayload *stream_close_payload;

  memset(&desc, 0, sizeof(desc));
  desc.src_handle = 0x40000041UL;
  desc.src_offset = 32U;
  desc.src_length = 2048U;
  desc.dst_handle = 0x40000042UL;
  desc.dst_offset = 64U;
  desc.dst_capacity = 8192U;
  desc.algorithm = ZZ9K_COMPRESSION_GZIP;
  desc.flags = ZZ9K_DECOMPRESS_FLAG_EXPECT_END;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_decompress(&request, &desc) != ZZ9K_STATUS_OK) {
    return 1;
  }

  if (request.entry.opcode != ZZ9K_OP_DECOMPRESS) return 2;
  if (request.entry.payload_len != sizeof(ZZ9KDecompressPayload)) return 3;
  payload = (const ZZ9KDecompressPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(payload->src_handle) != 0x40000041UL) return 4;
  if (zz9k_get_be32(payload->src_offset) != 32U) return 5;
  if (zz9k_get_be32(payload->src_length) != 2048U) return 6;
  if (zz9k_get_be32(payload->dst_handle) != 0x40000042UL) return 7;
  if (zz9k_get_be32(payload->dst_offset) != 64U) return 8;
  if (zz9k_get_be32(payload->dst_capacity) != 8192U) return 9;
  if (zz9k_get_be32(payload->algorithm) != ZZ9K_COMPRESSION_GZIP) {
    return 10;
  }
  if (zz9k_get_be32(payload->flags) != ZZ9K_DECOMPRESS_FLAG_EXPECT_END) {
    return 11;
  }

  desc.algorithm = ZZ9K_COMPRESSION_NONE;
  if (zz9k_request_decompress(&request, &desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 12;
  }

  desc.algorithm = ZZ9K_COMPRESSION_GZIP;
  desc.dst_capacity = 0U;
  if (zz9k_request_decompress(&request, &desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 13;
  }

  memset(&test_desc, 0, sizeof(test_desc));
  test_desc.src_handle = 0x40000051UL;
  test_desc.src_offset = 96U;
  test_desc.src_length = 4096U;
  test_desc.output_limit = 65536U;
  test_desc.algorithm = ZZ9K_COMPRESSION_LZMA_ALONE;
  test_desc.flags = ZZ9K_DECOMPRESS_FLAG_EXPECT_END;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_decompress_test(&request, &test_desc) !=
      ZZ9K_STATUS_OK) {
    return 14;
  }

  if (request.entry.opcode != ZZ9K_OP_DECOMPRESS_TEST) return 15;
  if (request.entry.payload_len != sizeof(ZZ9KDecompressTestPayload)) {
    return 16;
  }
  test_payload =
      (const ZZ9KDecompressTestPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(test_payload->src_handle) != 0x40000051UL) return 17;
  if (zz9k_get_be32(test_payload->src_offset) != 96U) return 18;
  if (zz9k_get_be32(test_payload->src_length) != 4096U) return 19;
  if (zz9k_get_be32(test_payload->output_limit) != 65536U) return 20;
  if (zz9k_get_be32(test_payload->algorithm) !=
      ZZ9K_COMPRESSION_LZMA_ALONE) {
    return 21;
  }
  if (zz9k_get_be32(test_payload->flags) !=
      ZZ9K_DECOMPRESS_FLAG_EXPECT_END) {
    return 22;
  }

  test_desc.output_limit = 0U;
  if (zz9k_request_decompress_test(&request, &test_desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 23;
  }

  memset(&stream_begin, 0, sizeof(stream_begin));
  stream_begin.src_handle = 0x40000061UL;
  stream_begin.src_offset = 128U;
  stream_begin.src_length = 8192U;
  stream_begin.output_limit = 65536U;
  stream_begin.algorithm = ZZ9K_COMPRESSION_LZMA_ALONE;
  stream_begin.flags = ZZ9K_DECOMPRESS_FLAG_EXPECT_END;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_decompress_stream_begin(&request, &stream_begin) !=
      ZZ9K_STATUS_OK) {
    return 24;
  }
  if (request.entry.opcode != ZZ9K_OP_DECOMPRESS_STREAM_BEGIN) return 25;
  if (request.entry.payload_len !=
      sizeof(ZZ9KDecompressStreamBeginPayload)) {
    return 26;
  }
  stream_begin_payload =
      (const ZZ9KDecompressStreamBeginPayload *)
      request.entry.payload.inline_data;
  if (zz9k_get_be32(stream_begin_payload->src_handle) != 0x40000061UL) {
    return 27;
  }
  if (zz9k_get_be32(stream_begin_payload->src_offset) != 128U) return 28;
  if (zz9k_get_be32(stream_begin_payload->src_length) != 8192U) return 29;
  if (zz9k_get_be32(stream_begin_payload->output_limit) != 65536U) {
    return 30;
  }
  if (zz9k_get_be32(stream_begin_payload->algorithm) !=
      ZZ9K_COMPRESSION_LZMA_ALONE) {
    return 31;
  }
  if (zz9k_get_be32(stream_begin_payload->flags) !=
      ZZ9K_DECOMPRESS_FLAG_EXPECT_END) {
    return 32;
  }

  stream_begin.output_limit = 0U;
  if (zz9k_request_decompress_stream_begin(&request, &stream_begin) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 33;
  }

  memset(&stream_begin, 0, sizeof(stream_begin));
  stream_begin.src_handle = ZZ9K_INVALID_HANDLE;
  stream_begin.src_offset = 0U;
  stream_begin.src_length = 0U;
  stream_begin.output_limit = 65536U;
  stream_begin.algorithm = ZZ9K_COMPRESSION_LZMA_ALONE;
  stream_begin.flags = ZZ9K_DECOMPRESS_FLAG_EXPECT_END |
                       ZZ9K_DECOMPRESS_FLAG_FEED_INPUT;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_decompress_stream_begin(&request, &stream_begin) !=
      ZZ9K_STATUS_OK) {
    return 49;
  }
  stream_begin_payload =
      (const ZZ9KDecompressStreamBeginPayload *)
      request.entry.payload.inline_data;
  if (zz9k_get_be32(stream_begin_payload->src_handle) !=
      ZZ9K_INVALID_HANDLE) {
    return 50;
  }
  if (zz9k_get_be32(stream_begin_payload->src_length) != 0U) return 51;
  if (zz9k_get_be32(stream_begin_payload->flags) !=
      (ZZ9K_DECOMPRESS_FLAG_EXPECT_END |
       ZZ9K_DECOMPRESS_FLAG_FEED_INPUT)) {
    return 52;
  }

  memset(&stream_read, 0, sizeof(stream_read));
  stream_read.session = 7U;
  stream_read.dst_handle = 0x40000062UL;
  stream_read.dst_offset = 256U;
  stream_read.dst_capacity = 32768U;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_decompress_stream_read(&request, &stream_read) !=
      ZZ9K_STATUS_OK) {
    return 34;
  }
  if (request.entry.opcode != ZZ9K_OP_DECOMPRESS_STREAM_READ) return 35;
  if (request.entry.payload_len != sizeof(ZZ9KDecompressStreamReadPayload)) {
    return 36;
  }
  stream_read_payload =
      (const ZZ9KDecompressStreamReadPayload *)
      request.entry.payload.inline_data;
  if (zz9k_get_be32(stream_read_payload->session) != 7U) return 37;
  if (zz9k_get_be32(stream_read_payload->dst_handle) != 0x40000062UL) {
    return 38;
  }
  if (zz9k_get_be32(stream_read_payload->dst_offset) != 256U) return 39;
  if (zz9k_get_be32(stream_read_payload->dst_capacity) != 32768U) {
    return 40;
  }
  if (zz9k_get_be32(stream_read_payload->flags) != 0U) return 41;

  stream_read.session = 0U;
  if (zz9k_request_decompress_stream_read(&request, &stream_read) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 42;
  }

  memset(&stream_feed, 0, sizeof(stream_feed));
  stream_feed.session = 7U;
  stream_feed.src_handle = 0x40000063UL;
  stream_feed.src_offset = 512U;
  stream_feed.src_length = 16384U;
  stream_feed.flags = ZZ9K_DECOMPRESS_STREAM_FEED_EOF;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_decompress_stream_feed(&request, &stream_feed) !=
      ZZ9K_STATUS_OK) {
    return 53;
  }
  if (request.entry.opcode != ZZ9K_OP_DECOMPRESS_STREAM_FEED) return 54;
  if (request.entry.payload_len != sizeof(ZZ9KDecompressStreamFeedPayload)) {
    return 55;
  }
  stream_feed_payload =
      (const ZZ9KDecompressStreamFeedPayload *)
      request.entry.payload.inline_data;
  if (zz9k_get_be32(stream_feed_payload->session) != 7U) return 56;
  if (zz9k_get_be32(stream_feed_payload->src_handle) != 0x40000063UL) {
    return 57;
  }
  if (zz9k_get_be32(stream_feed_payload->src_offset) != 512U) return 58;
  if (zz9k_get_be32(stream_feed_payload->src_length) != 16384U) return 59;
  if (zz9k_get_be32(stream_feed_payload->flags) !=
      ZZ9K_DECOMPRESS_STREAM_FEED_EOF) {
    return 60;
  }

  stream_feed.session = 0U;
  if (zz9k_request_decompress_stream_feed(&request, &stream_feed) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 61;
  }

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_decompress_stream_close(&request, 7U, 0U) !=
      ZZ9K_STATUS_OK) {
    return 43;
  }
  if (request.entry.opcode != ZZ9K_OP_DECOMPRESS_STREAM_CLOSE) return 44;
  if (request.entry.payload_len !=
      sizeof(ZZ9KDecompressStreamClosePayload)) {
    return 45;
  }
  stream_close_payload =
      (const ZZ9KDecompressStreamClosePayload *)
      request.entry.payload.inline_data;
  if (zz9k_get_be32(stream_close_payload->session) != 7U) return 46;
  if (zz9k_get_be32(stream_close_payload->flags) != 0U) return 47;
  if (zz9k_request_decompress_stream_close(&request, 0U, 0U) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 48;
  }

  return 0;
}

static int test_no_payload_builders(void)
{
  ZZ9KRequest request;

  memset(&request, 0xff, sizeof(request));
  zz9k_request_diag_read(&request);
  if (request.entry.opcode != ZZ9K_OP_DIAG_READ) return 1;
  if (request.entry.flags != ZZ9K_ENTRY_INLINE_PAYLOAD) return 2;
  if (request.entry.payload_len != 0U) return 3;

  memset(&request, 0xff, sizeof(request));
  zz9k_request_diag_timing(&request);
  if (request.entry.opcode != ZZ9K_OP_DIAG_TIMING) return 4;
  if (request.entry.flags != ZZ9K_ENTRY_INLINE_PAYLOAD) return 5;
  if (request.entry.payload_len != 0U) return 6;

  memset(&request, 0xff, sizeof(request));
  zz9k_request_map_framebuffer_surface(&request);
  if (request.entry.opcode != ZZ9K_OP_MAP_FRAMEBUFFER_SURFACE) return 7;
  if (request.entry.flags != ZZ9K_ENTRY_INLINE_PAYLOAD) return 8;
  if (request.entry.payload_len != 0U) return 9;

  return 0;
}

static int test_crypto_kx_builder_encodes_descriptor(void)
{
  ZZ9KCryptoKxDesc desc;
  ZZ9KRequest request;
  const struct ZZ9KCryptoKxPayload *payload;

  if (!zz9k_crypto_build_x25519_desc(&desc, 0xAAU, 0x10U, 0xBBU, 0x20U,
                                     0xCCU, 0x30U))
    return 1;
  zz9k_request_crypto_kx(&request, &desc);
  if (request.entry.opcode != ZZ9K_OP_CRYPTO_KX) return 2;
  if (request.entry.payload_len != sizeof(struct ZZ9KCryptoKxPayload)) {
    return 3;
  }
  payload =
      (const struct ZZ9KCryptoKxPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(payload->scalar_handle) != 0xAAU) return 4;
  if (zz9k_get_be32(payload->scalar_offset) != 0x10U) return 5;
  if (zz9k_get_be32(payload->point_handle)  != 0xBBU) return 6;
  if (zz9k_get_be32(payload->point_offset)  != 0x20U) return 7;
  if (zz9k_get_be32(payload->dst_handle)    != 0xCCU) return 8;
  if (zz9k_get_be32(payload->dst_offset)    != 0x30U) return 9;
  if (zz9k_get_be32(payload->algorithm) != ZZ9K_CRYPTO_KX_X25519) return 10;
  if (zz9k_get_be32(payload->flags) != 0U) return 11;

  /* P-256 reuses the same payload with the P-256 algorithm id. */
  if (!zz9k_crypto_build_p256_desc(&desc, 0x11U, 0U, 0x22U, 0U, 0x33U, 0U))
    return 12;
  if (zz9k_request_crypto_kx(&request, &desc) != ZZ9K_STATUS_OK) return 13;
  payload =
      (const struct ZZ9KCryptoKxPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(payload->algorithm) != ZZ9K_CRYPTO_KX_P256) return 14;
  return 0;
}

/* P-256 ephemeral keygen (scalar*G) has no peer point, so its descriptor
 * carries point_handle == ZZ9K_INVALID_HANDLE by design. The wire builder must
 * accept it: every P-256 ECDHE key share depends on it. (Regression guard —
 * the builder used to require a point_handle for *all* KX ops and rejected
 * keygen client-side with BAD_REQUEST, so it never reached the board and every
 * P-256 handshake failed with "provider keymgmt failure".) */
static int test_crypto_kx_builder_allows_p256_keygen(void)
{
  ZZ9KCryptoKxDesc desc;
  ZZ9KRequest request;
  const struct ZZ9KCryptoKxPayload *payload;

  if (!zz9k_crypto_build_p256_keygen_desc(&desc, 0x11U, 0x10U, 0x33U, 0x30U))
    return 1;
  if (zz9k_request_crypto_kx(&request, &desc) != ZZ9K_STATUS_OK) return 2;
  if (request.entry.opcode != ZZ9K_OP_CRYPTO_KX) return 3;
  if (request.entry.payload_len != sizeof(struct ZZ9KCryptoKxPayload)) return 4;
  payload =
      (const struct ZZ9KCryptoKxPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(payload->scalar_handle) != 0x11U) return 5;
  if (zz9k_get_be32(payload->scalar_offset) != 0x10U) return 6;
  if (zz9k_get_be32(payload->point_handle)  != ZZ9K_INVALID_HANDLE) return 7;
  if (zz9k_get_be32(payload->dst_handle)    != 0x33U) return 8;
  if (zz9k_get_be32(payload->dst_offset)    != 0x30U) return 9;
  if (zz9k_get_be32(payload->algorithm) != ZZ9K_CRYPTO_KX_P256) return 10;
  if (zz9k_get_be32(payload->flags) != ZZ9K_CRYPTO_KX_FLAG_KEYGEN) return 11;

  /* Keygen still needs a scalar and a destination buffer. */
  if (!zz9k_crypto_build_p256_keygen_desc(&desc, 0x11U, 0U, 0x33U, 0U))
    return 12;
  desc.scalar_handle = ZZ9K_INVALID_HANDLE;
  if (zz9k_request_crypto_kx(&request, &desc) != ZZ9K_STATUS_BAD_REQUEST)
    return 13;
  if (!zz9k_crypto_build_p256_keygen_desc(&desc, 0x11U, 0U, 0x33U, 0U))
    return 14;
  desc.dst_handle = ZZ9K_INVALID_HANDLE;
  if (zz9k_request_crypto_kx(&request, &desc) != ZZ9K_STATUS_BAD_REQUEST)
    return 15;

  /* The relaxation is keygen-specific: a non-keygen KX op (flags == 0) with a
   * missing peer point is still a bad request. */
  if (!zz9k_crypto_build_p256_desc(&desc, 0x11U, 0U, 0x22U, 0U, 0x33U, 0U))
    return 16;
  desc.point_handle = ZZ9K_INVALID_HANDLE;   /* flags stays 0 (derive) */
  if (zz9k_request_crypto_kx(&request, &desc) != ZZ9K_STATUS_BAD_REQUEST)
    return 17;
  return 0;
}

static int test_crypto_verify_builder_encodes_descriptor(void)
{
  ZZ9KCryptoVerifyDesc desc;
  ZZ9KRequest request;
  const ZZ9KCryptoVerifyPayload *payload;

  if (!zz9k_crypto_build_verify_desc(&desc,
                                     ZZ9K_CRYPTO_VERIFY_ECDSA_P256_SHA256,
                                     0xA1U, 0x01U, 32U,
                                     0xB2U, 0x02U, 64U,
                                     0xC3U, 0x03U, 65U))
    return 1;
  if (zz9k_request_crypto_verify(&request, &desc) != ZZ9K_STATUS_OK) return 2;
  if (request.entry.opcode != ZZ9K_OP_CRYPTO_VERIFY) return 3;
  if (request.entry.payload_len != sizeof(ZZ9KCryptoVerifyPayload)) return 4;
  payload =
      (const ZZ9KCryptoVerifyPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(payload->algorithm) !=
      ZZ9K_CRYPTO_VERIFY_ECDSA_P256_SHA256) return 5;
  if (zz9k_get_be32(payload->hash_handle) != 0xA1U) return 6;
  if (zz9k_get_be32(payload->hash_offset) != 0x01U) return 7;
  if (zz9k_get_be32(payload->hash_length) != 32U) return 8;
  if (zz9k_get_be32(payload->sig_handle) != 0xB2U) return 9;
  if (zz9k_get_be32(payload->sig_offset) != 0x02U) return 10;
  if (zz9k_get_be32(payload->sig_length) != 64U) return 11;
  if (zz9k_get_be32(payload->key_handle) != 0xC3U) return 12;
  if (zz9k_get_be32(payload->key_offset) != 0x03U) return 13;
  if (zz9k_get_be32(payload->key_length) != 65U) return 14;

  /* A descriptor missing the public key must not produce a request. */
  if (!zz9k_crypto_build_verify_desc(&desc,
                                     ZZ9K_CRYPTO_VERIFY_ECDSA_P256_SHA256,
                                     0xA1U, 0U, 32U, 0xB2U, 0U, 64U,
                                     0xC3U, 0U, 65U))
    return 15;
  desc.key_handle = ZZ9K_INVALID_HANDLE;
  if (zz9k_request_crypto_verify(&request, &desc) == ZZ9K_STATUS_OK) return 16;
  return 0;
}

static int test_decompress_batch_builder_encodes_arena(void)
{
  ZZ9KRequest request;
  const ZZ9KDecompressBatchPayload *payload;
  ZZ9KDecompressBatchDesc desc;

  desc.arena_handle = 0x1234U;
  desc.arena_offset = 0x10U;
  desc.arena_length = 0x20000U;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_decompress_batch(&request, &desc) != ZZ9K_STATUS_OK) {
    return 1;
  }
  if (request.entry.opcode != ZZ9K_OP_DECOMPRESS_BATCH) return 2;
  if (request.entry.flags != ZZ9K_ENTRY_INLINE_PAYLOAD) return 3;
  if (request.entry.payload_len != sizeof(ZZ9KDecompressBatchPayload)) {
    return 4;
  }
  payload =
      (const ZZ9KDecompressBatchPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(payload->arena_handle) != 0x1234U) return 5;
  if (zz9k_get_be32(payload->arena_offset) != 0x10U) return 6;
  if (zz9k_get_be32(payload->arena_length) != 0x20000U) return 7;

  if (zz9k_request_decompress_batch(&request, 0) != ZZ9K_STATUS_BAD_REQUEST) {
    return 8;
  }
  desc.arena_handle = ZZ9K_INVALID_HANDLE;
  if (zz9k_request_decompress_batch(&request, &desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 9;
  }
  desc.arena_handle = 0x1234U;
  desc.arena_length = ZZ9K_BATCH_HEADER_SIZE - 1U;
  if (zz9k_request_decompress_batch(&request, &desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 10;
  }
  return 0;
}

/* KTD9: the palette query is a bulk-reply op, so the builder is the only place
 * bounds are enforced before firmware sees a request. Cover the first, middle
 * and last entries and every rejection the descriptor allows. */
static int test_query_palette_builder_encodes_descriptor(void)
{
  ZZ9KPaletteQueryDesc desc;
  ZZ9KRequest request;
  const ZZ9KQueryPalettePayload *payload;

  memset(&desc, 0, sizeof(desc));
  desc.surface = ZZ9K_SURFACE_HANDLE_FRAMEBUFFER;
  desc.start = 128U;
  desc.count = 1U;
  desc.dst_handle = 0x40000007UL;
  desc.dst_offset = 0x120U;
  desc.flags = 0U;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_query_palette(&request, &desc) != ZZ9K_STATUS_OK) {
    return 1;
  }
  if (request.entry.opcode != ZZ9K_OP_QUERY_PALETTE) return 2;
  if (request.entry.payload_len != sizeof(ZZ9KQueryPalettePayload)) return 3;
  payload =
      (const ZZ9KQueryPalettePayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(payload->surface) != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) {
    return 4;
  }
  if (zz9k_get_be32(payload->start) != 128U) return 5;
  if (zz9k_get_be32(payload->count) != 1U) return 6;
  if (zz9k_get_be32(payload->dst_handle) != 0x40000007UL) return 7;
  if (zz9k_get_be32(payload->dst_offset) != 0x120U) return 8;
  if (zz9k_get_be32(payload->flags) != 0U) return 9;

  /* Entry 0 and the whole table. */
  desc.start = 0U;
  desc.count = (uint32_t)ZZ9K_PALETTE_MAX_ENTRIES;
  if (zz9k_request_query_palette(&request, &desc) != ZZ9K_STATUS_OK) {
    return 10;
  }

  /* The last entry alone: start + count lands exactly on the bound. */
  desc.start = (uint32_t)ZZ9K_PALETTE_MAX_ENTRIES - 1U;
  desc.count = 1U;
  if (zz9k_request_query_palette(&request, &desc) != ZZ9K_STATUS_OK) {
    return 11;
  }

  /* One past the end. */
  desc.start = (uint32_t)ZZ9K_PALETTE_MAX_ENTRIES;
  desc.count = 1U;
  if (zz9k_request_query_palette(&request, &desc) != ZZ9K_STATUS_BAD_REQUEST) {
    return 12;
  }

  /* A range that starts inside the table but runs past its end. */
  desc.start = 200U;
  desc.count = 100U;
  if (zz9k_request_query_palette(&request, &desc) != ZZ9K_STATUS_BAD_REQUEST) {
    return 13;
  }

  /* start + count must not be allowed to wrap past the bound check. */
  desc.start = 0xffffffffUL;
  desc.count = 2U;
  if (zz9k_request_query_palette(&request, &desc) != ZZ9K_STATUS_BAD_REQUEST) {
    return 14;
  }

  desc.start = 0U;
  desc.count = 0U;
  if (zz9k_request_query_palette(&request, &desc) != ZZ9K_STATUS_BAD_REQUEST) {
    return 15;
  }

  desc.count = (uint32_t)ZZ9K_PALETTE_MAX_ENTRIES + 1U;
  if (zz9k_request_query_palette(&request, &desc) != ZZ9K_STATUS_BAD_REQUEST) {
    return 16;
  }

  desc.count = 16U;
  desc.dst_handle = ZZ9K_INVALID_HANDLE;
  if (zz9k_request_query_palette(&request, &desc) != ZZ9K_STATUS_BAD_REQUEST) {
    return 17;
  }

  desc.dst_handle = 0x40000007UL;
  desc.flags = 0x80000000UL;
  if (zz9k_request_query_palette(&request, &desc) != ZZ9K_STATUS_BAD_REQUEST) {
    return 18;
  }

  /* The secondary bit is a defined flag: the builder passes it through and
   * firmware is what answers UNSUPPORTED. */
  desc.flags = (uint32_t)ZZ9K_PALETTE_QUERY_FLAG_SECONDARY;
  if (zz9k_request_query_palette(&request, &desc) != ZZ9K_STATUS_OK) {
    return 19;
  }

  if (zz9k_request_query_palette(NULL, &desc) != ZZ9K_STATUS_BAD_REQUEST) {
    return 20;
  }
  if (zz9k_request_query_palette(&request, NULL) != ZZ9K_STATUS_BAD_REQUEST) {
    return 21;
  }
  return 0;
}

int main(void)
{
  int result;

  result = test_ping_builder_clears_and_copies_inline_payload();
  if (result) return 10 + result;

  result = test_query_service_builder_uses_big_endian_payload();
  if (result) return 30 + result;

  result = test_memory_builders_encode_handles_and_lengths();
  if (result) return 50 + result;

  result = test_surface_and_scale_builders_encode_descriptors();
  if (result) return 90 + result;

  result = test_image_decode_builders_encode_descriptors();
  if (result) return 120 + result;

  result = test_crypto_hash_builder_encodes_descriptor();
  if (result) return 150 + result;

  result = test_audio_decode_builder_encodes_descriptor();
  if (result) return 160 + result;

  result = test_audio_stream_builders_encode_descriptors();
  if (result) return 165 + result;

  result = test_crypto_stream_builder_encodes_descriptor();
  if (result) return 170 + result;

  result = test_crypto_aead_builder_encodes_descriptor();
  if (result) return 180 + result;

  result = test_decompress_builder_encodes_descriptor();
  if (result) return 185 + result;

  result = test_no_payload_builders();
  if (result) return 190 + result;

  result = test_crypto_kx_builder_encodes_descriptor();
  if (result) return 200 + result;

  result = test_crypto_kx_builder_allows_p256_keygen();
  if (result) return 260 + result;

  result = test_crypto_verify_builder_encodes_descriptor();
  if (result) return 220 + result;

  result = test_crypto_aes_gcm_builder_encodes_descriptor();
  if (result) return 240 + result;

  result = test_decompress_batch_builder_encodes_arena();
  if (result) return 270 + result;

  result = test_query_palette_builder_encodes_descriptor();
  if (result) return 290 + result;

  return 0;
}
