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

#define SDK_SHA1_BLOCK_SIZE 64U
#define SDK_SHA1_DIGEST_SIZE 20U
#define SDK_SHA256_BLOCK_SIZE 64U
#define SDK_SHA256_DIGEST_SIZE 32U
#define SDK_CHACHA20_KEY_SIZE 32U
#define SDK_CHACHA20_NONCE_SIZE 12U
#define SDK_CHACHA20_BLOCK_SIZE 64U
#define SDK_POLY1305_KEY_SIZE 32U
#define SDK_POLY1305_TAG_SIZE 16U

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

#endif /* SDK_CRYPTO_H */
