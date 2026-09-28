/*
 * Streaming image session reply decoder checks.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/reply.h"
#include <string.h>

static void fill_session_result_reply(ZZ9KMailboxEntry *reply)
{
  memset(reply, 0, sizeof(*reply));
  reply->opcode = ZZ9K_OP_IMAGE_SESSION_FEED;
  reply->status = ZZ9K_STATUS_OK;
  reply->payload_len = sizeof(ZZ9KImageSessionResultPayload);
  zz9k_put_be32(&reply->payload.inline_data[0], 3U);
  zz9k_put_be32(&reply->payload.inline_data[4],
                ZZ9K_IMAGE_SESSION_STATE_TILE_READY);
  zz9k_put_be32(&reply->payload.inline_data[8], 640U);
  zz9k_put_be32(&reply->payload.inline_data[12], 480U);
  zz9k_put_be32(&reply->payload.inline_data[16],
                ZZ9K_SURFACE_FORMAT_BGRA8888);
  zz9k_put_be32(&reply->payload.inline_data[20], 0U);
  zz9k_put_be32(&reply->payload.inline_data[24], 64U);
  zz9k_put_be32(&reply->payload.inline_data[28], 640U);
  zz9k_put_be32(&reply->payload.inline_data[32], 32U);
  zz9k_put_be32(&reply->payload.inline_data[36], 4096U);
  zz9k_put_be32(&reply->payload.inline_data[40], 81920U);
  zz9k_put_be32(&reply->payload.inline_data[44],
                ZZ9K_IMAGE_SESSION_RESULT_HEADER_READY |
                ZZ9K_IMAGE_SESSION_RESULT_PARTIAL);
}

static int test_session_result_decoder_reads_big_endian_payload(void)
{
  ZZ9KMailboxEntry reply;
  ZZ9KImageSessionResult result;

  fill_session_result_reply(&reply);
  memset(&result, 0xff, sizeof(result));
  if (zz9k_reply_image_session_result(&reply, ZZ9K_OP_IMAGE_SESSION_FEED,
                                      &result) != ZZ9K_STATUS_OK) {
    return 1;
  }

  if (result.session != 3U) return 2;
  if (result.state != ZZ9K_IMAGE_SESSION_STATE_TILE_READY) return 3;
  if (result.image_width != 640U) return 4;
  if (result.image_height != 480U) return 5;
  if (result.output_format != ZZ9K_SURFACE_FORMAT_BGRA8888) return 6;
  if (result.tile_x != 0U) return 7;
  if (result.tile_y != 64U) return 8;
  if (result.tile_width != 640U) return 9;
  if (result.tile_height != 32U) return 10;
  if (result.bytes_consumed != 4096U) return 11;
  if (result.bytes_written != 81920U) return 12;
  if (result.flags != (ZZ9K_IMAGE_SESSION_RESULT_HEADER_READY |
                       ZZ9K_IMAGE_SESSION_RESULT_PARTIAL)) {
    return 13;
  }

  return 0;
}

static int test_session_result_decoder_rejects_invalid_reply(void)
{
  ZZ9KMailboxEntry reply;
  ZZ9KImageSessionResult result;

  fill_session_result_reply(&reply);
  if (zz9k_reply_image_session_result(&reply, ZZ9K_OP_IMAGE_SESSION_BEGIN,
                                      &result) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 1;
  }

  fill_session_result_reply(&reply);
  zz9k_put_be32(&reply.payload.inline_data[0], 0U);
  if (zz9k_reply_image_session_result(&reply, ZZ9K_OP_IMAGE_SESSION_FEED,
                                      &result) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 2;
  }
  if (result.session != 0U || result.state != 0U) return 3;

  fill_session_result_reply(&reply);
  zz9k_put_be32(&reply.payload.inline_data[4], 99U);
  if (zz9k_reply_image_session_result(&reply, ZZ9K_OP_IMAGE_SESSION_FEED,
                                      &result) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 4;
  }

  return 0;
}

int main(void)
{
  int result;

  result = test_session_result_decoder_reads_big_endian_payload();
  if (result) return 10 + result;

  result = test_session_result_decoder_rejects_invalid_reply();
  if (result) return 40 + result;

  return 0;
}
