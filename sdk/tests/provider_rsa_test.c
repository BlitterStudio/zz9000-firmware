/*
 * Phase 4.4b/C host parity test: RSA signature verification through the
 * ZZ9000 provider, for RSA-2048 and RSA-3072. A key and a signature are
 * produced with OpenSSL's default provider; the public key is bridged into
 * the ZZ9000 provider's RSA KEYMGMT (via todata/fromdata) and the signature
 * verified through the provider's RSA SIGNATURE op. A valid signature must
 * verify, a tampered digest must not, and the result must agree with the
 * default provider.
 *
 * test_rsa() covers PKCS#1 v1.5 + SHA-256 (the accelerated path: ZZ9000
 * offload on the Amiga, the software reference here) for RSA-2048 and
 * RSA-3072 (the >2048-bit path). test_rsa_pss() covers RSA-PSS (the
 * delegated path — the firmware/software reference only does PKCS#1, so a
 * PSS verify must forward the padding/digest/MGF1/salt-length params to a
 * shadow default-provider EVP_PKEY built at import time; this is the common
 * TLS1.3 RSA case, see provider_tls_handshake_test's "TLS1.3 RSA-2048 ...
 * (PSS)" case for the end-to-end version of the same path).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k_provider.h"

#include <openssl/provider.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/rsa.h>

#include <stdio.h>
#include <string.h>

static int verify(OSSL_LIB_CTX *libctx, const char *prop, EVP_PKEY *key,
                  const unsigned char *sig, size_t siglen,
                  const unsigned char *digest)
{
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_pkey(libctx, key, prop);
  int rc = -1;

  if (ctx != NULL && EVP_PKEY_verify_init(ctx) > 0 &&
      EVP_PKEY_CTX_set_signature_md(ctx, EVP_sha256()) > 0) {
    rc = EVP_PKEY_verify(ctx, sig, siglen, digest, 32);
  }
  EVP_PKEY_CTX_free(ctx);
  return rc;
}

static int test_rsa(OSSL_LIB_CTX *libctx, unsigned int bits)
{
  EVP_PKEY *genkey = NULL;
  EVP_PKEY *ourkey = NULL;
  EVP_PKEY_CTX *sctx = NULL;
  EVP_PKEY_CTX *kctx = NULL;
  OSSL_PARAM *params = NULL;
  unsigned char digest[32];
  unsigned char sig[512];
  size_t siglen = sizeof(sig);
  int rc = 1;
  unsigned int i;

  for (i = 0; i < sizeof(digest); i++) {
    digest[i] = (unsigned char)(0x40 + i);
  }

  genkey = EVP_PKEY_Q_keygen(libctx, "provider=default", "RSA", (size_t)bits);
  if (genkey == NULL) {
    printf("FAIL: RSA-%u keygen\n", bits);
    goto done;
  }
  sctx = EVP_PKEY_CTX_new_from_pkey(libctx, genkey, "provider=default");
  if (sctx == NULL || EVP_PKEY_sign_init(sctx) <= 0 ||
      EVP_PKEY_CTX_set_signature_md(sctx, EVP_sha256()) <= 0 ||
      EVP_PKEY_sign(sctx, sig, &siglen, digest, sizeof(digest)) <= 0) {
    printf("FAIL: RSA-%u sign\n", bits);
    goto done;
  }

  /* Bridge the public key into the ZZ9000 provider's RSA KEYMGMT. */
  if (EVP_PKEY_todata(genkey, EVP_PKEY_PUBLIC_KEY, &params) <= 0) {
    printf("FAIL: RSA-%u todata\n", bits);
    goto done;
  }
  kctx = EVP_PKEY_CTX_new_from_name(libctx, "RSA", "provider=zz9000");
  if (kctx == NULL || EVP_PKEY_fromdata_init(kctx) <= 0 ||
      EVP_PKEY_fromdata(kctx, &ourkey, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
    printf("FAIL: RSA-%u import via provider=zz9000\n", bits);
    goto done;
  }

  if (verify(libctx, "provider=zz9000", ourkey, sig, siglen, digest) != 1) {
    printf("FAIL: RSA-%u valid signature not accepted by provider=zz9000\n",
           bits);
    goto done;
  }
  if (verify(libctx, "provider=default", genkey, sig, siglen, digest) != 1) {
    printf("FAIL: RSA-%u valid signature not accepted by default\n", bits);
    goto done;
  }
  digest[0] ^= 0x01;
  if (verify(libctx, "provider=zz9000", ourkey, sig, siglen, digest) == 1) {
    printf("FAIL: RSA-%u tampered digest accepted by provider=zz9000\n", bits);
    goto done;
  }

  printf("  RSA-%u: ok (verify matches default, tamper rejected)\n", bits);
  rc = 0;

done:
  OSSL_PARAM_free(params);
  EVP_PKEY_CTX_free(kctx);
  EVP_PKEY_CTX_free(sctx);
  EVP_PKEY_free(ourkey);
  EVP_PKEY_free(genkey);
  return rc;
}

/* RSA-PSS: the delegated path. Same shape as verify() above but with PSS
 * padding/salt-length set on the EVP_PKEY_CTX before EVP_PKEY_verify — this
 * is what forces our provider's RSA SIGNATURE op to capture and replay the
 * PSS params onto the shadow default-provider key instead of accelerating. */
static int verify_pss(OSSL_LIB_CTX *libctx, const char *prop, EVP_PKEY *key,
                      const unsigned char *sig, size_t siglen,
                      const unsigned char *digest)
{
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_pkey(libctx, key, prop);
  int rc = -1;

  if (ctx != NULL && EVP_PKEY_verify_init(ctx) > 0 &&
      EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PSS_PADDING) > 0 &&
      EVP_PKEY_CTX_set_signature_md(ctx, EVP_sha256()) > 0 &&
      EVP_PKEY_CTX_set_rsa_pss_saltlen(ctx, RSA_PSS_SALTLEN_DIGEST) > 0) {
    rc = EVP_PKEY_verify(ctx, sig, siglen, digest, 32);
  }
  EVP_PKEY_CTX_free(ctx);
  return rc;
}

static int test_rsa_pss(OSSL_LIB_CTX *libctx, unsigned int bits)
{
  EVP_PKEY *genkey = NULL;
  EVP_PKEY *ourkey = NULL;
  EVP_PKEY_CTX *sctx = NULL;
  EVP_PKEY_CTX *kctx = NULL;
  OSSL_PARAM *params = NULL;
  unsigned char digest[32];
  unsigned char sig[512];
  size_t siglen = sizeof(sig);
  int rc = 1;
  unsigned int i;

  for (i = 0; i < sizeof(digest); i++) {
    digest[i] = (unsigned char)(0x80 + i);
  }

  genkey = EVP_PKEY_Q_keygen(libctx, "provider=default", "RSA", (size_t)bits);
  if (genkey == NULL) {
    printf("FAIL: RSA-%u PSS keygen\n", bits);
    goto done;
  }
  sctx = EVP_PKEY_CTX_new_from_pkey(libctx, genkey, "provider=default");
  if (sctx == NULL || EVP_PKEY_sign_init(sctx) <= 0 ||
      EVP_PKEY_CTX_set_rsa_padding(sctx, RSA_PKCS1_PSS_PADDING) <= 0 ||
      EVP_PKEY_CTX_set_signature_md(sctx, EVP_sha256()) <= 0 ||
      EVP_PKEY_CTX_set_rsa_pss_saltlen(sctx, RSA_PSS_SALTLEN_DIGEST) <= 0 ||
      EVP_PKEY_sign(sctx, sig, &siglen, digest, sizeof(digest)) <= 0) {
    printf("FAIL: RSA-%u PSS sign\n", bits);
    goto done;
  }

  /* Bridge the public key into the ZZ9000 provider's RSA KEYMGMT, exactly as
   * test_rsa() does — the same imported key object serves both the PKCS#1
   * (accelerated) and PSS (delegated) verify paths. */
  if (EVP_PKEY_todata(genkey, EVP_PKEY_PUBLIC_KEY, &params) <= 0) {
    printf("FAIL: RSA-%u PSS todata\n", bits);
    goto done;
  }
  kctx = EVP_PKEY_CTX_new_from_name(libctx, "RSA", "provider=zz9000");
  if (kctx == NULL || EVP_PKEY_fromdata_init(kctx) <= 0 ||
      EVP_PKEY_fromdata(kctx, &ourkey, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
    printf("FAIL: RSA-%u PSS import via provider=zz9000\n", bits);
    goto done;
  }

  if (verify_pss(libctx, "provider=zz9000", ourkey, sig, siglen, digest) != 1) {
    printf("FAIL: RSA-%u PSS valid signature not accepted by provider=zz9000 "
           "(delegation)\n", bits);
    goto done;
  }
  if (verify_pss(libctx, "provider=default", genkey, sig, siglen, digest) != 1) {
    printf("FAIL: RSA-%u PSS valid signature not accepted by default\n", bits);
    goto done;
  }
  digest[0] ^= 0x01;
  if (verify_pss(libctx, "provider=zz9000", ourkey, sig, siglen, digest) == 1) {
    printf("FAIL: RSA-%u PSS tampered digest accepted by provider=zz9000\n",
           bits);
    goto done;
  }

  printf("  RSA-%u PSS: ok (delegated verify matches default, tamper "
         "rejected)\n", bits);
  rc = 0;

done:
  OSSL_PARAM_free(params);
  EVP_PKEY_CTX_free(kctx);
  EVP_PKEY_CTX_free(sctx);
  EVP_PKEY_free(ourkey);
  EVP_PKEY_free(genkey);
  return rc;
}

/* Regression guard for the "own the whole RSA keytype" completeness: with
 * provider=zz9000 preferred, RSA keygen + signing (the client-certificate
 * path) must still work. Both are fully delegated (the firmware has no RSA
 * keygen or sign primitive). Before the gen/sign delegation these FAILED (the
 * RSA keymgmt had no gen and the signature op had no sign), a regression versus
 * v2.2.0 which left RSA to the default provider. The signature is checked under
 * the default provider via a bridged public key. */
static int test_rsa_gen_sign(OSSL_LIB_CTX *libctx, unsigned int bits)
{
  EVP_PKEY *ourkey = NULL;
  EVP_PKEY *pub = NULL;
  OSSL_PARAM *params = NULL;
  EVP_PKEY_CTX *kctx = NULL;
  EVP_MD_CTX *sctx = NULL;
  EVP_MD_CTX *vctx = NULL;
  unsigned char sig[512];
  size_t siglen = sizeof(sig);
  const unsigned char msg[] = "client rsa CertificateVerify transcript";
  int vrc = -1;
  int rc = 1;

  ourkey = EVP_PKEY_Q_keygen(libctx, "provider=zz9000", "RSA", (size_t)bits);
  if (ourkey == NULL) {
    printf("FAIL: RSA-%u keygen via provider=zz9000\n", bits);
    goto done;
  }
  sctx = EVP_MD_CTX_new();
  if (sctx == NULL ||
      EVP_DigestSignInit_ex(sctx, NULL, "SHA256", libctx, "provider=zz9000",
                            ourkey, NULL) <= 0 ||
      EVP_DigestSign(sctx, sig, &siglen, msg, sizeof(msg)) <= 0) {
    printf("FAIL: RSA-%u sign via provider=zz9000\n", bits);
    goto done;
  }
  if (EVP_PKEY_todata(ourkey, EVP_PKEY_PUBLIC_KEY, &params) <= 0) {
    printf("FAIL: RSA-%u gen todata\n", bits);
    goto done;
  }
  kctx = EVP_PKEY_CTX_new_from_name(libctx, "RSA", "provider=default");
  if (kctx == NULL || EVP_PKEY_fromdata_init(kctx) <= 0 ||
      EVP_PKEY_fromdata(kctx, &pub, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
    printf("FAIL: RSA-%u bridge public key to default\n", bits);
    goto done;
  }
  vctx = EVP_MD_CTX_new();
  if (vctx != NULL &&
      EVP_DigestVerifyInit_ex(vctx, NULL, "SHA256", libctx, "provider=default",
                              pub, NULL) > 0) {
    vrc = EVP_DigestVerify(vctx, sig, siglen, msg, sizeof(msg));
  }
  if (vrc != 1) {
    printf("FAIL: RSA-%u signature rejected by default provider\n", bits);
    goto done;
  }
  printf("  RSA-%u: gen+sign ok (delegated)\n", bits);
  rc = 0;

done:
  OSSL_PARAM_free(params);
  EVP_MD_CTX_free(vctx);
  EVP_PKEY_CTX_free(kctx);
  EVP_MD_CTX_free(sctx);
  EVP_PKEY_free(pub);
  EVP_PKEY_free(ourkey);
  return rc;
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

  rc |= test_rsa(libctx, 2048);
  rc |= test_rsa(libctx, 3072);
  rc |= test_rsa_pss(libctx, 2048);
  rc |= test_rsa_gen_sign(libctx, 2048);

  OSSL_LIB_CTX_free(libctx);
  if (rc == 0) {
    printf("provider_rsa_test: passed\n");
  }
  return rc;
}
