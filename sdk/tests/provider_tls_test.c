/*
 * TLS 1.2 record-layer parity test: the ZZ9000 provider's AEAD ciphers must
 * interoperate with OpenSSL's default provider when driven exactly the way
 * libssl drives a TLS 1.2 AEAD cipher — EVP_CTRL_AEAD_SET_IV_FIXED, then per
 * record EVP_CTRL_AEAD_TLS1_AAD (whose return is the record pad) and a single
 * in-place EVP_CipherUpdate over the whole record. Each cipher is checked in
 * both directions (encrypt zz9000 / decrypt default, and vice versa), plus a
 * tamper check, so a provider that mishandles the explicit IV, the AAD length
 * fixup, or the ChaCha sequence-number nonce cannot pass.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k_provider.h"

#include <openssl/provider.h>
#include <openssl/evp.h>

#include <stdio.h>
#include <string.h>

#define TLS_AAD_LEN     13
#define TLS_EXPLICIT_IV 8
#define TLS_TAG_LEN     16
#define PAYLOAD_LEN     45

static const unsigned char key32[32] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
  0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
  0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
};
static const unsigned char fixed_iv12[12] = {
  0xca, 0xfe, 0xba, 0xbe, 0xfa, 0xce, 0xdb, 0xad, 0xde, 0xca, 0xf8, 0x88
};
static const unsigned char payload[PAYLOAD_LEN] =
    "ZZ9000 TLS 1.2 record-layer self-test payload";

/* Build the 13-byte TLS 1.2 AAD: sequence || type || version || length. */
static void make_aad(unsigned char aad[TLS_AAD_LEN], unsigned long seq,
                     size_t len_field)
{
  memset(aad, 0, 8);
  aad[7] = (unsigned char)seq;
  aad[8] = 0x17;                       /* application data */
  aad[9] = 0x03;
  aad[10] = 0x03;                      /* TLS 1.2 */
  aad[11] = (unsigned char)(len_field >> 8);
  aad[12] = (unsigned char)(len_field & 0xff);
}

/* Encrypt or decrypt one TLS 1.2 record in place, exactly as libssl does.
 * Returns the EVP_CipherUpdate output length, or -1 on failure. */
static int tls_record(OSSL_LIB_CTX *libctx, const char *prop,
                      const char *cipher, int gcm, int enc, unsigned long seq,
                      unsigned char *rec, size_t reclen, size_t aad_len_field)
{
  EVP_CIPHER *c = EVP_CIPHER_fetch(libctx, cipher, prop);
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  unsigned char aad[TLS_AAD_LEN];
  int outl = -1;
  int pad;

  if (c == NULL || ctx == NULL) {
    goto end;
  }
  /* Mirror exactly how libssl's record layer sets up each cipher (see
   * ssl/record/methods/tls1_meth.c): GCM is keyed then handed its 4-byte salt
   * via EVP_CTRL_GCM_SET_IV_FIXED; ChaCha20-Poly1305 receives its full 12-byte
   * fixed IV through the ordinary cipher init (no SET_IV_FIXED ctrl). */
  if (gcm) {
    if (EVP_CipherInit_ex2(ctx, c, key32, NULL, enc, NULL) <= 0 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IV_FIXED, 4,
                            (void *)fixed_iv12) <= 0) {
      goto end;
    }
  } else {
    if (EVP_CipherInit_ex2(ctx, c, key32, fixed_iv12, enc, NULL) <= 0) {
      goto end;
    }
  }
  make_aad(aad, seq, aad_len_field);
  pad = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_TLS1_AAD, TLS_AAD_LEN, aad);
  if (pad != TLS_TAG_LEN) {
    goto end;
  }
  if (EVP_CipherUpdate(ctx, rec, &outl, rec, (int)reclen) <= 0) {
    outl = -1;
  }

end:
  EVP_CIPHER_CTX_free(ctx);
  EVP_CIPHER_free(c);
  return outl;
}

static int test_direction(OSSL_LIB_CTX *libctx, const char *cipher, int gcm,
                          const char *enc_prop, const char *dec_prop)
{
  unsigned char rec[TLS_EXPLICIT_IV + PAYLOAD_LEN + TLS_TAG_LEN];
  size_t eiv = gcm ? TLS_EXPLICIT_IV : 0;
  size_t reclen = eiv + PAYLOAD_LEN + TLS_TAG_LEN;
  int outl;

  /* Sending: libssl hands the cipher payload || room for the tag, with the
   * explicit-IV slot (GCM) for the provider to fill, and an AAD length field
   * of payload + explicit IV. */
  memset(rec, 0, sizeof(rec));
  memcpy(rec + eiv, payload, PAYLOAD_LEN);
  outl = tls_record(libctx, enc_prop, cipher, gcm, 1, 1, rec, reclen,
                    PAYLOAD_LEN + eiv);
  if (outl != (int)reclen) {
    printf("FAIL: %s encrypt via %s (outl=%d)\n", cipher, enc_prop, outl);
    return 1;
  }
  /* Receiving: the wire record comes back with an AAD length field covering
   * the whole record; the plaintext lands after the explicit IV. */
  outl = tls_record(libctx, dec_prop, cipher, gcm, 0, 1, rec, reclen, reclen);
  if (outl != PAYLOAD_LEN) {
    printf("FAIL: %s decrypt via %s (outl=%d)\n", cipher, dec_prop, outl);
    return 1;
  }
  if (memcmp(rec + eiv, payload, PAYLOAD_LEN) != 0) {
    printf("FAIL: %s %s->%s payload mismatch\n", cipher, enc_prop, dec_prop);
    return 1;
  }
  /* Tampered record must be rejected. */
  memset(rec, 0, sizeof(rec));
  memcpy(rec + eiv, payload, PAYLOAD_LEN);
  if (tls_record(libctx, enc_prop, cipher, gcm, 1, 2, rec, reclen,
                 PAYLOAD_LEN + eiv) != (int)reclen) {
    printf("FAIL: %s tamper-test encrypt via %s\n", cipher, enc_prop);
    return 1;
  }
  rec[eiv] ^= 0x01;
  if (tls_record(libctx, dec_prop, cipher, gcm, 0, 2, rec, reclen, reclen) >=
      0) {
    printf("FAIL: %s tampered record accepted by %s\n", cipher, dec_prop);
    return 1;
  }
  return 0;
}

static int test_cipher(OSSL_LIB_CTX *libctx, const char *cipher, int gcm)
{
  if (test_direction(libctx, cipher, gcm, "provider=zz9000",
                     "provider=default") ||
      test_direction(libctx, cipher, gcm, "provider=default",
                     "provider=zz9000") ||
      test_direction(libctx, cipher, gcm, "provider=zz9000",
                     "provider=zz9000")) {
    return 1;
  }
  printf("  %s: ok (TLS 1.2 records interoperate with default provider)\n",
         cipher);
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

  rc |= test_cipher(libctx, "AES-128-GCM", 1);
  rc |= test_cipher(libctx, "AES-256-GCM", 1);
  rc |= test_cipher(libctx, "ChaCha20-Poly1305", 0);

  OSSL_LIB_CTX_free(libctx);
  if (rc == 0) {
    printf("provider_tls_test: passed\n");
  }
  return rc;
}
