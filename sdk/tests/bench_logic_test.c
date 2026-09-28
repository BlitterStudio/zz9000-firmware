/*
 * Unit checks for the offscreen SDK benchmark helper logic.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define ZZ9K_BENCH_NO_MAIN
#include "../tools/zz9k-bench.c"

#include <stdint.h>
#include <string.h>

static int test_iteration_parser_defaults_and_clamps(void)
{
  char *default_argv[] = {"zz9k-bench"};
  char *low_argv[] = {"zz9k-bench", "0"};
  char *high_argv[] = {"zz9k-bench", "999"};

  if (zz9k_bench_parse_iterations(1, default_argv) != 8U) return 1;
  if (zz9k_bench_parse_iterations(2, low_argv) != 1U) return 2;
  if (zz9k_bench_parse_iterations(2, high_argv) != 100U) return 3;

  return 0;
}

static int test_rate_calculations_use_ticks_per_second(void)
{
  if (zz9k_bench_kib_per_second(4096U, 50U, 50U) != 4U) return 1;
  if (zz9k_bench_kib_per_second(4096U, 0U, 50U) != 0U) return 2;
  if (zz9k_bench_ticks_to_ms(25U, 50U) != 500U) return 3;
  if (zz9k_bench_ticks_to_ms(25U, 0U) != 0U) return 4;
  if (zz9k_bench_calls_per_second(100U, 50U, 50U) != 100U) return 5;
  if (zz9k_bench_calls_per_second(100U, 0U, 50U) != 0U) return 6;
  if (zz9k_bench_kib_per_second(65536U, 0x100000000ULL, 1000000U) !=
      0U) {
    return 7;
  }
  if (zz9k_bench_u32_delta(10U, 7U) != 3U) return 8;
  if (zz9k_bench_u32_delta(3U, 7U) != 0U) return 9;

  return 0;
}

static int test_high_resolution_tick_helpers_use_64_bit_values(void)
{
  ZZ9KBenchTick tick;

  tick = zz9k_bench_eclock_to_tick(0x00000001UL, 0x00000002UL);
  if (tick != 0x0000000100000002ULL) return 1;
  if (zz9k_bench_elapsed_ticks(10ULL, 42ULL) != 32ULL) return 2;
  if (zz9k_bench_ticks_to_ms(1500ULL, 1000U) != 1500U) return 3;
  if (zz9k_bench_calls_per_second(50U, 250000ULL, 1000000U) != 200U) {
    return 4;
  }
  if (zz9k_bench_timeout_ticks(50U) != 250ULL) return 5;
  if (zz9k_bench_timeout_ticks(1000000U) != 5000000ULL) return 6;

  return 0;
}

static int test_scale_descriptor_uses_requested_filter(void)
{
  ZZ9KScaleImageDesc desc;

  memset(&desc, 0xff, sizeof(desc));
  if (!zz9k_bench_build_scale_desc(&desc, 0x40000001UL, 0x40000002UL,
                                   160U, 100U, 320U, 200U,
                                   ZZ9K_SCALE_BILINEAR)) {
    return 1;
  }

  if (desc.src_surface != 0x40000001UL) return 2;
  if (desc.dst_surface != 0x40000002UL) return 3;
  if (desc.src_x != 0 || desc.src_y != 0) return 4;
  if (desc.src_w != 160U || desc.src_h != 100U) return 5;
  if (desc.dst_x != 0 || desc.dst_y != 0) return 6;
  if (desc.dst_w != 320U || desc.dst_h != 200U) return 7;
  if (desc.filter != ZZ9K_SCALE_BILINEAR) return 8;
  if (desc.flags != 0) return 9;
  if (zz9k_bench_build_scale_desc(&desc, ZZ9K_INVALID_HANDLE,
                                  0x40000002UL, 160U, 100U,
                                  320U, 200U, ZZ9K_SCALE_BILINEAR)) {
    return 10;
  }
  if (zz9k_bench_build_scale_desc(&desc, 0x40000001UL,
                                  0x40000002UL, 0U, 100U,
                                  320U, 200U, ZZ9K_SCALE_BILINEAR)) {
    return 11;
  }

  return 0;
}

static int test_bgra_surface_pattern_uses_native_alpha_byte(void)
{
  ZZ9KSurface surface;
  uint8_t pixels[16];

  memset(&surface, 0, sizeof(surface));
  memset(pixels, 0, sizeof(pixels));
  surface.data = pixels;
  surface.length = (uint32_t)sizeof(pixels);
  surface.width = 2U;
  surface.height = 2U;
  surface.pitch = 8U;
  surface.format = ZZ9K_SURFACE_FORMAT_BGRA8888;

  zz9k_bench_fill_bgra_surface(&surface);

  if (pixels[3] != 0xffU) return 1;
  if (pixels[0] == 0xffU) return 2;
  if (!zz9k_bench_check_surface_sample(&surface)) return 3;

  surface.length = 3U;
  if (zz9k_bench_check_surface_sample(&surface)) return 4;

  return 0;
}

static int test_local_surface_copy_descriptor_uses_rtg_geometry(void)
{
  ZZ9KSurfaceCopyDesc desc;
  ZZ9KSurfaceFillDesc fill;

  if (ZZ9K_BENCH_LOCAL_SURFACE_WIDTH != 1280U) return 1;
  if (ZZ9K_BENCH_LOCAL_SURFACE_HEIGHT != 720U) return 2;
  if (ZZ9K_BENCH_LOCAL_SURFACE_BYTES != 3686400UL) return 3;

  memset(&desc, 0xff, sizeof(desc));
  if (!zz9k_bench_build_surface_copy_desc(
          &desc, 0x40001000UL, 0x40002000UL,
          ZZ9K_BENCH_LOCAL_SURFACE_WIDTH,
          ZZ9K_BENCH_LOCAL_SURFACE_HEIGHT)) {
    return 4;
  }

  if (desc.src_surface != 0x40001000UL) return 5;
  if (desc.dst_surface != 0x40002000UL) return 6;
  if (desc.src_x != 0U || desc.src_y != 0U) return 7;
  if (desc.dst_x != 0U || desc.dst_y != 0U) return 8;
  if (desc.width != 1280U || desc.height != 720U) return 9;
  if (desc.flags != 0U) return 10;

  memset(&fill, 0xff, sizeof(fill));
  if (!zz9k_bench_build_surface_fill_desc(
          &fill, 0x40001000UL, ZZ9K_BENCH_LOCAL_SURFACE_WIDTH,
          ZZ9K_BENCH_LOCAL_SURFACE_HEIGHT, 0xff2a7fffUL)) {
    return 11;
  }
  if (fill.surface != 0x40001000UL) return 12;
  if (fill.x != 0U || fill.y != 0U) return 13;
  if (fill.width != 1280U || fill.height != 720U) return 14;
  if (fill.color != 0xff2a7fffUL) return 15;
  if (fill.flags != 0U) return 16;
  if (zz9k_bench_build_surface_fill_desc(
          &fill, ZZ9K_INVALID_HANDLE, ZZ9K_BENCH_LOCAL_SURFACE_WIDTH,
          ZZ9K_BENCH_LOCAL_SURFACE_HEIGHT, 0xff2a7fffUL)) {
    return 17;
  }
  if (zz9k_bench_build_surface_fill_desc(
          &fill, 0x40001000UL, 0U,
          ZZ9K_BENCH_LOCAL_SURFACE_HEIGHT, 0xff2a7fffUL)) {
    return 18;
  }
  if (zz9k_bench_build_surface_copy_desc(
          &desc, ZZ9K_INVALID_HANDLE, 0x40002000UL,
          ZZ9K_BENCH_LOCAL_SURFACE_WIDTH,
          ZZ9K_BENCH_LOCAL_SURFACE_HEIGHT)) {
    return 19;
  }
  if (zz9k_bench_build_surface_copy_desc(
          &desc, 0x40001000UL, 0x40002000UL, 0U,
          ZZ9K_BENCH_LOCAL_SURFACE_HEIGHT)) {
    return 20;
  }

  return 0;
}

static int test_pipelined_ping_batch_size_is_bounded(void)
{
  if (zz9k_bench_choose_ping_batch(0U) != 0U) return 1;
  if (zz9k_bench_choose_ping_batch(1U) != 1U) return 2;
  if (zz9k_bench_choose_ping_batch(16U) != 16U) return 3;
  if (zz9k_bench_choose_ping_batch(17U) != 16U) return 4;
  if (zz9k_bench_choose_ping_batch(100U) != 16U) return 5;

  return 0;
}

static int test_pipelined_ping_request_and_reply_validation(void)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;

  memset(&request, 0xff, sizeof(request));
  zz9k_bench_prepare_pipelined_ping(&request, 0x12345678UL);

  if (request.entry.opcode != ZZ9K_OP_PING) return 1;
  if (request.entry.flags != ZZ9K_ENTRY_INLINE_PAYLOAD) return 2;
  if (request.entry.payload_len != 4U) return 3;
  if (request.entry.user_cookie != 0x12345678UL) return 4;
  if (request.entry.payload.inline_data[0] != 'p') return 5;
  if (request.entry.payload.inline_data[1] != 'q') return 6;
  if (request.entry.payload.inline_data[2] != 0x56U) return 7;
  if (request.entry.payload.inline_data[3] != 0x78U) return 8;

  memset(&reply, 0, sizeof(reply));
  reply.opcode = ZZ9K_OP_PING;
  reply.status = ZZ9K_STATUS_OK;
  reply.payload_len = 4U;
  reply.user_cookie = request.entry.user_cookie;
  memcpy(reply.payload.inline_data, request.entry.payload.inline_data, 4);
  if (!zz9k_bench_pipelined_ping_reply_ok(&reply)) return 9;

  reply.payload.inline_data[0] = 'x';
  if (zz9k_bench_pipelined_ping_reply_ok(&reply)) return 10;

  return 0;
}

static int test_crypto_descriptor_helpers(void)
{
  ZZ9KCryptoHashDesc desc;
  ZZ9KCryptoStreamDesc stream;
  ZZ9KCryptoAeadDesc aead;

  if (zz9k_bench_crypto_digest_length(ZZ9K_CRYPTO_HASH_SHA1) != 20U) {
    return 1;
  }
  if (zz9k_bench_crypto_digest_length(ZZ9K_CRYPTO_HASH_SHA256) != 32U) {
    return 2;
  }
  if (zz9k_bench_crypto_digest_length(ZZ9K_CRYPTO_HASH_POLY1305) != 16U) {
    return 3;
  }
  if (zz9k_bench_crypto_digest_length(ZZ9K_CRYPTO_HASH_NONE) != 0U) {
    return 4;
  }

  memset(&desc, 0xff, sizeof(desc));
  zz9k_bench_build_crypto_hash_desc(&desc, ZZ9K_CRYPTO_HASH_SHA256,
                                    0x40000010UL, 0x00020000UL,
                                    0x40000020UL);

  if (desc.src_handle != 0x40000010UL) return 5;
  if (desc.src_offset != 0U) return 6;
  if (desc.src_length != 0x00020000UL) return 7;
  if (desc.dst_handle != 0x40000020UL) return 8;
  if (desc.dst_offset != 0U) return 9;
  if (desc.algorithm != ZZ9K_CRYPTO_HASH_SHA256) return 10;
  if (desc.flags != 0U) return 11;
  if (desc.key_handle != 0U) return 12;
  if (desc.key_length != 0U) return 13;

  memset(&desc, 0xff, sizeof(desc));
  zz9k_bench_build_crypto_hmac_desc(&desc, ZZ9K_CRYPTO_HASH_SHA1,
                                    0x40000030UL, 0x00004000UL,
                                    0x40000040UL, 0x40000050UL, 64U);

  if (desc.src_handle != 0x40000030UL) return 14;
  if (desc.src_offset != 0U) return 15;
  if (desc.src_length != 0x00004000UL) return 16;
  if (desc.dst_handle != 0x40000040UL) return 17;
  if (desc.dst_offset != 0U) return 18;
  if (desc.algorithm != ZZ9K_CRYPTO_HASH_SHA1) return 19;
  if (desc.flags != ZZ9K_CRYPTO_HASH_FLAG_HMAC) return 20;
  if (desc.key_handle != 0x40000050UL) return 21;
  if (desc.key_offset != 0U) return 22;
  if (desc.key_length != 64U) return 23;

  memset(&desc, 0xff, sizeof(desc));
  zz9k_bench_build_crypto_hmac_chunk_desc(
      &desc, ZZ9K_CRYPTO_HASH_SHA256, 0x40000060UL, 0x00008000UL,
      0x00004000UL, 0x40000070UL, 0x00000060UL, 0x40000080UL, 64U);

  if (desc.src_handle != 0x40000060UL) return 24;
  if (desc.src_offset != 0x00008000UL) return 25;
  if (desc.src_length != 0x00004000UL) return 26;
  if (desc.dst_handle != 0x40000070UL) return 27;
  if (desc.dst_offset != 0x00000060UL) return 28;
  if (desc.algorithm != ZZ9K_CRYPTO_HASH_SHA256) return 29;
  if (desc.flags != ZZ9K_CRYPTO_HASH_FLAG_HMAC) return 30;
  if (desc.key_handle != 0x40000080UL) return 31;
  if (desc.key_offset != 0U) return 32;
  if (desc.key_length != 64U) return 33;

  memset(&desc, 0xff, sizeof(desc));
  zz9k_bench_build_crypto_poly1305_desc(
      &desc, 0x40000090UL, 0x00002000UL, 0x00004000UL,
      0x40000091UL, 0x00000010UL, 0x40000092UL);

  if (desc.src_handle != 0x40000090UL) return 34;
  if (desc.src_offset != 0x00002000UL) return 35;
  if (desc.src_length != 0x00004000UL) return 36;
  if (desc.dst_handle != 0x40000091UL) return 37;
  if (desc.dst_offset != 0x00000010UL) return 38;
  if (desc.algorithm != ZZ9K_CRYPTO_HASH_POLY1305) return 39;
  if (desc.flags != 0U) return 40;
  if (desc.key_handle != 0x40000092UL) return 41;
  if (desc.key_offset != 0U) return 42;
  if (desc.key_length != 32U) return 43;

  memset(&stream, 0xff, sizeof(stream));
  zz9k_bench_build_crypto_stream_desc(
      &stream, 0x40000090UL, 0x00004000UL, 0x00004000UL,
      0x40000091UL, 0x00008000UL, 0x40000092UL, 0x00000020UL,
      0x40000093UL, 0x0000000cUL, 7U);

  if (stream.src_handle != 0x40000090UL) return 44;
  if (stream.src_offset != 0x00004000UL) return 45;
  if (stream.src_length != 0x00004000UL) return 46;
  if (stream.dst_handle != 0x40000091UL) return 47;
  if (stream.dst_offset != 0x00008000UL) return 48;
  if (stream.key_handle != 0x40000092UL) return 49;
  if (stream.key_offset != 0x00000020UL) return 50;
  if (stream.nonce_handle != 0x40000093UL) return 51;
  if (stream.nonce_offset != 0x0000000cUL) return 52;
  if (stream.counter != 7U) return 53;
  if (stream.algorithm != ZZ9K_CRYPTO_STREAM_CHACHA20) return 54;
  if (stream.flags != 0U) return 55;

  memset(&aead, 0xff, sizeof(aead));
  zz9k_bench_build_crypto_aead_desc(
      &aead, 0x400000a0UL, 0x00004000UL, 0x00004000UL,
      0x400000a1UL, 0x00008000UL, 0x400000a2UL, 0x00000010UL,
      12U, 0x400000a3UL, 0x00000020UL, 0x400000a4UL, 0U);

  if (aead.src_handle != 0x400000a0UL) return 56;
  if (aead.src_offset != 0x00004000UL) return 57;
  if (aead.src_length != 0x00004000UL) return 58;
  if (aead.dst_handle != 0x400000a1UL) return 59;
  if (aead.dst_offset != 0x00008000UL) return 60;
  if (aead.aad_handle != 0x400000a2UL) return 61;
  if (aead.aad_offset != 0x00000010UL) return 62;
  if (aead.aad_length != 12U) return 63;
  if (aead.key_handle != 0x400000a3UL) return 64;
  if (aead.key_offset != 0x00000020UL) return 65;
  if (aead.nonce_handle != 0x400000a4UL) return 66;
  if (aead.flags != 0U) return 67;

  return 0;
}

static int test_crypto_output_buffer_fits_aead_pipeline(void)
{
  unsigned long aead_record_bytes;
  unsigned long required_bytes;

  aead_record_bytes = ZZ9K_BENCH_TLS_RECORD_BYTES +
                      ZZ9K_BENCH_POLY1305_TAG_BYTES;
  required_bytes = ZZ9K_BENCH_CRYPTO_PIPE_DEPTH * aead_record_bytes;
  if (ZZ9K_BENCH_CRYPTO_OUTPUT_BYTES < required_bytes) return 1;

  return 0;
}

static int test_probe_microsecond_conversion(void)
{
  if (zz9k_bench_ticks_to_us(709379ULL, 709379U) != 1000000U) return 1;
  if (zz9k_bench_ticks_to_us(709ULL, 709379U) != 999U) return 2;
  if (zz9k_bench_ticks_to_us(0ULL, 709379U) != 0U) return 3;
  if (zz9k_bench_ticks_to_us(100ULL, 0U) != 0U) return 4;
  if (zz9k_bench_ticks_to_us(5000000000000ULL, 1U) != UINT32_MAX) return 5;
  return 0;
}

static int test_probe_stats_track_min_avg_max(void)
{
  ZZ9KBenchProbeStats stats;

  zz9k_bench_probe_stats_init(&stats);
  if (zz9k_bench_probe_stats_avg(&stats) != 0ULL) return 1;

  zz9k_bench_probe_stats_add(&stats, 300ULL);
  zz9k_bench_probe_stats_add(&stats, 100ULL);
  zz9k_bench_probe_stats_add(&stats, 200ULL);

  if (stats.samples != 3U) return 2;
  if (stats.min != 100ULL) return 3;
  if (stats.max != 300ULL) return 4;
  if (zz9k_bench_probe_stats_avg(&stats) != 200ULL) return 5;
  return 0;
}

int main(void)
{
  int result;

  result = test_iteration_parser_defaults_and_clamps();
  if (result) return 10 + result;

  result = test_rate_calculations_use_ticks_per_second();
  if (result) return 30 + result;

  result = test_high_resolution_tick_helpers_use_64_bit_values();
  if (result) return 45 + result;

  result = test_scale_descriptor_uses_requested_filter();
  if (result) return 60 + result;

  result = test_bgra_surface_pattern_uses_native_alpha_byte();
  if (result) return 70 + result;

  result = test_local_surface_copy_descriptor_uses_rtg_geometry();
  if (result) return 75 + result;

  result = test_pipelined_ping_batch_size_is_bounded();
  if (result) return 90 + result;

  result = test_pipelined_ping_request_and_reply_validation();
  if (result) return 120 + result;

  result = test_crypto_descriptor_helpers();
  if (result) return 140 + result;

  result = test_crypto_output_buffer_fits_aead_pipeline();
  if (result) return 210 + result;

  result = test_probe_microsecond_conversion();
  if (result) return 230 + result;

  result = test_probe_stats_track_min_avg_max();
  if (result) return 240 + result;

  return 0;
}
