/*
 * ZZ9000 SDK crypto hash smoke tool.
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

static void print_hex(const uint8_t *data, uint32_t length)
{
  uint32_t i;

  for (i = 0; i < length; i++) {
    printf("%02lx", (unsigned long)data[i]);
  }
}

static int digest_equals(const uint8_t *a, const uint8_t *b, uint32_t length)
{
  uint32_t i;

  for (i = 0; i < length; i++) {
    if (a[i] != b[i]) {
      return 0;
    }
  }

  return 1;
}

static const char *algorithm_name(uint32_t algorithm)
{
  switch (algorithm) {
    case ZZ9K_CRYPTO_HASH_SHA1:
      return "sha1";
    case ZZ9K_CRYPTO_HASH_SHA256:
      return "sha256";
    case ZZ9K_CRYPTO_HASH_POLY1305:
      return "poly1305";
    default:
      return "unknown";
  }
}

static int parse_algorithm(const char *name, uint32_t *algorithm)
{
  if (strcmp(name, "sha1") == 0) {
    *algorithm = ZZ9K_CRYPTO_HASH_SHA1;
    return 1;
  }
  if (strcmp(name, "sha256") == 0) {
    *algorithm = ZZ9K_CRYPTO_HASH_SHA256;
    return 1;
  }
  if (strcmp(name, "poly1305") == 0) {
    *algorithm = ZZ9K_CRYPTO_HASH_POLY1305;
    return 1;
  }

  return 0;
}

static void usage(const char *name)
{
  printf("usage: %s [--alg sha1|sha256|poly1305] [text]\n", name);
  printf("       %s [--alg sha1|sha256] --hmac <key> <text>\n", name);
  printf("       %s --alg poly1305\n", name);
}

int main(int argc, char **argv)
{
  static const uint8_t expected_sha1_abc[20] = {
    0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a,
    0xba, 0x3e, 0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c,
    0x9c, 0xd0, 0xd8, 0x9d
  };
  static const uint8_t expected_sha256_abc[32] = {
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
    0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
    0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
  };
  static const uint8_t poly1305_key_rfc8439[32] = {
    0x85, 0xd6, 0xbe, 0x78, 0x57, 0x55, 0x6d, 0x33,
    0x7f, 0x44, 0x52, 0xfe, 0x42, 0xd5, 0x06, 0xa8,
    0x01, 0x03, 0x80, 0x8a, 0xfb, 0x0d, 0xb2, 0xfd,
    0x4a, 0xbf, 0xf6, 0xaf, 0x41, 0x49, 0xf5, 0x1b
  };
  static const uint8_t expected_poly1305_rfc8439[16] = {
    0xa8, 0x06, 0x1d, 0xc1, 0x30, 0x51, 0x36, 0xc6,
    0xc2, 0x2b, 0x8b, 0xaf, 0x0c, 0x01, 0x27, 0xa9
  };
  ZZ9KContext *ctx = 0;
  ZZ9KCaps caps;
  ZZ9KServiceInfo service;
  ZZ9KSharedBuffer input;
  ZZ9KSharedBuffer output;
  ZZ9KSharedBuffer key_buffer;
  ZZ9KCryptoHashDesc desc;
  ZZ9KCryptoResult result;
  uint8_t digest[32];
  const char *text;
  const char *key;
  const uint8_t *binary_key;
  uint32_t text_length;
  uint32_t key_length;
  uint32_t algorithm;
  uint32_t expected_digest_length;
  int use_hmac;
  int status;
  int exit_code = 1;
  int arg;
  int text_set;

  text = "abc";
  key = 0;
  binary_key = 0;
  algorithm = ZZ9K_CRYPTO_HASH_SHA256;
  use_hmac = 0;
  text_set = 0;
  arg = 1;
  while (arg < argc) {
    if (strcmp(argv[arg], "--alg") == 0) {
      arg++;
      if (arg >= argc || !parse_algorithm(argv[arg], &algorithm)) {
        usage(argv[0]);
        return 2;
      }
      arg++;
    } else if (strcmp(argv[arg], "--hmac") == 0) {
      arg++;
      if (arg + 1 >= argc) {
        usage(argv[0]);
        return 2;
      }
      use_hmac = 1;
      key = argv[arg++];
      text = argv[arg++];
      text_set = 1;
    } else {
      if (text_set) {
        usage(argv[0]);
        return 2;
      }
      text = argv[arg++];
      text_set = 1;
    }
  }

  expected_digest_length = zz9k_crypto_digest_length(algorithm);
  if (expected_digest_length == 0) {
    usage(argv[0]);
    return 2;
  }
  if (algorithm == ZZ9K_CRYPTO_HASH_POLY1305) {
    if (use_hmac || text_set) {
      usage(argv[0]);
      return 2;
    }
    text = "Cryptographic Forum Research Group";
    binary_key = poly1305_key_rfc8439;
    key_length = 32U;
  }

  text_length = (uint32_t)strlen(text);
  if (algorithm != ZZ9K_CRYPTO_HASH_POLY1305) {
    key_length = key ? (uint32_t)strlen(key) : 0;
  }
  if (text_length == 0 || ((use_hmac || binary_key) && key_length == 0)) {
    printf("empty input and empty HMAC keys are not supported yet\n");
    return 2;
  }

  memset(&input, 0, sizeof(input));
  memset(&output, 0, sizeof(output));
  memset(&key_buffer, 0, sizeof(key_buffer));

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

  status = zz9k_alloc_shared(ctx, text_length, 16, 0, &input);
  if (status != ZZ9K_STATUS_OK) {
    printf("alloc input failed: %s (%d)\n", zz9k_status_name(status), status);
    goto cleanup;
  }
  status = zz9k_alloc_shared(ctx, expected_digest_length, 16, 0, &output);
  if (status != ZZ9K_STATUS_OK) {
    printf("alloc output failed: %s (%d)\n", zz9k_status_name(status), status);
    goto cleanup;
  }
  if (!zz9k_shared_copy_to(&input, 0U, text, text_length)) {
    printf("input copy failed\n");
    goto cleanup;
  }

  if (use_hmac || binary_key) {
    status = zz9k_alloc_shared(ctx, key_length, 16, 0, &key_buffer);
    if (status != ZZ9K_STATUS_OK) {
      printf("alloc key failed: %s (%d)\n", zz9k_status_name(status), status);
      goto cleanup;
    }
    if (!zz9k_shared_copy_to(&key_buffer, 0U,
                             binary_key ? binary_key : (const uint8_t *)key,
                             key_length)) {
      printf("key copy failed\n");
      goto cleanup;
    }
  }

  if (algorithm == ZZ9K_CRYPTO_HASH_POLY1305) {
    if (!zz9k_crypto_build_poly1305_desc(&desc, input.handle, 0U,
                                         text_length, output.handle, 0U,
                                         key_buffer.handle, 0U)) {
      printf("could not build Poly1305 descriptor\n");
      goto cleanup;
    }
  } else if (use_hmac) {
    if (!zz9k_crypto_build_hmac_desc(&desc, algorithm, input.handle, 0U,
                                     text_length, output.handle, 0U,
                                     key_buffer.handle, 0U, key_length)) {
      printf("could not build HMAC descriptor\n");
      goto cleanup;
    }
  } else {
    if (!zz9k_crypto_build_hash_desc(&desc, algorithm, input.handle, 0U,
                                     text_length, output.handle, 0U)) {
      printf("could not build hash descriptor\n");
      goto cleanup;
    }
  }

  memset(&result, 0, sizeof(result));
  status = zz9k_crypto_hash(ctx, &desc, &result);
  if (status != ZZ9K_STATUS_OK) {
    printf("hash failed: %s (%d)\n", zz9k_status_name(status), status);
    goto cleanup;
  }
  if (result.bytes_written != expected_digest_length ||
      result.algorithm != algorithm) {
    printf("unexpected hash result metadata\n");
    goto cleanup;
  }

  memset(digest, 0, sizeof(digest));
  if (!zz9k_shared_copy_from(digest, &output, 0U, expected_digest_length)) {
    printf("digest copy failed\n");
    goto cleanup;
  }
  printf("%s %s(\"%s\") = ",
         algorithm == ZZ9K_CRYPTO_HASH_POLY1305 ? "mac" :
         (use_hmac ? "hmac" : "hash"),
         algorithm_name(algorithm), text);
  print_hex(digest, expected_digest_length);
  printf("\n");

  if (algorithm == ZZ9K_CRYPTO_HASH_POLY1305) {
    if (!digest_equals(digest, expected_poly1305_rfc8439,
                       expected_digest_length)) {
      printf("digest mismatch for built-in Poly1305 vector\n");
      goto cleanup;
    }
    printf("known vector ok\n");
  } else if (!use_hmac && strcmp(text, "abc") == 0) {
    const uint8_t *expected = algorithm == ZZ9K_CRYPTO_HASH_SHA1 ?
                              expected_sha1_abc : expected_sha256_abc;
    if (!digest_equals(digest, expected, expected_digest_length)) {
      printf("digest mismatch for built-in abc vector\n");
      goto cleanup;
    }
    printf("known vector ok\n");
  }

  exit_code = 0;

cleanup:
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
