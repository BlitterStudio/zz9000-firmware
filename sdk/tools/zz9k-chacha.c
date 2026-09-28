/*
 * ZZ9000 SDK ChaCha20 stream cipher smoke tool.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/caps.h"
#include "zz9k/host.h"
#include "zz9k/crypto.h"
#include "zz9k/shared.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int bytes_equal(const uint8_t *a, const uint8_t *b, uint32_t length)
{
  uint32_t i;

  for (i = 0; i < length; i++) {
    if (a[i] != b[i]) {
      return 0;
    }
  }

  return 1;
}

static void print_hex_sample(const uint8_t *data)
{
  uint32_t i;

  for (i = 0; i < 16U; i++) {
    printf("%02lx", (unsigned long)data[i]);
  }
}

int main(void)
{
  static const uint8_t key[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
  };
  static const uint8_t nonce[12] = {
    0x00, 0x00, 0x00, 0x09, 0x00, 0x00,
    0x00, 0x4a, 0x00, 0x00, 0x00, 0x00
  };
  static const uint8_t expected[64] = {
    0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15,
    0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4,
    0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03,
    0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e,
    0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09,
    0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2,
    0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e, 0xb9,
    0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e
  };
  uint8_t zero[64];
  uint8_t actual[64];
  ZZ9KContext *ctx = 0;
  ZZ9KCaps caps;
  ZZ9KServiceInfo service;
  ZZ9KSharedBuffer input;
  ZZ9KSharedBuffer output;
  ZZ9KSharedBuffer key_buffer;
  ZZ9KSharedBuffer nonce_buffer;
  ZZ9KCryptoStreamDesc desc;
  ZZ9KCryptoResult result;
  int status;
  int exit_code = 1;

  memset(zero, 0, sizeof(zero));
  memset(&input, 0, sizeof(input));
  memset(&output, 0, sizeof(output));
  memset(&key_buffer, 0, sizeof(key_buffer));
  memset(&nonce_buffer, 0, sizeof(nonce_buffer));

  status = zz9k_open(&ctx);
  if (status != ZZ9K_STATUS_OK) {
    printf("open failed: %s (%d)\n", zz9k_status_name(status), status);
    return 10;
  }

  status = zz9k_query_caps(ctx, &caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("query caps failed: %s (%d)\n", zz9k_status_name(status), status);
    goto cleanup;
  }
  if ((caps.capability_bits & ZZ9K_CAP_CRYPTO) == 0) {
    printf("%s capability not advertised\n",
           zz9k_capability_name(ZZ9K_CAP_CRYPTO));
    exit_code = 3;
    goto cleanup;
  }
  if (!zz9k_shared_heap_board_visible(ctx)) {
    printf("note: this diagnostic stages its buffers in the default shared\n"
           "heap, which is not CPU-visible on Zorro II; its accelerated\n"
           "sections cannot run here (docs/zz9k-zorro2-services.md)\n");
  }

  status = zz9k_query_service(ctx, ZZ9K_SERVICE_CRYPTO, &service);
  if (status != ZZ9K_STATUS_OK) {
    printf("query crypto service failed: %s (%d)\n",
           zz9k_status_name(status), status);
    goto cleanup;
  }

  status = zz9k_alloc_shared(ctx, sizeof(zero), 16, 0, &input);
  if (status != ZZ9K_STATUS_OK) {
    printf("alloc input failed: %s (%d)\n", zz9k_status_name(status), status);
    goto cleanup;
  }
  status = zz9k_alloc_shared(ctx, sizeof(actual), 16, 0, &output);
  if (status != ZZ9K_STATUS_OK) {
    printf("alloc output failed: %s (%d)\n", zz9k_status_name(status), status);
    goto cleanup;
  }
  status = zz9k_alloc_shared(ctx, sizeof(key), 16, 0, &key_buffer);
  if (status != ZZ9K_STATUS_OK) {
    printf("alloc key failed: %s (%d)\n", zz9k_status_name(status), status);
    goto cleanup;
  }
  status = zz9k_alloc_shared(ctx, sizeof(nonce), 16, 0, &nonce_buffer);
  if (status != ZZ9K_STATUS_OK) {
    printf("alloc nonce failed: %s (%d)\n", zz9k_status_name(status), status);
    goto cleanup;
  }

  if (!zz9k_shared_copy_to(&input, 0U, zero, sizeof(zero)) ||
      !zz9k_shared_copy_to(&key_buffer, 0U, key, sizeof(key)) ||
      !zz9k_shared_copy_to(&nonce_buffer, 0U, nonce, sizeof(nonce))) {
    printf("shared input copy failed\n");
    goto cleanup;
  }

  if (!zz9k_crypto_build_chacha20_desc(
          &desc, input.handle, 0U, (uint32_t)sizeof(zero), output.handle,
          0U, key_buffer.handle, 0U, nonce_buffer.handle, 0U, 1U)) {
    printf("could not build chacha encrypt descriptor\n");
    goto cleanup;
  }

  memset(&result, 0, sizeof(result));
  status = zz9k_crypto_stream(ctx, &desc, &result);
  if (status != ZZ9K_STATUS_OK) {
    printf("chacha encrypt failed: %s (%d)\n",
           zz9k_status_name(status), status);
    goto cleanup;
  }
  if (result.bytes_written != sizeof(actual) ||
      result.algorithm != ZZ9K_CRYPTO_STREAM_CHACHA20) {
    printf("unexpected chacha result metadata\n");
    goto cleanup;
  }

  memset(actual, 0, sizeof(actual));
  if (!zz9k_shared_copy_from(actual, &output, 0U, sizeof(actual))) {
    printf("shared output copy failed\n");
    goto cleanup;
  }
  printf("chacha20 block sample = ");
  print_hex_sample(actual);
  printf("\n");
  if (!bytes_equal(actual, expected, sizeof(expected))) {
    printf("chacha20 RFC vector mismatch\n");
    goto cleanup;
  }

  if (!zz9k_crypto_build_chacha20_desc(
          &desc, output.handle, 0U, (uint32_t)sizeof(zero), input.handle,
          0U, key_buffer.handle, 0U, nonce_buffer.handle, 0U, 1U)) {
    printf("could not build chacha decrypt descriptor\n");
    goto cleanup;
  }
  memset(&result, 0, sizeof(result));
  status = zz9k_crypto_stream(ctx, &desc, &result);
  if (status != ZZ9K_STATUS_OK) {
    printf("chacha decrypt failed: %s (%d)\n",
           zz9k_status_name(status), status);
    goto cleanup;
  }

  memset(actual, 0, sizeof(actual));
  if (!zz9k_shared_copy_from(actual, &input, 0U, sizeof(actual))) {
    printf("shared roundtrip copy failed\n");
    goto cleanup;
  }
  if (!bytes_equal(actual, zero, sizeof(zero))) {
    printf("chacha20 roundtrip mismatch\n");
    goto cleanup;
  }

  printf("known vector ok\n");
  exit_code = 0;

cleanup:
  if (nonce_buffer.handle != 0) {
    zz9k_free_shared(ctx, nonce_buffer.handle);
  }
  if (key_buffer.handle != 0) {
    zz9k_free_shared(ctx, key_buffer.handle);
  }
  if (output.handle != 0) {
    zz9k_free_shared(ctx, output.handle);
  }
  if (input.handle != 0) {
    zz9k_free_shared(ctx, input.handle);
  }
  zz9k_close(ctx);
  return exit_code;
}
