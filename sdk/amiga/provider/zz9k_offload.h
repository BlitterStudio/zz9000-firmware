/*
 * ZZ9000 OpenSSL provider — hardware offload backend interface.
 *
 * Implemented in zz9k_offload.c, which is linked into the provider only for the
 * Amiga target (ZZ9K_PROVIDER_OFFLOAD defined). The context is an opaque
 * pointer (created by zz9k_offload_open) so the provider operation files need
 * not include the SDK headers. Each operation returns 1 if the offload ran and
 * produced a result, or -1 if it could not run so the caller falls back to the
 * software reference. Verify calls set *valid (1 = valid signature).
 *
 * The context wraps the SDK board handle plus persistent shared scratch
 * buffers: zz9k_alloc_shared/zz9k_free_shared are a full mailbox round trip
 * each, so per-operation allocation would multiply the documented ~6 ms call
 * latency by the buffer count. Scratch uses exact aligned HOST_WINDOW growth
 * and is released by zz9k_offload_close.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_OFFLOAD_H
#define ZZ9K_OFFLOAD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Verify algorithm ids, mirroring ZZ9K_CRYPTO_VERIFY_* in zz9k/abi.h (kept
 * local so the OpenSSL provider files need not include the SDK ABI header;
 * zz9k_offload.c pins the values to the enum with compile-time checks). */
#define ZZ9K_OFFLOAD_VERIFY_ECDSA_P256 1U
#define ZZ9K_OFFLOAD_VERIFY_RSA_PKCS1  2U

/* Open the board and the crypto service. Returns an offload context, or NULL
 * when the board is absent or its firmware lacks the crypto service (the
 * provider then stays pure software). *service_flags receives the advertised
 * ZZ9K_SERVICE_FLAG_CRYPTO_* bits for the per-algorithm gates. */
void *zz9k_offload_open(unsigned int *service_flags);

/* Release the scratch buffers and close the board. NULL is a no-op. */
void zz9k_offload_close(void *ctx);

int zz9k_offload_x25519(void *ctx, unsigned char out[32],
                        const unsigned char scalar[32],
                        const unsigned char point[32]);

/* P-256 ECDH derive: out = X-coordinate of scalar*peer (peer is the 65-byte
 * uncompressed peer point). Gated by the caller on CRYPTO_P256. */
int zz9k_offload_p256_derive(void *ctx, unsigned char out[32],
                             const unsigned char scalar[32],
                             const unsigned char peer[65]);

/* P-256 keygen: pub = scalar*G (65-byte uncompressed point). Gated by the
 * caller on CRYPTO_P256_KEYGEN (the firmware KX KEYGEN flag). */
int zz9k_offload_p256_keygen(void *ctx, unsigned char pub[65],
                             const unsigned char scalar[32]);

/* aes != 0 selects AES-GCM (keylen 16 or 32); aes == 0 selects
 * ChaCha20-Poly1305 (keylen 32). The 16-byte tag is produced (encrypt) or
 * consumed (decrypt) via `tag`. */
int zz9k_offload_aead(void *ctx, int aes, unsigned int keylen, int decrypt,
                      const unsigned char *key, const unsigned char *iv,
                      const unsigned char *aad, unsigned int aadlen,
                      const unsigned char *in, unsigned int inlen,
                      unsigned char *out, unsigned char *tag);

/* `algorithm` is a ZZ9K_OFFLOAD_VERIFY_* value. For ECDSA-P256 the key is the
 * 65-byte point and sig is r||s (64). For RSA the key is modulus||exponent and
 * sig is the modulus-width signature. The digest is always 32 bytes. */
int zz9k_offload_verify(void *ctx, unsigned int algorithm,
                        const unsigned char *hash, const unsigned char *sig,
                        unsigned int siglen, const unsigned char *key,
                        unsigned int keylen, int *valid);

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_OFFLOAD_H */
