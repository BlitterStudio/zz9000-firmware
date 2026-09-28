/*
 * Tests for the public zz9k.library vector layout.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/library_vectors.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void expect_u32(const char *name, unsigned long actual,
                       unsigned long expected)
{
  if (actual != expected) {
    printf("%s: got %lu expected %lu\n", name, actual, expected);
    failures++;
  }
}

static void expect_str(const char *name, const char *actual,
                       const char *expected)
{
  if (strcmp(actual, expected) != 0) {
    printf("%s: got %s expected %s\n", name, actual, expected);
    failures++;
  }
}

static void test_library_identity(void)
{
  expect_str("name", ZZ9K_LIBRARY_NAME, "zz9k.library");
  expect_u32("version", ZZ9K_LIBRARY_VERSION, 2);
  expect_u32("revision", ZZ9K_LIBRARY_REVISION, 29);
}

static void test_standard_lvos(void)
{
  expect_u32("open", ZZ9K_LVO_OPEN, -6);
  expect_u32("close", ZZ9K_LVO_CLOSE, -12);
  expect_u32("expunge", ZZ9K_LVO_EXPUNGE, -18);
  expect_u32("extfunc", ZZ9K_LVO_EXTFUNC, -24);
}

static void test_public_lvos(void)
{
  expect_u32("first", ZZ9K_LVO_QUERY_CAPS, -30);
  expect_u32("query_service", ZZ9K_LVO_QUERY_SERVICE, -36);
  expect_u32("ping", ZZ9K_LVO_PING, -42);
  expect_u32("call", ZZ9K_LVO_CALL, -48);
  expect_u32("call_async", ZZ9K_LVO_CALL_ASYNC, -54);
  expect_u32("call_async_batch", ZZ9K_LVO_CALL_ASYNC_BATCH, -60);
  expect_u32("poll", ZZ9K_LVO_POLL, -66);
  expect_u32("alloc_shared", ZZ9K_LVO_ALLOC_SHARED, -72);
  expect_u32("free_shared", ZZ9K_LVO_FREE_SHARED, -78);
  expect_u32("mem_fill", ZZ9K_LVO_MEM_FILL, -84);
  expect_u32("mem_copy", ZZ9K_LVO_MEM_COPY, -90);
  expect_u32("alloc_surface", ZZ9K_LVO_ALLOC_SURFACE, -96);
  expect_u32("alloc_surface_ex", ZZ9K_LVO_ALLOC_SURFACE_EX, -102);
  expect_u32("free_surface", ZZ9K_LVO_FREE_SURFACE, -108);
  expect_u32("map_framebuffer_surface", ZZ9K_LVO_MAP_FRAMEBUFFER_SURFACE,
             -114);
  expect_u32("scale_image", ZZ9K_LVO_SCALE_IMAGE, -120);
  expect_u32("read_diag", ZZ9K_LVO_READ_DIAG, -126);
  expect_u32("call_async_msg", ZZ9K_LVO_CALL_ASYNC_MSG, -132);
  expect_u32("call_async_batch_msg", ZZ9K_LVO_CALL_ASYNC_BATCH_MSG, -138);
  expect_u32("cancel_async", ZZ9K_LVO_CANCEL_ASYNC, -144);
  expect_u32("wait_async", ZZ9K_LVO_WAIT_ASYNC, -150);
  expect_u32("wait_async_batch", ZZ9K_LVO_WAIT_ASYNC_BATCH, -156);
  expect_u32("decode_image", ZZ9K_LVO_DECODE_IMAGE, -162);
  expect_u32("crypto_hash", ZZ9K_LVO_CRYPTO_HASH, -168);
  expect_u32("crypto_hash_batch", ZZ9K_LVO_CRYPTO_HASH_BATCH, -174);
  expect_u32("crypto_stream", ZZ9K_LVO_CRYPTO_STREAM, -180);
  expect_u32("crypto_stream_batch", ZZ9K_LVO_CRYPTO_STREAM_BATCH, -186);
  expect_u32("crypto_aead", ZZ9K_LVO_CRYPTO_AEAD, -192);
  expect_u32("crypto_aead_batch", ZZ9K_LVO_CRYPTO_AEAD_BATCH, -198);
  expect_u32("fill_surface", ZZ9K_LVO_FILL_SURFACE, -204);
  expect_u32("copy_surface", ZZ9K_LVO_COPY_SURFACE, -210);
  expect_u32("image_session_begin", ZZ9K_LVO_IMAGE_SESSION_BEGIN, -216);
  expect_u32("image_session_feed", ZZ9K_LVO_IMAGE_SESSION_FEED, -222);
  expect_u32("image_session_close", ZZ9K_LVO_IMAGE_SESSION_CLOSE, -228);
  expect_u32("scale_image_clipped", ZZ9K_LVO_SCALE_IMAGE_CLIPPED, -234);
  expect_u32("decode_jpeg", ZZ9K_LVO_DECODE_JPEG, -240);
  expect_u32("decode_png", ZZ9K_LVO_DECODE_PNG, -246);
  expect_u32("decode_gif", ZZ9K_LVO_DECODE_GIF, -252);
  expect_u32("decode_mp3", ZZ9K_LVO_DECODE_MP3, -258);
  expect_u32("audio_stream_begin", ZZ9K_LVO_AUDIO_STREAM_BEGIN, -264);
  expect_u32("audio_stream_feed", ZZ9K_LVO_AUDIO_STREAM_FEED, -270);
  expect_u32("audio_stream_read", ZZ9K_LVO_AUDIO_STREAM_READ, -276);
  expect_u32("audio_stream_close", ZZ9K_LVO_AUDIO_STREAM_CLOSE, -282);
  expect_u32("crypto_key_exchange", ZZ9K_LVO_CRYPTO_KEY_EXCHANGE, -288);
  expect_u32("crypto_verify", ZZ9K_LVO_CRYPTO_VERIFY, -294);
  expect_u32("audio_stream_play", ZZ9K_LVO_AUDIO_STREAM_PLAY, -300);
  expect_u32("audio_stream_stop", ZZ9K_LVO_AUDIO_STREAM_STOP, -306);
  expect_u32("query_palette", ZZ9K_LVO_QUERY_PALETTE, -312);
  expect_u32("function_count", ZZ9K_LVO_FUNCTION_COUNT, 48);
}

int main(void)
{
  test_library_identity();
  test_standard_lvos();
  test_public_lvos();

  if (failures) {
    return 1;
  }
  return 0;
}
