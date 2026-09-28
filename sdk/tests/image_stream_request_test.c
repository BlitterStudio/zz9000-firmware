/*
 * Streaming image session request builder checks.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/request.h"
#include <string.h>

static int test_begin_builder_encodes_descriptor(void)
{
  ZZ9KRequest request;
  ZZ9KImageSessionBeginDesc desc;
  const ZZ9KImageSessionBeginPayload *payload;

  memset(&desc, 0, sizeof(desc));
  desc.codec = ZZ9K_IMAGE_CODEC_JPEG;
  desc.output_mode = ZZ9K_IMAGE_OUTPUT_TILE_BUFFER;
  desc.dst_surface = ZZ9K_INVALID_HANDLE;
  desc.dst_x = 10U;
  desc.dst_y = 20U;
  desc.dst_width = 320U;
  desc.dst_height = 200U;
  desc.output_format = ZZ9K_SURFACE_FORMAT_BGRA8888;
  desc.tile_handle = 0x40000044UL;
  desc.tile_stride = 1280U;
  desc.tile_rows = 32U;
  desc.flags = ZZ9K_IMAGE_DECODE_FLAG_FIT;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_image_session_begin(&request, &desc) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (request.entry.opcode != ZZ9K_OP_IMAGE_SESSION_BEGIN) return 2;
  if (request.entry.flags != ZZ9K_ENTRY_INLINE_PAYLOAD) return 3;
  if (request.entry.payload_len != sizeof(ZZ9KImageSessionBeginPayload)) {
    return 4;
  }

  payload =
      (const ZZ9KImageSessionBeginPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(payload->codec) != ZZ9K_IMAGE_CODEC_JPEG) return 5;
  if (zz9k_get_be32(payload->output_mode) !=
      ZZ9K_IMAGE_OUTPUT_TILE_BUFFER) {
    return 6;
  }
  if (zz9k_get_be32(payload->dst_surface) != ZZ9K_INVALID_HANDLE) return 7;
  if (zz9k_get_be32(payload->dst_x) != 10U) return 8;
  if (zz9k_get_be32(payload->dst_y) != 20U) return 9;
  if (zz9k_get_be32(payload->dst_width) != 320U) return 10;
  if (zz9k_get_be32(payload->dst_height) != 200U) return 11;
  if (zz9k_get_be32(payload->output_format) !=
      ZZ9K_SURFACE_FORMAT_BGRA8888) {
    return 12;
  }
  if (zz9k_get_be32(payload->tile_handle) != 0x40000044UL) return 13;
  if (zz9k_get_be32(payload->tile_stride) != 1280U) return 14;
  if (zz9k_get_be32(payload->tile_rows) != 32U) return 15;
  if (zz9k_get_be32(payload->flags) != ZZ9K_IMAGE_DECODE_FLAG_FIT) {
    return 16;
  }

  desc.tile_rows = 0U;
  if (zz9k_request_image_session_begin(&request, &desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 17;
  }

  return 0;
}

static int test_begin_builder_validates_direct_output(void)
{
  ZZ9KRequest request;
  ZZ9KImageSessionBeginDesc desc;
  const ZZ9KImageSessionBeginPayload *payload;

  memset(&desc, 0, sizeof(desc));
  desc.codec = ZZ9K_IMAGE_CODEC_JPEG;
  desc.output_mode = ZZ9K_IMAGE_OUTPUT_FRAMEBUFFER;
  desc.dst_surface = ZZ9K_SURFACE_HANDLE_FRAMEBUFFER;
  desc.dst_width = 640U;
  desc.dst_height = 480U;
  desc.output_format = ZZ9K_SURFACE_FORMAT_BGRA8888;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_image_session_begin(&request, &desc) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  payload =
      (const ZZ9KImageSessionBeginPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(payload->output_mode) !=
      ZZ9K_IMAGE_OUTPUT_FRAMEBUFFER) {
    return 2;
  }
  if (zz9k_get_be32(payload->dst_surface) !=
      ZZ9K_SURFACE_HANDLE_FRAMEBUFFER) {
    return 3;
  }

  desc.dst_width = 0U;
  if (zz9k_request_image_session_begin(&request, &desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 4;
  }

  desc.dst_width = 640U;
  desc.output_mode = 0U;
  if (zz9k_request_image_session_begin(&request, &desc) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 5;
  }

  return 0;
}

static int test_feed_and_close_builders_encode_payloads(void)
{
  ZZ9KRequest request;
  ZZ9KImageSessionFeedDesc feed;
  const ZZ9KImageSessionFeedPayload *feed_payload;
  const ZZ9KImageSessionClosePayload *close_payload;

  memset(&feed, 0, sizeof(feed));
  feed.session = 7U;
  feed.src_handle = 0x40000055UL;
  feed.src_offset = 64U;
  feed.src_length = 4096U;
  feed.flags = ZZ9K_IMAGE_SESSION_FEED_EOF;

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_image_session_feed(&request, &feed) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }

  if (request.entry.opcode != ZZ9K_OP_IMAGE_SESSION_FEED) return 2;
  if (request.entry.payload_len != sizeof(ZZ9KImageSessionFeedPayload)) {
    return 3;
  }
  feed_payload =
      (const ZZ9KImageSessionFeedPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(feed_payload->session) != 7U) return 4;
  if (zz9k_get_be32(feed_payload->src_handle) != 0x40000055UL) return 5;
  if (zz9k_get_be32(feed_payload->src_offset) != 64U) return 6;
  if (zz9k_get_be32(feed_payload->src_length) != 4096U) return 7;
  if (zz9k_get_be32(feed_payload->flags) !=
      ZZ9K_IMAGE_SESSION_FEED_EOF) {
    return 8;
  }

  feed.src_length = 0U;
  feed.flags = 0U;
  if (zz9k_request_image_session_feed(&request, &feed) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 9;
  }

  feed.flags = ZZ9K_IMAGE_SESSION_FEED_EOF;
  if (zz9k_request_image_session_feed(&request, &feed) !=
      ZZ9K_STATUS_OK) {
    return 10;
  }

  memset(&request, 0xff, sizeof(request));
  if (zz9k_request_image_session_close(&request, 7U, 0x12U) !=
      ZZ9K_STATUS_OK) {
    return 11;
  }

  if (request.entry.opcode != ZZ9K_OP_IMAGE_SESSION_CLOSE) return 12;
  if (request.entry.payload_len != sizeof(ZZ9KImageSessionClosePayload)) {
    return 13;
  }
  close_payload =
      (const ZZ9KImageSessionClosePayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(close_payload->session) != 7U) return 14;
  if (zz9k_get_be32(close_payload->flags) != 0x12U) return 15;

  if (zz9k_request_image_session_close(&request, 0U, 0U) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 16;
  }

  return 0;
}

int main(void)
{
  int result;

  result = test_begin_builder_encodes_descriptor();
  if (result) return 10 + result;

  result = test_begin_builder_validates_direct_output();
  if (result) return 40 + result;

  result = test_feed_and_close_builders_encode_payloads();
  if (result) return 70 + result;

  return 0;
}
