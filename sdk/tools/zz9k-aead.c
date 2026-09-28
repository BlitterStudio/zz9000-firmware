/*
 * ZZ9000 SDK ChaCha20-Poly1305 AEAD smoke tool.
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
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f
  };
  static const uint8_t nonce[12] = {
    0x07, 0x00, 0x00, 0x00, 0x40, 0x41,
    0x42, 0x43, 0x44, 0x45, 0x46, 0x47
  };
  static const uint8_t aad[12] = {
    0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1,
    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7
  };
  static const uint8_t plaintext[] =
    "Ladies and Gentlemen of the class of '99: If I could offer "
    "you only one tip for the future, sunscreen would be it.";
  static const uint8_t expected_ciphertext[114] = {
    0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb,
    0x7b, 0x86, 0xaf, 0xbc, 0x53, 0xef, 0x7e, 0xc2,
    0xa4, 0xad, 0xed, 0x51, 0x29, 0x6e, 0x08, 0xfe,
    0xa9, 0xe2, 0xb5, 0xa7, 0x36, 0xee, 0x62, 0xd6,
    0x3d, 0xbe, 0xa4, 0x5e, 0x8c, 0xa9, 0x67, 0x12,
    0x82, 0xfa, 0xfb, 0x69, 0xda, 0x92, 0x72, 0x8b,
    0x1a, 0x71, 0xde, 0x0a, 0x9e, 0x06, 0x0b, 0x29,
    0x05, 0xd6, 0xa5, 0xb6, 0x7e, 0xcd, 0x3b, 0x36,
    0x92, 0xdd, 0xbd, 0x7f, 0x2d, 0x77, 0x8b, 0x8c,
    0x98, 0x03, 0xae, 0xe3, 0x28, 0x09, 0x1b, 0x58,
    0xfa, 0xb3, 0x24, 0xe4, 0xfa, 0xd6, 0x75, 0x94,
    0x55, 0x85, 0x80, 0x8b, 0x48, 0x31, 0xd7, 0xbc,
    0x3f, 0xf4, 0xde, 0xf0, 0x8e, 0x4b, 0x7a, 0x9d,
    0xe5, 0x76, 0xd2, 0x65, 0x86, 0xce, 0xc6, 0x4b,
    0x61, 0x16
  };
  static const uint8_t expected_tag[16] = {
    0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a,
    0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91
  };
  uint8_t actual[sizeof(expected_ciphertext) + sizeof(expected_tag)];
  uint8_t roundtrip[sizeof(plaintext) - 1U];
  ZZ9KContext *ctx = 0;
  ZZ9KCaps caps;
  ZZ9KServiceInfo service;
  ZZ9KSharedBuffer input;
  ZZ9KSharedBuffer output;
  ZZ9KSharedBuffer aad_buffer;
  ZZ9KSharedBuffer key_buffer;
  ZZ9KSharedBuffer nonce_buffer;
  ZZ9KSharedBuffer roundtrip_buffer;
  ZZ9KCryptoAeadDesc desc;
  ZZ9KCryptoResult result;
  int status;
  int exit_code = 1;

  memset(&input, 0, sizeof(input));
  memset(&output, 0, sizeof(output));
  memset(&aad_buffer, 0, sizeof(aad_buffer));
  memset(&key_buffer, 0, sizeof(key_buffer));
  memset(&nonce_buffer, 0, sizeof(nonce_buffer));
  memset(&roundtrip_buffer, 0, sizeof(roundtrip_buffer));

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

  status = zz9k_alloc_shared(ctx, sizeof(plaintext) - 1U, 16, 0, &input);
  if (status != ZZ9K_STATUS_OK) {
    printf("alloc input failed: %s (%d)\n", zz9k_status_name(status), status);
    goto cleanup;
  }
  status = zz9k_alloc_shared(ctx, sizeof(actual), 16, 0, &output);
  if (status != ZZ9K_STATUS_OK) {
    printf("alloc output failed: %s (%d)\n", zz9k_status_name(status), status);
    goto cleanup;
  }
  status = zz9k_alloc_shared(ctx, sizeof(aad), 16, 0, &aad_buffer);
  if (status != ZZ9K_STATUS_OK) {
    printf("alloc aad failed: %s (%d)\n", zz9k_status_name(status), status);
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
  status = zz9k_alloc_shared(ctx, sizeof(roundtrip), 16, 0,
                             &roundtrip_buffer);
  if (status != ZZ9K_STATUS_OK) {
    printf("alloc roundtrip failed: %s (%d)\n",
           zz9k_status_name(status), status);
    goto cleanup;
  }

  if (!zz9k_shared_copy_to(&input, 0U, plaintext,
                           (uint32_t)sizeof(plaintext) - 1U) ||
      !zz9k_shared_copy_to(&aad_buffer, 0U, aad, sizeof(aad)) ||
      !zz9k_shared_copy_to(&key_buffer, 0U, key, sizeof(key)) ||
      !zz9k_shared_copy_to(&nonce_buffer, 0U, nonce, sizeof(nonce))) {
    printf("shared input copy failed\n");
    goto cleanup;
  }

  if (!zz9k_crypto_build_chacha20_poly1305_desc(
          &desc, input.handle, 0U, (uint32_t)sizeof(plaintext) - 1U,
          output.handle, 0U, aad_buffer.handle, 0U,
          (uint32_t)sizeof(aad), key_buffer.handle, 0U,
          nonce_buffer.handle, 0U)) {
    printf("could not build aead encrypt descriptor\n");
    goto cleanup;
  }

  memset(&result, 0, sizeof(result));
  status = zz9k_crypto_aead(ctx, &desc, &result);
  if (status != ZZ9K_STATUS_OK) {
    printf("aead encrypt failed: %s (%d)\n",
           zz9k_status_name(status), status);
    goto cleanup;
  }
  if (result.bytes_written != sizeof(actual) ||
      result.algorithm != ZZ9K_CRYPTO_AEAD_CHACHA20_POLY1305 ||
      result.flags != 0U) {
    printf("unexpected aead encrypt metadata\n");
    goto cleanup;
  }

  memset(actual, 0, sizeof(actual));
  if (!zz9k_shared_copy_from(actual, &output, 0U, sizeof(actual))) {
    printf("shared output copy failed\n");
    goto cleanup;
  }
  printf("aead ciphertext sample = ");
  print_hex_sample(actual);
  printf("\n");
  if (!bytes_equal(actual, expected_ciphertext, sizeof(expected_ciphertext)) ||
      !bytes_equal(actual + sizeof(expected_ciphertext), expected_tag,
                   sizeof(expected_tag))) {
    printf("aead RFC vector mismatch\n");
    goto cleanup;
  }

  if (!zz9k_crypto_build_chacha20_poly1305_desc(
          &desc, output.handle, 0U, (uint32_t)sizeof(expected_ciphertext),
          roundtrip_buffer.handle, 0U, aad_buffer.handle, 0U,
          (uint32_t)sizeof(aad), key_buffer.handle, 0U,
          nonce_buffer.handle, ZZ9K_CRYPTO_AEAD_FLAG_DECRYPT)) {
    printf("could not build aead decrypt descriptor\n");
    goto cleanup;
  }
  memset(&result, 0, sizeof(result));
  status = zz9k_crypto_aead(ctx, &desc, &result);
  if (status != ZZ9K_STATUS_OK) {
    printf("aead decrypt failed: %s (%d)\n",
           zz9k_status_name(status), status);
    goto cleanup;
  }
  if (result.bytes_written != sizeof(roundtrip) ||
      result.algorithm != ZZ9K_CRYPTO_AEAD_CHACHA20_POLY1305 ||
      result.flags != ZZ9K_CRYPTO_AEAD_FLAG_DECRYPT) {
    printf("unexpected aead decrypt metadata\n");
    goto cleanup;
  }

  memset(roundtrip, 0, sizeof(roundtrip));
  if (!zz9k_shared_copy_from(roundtrip, &roundtrip_buffer, 0U,
                             sizeof(roundtrip))) {
    printf("shared roundtrip copy failed\n");
    goto cleanup;
  }
  if (!bytes_equal(roundtrip, plaintext, sizeof(roundtrip))) {
    printf("aead roundtrip mismatch\n");
    goto cleanup;
  }

  printf("known vector ok\n");
  exit_code = 0;

cleanup:
  if (roundtrip_buffer.handle != 0) {
    zz9k_free_shared(ctx, roundtrip_buffer.handle);
  }
  if (nonce_buffer.handle != 0) {
    zz9k_free_shared(ctx, nonce_buffer.handle);
  }
  if (key_buffer.handle != 0) {
    zz9k_free_shared(ctx, key_buffer.handle);
  }
  if (aad_buffer.handle != 0) {
    zz9k_free_shared(ctx, aad_buffer.handle);
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
