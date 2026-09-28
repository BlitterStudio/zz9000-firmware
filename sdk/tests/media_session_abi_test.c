/*
 * Additive media-session ABI contract.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>

#include "zz9k/abi.h"
#include "zz9k/reply.h"
#include "zz9k/request.h"

static int test_constants_and_layout(void)
{
  if (ZZ9K_OP_MEDIA_SESSION_BEGIN != 0x0b04U) return 1;
  if (ZZ9K_OP_MEDIA_SESSION_WRITE != 0x0b05U) return 2;
  if (ZZ9K_OP_MEDIA_SESSION_DECODE != 0x0b06U) return 3;
  if (ZZ9K_OP_MEDIA_SESSION_AUDIO_READ != 0x0b07U) return 4;
  if (ZZ9K_OP_MEDIA_SESSION_PRESENT != 0x0b08U) return 5;
  if (ZZ9K_OP_MEDIA_SESSION_DISCARD != 0x0b09U) return 6;
  if (ZZ9K_OP_MEDIA_SESSION_STATUS != 0x0b0aU) return 7;
  if (ZZ9K_OP_MEDIA_SESSION_AUDIO_BIND != 0x0b0bU) return 8;
  if (ZZ9K_OP_MEDIA_SESSION_AUDIO_UNBIND != 0x0b0cU) return 9;
  if (ZZ9K_OP_MEDIA_SESSION_CLOSE != 0x0b0dU) return 10;
  if (ZZ9K_CAP_MEDIA_SESSION != (1U << 22)) return 11;
  if (sizeof(ZZ9KMediaSessionBeginPayload) != 48U) return 12;
  if (sizeof(ZZ9KMediaSessionWritePayload) != 48U) return 13;
  if (sizeof(ZZ9KMediaSessionCommandPayload) != 48U) return 14;
  if (sizeof(ZZ9KMediaSessionStatusPayload) != 48U) return 15;
  if (sizeof(ZZ9KMediaSessionMainResultPayload) != 48U) return 16;
  if (sizeof(ZZ9KMediaSessionAudioResultPayload) != 48U) return 17;
  if (sizeof(ZZ9KMediaSessionStatusResultPayload) != 48U) return 18;
  if (ZZ9K_MEDIA_NO_PTS != UINT64_MAX) return 19;
  if (ZZ9K_MEDIA_AUDIO_BIND_PAUSE != (1U << 0)) return 20;
  if (ZZ9K_MEDIA_SESSION_RESULT_AUDIO_BOUND != (1U << 11)) return 21;
  if (ZZ9K_MEDIA_SESSION_RESULT_AUDIO_PLAYING != (1U << 12)) return 22;
  if (ZZ9K_MEDIA_SESSION_RESULT_AUDIO_DRAINED != (1U << 13)) return 23;
  if (ZZ9K_MEDIA_SESSION_RESULT_AUDIO_UNDERRUN != (1U << 14)) return 24;
  if (ZZ9K_MEDIA_STATUS_AUDIO_OUTPUT != 3U) return 25;
  if (ZZ9K_MEDIA_STATUS_PRESENTATION != 4U) return 26;
  if (ZZ9K_MEDIA_STATUS_PROFILE != 5U) return 34;
  if (ZZ9K_MEDIA_PROFILE_STAGES != 4) return 35;
  /* The packed (microseconds << 32) | calls encoding must round-trip. */
  if (ZZ9K_MEDIA_PROFILE_US(((uint64_t)1234U << 32) | 56U) != 1234U)
    return 36;
  if (ZZ9K_MEDIA_PROFILE_CALLS(((uint64_t)1234U << 32) | 56U) != 56U)
    return 37;
  /* A full 32-bit microsecond total must not be truncated or sign-extended. */
  if (ZZ9K_MEDIA_PROFILE_US(((uint64_t)0xffffffffU << 32) | 1U) !=
      0xffffffffU)
    return 38;
  if (ZZ9K_MEDIA_PRESENT_CONFIGURED != (1U << 0)) return 27;
  if (ZZ9K_MEDIA_PRESENT_ACTIVE != (1U << 1)) return 28;
  if (ZZ9K_MEDIA_PRESENT_NATIVE != (1U << 2)) return 29;
  if (ZZ9K_MEDIA_PRESENT_OWNED != (1U << 3)) return 30;
  if (ZZ9K_MEDIA_PACK_PAIR(320U, 240U) != ((320ULL << 16) | 240ULL))
    return 31;
  if (ZZ9K_MEDIA_PAIR_HI(ZZ9K_MEDIA_PACK_PAIR(320U, 240U)) != 320U ||
      ZZ9K_MEDIA_PAIR_LO(ZZ9K_MEDIA_PACK_PAIR(320U, 240U)) != 240U)
    return 32;
  /* Signed destination origins must round-trip through the packing. */
  if (ZZ9K_MEDIA_PAIR_HI_S(ZZ9K_MEDIA_PACK_PAIR(-16, -9)) != -16 ||
      ZZ9K_MEDIA_PAIR_LO_S(ZZ9K_MEDIA_PACK_PAIR(-16, -9)) != -9)
    return 33;
  return 0;
}

static int test_timestamp_contract(void)
{
  ZZ9KMediaClock clock;
  uint64_t wrapped;
  uint32_t i;

  memset(&clock, 0, sizeof(clock));
  if (zz9k_media_clock_advance(&clock, 1U, 25U) != 3600U) return 1;
  memset(&clock, 0, sizeof(clock));
  if (zz9k_media_clock_advance(&clock, 1001U, 30000U) != 3003U) return 2;

  memset(&clock, 0, sizeof(clock));
  for (i = 0U; i < 441U; i++) {
    if (zz9k_media_clock_advance(&clock, 1U, 44100U) ==
        ZZ9K_MEDIA_NO_PTS)
      return 3;
  }
  if (clock.ticks != 900U || clock.remainder != 0U) return 4;
  memset(&clock, 0, sizeof(clock));
  for (i = 0U; i < 480U; i++) {
    if (zz9k_media_clock_advance(&clock, 1U, 48000U) ==
        ZZ9K_MEDIA_NO_PTS)
      return 5;
  }
  if (clock.ticks != 900U || clock.remainder != 0U) return 6;

  wrapped = zz9k_media_pts_unwrap((1ULL << 33) - 2ULL, 1ULL);
  if (wrapped != (1ULL << 33) + 1ULL) return 7;
  wrapped = zz9k_media_pts_unwrap(ZZ9K_MEDIA_NO_PTS,
                                  (1ULL << 33) + 7ULL);
  if (wrapped != 7ULL) return 8;
  return 0;
}

static int test_request_builders(void)
{
  ZZ9KRequest request;
  ZZ9KMediaSessionBeginDesc begin;
  ZZ9KMediaSessionWriteDesc write;
  const ZZ9KMediaSessionBeginPayload *bp;
  const ZZ9KMediaSessionWritePayload *wp;
  static const uint16_t command_opcodes[] = {
    ZZ9K_OP_MEDIA_SESSION_DECODE,
    ZZ9K_OP_MEDIA_SESSION_AUDIO_READ,
    ZZ9K_OP_MEDIA_SESSION_PRESENT,
    ZZ9K_OP_MEDIA_SESSION_DISCARD,
    ZZ9K_OP_MEDIA_SESSION_AUDIO_BIND,
    ZZ9K_OP_MEDIA_SESSION_AUDIO_UNBIND,
    ZZ9K_OP_MEDIA_SESSION_CLOSE
  };
  uint32_t i;

  memset(&begin, 0, sizeof(begin));
  begin.video_codec = ZZ9K_VIDEO_CODEC_MPEG1;
  begin.container = ZZ9K_VIDEO_CONTAINER_MPEG_PS;
  begin.width = 320U;
  begin.height = 240U;
  begin.output_format = ZZ9K_VIDEO_OUTPUT_DIRECT_OVERLAY;
  if (zz9k_request_media_session_begin(&request, &begin) != ZZ9K_STATUS_OK)
    return 1;
  if (request.entry.opcode != ZZ9K_OP_MEDIA_SESSION_BEGIN ||
      request.entry.payload_len != 48U)
    return 2;
  bp = (const ZZ9KMediaSessionBeginPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(bp->width) != 320U ||
      zz9k_get_be32(bp->audio_codec) != ZZ9K_MEDIA_AUDIO_NONE)
    return 3;

  begin.audio_codec = ZZ9K_MEDIA_AUDIO_MP2;
  begin.pcm_ring_handle = 0x40000021UL;
  begin.pcm_ring_capacity = 32768U;
  begin.pcm_low_water_bytes = 4096U;
  begin.pcm_high_water_bytes = 24576U;
  if (zz9k_request_media_session_begin(&request, &begin) != ZZ9K_STATUS_OK)
    return 4;
  bp = (const ZZ9KMediaSessionBeginPayload *)request.entry.payload.inline_data;
  if (zz9k_get_be32(bp->audio_codec) != ZZ9K_MEDIA_AUDIO_MP2 ||
      zz9k_get_be32(bp->pcm_ring_capacity) != 32768U ||
      zz9k_get_be32(bp->pcm_low_water_bytes) != 4096U ||
      zz9k_get_be32(bp->pcm_high_water_bytes) != 24576U)
    return 5;
  begin.pcm_low_water_bytes = begin.pcm_high_water_bytes;
  if (zz9k_request_media_session_begin(&request, &begin) !=
      ZZ9K_STATUS_BAD_REQUEST)
    return 6;
  begin.pcm_low_water_bytes = 4096U;
  begin.pcm_high_water_bytes = begin.pcm_ring_capacity + 1U;
  if (zz9k_request_media_session_begin(&request, &begin) !=
      ZZ9K_STATUS_BAD_REQUEST)
    return 7;
  begin.pcm_high_water_bytes = 24576U;
  begin.pcm_ring_handle = 0U;
  if (zz9k_request_media_session_begin(&request, &begin) !=
      ZZ9K_STATUS_BAD_REQUEST)
    return 8;
  begin.audio_codec = ZZ9K_MEDIA_AUDIO_NONE;
  if (zz9k_request_media_session_begin(&request, &begin) !=
      ZZ9K_STATUS_BAD_REQUEST)
    return 9;

  memset(&write, 0, sizeof(write));
  write.session = 9U;
  write.src_handle = 0x40000022UL;
  write.src_offset = 64U;
  write.src_length = 4096U;
  write.flags = ZZ9K_MEDIA_SESSION_WRITE_EOF;
  if (zz9k_request_media_session_write(&request, &write) != ZZ9K_STATUS_OK)
    return 10;
  wp = (const ZZ9KMediaSessionWritePayload *)request.entry.payload.inline_data;
  if (request.entry.opcode != ZZ9K_OP_MEDIA_SESSION_WRITE ||
      zz9k_get_be32(wp->session) != 9U ||
      zz9k_get_be32(wp->flags) != ZZ9K_MEDIA_SESSION_WRITE_EOF)
    return 11;

  if (zz9k_request_media_session_command(
          &request, ZZ9K_OP_MEDIA_SESSION_PRESENT, 9U, 0U) !=
      ZZ9K_STATUS_OK)
    return 12;
  for (i = 0U; i < sizeof(command_opcodes) / sizeof(command_opcodes[0]); i++) {
    if (zz9k_request_media_session_command(
            &request, command_opcodes[i], 9U, 0U) != ZZ9K_STATUS_OK ||
        request.entry.opcode != command_opcodes[i] ||
        request.entry.payload_len != 48U)
      return 13;
  }
  if (zz9k_request_media_session_command(
          &request, ZZ9K_OP_VIDEO_SESSION_CLOSE, 9U, 0U) !=
      ZZ9K_STATUS_BAD_REQUEST)
    return 14;
  if (zz9k_request_media_session_command(
          &request, ZZ9K_OP_MEDIA_SESSION_AUDIO_BIND, 9U,
          ZZ9K_MEDIA_AUDIO_BIND_PAUSE) != ZZ9K_STATUS_OK)
    return 15;
  if (zz9k_request_media_session_command(
          &request, ZZ9K_OP_MEDIA_SESSION_AUDIO_UNBIND, 9U,
          ZZ9K_MEDIA_AUDIO_BIND_PAUSE) != ZZ9K_STATUS_BAD_REQUEST)
    return 16;
  if (zz9k_request_media_session_status(&request, 9U,
                                        ZZ9K_MEDIA_STATUS_TIMING, 0U) !=
      ZZ9K_STATUS_OK)
    return 17;
  if (zz9k_request_media_session_status(
          &request, 9U, ZZ9K_MEDIA_STATUS_AUDIO_OUTPUT, 0U) !=
      ZZ9K_STATUS_OK)
    return 18;
  if (zz9k_request_media_session_status(
          &request, 9U, ZZ9K_MEDIA_STATUS_PROFILE + 1U, 0U) !=
      ZZ9K_STATUS_BAD_REQUEST)
    return 19;
  return 0;
}

static int test_reply_decoders(void)
{
  ZZ9KMailboxEntry reply;
  ZZ9KMediaSessionMainResult main_result;
  ZZ9KMediaSessionAudioResult audio_result;
  ZZ9KMediaSessionStatusResult status_result;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_MEDIA_SESSION_DECODE;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = 48U;
  zz9k_put_be32(&reply.payload.inline_data[0], 9U);
  zz9k_put_be32(&reply.payload.inline_data[4],
                ZZ9K_MEDIA_SESSION_STATE_FRAME_HELD);
  zz9k_put_be32(&reply.payload.inline_data[16], 25U);
  zz9k_put_be32(&reply.payload.inline_data[20], 1U);
  zz9k_put_be32(&reply.payload.inline_data[32], 3600U);
  zz9k_put_be32(&reply.payload.inline_data[44],
                ZZ9K_MEDIA_SESSION_RESULT_FRAME_HELD |
                ZZ9K_MEDIA_SESSION_RESULT_DERIVED_TIME |
                ZZ9K_MEDIA_SESSION_RESULT_AUDIO_READY);
  if (zz9k_reply_media_session_main(
          &reply, ZZ9K_OP_MEDIA_SESSION_DECODE, &main_result) !=
      ZZ9K_STATUS_OK)
    return 1;
  if (main_result.session != 9U || main_result.video_pts != 3600U)
    return 2;

  reply.payload_len = 44U;
  if (zz9k_reply_media_session_main(
          &reply, ZZ9K_OP_MEDIA_SESSION_DECODE, &main_result) !=
      ZZ9K_STATUS_INTERNAL_ERROR)
    return 3;
  reply.payload_len = 48U;
  zz9k_put_be32(&reply.payload.inline_data[4], 99U);
  if (zz9k_reply_media_session_main(
          &reply, ZZ9K_OP_MEDIA_SESSION_DECODE, &main_result) !=
      ZZ9K_STATUS_INTERNAL_ERROR)
    return 4;
  zz9k_put_be32(&reply.payload.inline_data[4],
                ZZ9K_MEDIA_SESSION_STATE_FRAME_HELD);
  zz9k_put_be32(&reply.payload.inline_data[44], 1U << 31);
  if (zz9k_reply_media_session_main(
          &reply, ZZ9K_OP_MEDIA_SESSION_DECODE, &main_result) !=
          ZZ9K_STATUS_INTERNAL_ERROR ||
      main_result.session != 0U)
    return 5;
  zz9k_put_be32(&reply.payload.inline_data[44],
                ZZ9K_MEDIA_SESSION_RESULT_FRAME_HELD);
  reply.opcode = ZZ9K_OP_MEDIA_SESSION_PRESENT;
  if (zz9k_reply_media_session_main(
          &reply, ZZ9K_OP_MEDIA_SESSION_DECODE, &main_result) !=
      ZZ9K_STATUS_INTERNAL_ERROR)
    return 6;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_MEDIA_SESSION_AUDIO_READ;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = 48U;
  zz9k_put_be32(&reply.payload.inline_data[0], 9U);
  zz9k_put_be32(&reply.payload.inline_data[4],
                ZZ9K_MEDIA_SESSION_STATE_READY);
  zz9k_put_be32(&reply.payload.inline_data[8], 44100U);
  zz9k_put_be32(&reply.payload.inline_data[12], 2U);
  zz9k_put_be32(&reply.payload.inline_data[44],
                ZZ9K_MEDIA_SESSION_RESULT_AUDIO_READY);
  if (zz9k_reply_media_session_audio(
          &reply, ZZ9K_OP_MEDIA_SESSION_AUDIO_READ, &audio_result) !=
          ZZ9K_STATUS_OK ||
      audio_result.session != 9U)
    return 7;
  reply.payload_len = 44U;
  if (zz9k_reply_media_session_audio(
          &reply, ZZ9K_OP_MEDIA_SESSION_AUDIO_READ, &audio_result) !=
      ZZ9K_STATUS_INTERNAL_ERROR)
    return 8;
  reply.payload_len = 48U;
  zz9k_put_be32(&reply.payload.inline_data[44], 1U << 31);
  if (zz9k_reply_media_session_audio(
          &reply, ZZ9K_OP_MEDIA_SESSION_AUDIO_READ, &audio_result) !=
          ZZ9K_STATUS_INTERNAL_ERROR ||
      audio_result.session != 0U)
    return 9;
  zz9k_put_be32(&reply.payload.inline_data[44],
                ZZ9K_MEDIA_SESSION_RESULT_AUDIO_READY |
                ZZ9K_MEDIA_SESSION_RESULT_AUDIO_BOUND |
                ZZ9K_MEDIA_SESSION_RESULT_AUDIO_PLAYING |
                ZZ9K_MEDIA_SESSION_RESULT_AUDIO_UNDERRUN);
  reply.opcode = ZZ9K_OP_MEDIA_SESSION_AUDIO_BIND;
  if (zz9k_reply_media_session_audio(
          &reply, ZZ9K_OP_MEDIA_SESSION_AUDIO_BIND, &audio_result) !=
          ZZ9K_STATUS_OK ||
      (audio_result.flags & ZZ9K_MEDIA_SESSION_RESULT_AUDIO_BOUND) == 0U)
    return 10;
  if (zz9k_reply_media_session_audio(
          &reply, ZZ9K_OP_MEDIA_SESSION_AUDIO_READ, &audio_result) !=
      ZZ9K_STATUS_INTERNAL_ERROR)
    return 11;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_MEDIA_SESSION_STATUS;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = 48U;
  zz9k_put_be32(&reply.payload.inline_data[0], 9U);
  zz9k_put_be32(&reply.payload.inline_data[4],
                ZZ9K_MEDIA_SESSION_STATE_READY);
  zz9k_put_be32(&reply.payload.inline_data[8], ZZ9K_MEDIA_STATUS_TIMING);
  zz9k_put_be32(&reply.payload.inline_data[12],
                ZZ9K_MEDIA_SESSION_RESULT_DERIVED_TIME);
  zz9k_put_be32(&reply.payload.inline_data[16], 0xffffffffU);
  zz9k_put_be32(&reply.payload.inline_data[20], 0xffffffffU);
  if (zz9k_reply_media_session_status(&reply, &status_result) !=
      ZZ9K_STATUS_OK)
    return 12;
  if (status_result.value[0] != ZZ9K_MEDIA_NO_PTS) return 13;
  reply.payload_len = 44U;
  if (zz9k_reply_media_session_status(&reply, &status_result) !=
      ZZ9K_STATUS_INTERNAL_ERROR)
    return 14;
  reply.payload_len = 48U;
  zz9k_put_be32(&reply.payload.inline_data[12], 1U << 31);
  if (zz9k_reply_media_session_status(&reply, &status_result) !=
          ZZ9K_STATUS_INTERNAL_ERROR ||
      status_result.session != 0U)
    return 15;
  zz9k_put_be32(&reply.payload.inline_data[12], 0U);
  reply.opcode = ZZ9K_OP_MEDIA_SESSION_DECODE;
  if (zz9k_reply_media_session_status(&reply, &status_result) !=
      ZZ9K_STATUS_INTERNAL_ERROR)
    return 16;
  reply.opcode = ZZ9K_OP_MEDIA_SESSION_STATUS;
  zz9k_put_be32(&reply.payload.inline_data[8],
                ZZ9K_MEDIA_STATUS_PROFILE + 1U);
  if (zz9k_reply_media_session_status(&reply, &status_result) !=
      ZZ9K_STATUS_INTERNAL_ERROR)
    return 17;

  /* The presentation page reuses `flags` for its own namespace, so it must
   * be validated against ZZ9K_MEDIA_PRESENT_* and not the session-result
   * mask. A present-flag combination is legal here... */
  zz9k_put_be32(&reply.payload.inline_data[8],
                ZZ9K_MEDIA_STATUS_PRESENTATION);
  zz9k_put_be32(&reply.payload.inline_data[12],
                ZZ9K_MEDIA_PRESENT_CONFIGURED | ZZ9K_MEDIA_PRESENT_ACTIVE |
                    ZZ9K_MEDIA_PRESENT_NATIVE | ZZ9K_MEDIA_PRESENT_OWNED);
  if (zz9k_reply_media_session_status(&reply, &status_result) !=
          ZZ9K_STATUS_OK ||
      status_result.page != ZZ9K_MEDIA_STATUS_PRESENTATION)
    return 18;
  /* ...while an out-of-namespace bit is still rejected. */
  zz9k_put_be32(&reply.payload.inline_data[12],
                ZZ9K_MEDIA_SESSION_RESULT_AUDIO_UNDERRUN);
  if (zz9k_reply_media_session_status(&reply, &status_result) !=
      ZZ9K_STATUS_INTERNAL_ERROR)
    return 19;

  /* The profile page owns its own flag namespace too. Validating one page
   * against another's mask only appears to work while the bits overlap. */
  zz9k_put_be32(&reply.payload.inline_data[8], ZZ9K_MEDIA_STATUS_PROFILE);
  zz9k_put_be32(&reply.payload.inline_data[12],
                ZZ9K_MEDIA_PROFILE_FLAG_NEON_PACK);
  if (zz9k_reply_media_session_status(&reply, &status_result) !=
          ZZ9K_STATUS_OK ||
      (status_result.flags & ZZ9K_MEDIA_PROFILE_FLAG_NEON_PACK) == 0U)
    return 20;
  zz9k_put_be32(&reply.payload.inline_data[12],
                ZZ9K_MEDIA_SESSION_RESULT_AUDIO_UNDERRUN);
  if (zz9k_reply_media_session_status(&reply, &status_result) !=
      ZZ9K_STATUS_INTERNAL_ERROR)
    return 21;
  return 0;
}

int main(void)
{
  int status;

  status = test_constants_and_layout();
  if (status) return 10 + status;
  status = test_timestamp_contract();
  if (status) return 50 + status;
  status = test_request_builders();
  if (status) return 80 + status;
  status = test_reply_decoders();
  if (status) return 110 + status;
  return 0;
}
