/*
 * Public zz9k.library crypto hash and batch example.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proto/zz9k.h"
#include "zz9k/caps.h"
#include "zz9k/crypto.h"
#include "zz9k/shared.h"
#include "zz9k/text.h"
#include <proto/exec.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct Library *ZZ9KBase;

static const uint8_t input_abc[] = { 'a', 'b', 'c' };

static const uint8_t sha1_abc[20] = {
  0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a,
  0xba, 0x3e, 0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c,
  0x9c, 0xd0, 0xd8, 0x9d
};

static const uint8_t sha256_abc[32] = {
  0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
  0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
  0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
  0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
};

static const uint8_t chacha_key[32] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
  0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
};

static const uint8_t chacha_nonce[12] = {
  0x00, 0x00, 0x00, 0x09, 0x00, 0x00,
  0x00, 0x4a, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t chacha_expected[64] = {
  0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15,
  0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4,
  0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03,
  0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e,
  0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09,
  0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2,
  0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e, 0xb9,
  0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e
};

static const uint8_t aead_key[32] = {
  0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
  0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
  0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
  0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f
};

static const uint8_t aead_nonce[12] = {
  0x07, 0x00, 0x00, 0x00, 0x40, 0x41,
  0x42, 0x43, 0x44, 0x45, 0x46, 0x47
};

static const uint8_t aead_aad[12] = {
  0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1,
  0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7
};

static const uint8_t aead_plaintext[] =
  "Ladies and Gentlemen of the class of '99: If I could offer "
  "you only one tip for the future, sunscreen would be it.";

static const uint8_t aead_expected_ciphertext[114] = {
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

static const uint8_t aead_expected_tag[16] = {
  0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a,
  0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91
};

static int copy_to_shared(ZZ9KSharedBuffer *buffer,
                          const uint8_t *bytes,
                          uint32_t length)
{
  return zz9k_shared_copy_to(buffer, 0U, bytes, length);
}

static int copy_from_shared(uint8_t *dst,
                            const ZZ9KSharedBuffer *buffer,
                            uint32_t offset,
                            uint32_t length)
{
  return zz9k_shared_copy_from(dst, buffer, offset, length);
}

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

static void print_hex(const uint8_t *bytes, uint32_t length)
{
  uint32_t i;

  for (i = 0; i < length; i++) {
    printf("%02lx", (unsigned long)bytes[i]);
  }
}

static int alloc_shared_buffer(ZZ9KSharedBuffer *buffer, uint32_t length,
                               const char *name)
{
  int status;

  memset(buffer, 0, sizeof(*buffer));
  status = ZZ9KAllocShared(length, 16U, 0U, buffer);
  if (status != ZZ9K_STATUS_OK) {
    printf("%s alloc failed: %s (%d)\n", name, zz9k_status_text(status),
           status);
    return 0;
  }

  return 1;
}

static void free_shared_buffer(ZZ9KSharedBuffer *buffer)
{
  if (buffer->handle != 0U) {
    ZZ9KFreeShared(buffer->handle);
    memset(buffer, 0, sizeof(*buffer));
  }
}

static void print_missing_capabilities(uint32_t missing)
{
  uint32_t remaining;
  uint32_t count;
  uint32_t i;
  int first;

  printf("missing required capabilities:");
  remaining = missing;
  count = zz9k_known_capability_count();
  first = 1;
  for (i = 0; i < count; i++) {
    uint32_t bit;
    const char *name;

    bit = zz9k_known_capability_bit(i);
    if (bit == 0U || (missing & bit) == 0U)
      continue;
    name = zz9k_capability_name(bit);
    if (name) {
      printf("%s%s", first ? " " : ",", name);
      remaining &= ~bit;
      first = 0;
    }
  }
  if (remaining != 0U) {
    printf("%s0x%08lx", first ? " " : ",", (unsigned long)remaining);
    first = 0;
  }
  if (first)
    printf(" none");
  printf("\n");
}

static int require_crypto_support(void)
{
  ZZ9KCaps caps;
  uint32_t required;
  uint32_t missing;
  int status;

  memset(&caps, 0, sizeof(caps));
  status = ZZ9KQueryCaps(&caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("ZZ9KQueryCaps failed: %s (%d)\n", zz9k_status_text(status),
           status);
    return 0;
  }

  required = ZZ9K_CAP_SHARED_ALLOC | ZZ9K_CAP_CRYPTO;
  if (zz9k_has_capabilities(caps.capability_bits, required)) {
    return 1;
  }

  missing = zz9k_missing_capabilities(caps.capability_bits, required);
  print_missing_capabilities(missing);
  return 0;
}

static void build_hash_desc(ZZ9KCryptoHashDesc *desc,
                            const ZZ9KSharedBuffer *input,
                            const ZZ9KSharedBuffer *output,
                            uint32_t dst_offset,
                            uint32_t algorithm)
{
  (void)zz9k_crypto_build_hash_desc(
      desc, algorithm, input->handle, 0U, (uint32_t)sizeof(input_abc),
      output->handle, dst_offset);
}

static int run_single_sha256(const ZZ9KSharedBuffer *input,
                             const ZZ9KSharedBuffer *output)
{
  ZZ9KCryptoHashDesc desc;
  ZZ9KCryptoResult result;
  uint8_t digest[32];
  int status;

  build_hash_desc(&desc, input, output, 0U, ZZ9K_CRYPTO_HASH_SHA256);
  memset(&result, 0, sizeof(result));
  status = ZZ9KCryptoHash(&desc, &result);
  if (status != ZZ9K_STATUS_OK) {
    printf("ZZ9KCryptoHash failed: %s (%d)\n", zz9k_status_text(status),
           status);
    return 0;
  }
  if (result.bytes_written != 32U ||
      result.algorithm != ZZ9K_CRYPTO_HASH_SHA256) {
    printf("unexpected SHA-256 result metadata\n");
    return 0;
  }

  memset(digest, 0, sizeof(digest));
  if (!copy_from_shared(digest, output, 0U, 32U)) {
    printf("SHA-256 digest copy failed\n");
    return 0;
  }
  if (!bytes_equal(digest, sha256_abc, 32U)) {
    printf("SHA-256 digest mismatch\n");
    return 0;
  }

  printf("single SHA-256 ok: ");
  print_hex(digest, 32U);
  printf("\n");
  return 1;
}

static int run_batch_hashes(const ZZ9KSharedBuffer *input,
                            const ZZ9KSharedBuffer *output)
{
  ZZ9KCryptoHashDesc descs[2];
  ZZ9KCryptoResult results[2];
  uint8_t sha1_digest[20];
  uint8_t sha256_digest[32];
  int status;

  build_hash_desc(&descs[0], input, output, 0U, ZZ9K_CRYPTO_HASH_SHA1);
  build_hash_desc(&descs[1], input, output, 32U, ZZ9K_CRYPTO_HASH_SHA256);
  memset(results, 0, sizeof(results));
  status = ZZ9KCryptoHashBatch(descs, results, 2U, 2U,
                               ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    printf("ZZ9KCryptoHashBatch failed: %s (%d)\n",
           zz9k_status_text(status), status);
    return 0;
  }
  if (results[0].bytes_written != 20U ||
      results[0].algorithm != ZZ9K_CRYPTO_HASH_SHA1 ||
      results[1].bytes_written != 32U ||
      results[1].algorithm != ZZ9K_CRYPTO_HASH_SHA256) {
    printf("unexpected batch result metadata\n");
    return 0;
  }

  memset(sha1_digest, 0, sizeof(sha1_digest));
  memset(sha256_digest, 0, sizeof(sha256_digest));
  if (!copy_from_shared(sha1_digest, output, 0U, 20U) ||
      !copy_from_shared(sha256_digest, output, 32U, 32U)) {
    printf("batch digest copy failed\n");
    return 0;
  }
  if (!bytes_equal(sha1_digest, sha1_abc, 20U) ||
      !bytes_equal(sha256_digest, sha256_abc, 32U)) {
    printf("batch digest mismatch\n");
    return 0;
  }

  printf("batch SHA-1/SHA-256 ok\n");
  return 1;
}

static int run_chacha20_stream(void)
{
  uint8_t zero[64];
  uint8_t actual[64];
  ZZ9KSharedBuffer input;
  ZZ9KSharedBuffer output;
  ZZ9KSharedBuffer key_buffer;
  ZZ9KSharedBuffer nonce_buffer;
  ZZ9KCryptoStreamDesc stream_desc;
  ZZ9KCryptoResult result;
  int status;
  int ok;

  memset(zero, 0, sizeof(zero));
  memset(&input, 0, sizeof(input));
  memset(&output, 0, sizeof(output));
  memset(&key_buffer, 0, sizeof(key_buffer));
  memset(&nonce_buffer, 0, sizeof(nonce_buffer));
  ok = 0;

  if (!alloc_shared_buffer(&input, sizeof(zero), "stream input") ||
      !alloc_shared_buffer(&output, sizeof(actual), "stream output") ||
      !alloc_shared_buffer(&key_buffer, sizeof(chacha_key), "stream key") ||
      !alloc_shared_buffer(&nonce_buffer, sizeof(chacha_nonce),
                           "stream nonce")) {
    goto out;
  }

  if (!copy_to_shared(&input, zero, sizeof(zero)) ||
      !copy_to_shared(&key_buffer, chacha_key, sizeof(chacha_key)) ||
      !copy_to_shared(&nonce_buffer, chacha_nonce, sizeof(chacha_nonce))) {
    printf("ChaCha20 input copy failed\n");
    goto out;
  }

  if (!zz9k_crypto_build_chacha20_desc(
          &stream_desc, input.handle, 0U, (uint32_t)sizeof(zero),
          output.handle, 0U, key_buffer.handle, 0U, nonce_buffer.handle,
          0U, 1U)) {
    printf("could not build ChaCha20 encrypt descriptor\n");
    goto out;
  }

  memset(&result, 0, sizeof(result));
  status = ZZ9KCryptoStream(&stream_desc, &result);
  if (status != ZZ9K_STATUS_OK) {
    printf("ZZ9KCryptoStream encrypt failed: %s (%d)\n",
           zz9k_status_text(status), status);
    goto out;
  }
  if (result.bytes_written != sizeof(actual) ||
      result.algorithm != ZZ9K_CRYPTO_STREAM_CHACHA20) {
    printf("unexpected ChaCha20 result metadata\n");
    goto out;
  }

  memset(actual, 0, sizeof(actual));
  if (!copy_from_shared(actual, &output, 0U, sizeof(actual))) {
    printf("ChaCha20 output copy failed\n");
    goto out;
  }
  if (!bytes_equal(actual, chacha_expected, sizeof(chacha_expected))) {
    printf("ChaCha20 vector mismatch\n");
    goto out;
  }

  if (!zz9k_crypto_build_chacha20_desc(
          &stream_desc, output.handle, 0U, (uint32_t)sizeof(zero),
          input.handle, 0U, key_buffer.handle, 0U, nonce_buffer.handle,
          0U, 1U)) {
    printf("could not build ChaCha20 decrypt descriptor\n");
    goto out;
  }
  memset(&result, 0, sizeof(result));
  status = ZZ9KCryptoStream(&stream_desc, &result);
  if (status != ZZ9K_STATUS_OK) {
    printf("ZZ9KCryptoStream decrypt failed: %s (%d)\n",
           zz9k_status_text(status), status);
    goto out;
  }

  memset(actual, 0, sizeof(actual));
  if (!copy_from_shared(actual, &input, 0U, sizeof(actual))) {
    printf("ChaCha20 roundtrip copy failed\n");
    goto out;
  }
  if (!bytes_equal(actual, zero, sizeof(zero))) {
    printf("ChaCha20 roundtrip mismatch\n");
    goto out;
  }

  printf("ChaCha20 stream ok\n");
  ok = 1;

out:
  free_shared_buffer(&nonce_buffer);
  free_shared_buffer(&key_buffer);
  free_shared_buffer(&output);
  free_shared_buffer(&input);
  return ok;
}

static int run_chacha20_poly1305_aead(void)
{
  uint8_t actual[sizeof(aead_expected_ciphertext) + sizeof(aead_expected_tag)];
  uint8_t roundtrip[sizeof(aead_plaintext) - 1U];
  ZZ9KSharedBuffer input;
  ZZ9KSharedBuffer output;
  ZZ9KSharedBuffer aad_buffer;
  ZZ9KSharedBuffer key_buffer;
  ZZ9KSharedBuffer nonce_buffer;
  ZZ9KSharedBuffer roundtrip_buffer;
  ZZ9KCryptoAeadDesc aead_desc;
  ZZ9KCryptoResult result;
  int status;
  int ok;

  memset(&input, 0, sizeof(input));
  memset(&output, 0, sizeof(output));
  memset(&aad_buffer, 0, sizeof(aad_buffer));
  memset(&key_buffer, 0, sizeof(key_buffer));
  memset(&nonce_buffer, 0, sizeof(nonce_buffer));
  memset(&roundtrip_buffer, 0, sizeof(roundtrip_buffer));
  ok = 0;

  if (!alloc_shared_buffer(&input, sizeof(aead_plaintext) - 1U,
                           "AEAD input") ||
      !alloc_shared_buffer(&output, sizeof(actual), "AEAD output") ||
      !alloc_shared_buffer(&aad_buffer, sizeof(aead_aad), "AEAD AAD") ||
      !alloc_shared_buffer(&key_buffer, sizeof(aead_key), "AEAD key") ||
      !alloc_shared_buffer(&nonce_buffer, sizeof(aead_nonce), "AEAD nonce") ||
      !alloc_shared_buffer(&roundtrip_buffer, sizeof(roundtrip),
                           "AEAD roundtrip")) {
    goto out;
  }

  if (!copy_to_shared(&input, aead_plaintext,
                      sizeof(aead_plaintext) - 1U) ||
      !copy_to_shared(&aad_buffer, aead_aad, sizeof(aead_aad)) ||
      !copy_to_shared(&key_buffer, aead_key, sizeof(aead_key)) ||
      !copy_to_shared(&nonce_buffer, aead_nonce, sizeof(aead_nonce))) {
    printf("AEAD input copy failed\n");
    goto out;
  }

  if (!zz9k_crypto_build_chacha20_poly1305_desc(
          &aead_desc, input.handle, 0U,
          (uint32_t)sizeof(aead_plaintext) - 1U, output.handle, 0U,
          aad_buffer.handle, 0U, (uint32_t)sizeof(aead_aad),
          key_buffer.handle, 0U, nonce_buffer.handle, 0U)) {
    printf("could not build AEAD encrypt descriptor\n");
    goto out;
  }

  memset(&result, 0, sizeof(result));
  status = ZZ9KCryptoAead(&aead_desc, &result);
  if (status != ZZ9K_STATUS_OK) {
    printf("ZZ9KCryptoAead encrypt failed: %s (%d)\n",
           zz9k_status_text(status), status);
    goto out;
  }
  if (result.bytes_written != sizeof(actual) ||
      result.algorithm != ZZ9K_CRYPTO_AEAD_CHACHA20_POLY1305 ||
      result.flags != 0U) {
    printf("unexpected AEAD encrypt result metadata\n");
    goto out;
  }

  memset(actual, 0, sizeof(actual));
  if (!copy_from_shared(actual, &output, 0U, sizeof(actual))) {
    printf("AEAD output copy failed\n");
    goto out;
  }
  if (!bytes_equal(actual, aead_expected_ciphertext,
                   sizeof(aead_expected_ciphertext)) ||
      !bytes_equal(actual + sizeof(aead_expected_ciphertext),
                   aead_expected_tag, sizeof(aead_expected_tag))) {
    printf("ChaCha20-Poly1305 vector mismatch\n");
    goto out;
  }

  if (!zz9k_crypto_build_chacha20_poly1305_desc(
          &aead_desc, output.handle, 0U,
          (uint32_t)sizeof(aead_expected_ciphertext),
          roundtrip_buffer.handle, 0U, aad_buffer.handle, 0U,
          (uint32_t)sizeof(aead_aad), key_buffer.handle, 0U,
          nonce_buffer.handle, ZZ9K_CRYPTO_AEAD_FLAG_DECRYPT)) {
    printf("could not build AEAD decrypt descriptor\n");
    goto out;
  }
  memset(&result, 0, sizeof(result));
  status = ZZ9KCryptoAead(&aead_desc, &result);
  if (status != ZZ9K_STATUS_OK) {
    printf("ZZ9KCryptoAead decrypt failed: %s (%d)\n",
           zz9k_status_text(status), status);
    goto out;
  }
  if (result.bytes_written != sizeof(roundtrip) ||
      result.algorithm != ZZ9K_CRYPTO_AEAD_CHACHA20_POLY1305 ||
      result.flags != ZZ9K_CRYPTO_AEAD_FLAG_DECRYPT) {
    printf("unexpected AEAD decrypt result metadata\n");
    goto out;
  }

  memset(roundtrip, 0, sizeof(roundtrip));
  if (!copy_from_shared(roundtrip, &roundtrip_buffer, 0U,
                        sizeof(roundtrip))) {
    printf("AEAD roundtrip copy failed\n");
    goto out;
  }
  if (!bytes_equal(roundtrip, aead_plaintext, sizeof(roundtrip))) {
    printf("ChaCha20-Poly1305 roundtrip mismatch\n");
    goto out;
  }

  printf("ChaCha20-Poly1305 AEAD ok\n");
  ok = 1;

out:
  free_shared_buffer(&roundtrip_buffer);
  free_shared_buffer(&nonce_buffer);
  free_shared_buffer(&key_buffer);
  free_shared_buffer(&aad_buffer);
  free_shared_buffer(&output);
  free_shared_buffer(&input);
  return ok;
}

int main(void)
{
  ZZ9KSharedBuffer input;
  ZZ9KSharedBuffer output;
  int input_allocated;
  int output_allocated;
  int status;
  int rc;

  memset(&input, 0, sizeof(input));
  memset(&output, 0, sizeof(output));
  input_allocated = 0;
  output_allocated = 0;
  rc = 1;

  ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME,
                         ZZ9K_LIBRARY_VERSION);
  if (!ZZ9KBase) {
    printf("OpenLibrary(%s, %u) failed\n",
           ZZ9K_LIBRARY_NAME, ZZ9K_LIBRARY_VERSION);
    return 1;
  }

  if (ZZ9KBase->lib_Revision < ZZ9K_LIBRARY_MIN_REVISION_CRYPTO_AEAD) {
    printf("zz9k.library revision too old for crypto stream/AEAD LVOs\n");
    goto out;
  }

  if (!require_crypto_support()) {
    goto out;
  }

  status = ZZ9KAllocShared((uint32_t)sizeof(input_abc), 16U, 0U, &input);
  if (status != ZZ9K_STATUS_OK) {
    printf("input alloc failed: %s (%d)\n", zz9k_status_text(status),
           status);
    goto out;
  }
  input_allocated = 1;
  if (!copy_to_shared(&input, input_abc, (uint32_t)sizeof(input_abc))) {
    printf("input copy failed\n");
    goto out;
  }

  status = ZZ9KAllocShared(64U, 16U, 0U, &output);
  if (status != ZZ9K_STATUS_OK) {
    printf("output alloc failed: %s (%d)\n", zz9k_status_text(status),
           status);
    goto out;
  }
  output_allocated = 1;

  if (!run_single_sha256(&input, &output)) {
    goto out;
  }
  if (!run_batch_hashes(&input, &output)) {
    goto out;
  }
  if (!run_chacha20_stream()) {
    goto out;
  }
  if (!run_chacha20_poly1305_aead()) {
    goto out;
  }

  rc = 0;

out:
  if (output_allocated) {
    ZZ9KFreeShared(output.handle);
  }
  if (input_allocated) {
    ZZ9KFreeShared(input.handle);
  }
  CloseLibrary(ZZ9KBase);
  return rc;
}
