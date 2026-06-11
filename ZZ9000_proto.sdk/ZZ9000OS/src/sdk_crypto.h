/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Small SDK crypto helpers used by firmware mailbox services.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_CRYPTO_H
#define SDK_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#define SDK_SHA1_BLOCK_SIZE 64U
#define SDK_SHA1_DIGEST_SIZE 20U
#define SDK_SHA256_BLOCK_SIZE 64U
#define SDK_SHA256_DIGEST_SIZE 32U
#define SDK_CHACHA20_KEY_SIZE 32U
#define SDK_CHACHA20_NONCE_SIZE 12U
#define SDK_CHACHA20_BLOCK_SIZE 64U
#define SDK_POLY1305_KEY_SIZE 32U
#define SDK_POLY1305_TAG_SIZE 16U
#define SDK_AES128_KEY_SIZE 16U
#define SDK_AES256_KEY_SIZE 32U
#define SDK_AES_GCM_NONCE_SIZE 12U
#define SDK_AES_GCM_TAG_SIZE 16U
#define SDK_X25519_KEY_SIZE      32U
#define SDK_X25519_POINT_SIZE    32U
#define SDK_X25519_SHARED_SIZE   32U

/* P-256 ECDH (RFC 8422) */
#define SDK_P256_KEY_SIZE      32U
#define SDK_P256_POINT_SIZE    65U  /* uncompressed: 0x04 || X(32) || Y(32) */
#define SDK_P256_SHARED_SIZE   32U  /* shared secret is the X coordinate (32 bytes) */

/* P-256 ECDSA with SHA-256. Signature in raw r||s format (64 bytes). */
#define SDK_P256_ECDSA_SIG_SIZE    64U
#define SDK_P256_ECDSA_POINT_SIZE  65U  /* uncompressed public key: 0x04 || X || Y */

/* RSA-2048 PKCS#1 v1.5 with SHA-256 */
#define SDK_RSA_2048_KEY_BYTES     256U
#define SDK_RSA_SHA256_SIG_SIZE    256U

void sdk_sha1(const uint8_t *data, uint32_t length,
              uint8_t digest[SDK_SHA1_DIGEST_SIZE]);
void sdk_hmac_sha1(const uint8_t *key, uint32_t key_length,
                   const uint8_t *data, uint32_t data_length,
                   uint8_t digest[SDK_SHA1_DIGEST_SIZE]);
void sdk_sha256(const uint8_t *data, uint32_t length,
                uint8_t digest[SDK_SHA256_DIGEST_SIZE]);
void sdk_hmac_sha256(const uint8_t *key, uint32_t key_length,
                     const uint8_t *data, uint32_t data_length,
                     uint8_t digest[SDK_SHA256_DIGEST_SIZE]);
void sdk_chacha20_xor(const uint8_t key[SDK_CHACHA20_KEY_SIZE],
                      const uint8_t nonce[SDK_CHACHA20_NONCE_SIZE],
                      uint32_t counter,
                      const uint8_t *input,
                      uint8_t *output,
                      uint32_t length);
void sdk_poly1305(const uint8_t key[SDK_POLY1305_KEY_SIZE],
                  const uint8_t *data,
                  uint32_t length,
                  uint8_t tag[SDK_POLY1305_TAG_SIZE]);
void sdk_chacha20_poly1305_encrypt(
	const uint8_t key[SDK_CHACHA20_KEY_SIZE],
	const uint8_t nonce[SDK_CHACHA20_NONCE_SIZE],
	const uint8_t *aad,
	uint32_t aad_length,
	const uint8_t *plaintext,
	uint32_t plaintext_length,
	uint8_t *ciphertext,
	uint8_t tag[SDK_POLY1305_TAG_SIZE]);
int sdk_chacha20_poly1305_decrypt(
	const uint8_t key[SDK_CHACHA20_KEY_SIZE],
	const uint8_t nonce[SDK_CHACHA20_NONCE_SIZE],
	const uint8_t *aad,
	uint32_t aad_length,
	const uint8_t *ciphertext,
	uint32_t ciphertext_length,
	const uint8_t tag[SDK_POLY1305_TAG_SIZE],
	uint8_t *plaintext);

/* AES-GCM (NIST SP 800-38D) via BearSSL. key_length is 16 (AES-128) or 32
 * (AES-256); nonce is 12 bytes; the 16-byte tag is produced/checked
 * separately. The encrypt path writes plaintext_length ciphertext bytes plus
 * the tag; decrypt verifies the tag and returns 1 on success, 0 on mismatch. */
void sdk_aes_gcm_encrypt(
	const uint8_t *key,
	uint32_t key_length,
	const uint8_t nonce[SDK_AES_GCM_NONCE_SIZE],
	const uint8_t *aad,
	uint32_t aad_length,
	const uint8_t *plaintext,
	uint32_t plaintext_length,
	uint8_t *ciphertext,
	uint8_t tag[SDK_AES_GCM_TAG_SIZE]);
int sdk_aes_gcm_decrypt(
	const uint8_t *key,
	uint32_t key_length,
	const uint8_t nonce[SDK_AES_GCM_NONCE_SIZE],
	const uint8_t *aad,
	uint32_t aad_length,
	const uint8_t *ciphertext,
	uint32_t ciphertext_length,
	const uint8_t tag[SDK_AES_GCM_TAG_SIZE],
	uint8_t *plaintext);

/* X25519 scalar multiplication (RFC 7748). Returns 1 on success, 0 if the
 * result is the all-zero (small-order) point and must be rejected. */
int sdk_x25519(const uint8_t scalar[SDK_X25519_KEY_SIZE],
               const uint8_t point[SDK_X25519_POINT_SIZE],
               uint8_t shared[SDK_X25519_SHARED_SIZE]);

/* P-256 ECDH (RFC 8422). scalar = private key (32 bytes BE),
   peer_point = uncompressed public key (65 bytes: 0x04 || X || Y),
   shared = output X coordinate of scalar*peer_point (32 bytes BE).
   Returns 1 on success, 0 on invalid point or other error. */
int sdk_p256_ecdh(const uint8_t scalar[SDK_P256_KEY_SIZE],
                   const uint8_t peer_point[SDK_P256_POINT_SIZE],
                   uint8_t shared[SDK_P256_SHARED_SIZE]);

/* P-256 ECDSA verification with SHA-256 (raw r||s format, 64 bytes).
   pubkey = uncompressed public key point (65 bytes: 0x04 || X || Y),
   signature = raw ECDSA signature (r||s, 64 bytes),
   hash = pre-computed SHA-256 digest of the data to verify (32 bytes).
   Returns 1 on valid signature, 0 on invalid signature or bad public key. */
int sdk_ecdsa_verify_p256(const uint8_t pubkey[SDK_P256_ECDSA_POINT_SIZE],
                           const uint8_t signature[SDK_P256_ECDSA_SIG_SIZE],
                           const uint8_t hash[SDK_SHA256_DIGEST_SIZE]);

/* RSA-2048 PKCS#1 v1.5 verification with SHA-256.
   modulus = RSA public modulus (256 bytes, big-endian),
   exponent = RSA public exponent (big-endian),
   signature = PKCS#1 v1.5 padded signature (256 bytes),
   hash = pre-computed SHA-256 digest of the data to verify (32 bytes).
   Returns 1 on valid signature, 0 on invalid signature or bad key. */
int sdk_rsa_verify_pkcs1_sha256(const uint8_t modulus[SDK_RSA_2048_KEY_BYTES],
                                 const uint8_t *exponent,
                                 size_t exponent_len,
                                 const uint8_t signature[SDK_RSA_SHA256_SIG_SIZE],
                                 const uint8_t hash[SDK_SHA256_DIGEST_SIZE]);

#endif /* SDK_CRYPTO_H */
