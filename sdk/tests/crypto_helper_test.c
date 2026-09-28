/*
 * Unit checks for public crypto descriptor helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/crypto.h"

#include <stdint.h>
#include <string.h>

static int test_digest_lengths(void)
{
  if (zz9k_crypto_digest_length(ZZ9K_CRYPTO_HASH_NONE) != 0U) return 1;
  if (zz9k_crypto_digest_length(ZZ9K_CRYPTO_HASH_SHA1) != 20U) return 2;
  if (zz9k_crypto_digest_length(ZZ9K_CRYPTO_HASH_SHA256) != 32U) return 3;
  if (zz9k_crypto_digest_length(ZZ9K_CRYPTO_HASH_SHA384) != 48U) return 4;
  if (zz9k_crypto_digest_length(ZZ9K_CRYPTO_HASH_SHA512) != 64U) return 5;
  if (zz9k_crypto_digest_length(ZZ9K_CRYPTO_HASH_BLAKE2S) != 32U) return 6;
  if (zz9k_crypto_digest_length(ZZ9K_CRYPTO_HASH_POLY1305) != 16U) return 7;
  return 0;
}

static int test_hash_descriptors(void)
{
  ZZ9KCryptoHashDesc desc;

  memset(&desc, 0xff, sizeof(desc));
  if (!zz9k_crypto_build_hash_desc(&desc, ZZ9K_CRYPTO_HASH_SHA256,
                                   0x40000010UL, 0x20U, 0x100U,
                                   0x40000020UL, 0x40U)) {
    return 1;
  }
  if (desc.src_handle != 0x40000010UL) return 2;
  if (desc.src_offset != 0x20U) return 3;
  if (desc.src_length != 0x100U) return 4;
  if (desc.dst_handle != 0x40000020UL) return 5;
  if (desc.dst_offset != 0x40U) return 6;
  if (desc.key_handle != 0U || desc.key_offset != 0U ||
      desc.key_length != 0U) return 7;
  if (desc.algorithm != ZZ9K_CRYPTO_HASH_SHA256) return 8;
  if (desc.flags != 0U) return 9;

  if (zz9k_crypto_build_hash_desc(&desc, ZZ9K_CRYPTO_HASH_NONE,
                                  0x40000010UL, 0U, 1U,
                                  0x40000020UL, 0U)) return 10;
  if (zz9k_crypto_build_hash_desc(&desc, ZZ9K_CRYPTO_HASH_POLY1305,
                                  0x40000010UL, 0U, 1U,
                                  0x40000020UL, 0U)) return 11;
  if (zz9k_crypto_build_hash_desc(&desc, ZZ9K_CRYPTO_HASH_SHA256,
                                  ZZ9K_INVALID_HANDLE, 0U, 1U,
                                  0x40000020UL, 0U)) return 12;
  if (zz9k_crypto_build_hash_desc(&desc, ZZ9K_CRYPTO_HASH_SHA256,
                                  0x40000010UL, 0U, 0U,
                                  0x40000020UL, 0U)) return 13;

  return 0;
}

static int test_keyed_hash_descriptors(void)
{
  ZZ9KCryptoHashDesc desc;

  memset(&desc, 0xff, sizeof(desc));
  if (!zz9k_crypto_build_hmac_desc(&desc, ZZ9K_CRYPTO_HASH_SHA1,
                                   0x40000030UL, 0x10U, 0x200U,
                                   0x40000040UL, 0x20U,
                                   0x40000050UL, 0x30U, 64U)) {
    return 1;
  }
  if (desc.src_handle != 0x40000030UL) return 2;
  if (desc.src_offset != 0x10U || desc.src_length != 0x200U) return 3;
  if (desc.dst_handle != 0x40000040UL || desc.dst_offset != 0x20U) {
    return 4;
  }
  if (desc.key_handle != 0x40000050UL || desc.key_offset != 0x30U ||
      desc.key_length != 64U) return 5;
  if (desc.algorithm != ZZ9K_CRYPTO_HASH_SHA1) return 6;
  if (desc.flags != ZZ9K_CRYPTO_HASH_FLAG_HMAC) return 7;
  if (zz9k_crypto_build_hmac_desc(&desc, ZZ9K_CRYPTO_HASH_POLY1305,
                                  0x40000030UL, 0U, 1U, 0x40000040UL,
                                  0U, 0x40000050UL, 0U, 64U)) return 8;
  if (zz9k_crypto_build_hmac_desc(&desc, ZZ9K_CRYPTO_HASH_SHA1,
                                  0x40000030UL, 0U, 1U, 0x40000040UL,
                                  0U, ZZ9K_INVALID_HANDLE, 0U, 64U)) {
    return 9;
  }
  if (zz9k_crypto_build_hmac_desc(&desc, ZZ9K_CRYPTO_HASH_SHA1,
                                  0x40000030UL, 0U, 1U, 0x40000040UL,
                                  0U, 0x40000050UL, 0U, 0U)) {
    return 10;
  }

  memset(&desc, 0xff, sizeof(desc));
  if (!zz9k_crypto_build_poly1305_desc(&desc, 0x40000060UL, 0x70U,
                                       0x80U, 0x40000070UL, 0x90U,
                                       0x40000080UL, 0xa0U)) {
    return 11;
  }
  if (desc.algorithm != ZZ9K_CRYPTO_HASH_POLY1305) return 12;
  if (desc.key_handle != 0x40000080UL || desc.key_offset != 0xa0U ||
      desc.key_length != ZZ9K_CRYPTO_POLY1305_KEY_BYTES) return 13;
  if (desc.flags != 0U) return 14;
  if (zz9k_crypto_build_poly1305_desc(&desc, 0x40000060UL, 0U, 1U,
                                      0x40000070UL, 0U,
                                      ZZ9K_INVALID_HANDLE, 0U)) {
    return 15;
  }

  return 0;
}

static int test_stream_and_aead_descriptors(void)
{
  ZZ9KCryptoStreamDesc stream;
  ZZ9KCryptoAeadDesc aead;

  memset(&stream, 0xff, sizeof(stream));
  if (!zz9k_crypto_build_chacha20_desc(
          &stream, 0x40000090UL, 0x10U, 64U, 0x40000091UL,
          0x20U, 0x40000092UL, 0x30U, 0x40000093UL, 0x40U, 7U)) {
    return 1;
  }
  if (stream.src_handle != 0x40000090UL ||
      stream.src_offset != 0x10U || stream.src_length != 64U) return 2;
  if (stream.dst_handle != 0x40000091UL ||
      stream.dst_offset != 0x20U) return 3;
  if (stream.key_handle != 0x40000092UL ||
      stream.key_offset != 0x30U) return 4;
  if (stream.nonce_handle != 0x40000093UL ||
      stream.nonce_offset != 0x40U) return 5;
  if (stream.counter != 7U ||
      stream.algorithm != ZZ9K_CRYPTO_STREAM_CHACHA20 ||
      stream.flags != 0U) return 6;
  if (zz9k_crypto_build_chacha20_desc(
          &stream, 0x40000090UL, 0U, 0U, 0x40000091UL,
          0U, 0x40000092UL, 0U, 0x40000093UL, 0U, 0U)) {
    return 7;
  }
  if (zz9k_crypto_build_stream_desc(
          &stream, ZZ9K_CRYPTO_STREAM_NONE, 0x40000090UL, 0U, 1U,
          0x40000091UL, 0U, 0x40000092UL, 0U,
          0x40000093UL, 0U, 0U, 0U)) {
    return 8;
  }

  memset(&aead, 0xff, sizeof(aead));
  if (!zz9k_crypto_build_chacha20_poly1305_desc(
          &aead, 0x400000a0UL, 0x10U, 114U, 0x400000a1UL, 0x20U,
          0x400000a2UL, 0x30U, 12U, 0x400000a3UL, 0x40U,
          0x400000a4UL, ZZ9K_CRYPTO_AEAD_FLAG_DECRYPT)) {
    return 9;
  }
  if (aead.src_handle != 0x400000a0UL || aead.src_offset != 0x10U ||
      aead.src_length != 114U) return 10;
  if (aead.dst_handle != 0x400000a1UL || aead.dst_offset != 0x20U) {
    return 11;
  }
  if (aead.aad_handle != 0x400000a2UL || aead.aad_offset != 0x30U ||
      aead.aad_length != 12U) return 12;
  if (aead.key_handle != 0x400000a3UL || aead.key_offset != 0x40U) {
    return 13;
  }
  if (aead.nonce_handle != 0x400000a4UL ||
      aead.flags != ZZ9K_CRYPTO_AEAD_FLAG_DECRYPT) return 14;
  if (zz9k_crypto_build_chacha20_poly1305_desc(
          &aead, 0x400000a0UL, 0U, 1U, 0x400000a1UL, 0U,
          ZZ9K_INVALID_HANDLE, 0U, 12U, 0x400000a3UL, 0U,
          0x400000a4UL, 0U)) {
    return 15;
  }
  if (zz9k_crypto_build_chacha20_poly1305_desc(
          &aead, 0x400000a0UL, 0U, 1U, 0x400000a1UL, 0U,
          ZZ9K_INVALID_HANDLE, 1U, 0U, 0x400000a3UL, 0U,
          0x400000a4UL, 0U) == 0) {
    return 16;
  }
  if (aead.aad_handle != 0U || aead.aad_offset != 0U ||
      aead.aad_length != 0U) return 17;

  return 0;
}

static int test_kx_descriptor(void)
{
  ZZ9KCryptoKxDesc desc;

  /* valid call: all handles non-invalid */
  if (!zz9k_crypto_build_x25519_desc(&desc, 1U, 0U, 2U, 0U, 3U, 0U))
    return 1;
  if (desc.algorithm != ZZ9K_CRYPTO_KX_X25519) return 2;
  if (desc.scalar_handle != 1U) return 3;
  if (desc.point_handle != 2U) return 4;
  if (desc.dst_handle != 3U) return 5;
  if (desc.scalar_offset != 0U) return 6;
  if (desc.point_offset != 0U) return 7;
  if (desc.dst_offset != 0U) return 8;

  /* invalid handle must be rejected */
  if (zz9k_crypto_build_x25519_desc(&desc, ZZ9K_INVALID_HANDLE, 0U, 2U, 0U,
                                    3U, 0U))
    return 9;

  /* NULL desc must be rejected like every other builder */
  if (zz9k_crypto_build_x25519_desc(0, 1U, 0U, 2U, 0U, 3U, 0U))
    return 10;

  /* P-256 reuses the KX descriptor but stamps the P-256 algorithm. */
  if (!zz9k_crypto_build_p256_desc(&desc, 4U, 8U, 5U, 16U, 6U, 0U))
    return 11;
  if (desc.algorithm != ZZ9K_CRYPTO_KX_P256) return 12;
  if (desc.scalar_handle != 4U || desc.scalar_offset != 8U) return 13;
  if (desc.point_handle != 5U || desc.point_offset != 16U) return 14;
  if (desc.dst_handle != 6U) return 15;
  if (zz9k_crypto_build_p256_desc(&desc, 4U, 0U, ZZ9K_INVALID_HANDLE, 0U,
                                  6U, 0U))
    return 16;

  return 0;
}

static int test_verify_descriptor(void)
{
  ZZ9KCryptoVerifyDesc desc;

  /* ECDSA-P256: hash, signature and public-key handles all populated. */
  if (!zz9k_crypto_build_verify_desc(&desc,
                                     ZZ9K_CRYPTO_VERIFY_ECDSA_P256_SHA256,
                                     1U, 0U, 32U, 2U, 4U, 64U, 3U, 8U, 65U))
    return 1;
  if (desc.algorithm != ZZ9K_CRYPTO_VERIFY_ECDSA_P256_SHA256) return 2;
  if (desc.hash_handle != 1U || desc.hash_length != 32U) return 3;
  if (desc.sig_handle != 2U || desc.sig_offset != 4U || desc.sig_length != 64U)
    return 4;
  if (desc.key_handle != 3U || desc.key_offset != 8U || desc.key_length != 65U)
    return 5;

  /* RSA-PKCS1 algorithm id is also accepted. */
  if (!zz9k_crypto_build_verify_desc(&desc,
                                     ZZ9K_CRYPTO_VERIFY_RSA_PKCS1_2048_SHA256,
                                     1U, 0U, 32U, 2U, 0U, 256U, 3U, 0U, 256U))
    return 6;

  /* Missing public key (or any handle) must be rejected. */
  if (zz9k_crypto_build_verify_desc(&desc,
                                    ZZ9K_CRYPTO_VERIFY_ECDSA_P256_SHA256,
                                    1U, 0U, 32U, 2U, 0U, 64U,
                                    ZZ9K_INVALID_HANDLE, 0U, 65U))
    return 7;

  /* Unknown algorithm must be rejected. */
  if (zz9k_crypto_build_verify_desc(&desc, 0U,
                                    1U, 0U, 32U, 2U, 0U, 64U, 3U, 0U, 65U))
    return 8;

  /* NULL desc must be rejected. */
  if (zz9k_crypto_build_verify_desc(0, ZZ9K_CRYPTO_VERIFY_ECDSA_P256_SHA256,
                                    1U, 0U, 32U, 2U, 0U, 64U, 3U, 0U, 65U))
    return 9;

  return 0;
}

static int test_aes_gcm_descriptor(void)
{
  ZZ9KCryptoAeadDesc desc;

  /* AES-128-GCM: key_length 16 selects the AES128 algorithm in the flags. */
  if (!zz9k_crypto_build_aes_gcm_desc(&desc, 1U, 0U, 64U, 2U, 0U, 3U, 0U, 12U,
                                      4U, 0U, ZZ9K_CRYPTO_AES128_KEY_BYTES,
                                      5U, 0U))
    return 1;
  if (ZZ9K_CRYPTO_AEAD_FLAG_GET_ALG(desc.flags) != ZZ9K_CRYPTO_AEAD_AES128_GCM)
    return 2;
  if (desc.src_length != 64U || desc.key_handle != 4U || desc.nonce_handle != 5U)
    return 3;

  /* AES-256-GCM with the decrypt flag preserved alongside the algorithm. */
  if (!zz9k_crypto_build_aes_gcm_desc(&desc, 1U, 0U, 64U, 2U, 0U, 0U, 0U, 0U,
                                      4U, 0U, ZZ9K_CRYPTO_AES256_KEY_BYTES,
                                      5U, ZZ9K_CRYPTO_AEAD_FLAG_DECRYPT))
    return 4;
  if (ZZ9K_CRYPTO_AEAD_FLAG_GET_ALG(desc.flags) != ZZ9K_CRYPTO_AEAD_AES256_GCM)
    return 5;
  if ((desc.flags & ZZ9K_CRYPTO_AEAD_FLAG_DECRYPT) == 0U) return 6;

  /* A key length that is neither 16 nor 32 must be rejected. */
  if (zz9k_crypto_build_aes_gcm_desc(&desc, 1U, 0U, 64U, 2U, 0U, 0U, 0U, 0U,
                                     4U, 0U, 24U, 5U, 0U))
    return 7;

  /* A missing nonce handle must be rejected. */
  if (zz9k_crypto_build_aes_gcm_desc(&desc, 1U, 0U, 64U, 2U, 0U, 0U, 0U, 0U,
                                     4U, 0U, ZZ9K_CRYPTO_AES128_KEY_BYTES,
                                     ZZ9K_INVALID_HANDLE, 0U))
    return 8;

  return 0;
}

int main(void)
{
  int result;

  result = test_digest_lengths();
  if (result) return 10 + result;

  result = test_hash_descriptors();
  if (result) return 30 + result;

  result = test_keyed_hash_descriptors();
  if (result) return 60 + result;

  result = test_stream_and_aead_descriptors();
  if (result) return 100 + result;

  result = test_kx_descriptor();
  if (result) return 130 + result;

  result = test_verify_descriptor();
  if (result) return 160 + result;

  result = test_aes_gcm_descriptor();
  if (result) return 190 + result;

  return 0;
}
