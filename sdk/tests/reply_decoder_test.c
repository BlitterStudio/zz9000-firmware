/*
 * Typed ZZ9KMailboxEntry reply decoder checks.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/reply.h"
#include <string.h>

static int test_reply_require_checks_status_opcode_and_length(void)
{
  ZZ9KMailboxEntry reply;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_PING;
  reply.status = ZZ9K_STATUS_UNSUPPORTED;
  reply.payload_len = 0;
  if (zz9k_reply_require(&reply, ZZ9K_OP_PING, 0) !=
      ZZ9K_STATUS_UNSUPPORTED) {
    return 1;
  }

  reply.status = ZZ9K_STATUS_OK;
  if (zz9k_reply_require(&reply, ZZ9K_OP_QUERY_CAPS, 0) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 2;
  }

  if (zz9k_reply_require(&reply, ZZ9K_OP_PING, 1) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 3;
  }

  reply.payload_len = 1;
  if (zz9k_reply_require(&reply, ZZ9K_OP_PING, 1) != ZZ9K_STATUS_OK) {
    return 4;
  }

  return 0;
}

static int test_copy_inline_payload_checks_capacity(void)
{
  ZZ9KMailboxEntry reply;
  uint8_t payload[4];
  uint32_t len;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_PING;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = 4;
  memcpy(reply.payload.inline_data, "pong", 4);

  len = 3;
  if (zz9k_reply_copy_inline_payload(&reply, ZZ9K_OP_PING, payload,
                                     &len) != ZZ9K_STATUS_BAD_REQUEST) {
    return 1;
  }
  if (len != 4U) return 2;

  memset(payload, 0, sizeof(payload));
  len = sizeof(payload);
  if (zz9k_reply_copy_inline_payload(&reply, ZZ9K_OP_PING, payload,
                                     &len) != ZZ9K_STATUS_OK) {
    return 3;
  }
  if (len != 4U) return 4;
  if (memcmp(payload, "pong", 4) != 0) return 5;

  return 0;
}

static int test_caps_and_service_decoders(void)
{
  ZZ9KMailboxEntry reply;
  ZZ9KCaps caps;
  ZZ9KServiceInfo service;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_QUERY_CAPS;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = 36;
  zz9k_put_be32(&reply.payload.inline_data[0], ZZ9K_ABI_MAGIC);
  zz9k_put_be16(&reply.payload.inline_data[4], ZZ9K_ABI_VERSION_MAJOR);
  zz9k_put_be16(&reply.payload.inline_data[6], ZZ9K_ABI_VERSION_MINOR);
  zz9k_put_be32(&reply.payload.inline_data[8], ZZ9K_CAP_MAILBOX);
  zz9k_put_be32(&reply.payload.inline_data[12], 48);
  zz9k_put_be32(&reply.payload.inline_data[16], 17);
  zz9k_put_be32(&reply.payload.inline_data[20], 18);
  zz9k_put_be32(&reply.payload.inline_data[24], 0x1234);
  zz9k_put_be32(&reply.payload.inline_data[28], 64);
  zz9k_put_be32(&reply.payload.inline_data[32], 32);

  memset(&caps, 0xff, sizeof(caps));
  if (zz9k_reply_caps(&reply, &caps) != ZZ9K_STATUS_OK) return 1;
  if (caps.magic != ZZ9K_ABI_MAGIC) return 2;
  if (caps.capability_bits != ZZ9K_CAP_MAILBOX) return 3;
  if (caps.max_shared_buffers != 17U) return 4;
  if (caps.completion_ring_entries != 32U) return 5;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_QUERY_SERVICE;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = sizeof(ZZ9KServiceInfoPayload);
  zz9k_put_be32(&reply.payload.inline_data[0], ZZ9K_SERVICE_IMAGE);
  zz9k_put_be32(&reply.payload.inline_data[4], 0x00020004UL);
  zz9k_put_be32(&reply.payload.inline_data[8], ZZ9K_CAP_IMAGE_SCALE);
  zz9k_put_be32(&reply.payload.inline_data[12], ZZ9K_SERVICE_FLAG_FIRMWARE);
  zz9k_put_be32(&reply.payload.inline_data[16], ZZ9K_SERVICE_IMAGE);
  zz9k_put_be32(&reply.payload.inline_data[20], 4);
  zz9k_put_be32(&reply.payload.inline_data[24], 48);
  memcpy(&reply.payload.inline_data[28], "image", 5);

  memset(&service, 0xff, sizeof(service));
  if (zz9k_reply_service_info(&reply, ZZ9K_SERVICE_IMAGE, &service) !=
      ZZ9K_STATUS_OK) {
    return 6;
  }
  if (service.service_id != ZZ9K_SERVICE_IMAGE) return 7;
  if (service.version != 0x00020004UL) return 8;
  if (strcmp(service.name, "image") != 0) return 9;

  if (zz9k_reply_service_info(&reply, ZZ9K_SERVICE_AUDIO, &service) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 10;
  }

  return 0;
}

static int test_shared_surface_and_diag_decoders(void)
{
  ZZ9KMailboxEntry reply;
  ZZ9KSharedBufferInfo shared;
  ZZ9KSurface surface;
  ZZ9KDiagInfo diag;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_ALLOC_SHARED;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = 16;
  zz9k_put_be32(&reply.payload.inline_data[0], 0x40000001UL);
  zz9k_put_be32(&reply.payload.inline_data[4], 0x01020304UL);
  zz9k_put_be32(&reply.payload.inline_data[8], 8192);
  zz9k_put_be32(&reply.payload.inline_data[12], 3);
  memset(&shared, 0, sizeof(shared));
  if (zz9k_reply_shared_buffer(&reply, &shared) != ZZ9K_STATUS_OK) {
    return 1;
  }
  if (shared.handle != 0x40000001UL) return 2;
  if (shared.arm_addr != 0x01020304UL) return 3;
  if (shared.length != 8192U) return 4;
  if (shared.flags != 3U) return 5;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_ALLOC_SURFACE;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = 32;
  zz9k_put_be32(&reply.payload.inline_data[0], 0x40000002UL);
  zz9k_put_be32(&reply.payload.inline_data[4], 0x02030405UL);
  zz9k_put_be32(&reply.payload.inline_data[8], 320);
  zz9k_put_be32(&reply.payload.inline_data[12], 200);
  zz9k_put_be32(&reply.payload.inline_data[16], 1280);
  zz9k_put_be32(&reply.payload.inline_data[20], ZZ9K_SURFACE_FORMAT_ARGB8888);
  zz9k_put_be32(&reply.payload.inline_data[24], ZZ9K_SURFACE_FLAG_CPU_VISIBLE);
  zz9k_put_be32(&reply.payload.inline_data[28], 256000);
  memset(&surface, 0, sizeof(surface));
  if (zz9k_reply_surface(&reply, ZZ9K_OP_ALLOC_SURFACE, &surface) !=
      ZZ9K_STATUS_OK) {
    return 6;
  }
  if (surface.handle != 0x40000002UL) return 7;
  if (surface.arm_addr != 0x02030405UL) return 8;
  if (surface.width != 320U || surface.height != 200U) return 9;
  if (surface.data != 0) return 10;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_DIAG_READ;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = 48;
  zz9k_put_be32(&reply.payload.inline_data[0], 11);
  zz9k_put_be32(&reply.payload.inline_data[4], 12);
  zz9k_put_be32(&reply.payload.inline_data[8], ZZ9K_STATUS_BUSY);
  zz9k_put_be32(&reply.payload.inline_data[12], 13);
  zz9k_put_be32(&reply.payload.inline_data[16], 14);
  zz9k_put_be32(&reply.payload.inline_data[20], 15);
  zz9k_put_be32(&reply.payload.inline_data[24], 16);
  zz9k_put_be32(&reply.payload.inline_data[28], 17);
  zz9k_put_be32(&reply.payload.inline_data[32], 0x3fe4d000UL);
  zz9k_put_be32(&reply.payload.inline_data[36], 64);
  zz9k_put_be32(&reply.payload.inline_data[40], 18);
  zz9k_put_be32(&reply.payload.inline_data[44], 19);
  memset(&diag, 0, sizeof(diag));
  if (zz9k_reply_diag_info(&reply, &diag) != ZZ9K_STATUS_OK) return 11;
  if (diag.requests_completed != 11U) return 12;
  if (diag.last_status != ZZ9K_STATUS_BUSY) return 13;
  if (diag.mailbox_arm_addr != 0x3fe4d000UL) return 14;
  if (diag.surfaces_used != 18U) return 15;
  if (diag.allocator_invalid_slots != 19U) return 16;

  return 0;
}

static int test_image_decode_result_decoder(void)
{
  ZZ9KMailboxEntry reply;
  ZZ9KImageDecodeResult result;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_DECODE_PNG;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = sizeof(ZZ9KImageDecodeResultPayload);
  zz9k_put_be32(&reply.payload.inline_data[0], 640);
  zz9k_put_be32(&reply.payload.inline_data[4], 480);
  zz9k_put_be32(&reply.payload.inline_data[8], ZZ9K_SURFACE_FORMAT_ARGB8888);
  zz9k_put_be32(&reply.payload.inline_data[12],
                ZZ9K_IMAGE_DECODE_RESULT_ALPHA);
  zz9k_put_be32(&reply.payload.inline_data[16], 1228800);

  memset(&result, 0, sizeof(result));
  if (zz9k_reply_image_decode_result(&reply, ZZ9K_OP_DECODE_PNG,
                                     &result) != ZZ9K_STATUS_OK) {
    return 1;
  }
  if (result.width != 640U) return 2;
  if (result.height != 480U) return 3;
  if (result.output_format != ZZ9K_SURFACE_FORMAT_ARGB8888) return 4;
  if (result.flags != ZZ9K_IMAGE_DECODE_RESULT_ALPHA) return 5;
  if (result.bytes_written != 1228800U) return 6;

  if (zz9k_reply_image_decode_result(&reply, ZZ9K_OP_DECODE_JPEG,
                                     &result) != ZZ9K_STATUS_INTERNAL_ERROR) {
    return 7;
  }

  return 0;
}

static int test_diag_timing_decoder(void)
{
  ZZ9KMailboxEntry reply;
  ZZ9KDiagTimingInfo timing;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_DIAG_TIMING;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = sizeof(ZZ9KDiagTimingPayload);
  zz9k_put_be32(&reply.payload.inline_data[0], 1U);
  zz9k_put_be32(&reply.payload.inline_data[4], 333333333U);
  zz9k_put_be32(&reply.payload.inline_data[8], 123U);
  zz9k_put_be32(&reply.payload.inline_data[12], 4567U);
  zz9k_put_be32(&reply.payload.inline_data[16], 44U);
  zz9k_put_be32(&reply.payload.inline_data[20], 890U);
  zz9k_put_be32(&reply.payload.inline_data[24], 55U);
  zz9k_put_be32(&reply.payload.inline_data[28], 1200U);
  zz9k_put_be32(&reply.payload.inline_data[32], ZZ9K_OP_AUDIO_STREAM_READ);
  zz9k_put_be32(&reply.payload.inline_data[36], 33U);
  zz9k_put_be32(&reply.payload.inline_data[40], ZZ9K_OP_COPY_SURFACE);
  zz9k_put_be32(&reply.payload.inline_data[44], 77U);

  memset(&timing, 0, sizeof(timing));
  if (zz9k_reply_diag_timing(&reply, &timing) != ZZ9K_STATUS_OK) return 1;
  if (timing.version != 1U) return 2;
  if (timing.timer_hz != 333333333U) return 3;
  if (timing.requests_timed != 123U) return 4;
  if (timing.total_us != 4567U) return 5;
  if (timing.surface_requests != 44U || timing.surface_us != 890U) return 6;
  if (timing.audio_requests != 55U || timing.audio_us != 1200U) return 7;
  if (timing.last_opcode != ZZ9K_OP_AUDIO_STREAM_READ ||
      timing.last_us != 33U) {
    return 8;
  }
  if (timing.max_opcode != ZZ9K_OP_COPY_SURFACE ||
      timing.max_us != 77U) {
    return 9;
  }

  reply.opcode = ZZ9K_OP_DIAG_READ;
  if (zz9k_reply_diag_timing(&reply, &timing) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 10;
  }

  return 0;
}

static int test_diag_sched_decoder(void)
{
  ZZ9KMailboxEntry reply;
  ZZ9KDiagSchedInfo sched;

  /* Version-1 firmware: only the 16-byte base payload. Decode counters must
   * default to zero and the decode must still succeed (backward compat). */
  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_DIAG_SCHED;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = ZZ9K_DIAG_SCHED_PAYLOAD_V1_BYTES;
  zz9k_put_be32(&reply.payload.inline_data[0], 1U);
  zz9k_put_be32(&reply.payload.inline_data[4], 1U);
  zz9k_put_be32(&reply.payload.inline_data[8], 1234U);
  zz9k_put_be32(&reply.payload.inline_data[12], 57U);

  memset(&sched, 0, sizeof(sched));
  if (zz9k_reply_diag_sched(&reply, &sched) != ZZ9K_STATUS_OK) return 1;
  if (sched.version != 1U) return 2;
  if (sched.core1_online != 1U) return 3;
  if (sched.tasks_on_core1 != 1234U) return 4;
  if (sched.tasks_on_core0 != 57U) return 5;
  if (sched.decode_requests != 0U) return 6;
  if (sched.decode_us != 0U) return 7;

  /* Version-2 firmware: full 24-byte payload with decode counters. */
  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_DIAG_SCHED;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = sizeof(ZZ9KDiagSchedPayload);
  zz9k_put_be32(&reply.payload.inline_data[0], 2U);
  zz9k_put_be32(&reply.payload.inline_data[4], 1U);
  zz9k_put_be32(&reply.payload.inline_data[8], 4096U);
  zz9k_put_be32(&reply.payload.inline_data[12], 12U);
  zz9k_put_be32(&reply.payload.inline_data[16], 88U);
  zz9k_put_be32(&reply.payload.inline_data[20], 654321U);

  memset(&sched, 0, sizeof(sched));
  if (zz9k_reply_diag_sched(&reply, &sched) != ZZ9K_STATUS_OK) return 8;
  if (sched.version != 2U) return 9;
  if (sched.core1_online != 1U) return 10;
  if (sched.tasks_on_core1 != 4096U) return 11;
  if (sched.tasks_on_core0 != 12U) return 12;
  if (sched.decode_requests != 88U) return 13;
  if (sched.decode_us != 654321U) return 14;

  reply.opcode = ZZ9K_OP_DIAG_TIMING;
  if (zz9k_reply_diag_sched(&reply, &sched) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 15;
  }

  return 0;
}

static int test_crypto_result_decoder(void)
{
  ZZ9KMailboxEntry reply;
  ZZ9KCryptoResult result;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_CRYPTO_HASH;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = sizeof(ZZ9KCryptoResultPayload);
  zz9k_put_be32(&reply.payload.inline_data[0], 32);
  zz9k_put_be32(&reply.payload.inline_data[4], ZZ9K_CRYPTO_HASH_SHA256);
  zz9k_put_be32(&reply.payload.inline_data[8], ZZ9K_CRYPTO_HASH_FLAG_HMAC);

  memset(&result, 0, sizeof(result));
  if (zz9k_reply_crypto_result(&reply, ZZ9K_OP_CRYPTO_HASH, &result) !=
      ZZ9K_STATUS_OK) {
    return 1;
  }
  if (result.bytes_written != 32U) return 2;
  if (result.algorithm != ZZ9K_CRYPTO_HASH_SHA256) return 3;
  if (result.flags != ZZ9K_CRYPTO_HASH_FLAG_HMAC) return 4;

  reply.payload.inline_data[0] = 0;
  reply.payload.inline_data[1] = 0;
  reply.payload.inline_data[2] = 0;
  reply.payload.inline_data[3] = 0;
  if (zz9k_reply_crypto_result(&reply, ZZ9K_OP_CRYPTO_HASH, &result) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 5;
  }

  return 0;
}

static int test_audio_decode_result_decoder(void)
{
  ZZ9KMailboxEntry reply;
  ZZ9KAudioDecodeResult result;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_DECODE_MP3;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = sizeof(ZZ9KAudioDecodeResultPayload);
  zz9k_put_be32(&reply.payload.inline_data[0], 1234U);
  zz9k_put_be32(&reply.payload.inline_data[4], 4096U);
  zz9k_put_be32(&reply.payload.inline_data[8], 44100U);
  zz9k_put_be32(&reply.payload.inline_data[12], 2U);
  zz9k_put_be32(&reply.payload.inline_data[16],
                ZZ9K_AUDIO_SAMPLE_FORMAT_S16LE);
  zz9k_put_be32(&reply.payload.inline_data[20], 1024U);
  zz9k_put_be32(&reply.payload.inline_data[24],
                ZZ9K_AUDIO_DECODE_RESULT_END);

  memset(&result, 0, sizeof(result));
  if (zz9k_reply_audio_decode_result(&reply, &result) != ZZ9K_STATUS_OK) {
    return 1;
  }
  if (result.bytes_consumed != 1234U) return 2;
  if (result.bytes_written != 4096U) return 3;
  if (result.sample_rate != 44100U) return 4;
  if (result.channels != 2U) return 5;
  if (result.sample_format != ZZ9K_AUDIO_SAMPLE_FORMAT_S16LE) return 6;
  if (result.frames_written != 1024U) return 7;
  if (result.flags != ZZ9K_AUDIO_DECODE_RESULT_END) return 8;

  reply.opcode = ZZ9K_OP_CRYPTO_HASH;
  if (zz9k_reply_audio_decode_result(&reply, &result) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 9;
  }

  return 0;
}

static int test_audio_stream_result_decoder(void)
{
  ZZ9KMailboxEntry reply;
  ZZ9KAudioStreamResult result;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_AUDIO_STREAM_FEED;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = sizeof(ZZ9KAudioStreamResultPayload);
  zz9k_put_be32(&reply.payload.inline_data[0], 9U);
  zz9k_put_be32(&reply.payload.inline_data[4],
                ZZ9K_AUDIO_STREAM_STATE_STREAMING);
  zz9k_put_be32(&reply.payload.inline_data[8], 44100U);
  zz9k_put_be32(&reply.payload.inline_data[12], 2U);
  zz9k_put_be32(&reply.payload.inline_data[16],
                ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE);
  zz9k_put_be32(&reply.payload.inline_data[20], 4096U);
  zz9k_put_be32(&reply.payload.inline_data[24], 8192U);
  zz9k_put_be32(&reply.payload.inline_data[28], 2048U);
  zz9k_put_be32(&reply.payload.inline_data[32], 3U);
  zz9k_put_be32(&reply.payload.inline_data[36], 4096U);
  zz9k_put_be32(&reply.payload.inline_data[40], 8192U);
  zz9k_put_be32(&reply.payload.inline_data[44],
                ZZ9K_AUDIO_STREAM_RESULT_NEED_INPUT);

  memset(&result, 0, sizeof(result));
  if (zz9k_reply_audio_stream_result(&reply, ZZ9K_OP_AUDIO_STREAM_FEED,
                                     &result) != ZZ9K_STATUS_OK) {
    return 1;
  }
  if (result.session != 9U) return 2;
  if (result.state != ZZ9K_AUDIO_STREAM_STATE_STREAMING) return 3;
  if (result.sample_rate != 44100U || result.channels != 2U) return 4;
  if (result.sample_format != ZZ9K_AUDIO_SAMPLE_FORMAT_S16BE) return 5;
  if (result.mp3_read != 4096U || result.pcm_write != 8192U ||
      result.pcm_read != 2048U) {
    return 6;
  }
  if (result.frames_decoded != 3U || result.bytes_consumed != 4096U ||
      result.bytes_produced != 8192U) {
    return 7;
  }
  if (result.flags != ZZ9K_AUDIO_STREAM_RESULT_NEED_INPUT) return 8;

  reply.opcode = ZZ9K_OP_DECODE_MP3;
  if (zz9k_reply_audio_stream_result(&reply, ZZ9K_OP_AUDIO_STREAM_FEED,
                                     &result) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 9;
  }

  return 0;
}

static int test_decompress_result_decoder(void)
{
  ZZ9KMailboxEntry reply;
  ZZ9KDecompressResult result;
  ZZ9KDecompressStreamResult stream_result;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_DECOMPRESS;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = sizeof(ZZ9KDecompressResultPayload);
  zz9k_put_be32(&reply.payload.inline_data[0], 2048U);
  zz9k_put_be32(&reply.payload.inline_data[4], 8192U);
  zz9k_put_be32(&reply.payload.inline_data[8], 0x12345678UL);
  zz9k_put_be32(&reply.payload.inline_data[12], ZZ9K_COMPRESSION_GZIP);
  zz9k_put_be32(&reply.payload.inline_data[16],
                ZZ9K_DECOMPRESS_RESULT_STREAM_END |
                ZZ9K_DECOMPRESS_RESULT_CHECKSUM_VALID);

  memset(&result, 0, sizeof(result));
  if (zz9k_reply_decompress_result(&reply, &result) != ZZ9K_STATUS_OK) {
    return 1;
  }
  if (result.bytes_consumed != 2048U) return 2;
  if (result.bytes_written != 8192U) return 3;
  if (result.checksum != 0x12345678UL) return 4;
  if (result.algorithm != ZZ9K_COMPRESSION_GZIP) return 5;
  if (result.flags != (ZZ9K_DECOMPRESS_RESULT_STREAM_END |
                       ZZ9K_DECOMPRESS_RESULT_CHECKSUM_VALID)) {
    return 6;
  }

  reply.opcode = ZZ9K_OP_CRYPTO_HASH;
  if (zz9k_reply_decompress_result(&reply, &result) !=
      ZZ9K_STATUS_INTERNAL_ERROR) {
    return 7;
  }

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_DECOMPRESS_STREAM_BEGIN;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = sizeof(ZZ9KDecompressStreamResultPayload);
  zz9k_put_be32(&reply.payload.inline_data[0], 12U);
  zz9k_put_be32(&reply.payload.inline_data[4], 0U);
  zz9k_put_be32(&reply.payload.inline_data[8], 0U);
  zz9k_put_be32(&reply.payload.inline_data[12], 0U);
  zz9k_put_be32(&reply.payload.inline_data[16],
                ZZ9K_COMPRESSION_LZMA_ALONE);
  zz9k_put_be32(&reply.payload.inline_data[20], 0U);

  memset(&stream_result, 0, sizeof(stream_result));
  if (zz9k_reply_decompress_stream_result(
          &reply, ZZ9K_OP_DECOMPRESS_STREAM_BEGIN,
          &stream_result) != ZZ9K_STATUS_OK) {
    return 8;
  }
  if (stream_result.session != 12U ||
      stream_result.algorithm != ZZ9K_COMPRESSION_LZMA_ALONE ||
      stream_result.bytes_written != 0U) {
    return 9;
  }

  reply.opcode = ZZ9K_OP_DECOMPRESS_STREAM_READ;
  zz9k_put_be32(&reply.payload.inline_data[4], 4096U);
  zz9k_put_be32(&reply.payload.inline_data[8], 8192U);
  zz9k_put_be32(&reply.payload.inline_data[12], 0x88776655UL);
  zz9k_put_be32(&reply.payload.inline_data[20],
                ZZ9K_DECOMPRESS_RESULT_STREAM_END |
                ZZ9K_DECOMPRESS_RESULT_CHECKSUM_VALID);

  memset(&stream_result, 0, sizeof(stream_result));
  if (zz9k_reply_decompress_stream_result(
          &reply, ZZ9K_OP_DECOMPRESS_STREAM_READ,
          &stream_result) != ZZ9K_STATUS_OK) {
    return 10;
  }
  if (stream_result.session != 12U ||
      stream_result.bytes_consumed != 4096U ||
      stream_result.bytes_written != 8192U ||
      stream_result.checksum != 0x88776655UL ||
      stream_result.flags != (ZZ9K_DECOMPRESS_RESULT_STREAM_END |
                              ZZ9K_DECOMPRESS_RESULT_CHECKSUM_VALID)) {
    return 11;
  }

  reply.opcode = ZZ9K_OP_DECOMPRESS_STREAM_FEED;
  zz9k_put_be32(&reply.payload.inline_data[8], 0U);
  zz9k_put_be32(&reply.payload.inline_data[20],
                ZZ9K_DECOMPRESS_RESULT_NEED_INPUT);
  memset(&stream_result, 0, sizeof(stream_result));
  if (zz9k_reply_decompress_stream_result(
          &reply, ZZ9K_OP_DECOMPRESS_STREAM_FEED,
          &stream_result) != ZZ9K_STATUS_OK) {
    return 13;
  }
  if (stream_result.session != 12U ||
      stream_result.flags != ZZ9K_DECOMPRESS_RESULT_NEED_INPUT) {
    return 14;
  }

  reply.payload.inline_data[0] = 0;
  reply.payload.inline_data[1] = 0;
  reply.payload.inline_data[2] = 0;
  reply.payload.inline_data[3] = 0;
  if (zz9k_reply_decompress_stream_result(
          &reply, ZZ9K_OP_DECOMPRESS_STREAM_READ,
          &stream_result) != ZZ9K_STATUS_INTERNAL_ERROR) {
    return 12;
  }

  return 0;
}

static int test_decompress_batch_result_decodes_counts(void)
{
  ZZ9KMailboxEntry reply;
  ZZ9KDecompressBatchResult result;
  ZZ9KDecompressBatchResultPayload *payload;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_DECOMPRESS_BATCH;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = sizeof(ZZ9KDecompressBatchResultPayload);
  payload = (ZZ9KDecompressBatchResultPayload *)reply.payload.inline_data;
  zz9k_put_be32(payload->members_total, 5U);
  zz9k_put_be32(payload->members_ok, 4U);
  zz9k_put_be32(payload->members_failed, 1U);
  zz9k_put_be32(payload->flags, 0U);

  if (zz9k_reply_decompress_batch_result(&reply, &result) != ZZ9K_STATUS_OK) {
    return 1;
  }
  if (result.members_total != 5U) return 2;
  if (result.members_ok != 4U) return 3;
  if (result.members_failed != 1U) return 4;
  if (result.flags != 0U) return 5;

  reply.opcode = ZZ9K_OP_DECOMPRESS;
  if (zz9k_reply_decompress_batch_result(&reply, &result) == ZZ9K_STATUS_OK) {
    return 6; /* wrong opcode must be rejected */
  }
  reply.opcode = ZZ9K_OP_DECOMPRESS_BATCH;
  reply.status = ZZ9K_STATUS_UNSUPPORTED;
  if (zz9k_reply_decompress_batch_result(&reply, &result) !=
      ZZ9K_STATUS_UNSUPPORTED) {
    return 7; /* firmware status must propagate */
  }
  if (zz9k_reply_decompress_batch_result(&reply, 0) !=
      ZZ9K_STATUS_BAD_REQUEST) {
    return 8;
  }
  return 0;
}

int main(void)
{
  int result;

  result = test_reply_require_checks_status_opcode_and_length();
  if (result) return 10 + result;

  result = test_copy_inline_payload_checks_capacity();
  if (result) return 30 + result;

  result = test_caps_and_service_decoders();
  if (result) return 50 + result;

  result = test_shared_surface_and_diag_decoders();
  if (result) return 90 + result;

  result = test_image_decode_result_decoder();
  if (result) return 130 + result;

  result = test_diag_timing_decoder();
  if (result) return 150 + result;

  result = test_diag_sched_decoder();
  if (result) return 155 + result;

  result = test_crypto_result_decoder();
  if (result) return 160 + result;

  result = test_audio_decode_result_decoder();
  if (result) return 170 + result;

  result = test_audio_stream_result_decoder();
  if (result) return 175 + result;

  result = test_decompress_result_decoder();
  if (result) return 180 + result;

  result = test_decompress_batch_result_decodes_counts();
  if (result) return 190 + result;

  return 0;
}
