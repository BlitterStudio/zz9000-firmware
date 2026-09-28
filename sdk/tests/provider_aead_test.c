/*
 * Phase 4.3 host parity test: AES-128/256-GCM and ChaCha20-Poly1305 through
 * the ZZ9000 provider match OpenSSL's default provider byte-for-byte (cipher
 * text and tag), round-trip on decrypt, and reject a tampered tag. The ciphers
 * are driven through EVP exactly as a TLS record layer would (init, optional
 * AAD update, single data update, final, tag get/set). On the host the AEAD
 * uses the software reference; on the Amiga the same path offloads.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k_provider.h"

#include <openssl/provider.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/params.h>

#include <stdio.h>
#include <string.h>

static int aead_encrypt(OSSL_LIB_CTX *libctx, const char *prop,
                        const char *cipher, const unsigned char *key,
                        const unsigned char *iv, const unsigned char *aad,
                        int aadlen, const unsigned char *pt, int ptlen,
                        unsigned char *ct, unsigned char *tag)
{
  EVP_CIPHER *c = EVP_CIPHER_fetch(libctx, cipher, prop);
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  OSSL_PARAM p[2];
  int len = 0;
  int ok = 0;

  if (c == NULL || ctx == NULL) {
    goto end;
  }
  if (EVP_EncryptInit_ex2(ctx, c, key, iv, NULL) <= 0 ||
      (aadlen > 0 && EVP_EncryptUpdate(ctx, NULL, &len, aad, aadlen) <= 0) ||
      EVP_EncryptUpdate(ctx, ct, &len, pt, ptlen) <= 0 ||
      EVP_EncryptFinal_ex(ctx, ct + len, &len) <= 0) {
    goto end;
  }
  p[0] = OSSL_PARAM_construct_octet_string(OSSL_CIPHER_PARAM_AEAD_TAG, tag, 16);
  p[1] = OSSL_PARAM_construct_end();
  ok = EVP_CIPHER_CTX_get_params(ctx, p) > 0;

end:
  EVP_CIPHER_CTX_free(ctx);
  EVP_CIPHER_free(c);
  return ok;
}

static int aead_decrypt(OSSL_LIB_CTX *libctx, const char *prop,
                        const char *cipher, const unsigned char *key,
                        const unsigned char *iv, const unsigned char *aad,
                        int aadlen, const unsigned char *ct, int ctlen,
                        const unsigned char *tag, unsigned char *pt)
{
  EVP_CIPHER *c = EVP_CIPHER_fetch(libctx, cipher, prop);
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  OSSL_PARAM p[2];
  int len = 0;
  int ok = 0;

  if (c == NULL || ctx == NULL) {
    goto end;
  }
  p[0] = OSSL_PARAM_construct_octet_string(OSSL_CIPHER_PARAM_AEAD_TAG,
                                           (void *)tag, 16);
  p[1] = OSSL_PARAM_construct_end();
  /* The tag is set before the data update, as the TLS receive path does. */
  if (EVP_DecryptInit_ex2(ctx, c, key, iv, NULL) <= 0 ||
      EVP_CIPHER_CTX_set_params(ctx, p) <= 0 ||
      (aadlen > 0 && EVP_DecryptUpdate(ctx, NULL, &len, aad, aadlen) <= 0) ||
      EVP_DecryptUpdate(ctx, pt, &len, ct, ctlen) <= 0) {
    goto end;
  }
  ok = EVP_DecryptFinal_ex(ctx, pt + len, &len) > 0;

end:
  EVP_CIPHER_CTX_free(ctx);
  EVP_CIPHER_free(c);
  return ok;
}

static int test_cipher(OSSL_LIB_CTX *libctx, const char *name, int keylen)
{
  static const unsigned char key[32] = {
    0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe, 0x2b, 0x73, 0xae, 0xf0,
    0x85, 0x7d, 0x77, 0x81, 0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7,
    0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4
  };
  static const unsigned char iv[12] = {
    0xca, 0xfe, 0xba, 0xbe, 0xfa, 0xce, 0xdb, 0xad, 0xde, 0xca, 0xf8, 0x88
  };
  static const unsigned char aad[20] = {
    0xfe, 0xed, 0xfa, 0xce, 0xde, 0xad, 0xbe, 0xef, 0xfe, 0xed, 0xfa, 0xce,
    0xde, 0xad, 0xbe, 0xef, 0xab, 0xad, 0xda, 0xd2
  };
  static const unsigned char pt[80] = {
    0xd9, 0x31, 0x32, 0x25, 0xf8, 0x84, 0x06, 0xe5, 0xa5, 0x59, 0x09, 0xc5,
    0xaf, 0xf5, 0x26, 0x9a, 0x86, 0xa7, 0xa9, 0x53, 0x15, 0x34, 0xf7, 0xda,
    0x2e, 0x4c, 0x30, 0x3d, 0x8a, 0x31, 0x8a, 0x72, 0x1c, 0x3c, 0x0c, 0x95,
    0x95, 0x68, 0x09, 0x53, 0x2f, 0xcf, 0x0e, 0x24, 0x49, 0xa6, 0xb5, 0x25,
    0xb1, 0x6a, 0xed, 0xf5, 0xaa, 0x0d, 0xe6, 0x57, 0xba, 0x63, 0x7b, 0x39,
    0x1a, 0xaf, 0xd2, 0x55, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00
  };
  unsigned char ct[80], ct_def[80], tag[16], tag_def[16], out[80];
  const int ptlen = (int)sizeof(pt);

  (void)keylen;
  if (!aead_encrypt(libctx, "provider=zz9000", name, key, iv, aad,
                    (int)sizeof(aad), pt, ptlen, ct, tag)) {
    printf("FAIL: %s encrypt via provider=zz9000\n", name);
    return 1;
  }
  if (!aead_encrypt(libctx, "provider=default", name, key, iv, aad,
                    (int)sizeof(aad), pt, ptlen, ct_def, tag_def)) {
    printf("FAIL: %s encrypt via provider=default\n", name);
    return 1;
  }
  if (memcmp(ct, ct_def, ptlen) != 0 || memcmp(tag, tag_def, 16) != 0) {
    printf("FAIL: %s ciphertext/tag != default provider\n", name);
    return 1;
  }
  if (!aead_decrypt(libctx, "provider=zz9000", name, key, iv, aad,
                    (int)sizeof(aad), ct, ptlen, tag, out)) {
    printf("FAIL: %s decrypt via provider=zz9000\n", name);
    return 1;
  }
  if (memcmp(out, pt, ptlen) != 0) {
    printf("FAIL: %s decrypted plaintext mismatch\n", name);
    return 1;
  }
  /* A tampered tag must be rejected. */
  tag[0] ^= 0x01;
  if (aead_decrypt(libctx, "provider=zz9000", name, key, iv, aad,
                   (int)sizeof(aad), ct, ptlen, tag, out)) {
    printf("FAIL: %s tampered tag not rejected\n", name);
    return 1;
  }
  printf("  %s: ok (matches default provider, round-trips, tamper rejected)\n",
         name);
  return 0;
}

int main(void)
{
  OSSL_LIB_CTX *libctx = OSSL_LIB_CTX_new();
  int rc = 0;

  if (libctx == NULL ||
      !OSSL_PROVIDER_add_builtin(libctx, ZZ9K_PROVIDER_NAME,
                                 zz9k_provider_init) ||
      OSSL_PROVIDER_load(libctx, ZZ9K_PROVIDER_NAME) == NULL ||
      OSSL_PROVIDER_load(libctx, "default") == NULL) {
    printf("FAIL: provider registration\n");
    return 1;
  }

  rc |= test_cipher(libctx, "AES-128-GCM", 16);
  rc |= test_cipher(libctx, "AES-256-GCM", 32);
  rc |= test_cipher(libctx, "ChaCha20-Poly1305", 32);

  OSSL_LIB_CTX_free(libctx);
  if (rc == 0) {
    printf("provider_aead_test: passed\n");
  }
  return rc;
}
