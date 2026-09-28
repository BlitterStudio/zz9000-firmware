/*
 * Portable software reference for ChaCha20, Poly1305, and the
 * ChaCha20-Poly1305 AEAD (RFC 8439).
 *
 * This is the m68k software baseline the crypto benchmark measures the
 * ZZ9000 offload against, and the verification oracle for the planned
 * AmiSSL provider. It is plain C99 with no AmigaOS or SDK dependencies so
 * it compiles unchanged on the host (for CTest) and on m68k.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_CRYPTO_SOFT_H
#define ZZ9K_CRYPTO_SOFT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZZ9K_SOFT_CHACHA20_KEY_BYTES 32U
#define ZZ9K_SOFT_CHACHA20_NONCE_BYTES 12U
#define ZZ9K_SOFT_POLY1305_KEY_BYTES 32U
#define ZZ9K_SOFT_POLY1305_TAG_BYTES 16U

/*
 * ChaCha20 keystream XOR (RFC 8439 section 2.4). Encrypts or decrypts
 * `length` bytes from `in` into `out` (may alias). `counter` is the initial
 * 32-bit block counter.
 */
void zz9k_soft_chacha20_xor(uint8_t *out, const uint8_t *in, uint32_t length,
                            const uint8_t key[ZZ9K_SOFT_CHACHA20_KEY_BYTES],
                            const uint8_t nonce[ZZ9K_SOFT_CHACHA20_NONCE_BYTES],
                            uint32_t counter);

/*
 * Poly1305 one-time authenticator (RFC 8439 section 2.5). `key` is the
 * 32-byte one-time key; the 16-byte tag is written to `tag`.
 */
void zz9k_soft_poly1305(uint8_t tag[ZZ9K_SOFT_POLY1305_TAG_BYTES],
                        const uint8_t *message, uint32_t length,
                        const uint8_t key[ZZ9K_SOFT_POLY1305_KEY_BYTES]);

/*
 * ChaCha20-Poly1305 AEAD encrypt (RFC 8439 section 2.8). Writes `length`
 * ciphertext bytes to `ciphertext` and the 16-byte tag to `tag`.
 */
void zz9k_soft_chacha20_poly1305_encrypt(
    uint8_t *ciphertext, uint8_t tag[ZZ9K_SOFT_POLY1305_TAG_BYTES],
    const uint8_t *plaintext, uint32_t length,
    const uint8_t *aad, uint32_t aad_length,
    const uint8_t key[ZZ9K_SOFT_CHACHA20_KEY_BYTES],
    const uint8_t nonce[ZZ9K_SOFT_CHACHA20_NONCE_BYTES]);

/*
 * ChaCha20-Poly1305 AEAD decrypt (RFC 8439 section 2.8). Verifies `tag` in
 * constant time and, only on success, writes `length` plaintext bytes to
 * `plaintext`. Returns 1 when the tag is valid, 0 otherwise.
 */
int zz9k_soft_chacha20_poly1305_decrypt(
    uint8_t *plaintext,
    const uint8_t *ciphertext, uint32_t length,
    const uint8_t *aad, uint32_t aad_length,
    const uint8_t tag[ZZ9K_SOFT_POLY1305_TAG_BYTES],
    const uint8_t key[ZZ9K_SOFT_CHACHA20_KEY_BYTES],
    const uint8_t nonce[ZZ9K_SOFT_CHACHA20_NONCE_BYTES]);

/*
 * X25519 Diffie-Hellman scalar multiplication (RFC 7748).
 * Returns 1 on success, 0 if result is the all-zero point (must be rejected).
 */
int zz9k_soft_x25519(uint8_t out[32], const uint8_t scalar[32],
                     const uint8_t point[32]);

/* ---- P-256 (secp256r1) Diffie-Hellman and ECDSA ---- */

#define ZZ9K_SOFT_P256_POINT_BYTES 65U   /* uncompressed: 0x04 || X(32) || Y(32) */
#define ZZ9K_SOFT_P256_PRIVATE_BYTES 32U /* scalar in [1, n-1] */
#define ZZ9K_SOFT_P256_SHARED_BYTES 32U  /* shared secret = X coordinate */

/* P-256 ECDSA signature: raw r || s (two 32-byte big-endian integers). */
#define ZZ9K_SOFT_ECDSA_P256_SIG_SIZE 64U

/* RSA-2048 PKCS#1 v1.5 SHA-256 verification constants. */
#define ZZ9K_SOFT_RSA_2048_KEY_BYTES 256U /* modulus in bytes */
#define ZZ9K_SOFT_SHA256_DIGEST_SIZE 32U

/*
 * P-256 ECDH key exchange (RFC 5753 / SEC 1).
 * `private_key` is a 32-byte scalar; `public_point` is an uncompressed
 * 65-byte point (0x04 || X || Y). Writes the 32-byte X coordinate of
 * shared_secret to `shared_secret`. Returns 1 on success, 0 on error.
 */
int zz9k_soft_p256_ecdh(uint8_t shared_secret[32],
                        const uint8_t private_key[32],
                        const uint8_t public_point[65]);

/*
 * P-256 keygen: public_point = private_key * G (SEC 1). `private_key` is a
 * 32-byte scalar in [1, n-1]; writes the uncompressed 65-byte public point
 * (0x04 || X || Y) to `public_point`. Returns 1 on success, 0 if the scalar is
 * out of range or the result is the point at infinity.
 */
int zz9k_soft_p256_keygen(uint8_t public_point[65],
                          const uint8_t private_key[32]);

/*
 * ECDSA-P256 SHA-256 signature verification (SEC 1 / FIPS 186-4).
 * `signature_r` and `signature_s` are each 32-byte big-endian integers.
 * `message_hash` is the 32-byte SHA-256 digest of the message.
 * `public_point` is an uncompressed 65-byte point (0x04 || X || Y).
 * Returns 1 if valid, 0 otherwise.
 */
int zz9k_soft_ecdsa_verify_p256(const uint8_t signature_r[32],
                                const uint8_t signature_s[32],
                                const uint8_t message_hash[32],
                                const uint8_t public_point[65]);

/*
 * RSA-2048 PKCS#1 v1.5 SHA-256 signature verification (RFC 8017).
 * `signature` is `sig_len` bytes of the raw RSA signature (modular exponentiation result).
 * `message_hash` is the 32-byte SHA-256 digest to verify against.
 * `n` is the modulus as big-endian bytes; `n_bits` is its bit length.
 * `e` is the public exponent (typically 65537).
 * Returns 1 if valid, 0 otherwise.
 */
int zz9k_soft_rsa_verify_pkcs1_sha256(const uint8_t *signature,
                                      uint32_t sig_len,
                                      const uint8_t *message_hash,
                                      const uint8_t *n,
                                      uint32_t n_bits,
                                      uint32_t e);

/* ---- AES-128/256-GCM (NIST SP 800-38D) ---- */

#define ZZ9K_SOFT_AES_GCM_NONCE_BYTES 12U /* 96-bit IV */
#define ZZ9K_SOFT_AES_GCM_TAG_BYTES   16U

/*
 * AES-GCM authenticated encryption. `key_length` is 16 (AES-128) or 32
 * (AES-256); `nonce` is 12 bytes. Writes `length` ciphertext bytes to
 * `ciphertext` and the 16-byte authentication tag to `tag`. `aad` may be NULL
 * when `aad_length` is 0. Returns 1 on success, 0 on a bad key length.
 */
int zz9k_soft_aes_gcm_encrypt(uint8_t *ciphertext,
                              uint8_t tag[ZZ9K_SOFT_AES_GCM_TAG_BYTES],
                              const uint8_t *plaintext, uint32_t length,
                              const uint8_t *aad, uint32_t aad_length,
                              const uint8_t *key, uint32_t key_length,
                              const uint8_t nonce[ZZ9K_SOFT_AES_GCM_NONCE_BYTES]);

/*
 * AES-GCM authenticated decryption. Verifies `tag` in constant time before
 * releasing plaintext. Returns 1 if the tag is valid (and `plaintext` holds
 * the result), 0 on tag mismatch or bad key length (no plaintext released).
 */
int zz9k_soft_aes_gcm_decrypt(uint8_t *plaintext,
                              const uint8_t *ciphertext, uint32_t length,
                              const uint8_t *aad, uint32_t aad_length,
                              const uint8_t tag[ZZ9K_SOFT_AES_GCM_TAG_BYTES],
                              const uint8_t *key, uint32_t key_length,
                              const uint8_t nonce[ZZ9K_SOFT_AES_GCM_NONCE_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_CRYPTO_SOFT_H */
