/*
 * Phase 4.2 host parity test: X25519 ECDH through the ZZ9000 provider matches
 * the RFC 7748 section 6.1 known answer and OpenSSL's default provider. Keys
 * are created with provider=zz9000 (exercising the provider's KEYMGMT import
 * and public-key derivation) and the shared secret is computed through the
 * provider's KEYEXCH derive. On the host the derive uses the software
 * reference; on the Amiga the same path offloads to the ZZ9000.
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

/* RFC 7748 section 6.1. */
static const unsigned char alice_priv[32] = {
  0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d, 0x3c, 0x16, 0xc1, 0x72,
  0x51, 0xb2, 0x66, 0x45, 0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
  0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a
};
static const unsigned char bob_pub[32] = {
  0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4, 0xd3, 0x5b, 0x61, 0xc2,
  0xec, 0xe4, 0x35, 0x37, 0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78, 0x67, 0x4d,
  0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f
};
static const unsigned char shared_k[32] = {
  0x4a, 0x5d, 0x9d, 0x5b, 0xa4, 0xce, 0x2d, 0xe1, 0x72, 0x8e, 0x3b, 0xf4,
  0x80, 0x35, 0x0f, 0x25, 0xe0, 0x7e, 0x21, 0xc9, 0x47, 0xd1, 0x9e, 0x33,
  0x76, 0xf0, 0x9b, 0x3c, 0x1e, 0x16, 0x17, 0x42
};

static EVP_PKEY *import_key(OSSL_LIB_CTX *libctx, const char *prop,
                            const char *param_key, const unsigned char *raw,
                            int selection)
{
  EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_from_name(libctx, "X25519", prop);
  EVP_PKEY *pkey = NULL;
  OSSL_PARAM params[2];

  if (kctx == NULL) {
    return NULL;
  }
  params[0] = OSSL_PARAM_construct_octet_string(param_key, (void *)raw, 32);
  params[1] = OSSL_PARAM_construct_end();
  if (EVP_PKEY_fromdata_init(kctx) <= 0 ||
      EVP_PKEY_fromdata(kctx, &pkey, selection, params) <= 0) {
    pkey = NULL;
  }
  EVP_PKEY_CTX_free(kctx);
  return pkey;
}

static int derive(OSSL_LIB_CTX *libctx, const char *prop, EVP_PKEY *priv,
                  EVP_PKEY *peer, unsigned char out[32])
{
  EVP_PKEY_CTX *dctx = EVP_PKEY_CTX_new_from_pkey(libctx, priv, prop);
  size_t len = 32;
  int ok = 0;

  if (dctx == NULL) {
    return 0;
  }
  if (EVP_PKEY_derive_init(dctx) > 0 &&
      EVP_PKEY_derive_set_peer(dctx, peer) > 0 &&
      EVP_PKEY_derive(dctx, out, &len) > 0 && len == 32) {
    ok = 1;
  }
  EVP_PKEY_CTX_free(dctx);
  return ok;
}

int main(void)
{
  OSSL_LIB_CTX *libctx;
  EVP_PKEY *priv = NULL;
  EVP_PKEY *peer = NULL;
  unsigned char secret[32];
  unsigned char secret_def[32];
  int rc = 1;

  libctx = OSSL_LIB_CTX_new();
  if (libctx == NULL ||
      !OSSL_PROVIDER_add_builtin(libctx, ZZ9K_PROVIDER_NAME,
                                 zz9k_provider_init) ||
      OSSL_PROVIDER_load(libctx, ZZ9K_PROVIDER_NAME) == NULL ||
      /* A custom libctx does not auto-load the default provider; it is needed
       * both for the independent oracle and for real "?provider=zz9000"
       * fallback. */
      OSSL_PROVIDER_load(libctx, "default") == NULL) {
    printf("FAIL: provider registration\n");
    return 1;
  }

  /* Keys created through our provider (KEYMGMT import + pub derivation). */
  priv = import_key(libctx, "provider=zz9000", OSSL_PKEY_PARAM_PRIV_KEY,
                    alice_priv, EVP_PKEY_KEYPAIR);
  peer = import_key(libctx, "provider=zz9000", OSSL_PKEY_PARAM_PUB_KEY,
                    bob_pub, EVP_PKEY_PUBLIC_KEY);
  if (priv == NULL || peer == NULL) {
    printf("FAIL: key import via provider=zz9000\n");
    goto done;
  }

  /* Derive through our provider's KEYEXCH. */
  if (!derive(libctx, "provider=zz9000", priv, peer, secret)) {
    printf("FAIL: derive via provider=zz9000\n");
    goto done;
  }
  if (memcmp(secret, shared_k, 32) != 0) {
    printf("FAIL: shared secret != RFC 7748 known answer\n");
    goto done;
  }

  /* Independent oracle: OpenSSL's default provider, with its own keys built
   * from the same raw bytes (keys are bound to the provider that created
   * them, so the default provider gets a fresh keypair rather than ours). */
  {
    EVP_PKEY *priv_def = import_key(libctx, "provider=default",
                                    OSSL_PKEY_PARAM_PRIV_KEY, alice_priv,
                                    EVP_PKEY_KEYPAIR);
    EVP_PKEY *peer_def = import_key(libctx, "provider=default",
                                    OSSL_PKEY_PARAM_PUB_KEY, bob_pub,
                                    EVP_PKEY_PUBLIC_KEY);
    int ok = priv_def != NULL && peer_def != NULL &&
             derive(libctx, "provider=default", priv_def, peer_def,
                    secret_def);
    EVP_PKEY_free(peer_def);
    EVP_PKEY_free(priv_def);
    if (!ok) {
      printf("FAIL: default-provider oracle derive\n");
      goto done;
    }
  }
  if (memcmp(secret, secret_def, 32) != 0) {
    printf("FAIL: provider secret != default provider secret\n");
    goto done;
  }

  printf("provider_x25519_test: passed (matches RFC 7748 + default provider)\n");
  rc = 0;

done:
  EVP_PKEY_free(peer);
  EVP_PKEY_free(priv);
  OSSL_LIB_CTX_free(libctx);
  return rc;
}
