/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zz9k/reply.h"
#include "zz9k/request.h"

#include <stdint.h>
#include <string.h>

static int test_layout_and_constants(void)
{
  if (ZZ9K_SERVICE_VIDEO != 0x0b00U) return 1;
  if (ZZ9K_OP_VIDEO_SESSION_BEGIN != 0x0b00U) return 2;
  if (ZZ9K_OP_VIDEO_SESSION_WRITE != 0x0b01U) return 3;
  if (ZZ9K_OP_VIDEO_SESSION_DECODE != 0x0b02U) return 4;
  if (ZZ9K_OP_VIDEO_SESSION_CLOSE != 0x0b03U) return 5;
  if (ZZ9K_CAP_VIDEO_DECODE != (1U << 21)) return 6;
  if (sizeof(ZZ9KVideoSessionBeginPayload) != 48U) return 7;
  if (sizeof(ZZ9KVideoSessionWritePayload) != 48U) return 8;
  if (sizeof(ZZ9KVideoSessionDecodePayload) != 48U) return 9;
  if (sizeof(ZZ9KVideoSessionClosePayload) != 48U) return 10;
  if (sizeof(ZZ9KVideoSessionResultPayload) != 48U) return 11;
  return 0;
}

static int test_request_builders(void)
{
  ZZ9KVideoSessionBeginDesc begin;
  ZZ9KVideoSessionWriteDesc write;
  ZZ9KVideoSessionDecodeDesc decode;
  ZZ9KRequest request;
  const ZZ9KVideoSessionBeginPayload *bp;
  const ZZ9KVideoSessionWritePayload *wp;
  const ZZ9KVideoSessionDecodePayload *dp;

  memset(&begin, 0, sizeof(begin));
  begin.codec = ZZ9K_VIDEO_CODEC_MPEG1;
  begin.container = ZZ9K_VIDEO_CONTAINER_MPEG_PS;
  begin.width = 319U;
  begin.height = 240U;
  begin.output_format = ZZ9K_VIDEO_OUTPUT_DIRECT_OVERLAY;
  if (zz9k_request_video_session_begin(&request, &begin) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }
  if (request.entry.opcode != ZZ9K_OP_VIDEO_SESSION_BEGIN ||
      request.entry.payload_len != sizeof(*bp)) {
    return 2;
  }
  bp = (const ZZ9KVideoSessionBeginPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(bp->width) != 319U ||
      zz9k_get_be32(bp->height) != 240U) {
    return 3;
  }
  begin.width = 0U;
  if (zz9k_request_video_session_begin(&request, &begin) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 4;
  }

  memset(&write, 0, sizeof(write));
  write.session = 7U;
  write.src_handle = 0x40000021UL;
  write.src_offset = 32U;
  write.src_length = 65536U;
  write.flags = ZZ9K_VIDEO_SESSION_WRITE_EOF;
  if (zz9k_request_video_session_write(&request, &write) !=
      ZZ9K_STATUS_OK) {
    return 5;
  }
  wp = (const ZZ9KVideoSessionWritePayload *)request.entry.payload.inline_data;
  if (request.entry.opcode != ZZ9K_OP_VIDEO_SESSION_WRITE ||
      zz9k_get_be32(wp->session) != 7U ||
      zz9k_get_be32(wp->src_length) != 65536U ||
      zz9k_get_be32(wp->flags) != ZZ9K_VIDEO_SESSION_WRITE_EOF) {
    return 6;
  }
  memset(&decode, 0, sizeof(decode));
  decode.session = 7U;
  if (zz9k_request_video_session_decode(&request, &decode) !=
      ZZ9K_STATUS_OK || request.entry.opcode != ZZ9K_OP_VIDEO_SESSION_DECODE) {
    return 7;
  }
  dp = (const ZZ9KVideoSessionDecodePayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(dp->session) != 7U ||
      zz9k_get_be32(dp->flags) != 0U) {
    return 8;
  }
  if (zz9k_request_video_session_close(&request, 7U, 0U) !=
      ZZ9K_STATUS_OK || request.entry.opcode != ZZ9K_OP_VIDEO_SESSION_CLOSE) {
    return 9;
  }
  return 0;
}

static int test_reply_decoder(void)
{
  ZZ9KMailboxEntry reply;
  ZZ9KVideoSessionResult result;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_VIDEO_SESSION_DECODE;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = sizeof(ZZ9KVideoSessionResultPayload);
  zz9k_put_be32(&reply.payload.inline_data[0], 9U);
  zz9k_put_be32(&reply.payload.inline_data[4],
                ZZ9K_VIDEO_SESSION_STATE_FRAME_READY);
  zz9k_put_be32(&reply.payload.inline_data[8], 318U);
  zz9k_put_be32(&reply.payload.inline_data[12], 240U);
  zz9k_put_be32(&reply.payload.inline_data[16], 25000U);
  zz9k_put_be32(&reply.payload.inline_data[20], 3U);
  zz9k_put_be32(&reply.payload.inline_data[24], 80U);
  zz9k_put_be32(&reply.payload.inline_data[28], 0U);
  zz9k_put_be32(&reply.payload.inline_data[32], 0U);
  zz9k_put_be32(&reply.payload.inline_data[36],
                ZZ9K_VIDEO_SESSION_RESULT_HEADER_READY |
                ZZ9K_VIDEO_SESSION_RESULT_FRAME_READY);

  if (zz9k_reply_video_session_result(
          &reply, ZZ9K_OP_VIDEO_SESSION_DECODE, &result) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }
  if (result.session != 9U || result.width != 318U ||
      result.height != 240U || result.frame_rate_milli != 25000U ||
      result.frame_number != 3U || result.frame_time_millis != 80U ||
      result.bytes_written != 0U) {
    return 2;
  }
  reply.opcode = ZZ9K_OP_AUDIO_STREAM_FEED;
  if (zz9k_reply_video_session_result(
          &reply, ZZ9K_OP_VIDEO_SESSION_DECODE, &result) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 3;
  }
  return 0;
}

int main(void)
{
  int status;

  status = test_layout_and_constants();
  if (status) return 10 + status;
  status = test_request_builders();
  if (status) return 50 + status;
  status = test_reply_decoder();
  if (status) return 70 + status;
  return 0;
}
