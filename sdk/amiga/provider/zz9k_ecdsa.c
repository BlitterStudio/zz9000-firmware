/*
 * ZZ9000 OpenSSL provider — EC key management, P-256 ECDH key exchange, and
 * ECDSA signature verification.
 *
 * The provider owns the OpenSSL "EC" keytype so P-256 ECDHE (keygen +
 * derive) and ECDSA-P256/SHA-256 certificate-chain verification can run
 * through it. "EC" spans every curve, though, and only P-256 is ever
 * accelerated (offload on the Amiga, the SDK's software reference on the
 * host), so the KEYMGMT tags each key object `is_p256`:
 *
 *   - is_p256: a 65-byte uncompressed point and/or a 32-byte private scalar
 *     live directly in the key object; ECDH derive and ECDSA verify run
 *     through the ZZ9000 offload hooks below.
 *   - otherwise: the import material is used to build a "shadow" EVP_PKEY
 *     against the DEFAULT provider (EVP_PKEY_fromdata with
 *     "provider=default"), built lazily at import time. The only operation
 *     that ever needs a non-P256 key is ECDSA verify, which forwards to the
 *     shadow via EVP_PKEY_verify — fail-closed on any error or missing
 *     shadow. This is what lets the provider safely shadow the whole "EC"
 *     keytype (forced by owning P-256 ECDHE, since amissl's
 *     "?provider=zz9000" default property query would otherwise route every
 *     EC fetch, including non-P256 certificate keys, to us) without breaking
 *     chain verification for P-384/P-521 servers.
 *
 * Only the secp256r1 TLS group is declared (zz9k_provider.c), so the ECDH
 * KEYEXCH and the KEYMGMT's gen() only ever see P-256 keys; delegation is
 * exercised solely by signature verify.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k_prov_local.h"

#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/param_build.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/bn.h>
#include <openssl/evp.h>

#include "zz9k-crypto-soft.h"
#include "zz9k_offload.h"

#include <string.h>

#define ZZ9K_P256_POINT_LEN 65
#define ZZ9K_P256_SCALAR_LEN 32

/* ---- Default-provider software fallback for the P-256 key exchange ----
 *
 * The board runs P-256 keygen and derive in ~10 ms, but only when it is free.
 * During live browsing the ZZ9000 is also driving the display, and a mailbox
 * reply can land after zz9k_call's poll timeout, so the offload reports
 * failure. Failing the TLS key share then would stall the handshake far worse
 * than stock amissl. Instead we recompute the operation through the *default*
 * provider — AmiSSL's optimised software, roughly 10x faster than the in-tree
 * zz9k_soft reference (which is why that reference is NOT used here) — so the
 * P-256 path is never slower than stock. This also covers older firmware that
 * does not advertise the keygen capability. "provider=default" is forced so
 * the fetch cannot bridge back into this provider and recurse. Compiled for
 * the Amiga offload build and for the host ZZ9K_TEST_DEFAULT_FALLBACK build
 * that parity-tests this path against the default provider. */
#if defined(ZZ9K_PROVIDER_OFFLOAD) || defined(ZZ9K_TEST_DEFAULT_FALLBACK)
/* Generate a fresh P-256 keypair via the default provider and copy the raw
 * 32-byte scalar and 65-byte uncompressed point out. Both are read from a
 * fully generated keypair, so no lazy public-point computation is relied on. */
static int zz9k_p256_default_genkey(unsigned char priv[ZZ9K_P256_SCALAR_LEN],
                                    unsigned char point[ZZ9K_P256_POINT_LEN],
                                    ZZ9K_PROV_CTX *provctx)
{
  OSSL_LIB_CTX *libctx =
      (OSSL_LIB_CTX *)(provctx != NULL ? provctx->libctx : NULL);
  EVP_PKEY_CTX *gctx;
  EVP_PKEY *pkey = NULL;
  BIGNUM *d = NULL;
  OSSL_PARAM gp[2];
  size_t publen = 0;
  int rc = 0;

  gctx = EVP_PKEY_CTX_new_from_name(libctx, "EC", "provider=default");
  if (gctx == NULL) {
    return 0;
  }
  ZZ9K_PARAM_UTF8(&gp[0], OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1");
  ZZ9K_PARAM_END(&gp[1]);
  if (EVP_PKEY_keygen_init(gctx) <= 0 ||
      EVP_PKEY_CTX_set_params(gctx, gp) <= 0 ||
      EVP_PKEY_generate(gctx, &pkey) <= 0) {
    goto done;
  }
  if (EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PUB_KEY, point,
                                      ZZ9K_P256_POINT_LEN, &publen) <= 0 ||
      publen != ZZ9K_P256_POINT_LEN || point[0] != 0x04) {
    goto done;
  }
  if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_PRIV_KEY, &d) <= 0 ||
      BN_bn2binpad(d, priv, ZZ9K_P256_SCALAR_LEN) != ZZ9K_P256_SCALAR_LEN) {
    goto done;
  }
  rc = 1;
done:
  BN_clear_free(d);
  EVP_PKEY_free(pkey);
  EVP_PKEY_CTX_free(gctx);
  return rc;
}

/* out = X-coordinate of scalar*peer via the default provider. */
static int zz9k_p256_default_derive(
    unsigned char out[ZZ9K_P256_SCALAR_LEN],
    const unsigned char scalar[ZZ9K_P256_SCALAR_LEN],
    const unsigned char peer[ZZ9K_P256_POINT_LEN], ZZ9K_PROV_CTX *provctx)
{
  OSSL_LIB_CTX *libctx =
      (OSSL_LIB_CTX *)(provctx != NULL ? provctx->libctx : NULL);
  EVP_PKEY_CTX *kctx = NULL;
  EVP_PKEY_CTX *dctx = NULL;
  EVP_PKEY *mine = NULL;
  EVP_PKEY *theirs = NULL;
  OSSL_PARAM_BLD *bld = NULL;
  OSSL_PARAM *params = NULL;
  OSSL_PARAM peer_params[3];
  BIGNUM *priv = NULL;
  size_t outlen = ZZ9K_P256_SCALAR_LEN;
  int n = 0;
  int rc = 0;

  priv = BN_bin2bn(scalar, ZZ9K_P256_SCALAR_LEN, NULL);
  bld = OSSL_PARAM_BLD_new();
  if (priv == NULL || bld == NULL) {
    goto done;
  }
  if (!OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME,
                                       "prime256v1", 0) ||
      !OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, priv)) {
    goto done;
  }
  params = OSSL_PARAM_BLD_to_param(bld);
  if (params == NULL) {
    goto done;
  }
  kctx = EVP_PKEY_CTX_new_from_name(libctx, "EC", "provider=default");
  if (kctx == NULL || EVP_PKEY_fromdata_init(kctx) <= 0 ||
      EVP_PKEY_fromdata(kctx, &mine, EVP_PKEY_KEYPAIR, params) <= 0) {
    goto done;
  }
  EVP_PKEY_CTX_free(kctx);
  kctx = NULL;

  ZZ9K_PARAM_UTF8(&peer_params[n], OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1");
  n++;
  ZZ9K_PARAM_OCTET(&peer_params[n], OSSL_PKEY_PARAM_PUB_KEY, peer,
                   ZZ9K_P256_POINT_LEN);
  n++;
  ZZ9K_PARAM_END(&peer_params[n]);
  kctx = EVP_PKEY_CTX_new_from_name(libctx, "EC", "provider=default");
  if (kctx == NULL || EVP_PKEY_fromdata_init(kctx) <= 0 ||
      EVP_PKEY_fromdata(kctx, &theirs, EVP_PKEY_PUBLIC_KEY, peer_params) <= 0) {
    goto done;
  }

  dctx = EVP_PKEY_CTX_new_from_pkey(libctx, mine, "provider=default");
  if (dctx == NULL || EVP_PKEY_derive_init(dctx) <= 0 ||
      EVP_PKEY_derive_set_peer(dctx, theirs) <= 0 ||
      EVP_PKEY_derive(dctx, out, &outlen) <= 0 ||
      outlen != ZZ9K_P256_SCALAR_LEN) {
    goto done;
  }
  rc = 1;
done:
  OSSL_PARAM_free(params);
  OSSL_PARAM_BLD_free(bld);
  BN_clear_free(priv);
  EVP_PKEY_free(mine);
  EVP_PKEY_free(theirs);
  EVP_PKEY_CTX_free(kctx);
  EVP_PKEY_CTX_free(dctx);
  return rc;
}
#endif /* ZZ9K_PROVIDER_OFFLOAD || ZZ9K_TEST_DEFAULT_FALLBACK */

/* ---- P-256 keygen / derive hooks (mirror zz9k_prov_x25519 in zz9k_x25519.c) ---- */

/* pub = scalar*G on the board. Returns 0 when the board cannot run keygen
 * (capability absent, or a mailbox timeout under display contention); the
 * caller (zz9k_ec_gen) then falls back to a default-provider keypair via
 * zz9k_p256_default_genkey rather than failing the key share.
 *
 * CONTRACT: this is a 0/1 BOOLEAN, NOT the -1-on-miss sentinel used by
 * zz9k_prov_{ecdsa,rsa}_verify. Callers use the result directly in boolean `&&`
 * context (the import path at zz9k_ec_import and the keygen at zz9k_ec_gen), so
 * returning -1 here would read as truthy and fail OPEN — a partial key marked
 * has_pub with a garbage point. Keep the boolean contract; the fallback lives in
 * the caller, not in a sentinel. Same for zz9k_prov_p256_derive below. */
static int zz9k_prov_p256_keygen(unsigned char pub[ZZ9K_P256_POINT_LEN],
                                 const unsigned char scalar[ZZ9K_P256_SCALAR_LEN],
                                 ZZ9K_PROV_CTX *provctx)
{
#ifdef ZZ9K_PROVIDER_OFFLOAD
  if (!ZZ9K_PROV_CAN_OFFLOAD(provctx, ZZ9K_SERVICE_FLAG_CRYPTO_P256_KEYGEN)) {
    return 0;
  }
  return zz9k_offload_p256_keygen(provctx->sdk_ctx, pub, scalar) > 0 ? 1 : 0;
#elif defined(ZZ9K_TEST_DEFAULT_FALLBACK)
  (void)pub;
  (void)scalar;
  (void)provctx;
  return 0;   /* force zz9k_ec_gen to exercise the default-provider fallback */
#else
  (void)provctx;
  return zz9k_soft_p256_keygen(pub, scalar);
#endif
}

/* out = X-coordinate of scalar*peer. */
static int zz9k_prov_p256_derive(unsigned char out[ZZ9K_P256_SCALAR_LEN],
                                 const unsigned char scalar[ZZ9K_P256_SCALAR_LEN],
                                 const unsigned char peer[ZZ9K_P256_POINT_LEN],
                                 ZZ9K_PROV_CTX *provctx)
{
#ifdef ZZ9K_PROVIDER_OFFLOAD
  /* Try the board; on capability-absent or a contention timeout, fall back to
   * the default provider (zz9k_p256_default_derive) instead of failing. */
  if (ZZ9K_PROV_CAN_OFFLOAD(provctx, ZZ9K_SERVICE_FLAG_CRYPTO_P256) &&
      zz9k_offload_p256_derive(provctx->sdk_ctx, out, scalar, peer) > 0) {
    return 1;
  }
  return zz9k_p256_default_derive(out, scalar, peer, provctx);
#elif defined(ZZ9K_TEST_DEFAULT_FALLBACK)
  return zz9k_p256_default_derive(out, scalar, peer, provctx);
#else
  (void)provctx;
  return zz9k_soft_p256_ecdh(out, scalar, peer);
#endif
}

static int zz9k_prov_ecdsa_verify(const unsigned char r[32],
                                  const unsigned char s[32],
                                  const unsigned char *hash,
                                  const unsigned char point[65],
                                  ZZ9K_PROV_CTX *provctx)
{
#ifdef ZZ9K_PROVIDER_OFFLOAD
  /* Verify on the ZZ9000 (zz9k_crypto_verify) when the firmware advertises
   * ECDSA-P256. The signature is passed as r||s (64 bytes) and the key as the
   * 65-byte uncompressed point. */
  if (ZZ9K_PROV_CAN_OFFLOAD(provctx, ZZ9K_SERVICE_FLAG_CRYPTO_ECDSA_P256)) {
    unsigned char sig[64];
    int valid = 0;
    memcpy(sig, r, 32);
    memcpy(sig + 32, s, 32);
    if (zz9k_offload_verify(provctx->sdk_ctx, ZZ9K_OFFLOAD_VERIFY_ECDSA_P256,
                            hash, sig, sizeof(sig), point, ZZ9K_P256_POINT_LEN,
                            &valid) >= 0) {
      return valid;
    }
  }
  /* Capability absent, or the offload timed out because the ZZ9000 is busy
   * driving the display: return -1 so the caller falls back to the default
   * provider (fast) instead of the ~19x-slower in-tree software reference
   * (used only on the host build below). */
  return -1;
#elif defined(ZZ9K_TEST_DEFAULT_FALLBACK)
  (void)r;
  (void)s;
  (void)hash;
  (void)point;
  (void)provctx;
  return -1;   /* force the caller to exercise the via_default verify fallback */
#else
  (void)provctx;
  return zz9k_soft_ecdsa_verify_p256(r, s, hash, point);
#endif
}

/* Copy a DER INTEGER body (big-endian, possibly with a leading sign 0x00 or
 * shorter than 32 bytes) right-aligned into out[32]. Returns 1 on success. */
static int zz9k_der_int_to_32(const unsigned char *p, size_t len,
                              unsigned char out[32])
{
  while (len > 0 && p[0] == 0x00) {
    p++;
    len--;
  }
  if (len > 32) {
    return 0;
  }
  memset(out, 0, 32);
  memcpy(out + (32 - len), p, len);
  return 1;
}

/* Decode a DER ECDSA-Sig-Value (SEQUENCE { INTEGER r, INTEGER s }) into raw
 * 32-byte r and s. Only the short-form lengths used by P-256 are accepted. */
static int zz9k_decode_ecdsa_sig(const unsigned char *der, size_t derlen,
                                 unsigned char r[32], unsigned char s[32])
{
  size_t pos = 0;
  size_t seqlen, ilen;

  if (derlen < 2 || der[pos++] != 0x30) {
    return 0;
  }
  seqlen = der[pos++];
  if ((seqlen & 0x80) != 0 || pos + seqlen != derlen) {
    return 0;
  }
  if (pos >= derlen || der[pos++] != 0x02) {        /* INTEGER r */
    return 0;
  }
  ilen = der[pos++];
  if (ilen == 0 || pos + ilen > derlen || !zz9k_der_int_to_32(der + pos, ilen, r)) {
    return 0;
  }
  pos += ilen;
  if (pos >= derlen || der[pos++] != 0x02) {        /* INTEGER s */
    return 0;
  }
  ilen = der[pos++];
  if (ilen == 0 || pos + ilen != derlen || !zz9k_der_int_to_32(der + pos, ilen, s)) {
    return 0;
  }
  return 1;
}

/* ---- EC KEYMGMT ---- */

typedef struct {
  ZZ9K_PROV_CTX *provctx;
  int is_p256;
  unsigned char point[ZZ9K_P256_POINT_LEN];
  int has_pub;
  unsigned char priv[ZZ9K_P256_SCALAR_LEN];
  int has_priv;
  EVP_PKEY *shadow;   /* delegated (non-P256) key material, default provider */
} ZZ9K_EC_KEY;

static void *zz9k_ec_keymgmt_new(void *provctx)
{
  ZZ9K_EC_KEY *key = OPENSSL_zalloc(sizeof(*key));
  if (key != NULL) {
    key->provctx = provctx;
  }
  return key;
}

static void zz9k_ec_keymgmt_free(void *keydata)
{
  ZZ9K_EC_KEY *key = keydata;
  if (key != NULL) {
    EVP_PKEY_free(key->shadow);
    OPENSSL_cleanse(key->priv, sizeof(key->priv));
    OPENSSL_free(key);
  }
}

static int zz9k_ec_keymgmt_has(const void *keydata, int selection)
{
  const ZZ9K_EC_KEY *key = keydata;

  if (key == NULL) {
    return 0;
  }
  if (!key->is_p256) {
    if (key->shadow == NULL) {
      return 0;
    }
    if ((selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) != 0 && !key->has_pub) {
      return 0;
    }
    if ((selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) != 0 && !key->has_priv) {
      return 0;
    }
    return 1;
  }
  if ((selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) != 0 && !key->has_pub) {
    return 0;
  }
  if ((selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) != 0 && !key->has_priv) {
    return 0;
  }
  return 1;
}

static int zz9k_ec_keymgmt_import(void *keydata, int selection,
                                  const OSSL_PARAM params[])
{
  ZZ9K_EC_KEY *key = keydata;
  const OSSL_PARAM *p;
  char group[32];
  char *gp = group;
  int is_p256_group;

  if (key == NULL) {
    return 0;
  }
  group[0] = '\0';
  p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_GROUP_NAME);
  if (p == NULL || !OSSL_PARAM_get_utf8_string(p, &gp, sizeof(group))) {
    return 0;   /* the curve is what classifies accelerated vs. delegated */
  }
  is_p256_group = (strcmp(group, "prime256v1") == 0 ||
                   strcmp(group, "P-256") == 0);

  if (is_p256_group) {
    key->is_p256 = 1;
    EVP_PKEY_free(key->shadow);
    key->shadow = NULL;

    if ((selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) != 0) {
      p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_PUB_KEY);
      if (p != NULL) {
        void *out = key->point;
        size_t len = 0;
        if (!OSSL_PARAM_get_octet_string(p, &out, ZZ9K_P256_POINT_LEN, &len) ||
            len != ZZ9K_P256_POINT_LEN || key->point[0] != 0x04) {
          return 0;
        }
        key->has_pub = 1;
      }
    }
    if ((selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) != 0) {
      p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_PRIV_KEY);
      if (p != NULL) {
        /* An EC private key is canonically an OSSL_PARAM BIGNUM (unsigned
         * integer) — unlike X25519's octet string — so a loaded P-256 client
         * key arrives that way; read it as a BN and left-pad into the fixed
         * 32-byte scalar. Keep an octet-string branch for any caller that
         * supplies one that way (e.g. our own re-imports). */
        if (p->data_type == OSSL_PARAM_UNSIGNED_INTEGER) {
          BIGNUM *bn = NULL;
          int ok;
          if (!OSSL_PARAM_get_BN(p, &bn)) {
            return 0;
          }
          ok = BN_num_bytes(bn) <= ZZ9K_P256_SCALAR_LEN &&
               BN_bn2binpad(bn, key->priv, ZZ9K_P256_SCALAR_LEN) ==
                   ZZ9K_P256_SCALAR_LEN;
          BN_clear_free(bn);
          if (!ok) {
            return 0;
          }
        } else {
          void *out = key->priv;
          size_t len = 0;
          if (!OSSL_PARAM_get_octet_string(p, &out, ZZ9K_P256_SCALAR_LEN,
                                           &len) ||
              len != ZZ9K_P256_SCALAR_LEN) {
            return 0;
          }
        }
        key->has_priv = 1;
        /* Derive the public point if it was not supplied alongside, so the
         * object is a self-consistent keypair (mirrors the X25519 import). */
        if (!key->has_pub &&
            zz9k_prov_p256_keygen(key->point, key->priv, key->provctx)) {
          key->has_pub = 1;
        }
      }
    }
    /* The group name alone (no PUB_KEY/PRIV_KEY, e.g. a domain-parameters
     * import) is still a valid, if trivial, import: we hardcode P-256. */
    return 1;
  }

  /* Any other curve: delegate. Build a shadow EVP_PKEY against the default
   * provider from the SAME import material, so ECDSA verify (the only
   * operation a non-P256 key ever reaches — see the file header) can be
   * forwarded to it. */
  {
    ZZ9K_PROV_CTX *pc = key->provctx;
    EVP_PKEY_CTX *kctx;
    EVP_PKEY *shadow = NULL;

    key->is_p256 = 0;
    key->has_pub = 0;
    key->has_priv = 0;
    EVP_PKEY_free(key->shadow);
    key->shadow = NULL;

    if (pc == NULL) {
      return 0;
    }
    kctx = EVP_PKEY_CTX_new_from_name((OSSL_LIB_CTX *)pc->libctx, "EC",
                                      "provider=default");
    if (kctx == NULL) {
      return 0;
    }
    if (EVP_PKEY_fromdata_init(kctx) <= 0 ||
        /* OSSL_PARAM params[] is non-const in the public API even though
         * fromdata only reads it; params[] itself is caller-owned. */
        EVP_PKEY_fromdata(kctx, &shadow, selection,
                          (OSSL_PARAM *)params) <= 0) {
      EVP_PKEY_CTX_free(kctx);
      return 0;
    }
    EVP_PKEY_CTX_free(kctx);
    key->shadow = shadow;
    key->has_pub = (selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) != 0;
    key->has_priv = (selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) != 0;
    return 1;
  }
}

static const OSSL_PARAM *zz9k_ec_keymgmt_import_types(int selection)
{
  static const OSSL_PARAM types[] = {
    OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, NULL, 0),
    OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PUB_KEY, NULL, 0),
    OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PRIV_KEY, NULL, 0),
    OSSL_PARAM_END
  };
  (void)selection;
  return types;
}

static int zz9k_ec_keymgmt_get_params(void *keydata, OSSL_PARAM params[])
{
  ZZ9K_EC_KEY *key = keydata;
  OSSL_PARAM *p;
  int bits = 256;
  int secbits = 128;
  int maxsize = 72;   /* max DER ECDSA-Sig-Value size for P-256 */

  if (key == NULL) {
    return 0;
  }
  if (!key->is_p256) {
    /* Forward the numeric params to the shadow so introspection (e.g. log
     * messages, X509 printing) reports the real curve's size. */
    if (key->shadow == NULL) {
      return 0;
    }
    bits = EVP_PKEY_get_bits(key->shadow);
    secbits = EVP_PKEY_get_security_bits(key->shadow);
    maxsize = EVP_PKEY_get_size(key->shadow);
  }
  p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_BITS);
  if (p != NULL && !OSSL_PARAM_set_int(p, bits)) {
    return 0;
  }
  p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_SECURITY_BITS);
  if (p != NULL && !OSSL_PARAM_set_int(p, secbits)) {
    return 0;
  }
  p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_MAX_SIZE);
  if (p != NULL && !OSSL_PARAM_set_int(p, maxsize)) {
    return 0;
  }
  if (key->is_p256) {
    p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY);
    if (p != NULL) {
      if (!key->has_pub ||
          !OSSL_PARAM_set_octet_string(p, key->point, ZZ9K_P256_POINT_LEN)) {
        return 0;
      }
    }
    p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_PUB_KEY);
    if (p != NULL) {
      if (!key->has_pub ||
          !OSSL_PARAM_set_octet_string(p, key->point, ZZ9K_P256_POINT_LEN)) {
        return 0;
      }
    }
    p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_PRIV_KEY);
    if (p != NULL) {
      if (!key->has_priv ||
          !OSSL_PARAM_set_octet_string(p, key->priv, ZZ9K_P256_SCALAR_LEN)) {
        return 0;
      }
    }
  }
  return 1;
}

static const OSSL_PARAM *zz9k_ec_keymgmt_gettable_params(void *provctx)
{
  static const OSSL_PARAM types[] = {
    OSSL_PARAM_int(OSSL_PKEY_PARAM_BITS, NULL),
    OSSL_PARAM_int(OSSL_PKEY_PARAM_SECURITY_BITS, NULL),
    OSSL_PARAM_int(OSSL_PKEY_PARAM_MAX_SIZE, NULL),
    OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, NULL, 0),
    OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PUB_KEY, NULL, 0),
    OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PRIV_KEY, NULL, 0),
    OSSL_PARAM_END
  };
  (void)provctx;
  return types;
}

/* libssl installs the peer key share with EVP_PKEY_set1_encoded_public_key,
 * exactly like X25519. Only P-256 peer shares ever reach this: the provider
 * declares just the secp256r1 TLS group (zz9k_provider.c). */
static int zz9k_ec_keymgmt_set_params(void *keydata, const OSSL_PARAM params[])
{
  ZZ9K_EC_KEY *key = keydata;
  const OSSL_PARAM *p;

  if (key == NULL) {
    return 0;
  }
  p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY);
  if (p != NULL) {
    if (p->data_type != OSSL_PARAM_OCTET_STRING ||
        p->data_size != ZZ9K_P256_POINT_LEN ||
        ((const unsigned char *)p->data)[0] != 0x04) {
      return 0;
    }
    memcpy(key->point, p->data, ZZ9K_P256_POINT_LEN);
    key->has_pub = 1;
    key->is_p256 = 1;
    EVP_PKEY_free(key->shadow);
    key->shadow = NULL;
  }
  return 1;
}

static const OSSL_PARAM *zz9k_ec_keymgmt_settable_params(void *provctx)
{
  static const OSSL_PARAM types[] = {
    OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, NULL, 0),
    OSSL_PARAM_END
  };
  (void)provctx;
  return types;
}

static int zz9k_ec_keymgmt_match(const void *keydata1, const void *keydata2,
                                 int selection)
{
  const ZZ9K_EC_KEY *a = keydata1;
  const ZZ9K_EC_KEY *b = keydata2;

  if (a == NULL || b == NULL || a->is_p256 != b->is_p256) {
    return 0;
  }
  if (!a->is_p256) {
    if (a->shadow == NULL || b->shadow == NULL) {
      return 0;
    }
    return EVP_PKEY_eq(a->shadow, b->shadow) == 1;
  }
  if ((selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) != 0) {
    if (!a->has_pub || !b->has_pub ||
        memcmp(a->point, b->point, ZZ9K_P256_POINT_LEN) != 0) {
      return 0;
    }
  }
  if ((selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) != 0) {
    if (!a->has_priv || !b->has_priv ||
        memcmp(a->priv, b->priv, ZZ9K_P256_SCALAR_LEN) != 0) {
      return 0;
    }
  }
  return 1;
}

static void *zz9k_ec_keymgmt_dup(const void *keydata_from, int selection)
{
  const ZZ9K_EC_KEY *from = keydata_from;
  ZZ9K_EC_KEY *to;

  if (from == NULL) {
    return NULL;
  }
  to = OPENSSL_zalloc(sizeof(*to));
  if (to == NULL) {
    return NULL;
  }
  to->provctx = from->provctx;
  to->is_p256 = from->is_p256;
  if (!from->is_p256) {
    if (from->shadow != NULL) {
      to->shadow = EVP_PKEY_dup(from->shadow);
      if (to->shadow == NULL) {
        OPENSSL_free(to);
        return NULL;
      }
    }
    to->has_pub = from->has_pub;
    to->has_priv = from->has_priv;
    return to;
  }
  if ((selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) != 0 && from->has_pub) {
    memcpy(to->point, from->point, ZZ9K_P256_POINT_LEN);
    to->has_pub = 1;
  }
  if ((selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) != 0 && from->has_priv) {
    memcpy(to->priv, from->priv, ZZ9K_P256_SCALAR_LEN);
    to->has_priv = 1;
  }
  return to;
}

static int zz9k_ec_keymgmt_export(void *keydata, int selection,
                                  OSSL_CALLBACK *param_cb, void *cbarg)
{
  ZZ9K_EC_KEY *key = keydata;
  OSSL_PARAM params[4];
  int n = 0;

  if (key == NULL) {
    return 0;
  }
  if (!key->is_p256) {
    /* Delegated keys export through the shadow so a caller sees the same
     * result whichever provider ends up doing the exporting. */
    if (key->shadow == NULL) {
      return 0;
    }
    return EVP_PKEY_export(key->shadow, selection, param_cb, cbarg);
  }
  ZZ9K_PARAM_UTF8(&params[n], OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1");
  n++;
  if ((selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) != 0 && key->has_pub) {
    ZZ9K_PARAM_OCTET(&params[n], OSSL_PKEY_PARAM_PUB_KEY, key->point,
                     ZZ9K_P256_POINT_LEN);
    n++;
  }
  if ((selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) != 0 && key->has_priv) {
    ZZ9K_PARAM_OCTET(&params[n], OSSL_PKEY_PARAM_PRIV_KEY, key->priv,
                     ZZ9K_P256_SCALAR_LEN);
    n++;
  }
  ZZ9K_PARAM_END(&params[n]);
  return param_cb(params, cbarg);
}

static const OSSL_PARAM *zz9k_ec_keymgmt_export_types(int selection)
{
  return zz9k_ec_keymgmt_import_types(selection);
}

/* ---- key generation (the TLS key share; secp256r1 only) ---- */

typedef struct {
  ZZ9K_PROV_CTX *provctx;
  int selection;
  char group[64];      /* recorded curve name; "" => none set (=> P-256) */
  OSSL_PARAM *params;  /* dup of the gen params, replayed for delegated gen */
} ZZ9K_EC_GEN;

static void *zz9k_ec_gen_init(void *provctx, int selection,
                              const OSSL_PARAM params[])
{
  ZZ9K_EC_GEN *gen;

  (void)params;
  /* Accept both keygen (KEYPAIR bits set) and paramgen (only the
   * parameters bits set — no key material). TLS1.2's legacy
   * tls_process_ske_ecdhe builds the peer-key template via paramgen
   * (selection == OSSL_KEYMGMT_SELECT_ALL_PARAMETERS) before overwriting the
   * public point with set_params(ENCODED_PUBLIC_KEY); TLS1.3's newer
   * tls_process_key_share does not need this. Reject a selection that
   * requests neither. */
  if ((selection &
       (OSSL_KEYMGMT_SELECT_KEYPAIR | OSSL_KEYMGMT_SELECT_ALL_PARAMETERS)) ==
      0) {
    return NULL;
  }
  gen = OPENSSL_zalloc(sizeof(*gen));
  if (gen != NULL) {
    gen->provctx = provctx;
    gen->selection = selection;
  }
  return gen;
}

/* libssl calls EVP_PKEY_CTX_set_group_name(ctx, "P-256") (or "prime256v1")
 * on the keygen context, mirroring the X25519 group-name check. P-256 keygen
 * is accelerated; any other named curve is recorded and delegated to the
 * default provider at gen() time (owning the "EC" keytype means app keygen of
 * other curves routes here and must not regress versus v2.2.0). The full param
 * set is kept verbatim so the delegated keygen ctx gets whatever the caller
 * set, not just the group name. */
static int zz9k_ec_gen_set_params(void *genctx, const OSSL_PARAM params[])
{
  ZZ9K_EC_GEN *gen = genctx;
  const OSSL_PARAM *p;

  if (gen == NULL) {
    return 0;
  }
  if (params == NULL) {
    return 1;
  }
  p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_GROUP_NAME);
  if (p != NULL) {
    char *gp = gen->group;
    if (p->data_type != OSSL_PARAM_UTF8_STRING || p->data == NULL ||
        !OSSL_PARAM_get_utf8_string(p, &gp, sizeof(gen->group))) {
      return 0;
    }
  }
  OSSL_PARAM_free(gen->params);
  gen->params = OSSL_PARAM_dup(params);
  return 1;
}

static const OSSL_PARAM *zz9k_ec_gen_settable_params(void *genctx,
                                                      void *provctx)
{
  static const OSSL_PARAM types[] = {
    OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, NULL, 0),
    OSSL_PARAM_END
  };
  (void)genctx;
  (void)provctx;
  return types;
}


/* Non-P256 EC keygen: no ZZ9000 acceleration exists for it, so generate
 * through the default provider (replaying the caller's gen params, which
 * carry the curve) and wrap the result as a delegated key whose shadow is the
 * fresh EVP_PKEY. Every later op on a delegated key (sign, export, get_params,
 * match, dup) already forwards to the shadow. */
static void *zz9k_ec_gen_delegated(ZZ9K_EC_GEN *gen)
{
  OSSL_LIB_CTX *libctx =
      (OSSL_LIB_CTX *)(gen->provctx != NULL ? gen->provctx->libctx : NULL);
  EVP_PKEY_CTX *dctx;
  EVP_PKEY *pk = NULL;
  ZZ9K_EC_KEY *key;

  if (gen->params == NULL) {
    return NULL;   /* a non-P256 curve was named, so params must carry it */
  }
  dctx = EVP_PKEY_CTX_new_from_name(libctx, "EC", "provider=default");
  if (dctx == NULL) {
    return NULL;
  }
  {
    /* Replay any caller params (e.g. point format), then set the curve
     * authoritatively from the recorded name (gen->group) LAST, so it is the
     * final word — rather than trusting gen->params to still carry GROUP_NAME.
     * This keeps a caller that sets params across multiple calls, dropping
     * GROUP_NAME from a later one, from silently losing the curve. */
    OSSL_PARAM gp[2];
    ZZ9K_PARAM_UTF8(&gp[0], OSSL_PKEY_PARAM_GROUP_NAME, gen->group);
    ZZ9K_PARAM_END(&gp[1]);
    if (EVP_PKEY_keygen_init(dctx) <= 0 ||
        EVP_PKEY_CTX_set_params(dctx, gen->params) <= 0 ||
        EVP_PKEY_CTX_set_params(dctx, gp) <= 0 ||
        EVP_PKEY_generate(dctx, &pk) <= 0) {
      EVP_PKEY_CTX_free(dctx);
      EVP_PKEY_free(pk);
      return NULL;
    }
  }
  EVP_PKEY_CTX_free(dctx);

  key = OPENSSL_zalloc(sizeof(*key));
  if (key == NULL) {
    EVP_PKEY_free(pk);
    return NULL;
  }
  key->provctx = gen->provctx;
  key->is_p256 = 0;
  key->shadow = pk;
  key->has_pub = (gen->selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) != 0;
  key->has_priv = (gen->selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) != 0;
  return key;
}

static void *zz9k_ec_gen(void *genctx, OSSL_CALLBACK *cb, void *cbarg)
{
  ZZ9K_EC_GEN *gen = genctx;
  ZZ9K_EC_KEY *key;

  (void)cb;
  (void)cbarg;
  if (gen == NULL) {
    return NULL;
  }
  /* An empty group means none was set — default to the accelerated P-256 path
   * (preserves the pre-delegation behaviour for the TLS key-share keygen). */
  if (gen->group[0] != '\0' &&
      OPENSSL_strcasecmp(gen->group, "P-256") != 0 &&
      OPENSSL_strcasecmp(gen->group, "prime256v1") != 0) {
    return zz9k_ec_gen_delegated(gen);
  }
  key = OPENSSL_zalloc(sizeof(*key));
  if (key == NULL) {
    return NULL;
  }
  key->provctx = gen->provctx;
  key->is_p256 = 1;
  if ((gen->selection & OSSL_KEYMGMT_SELECT_KEYPAIR) == 0) {
    /* Paramgen: just the P-256 group, no key material — the caller (e.g.
     * TLS1.2's peer-key template) fills in the public point separately via
     * set_params(ENCODED_PUBLIC_KEY). */
    return key;
  }
  /* Try the board once for scalar*G. On success the key is fully accelerated;
   * on failure — capability absent, or a mailbox timeout because the ZZ9000 is
   * busy driving the display — fall back to a default-provider keypair instead
   * of retry-spinning the mailbox, which would stall the handshake far worse
   * than stock amissl. A vanishingly rare bad random scalar is handled by the
   * same fallback, so no offload retry loop is needed. */
  if (RAND_priv_bytes(key->priv, ZZ9K_P256_SCALAR_LEN) > 0 &&
      zz9k_prov_p256_keygen(key->point, key->priv, key->provctx)) {
    key->has_priv = 1;
    key->has_pub = 1;
    return key;
  }
#if defined(ZZ9K_PROVIDER_OFFLOAD) || defined(ZZ9K_TEST_DEFAULT_FALLBACK)
  if (zz9k_p256_default_genkey(key->priv, key->point, key->provctx)) {
    key->has_priv = 1;
    key->has_pub = 1;
    return key;
  }
#endif
  OPENSSL_clear_free(key, sizeof(*key));
  return NULL;
}

static void zz9k_ec_gen_cleanup(void *genctx)
{
  ZZ9K_EC_GEN *gen = genctx;
  if (gen != NULL) {
    OSSL_PARAM_free(gen->params);
  }
  OPENSSL_free(genctx);
}

static const char *zz9k_ec_keymgmt_query_operation_name(int operation_id)
{
  if (operation_id == OSSL_OP_KEYEXCH) {
    return "ECDH";
  }
  if (operation_id == OSSL_OP_SIGNATURE) {
    return "ECDSA";
  }
  return NULL;
}

const OSSL_DISPATCH zz9k_ec_keymgmt_functions[] = {
  { OSSL_FUNC_KEYMGMT_NEW, (void (*)(void))zz9k_ec_keymgmt_new },
  { OSSL_FUNC_KEYMGMT_FREE, (void (*)(void))zz9k_ec_keymgmt_free },
  { OSSL_FUNC_KEYMGMT_HAS, (void (*)(void))zz9k_ec_keymgmt_has },
  { OSSL_FUNC_KEYMGMT_IMPORT, (void (*)(void))zz9k_ec_keymgmt_import },
  { OSSL_FUNC_KEYMGMT_IMPORT_TYPES,
    (void (*)(void))zz9k_ec_keymgmt_import_types },
  { OSSL_FUNC_KEYMGMT_EXPORT, (void (*)(void))zz9k_ec_keymgmt_export },
  { OSSL_FUNC_KEYMGMT_EXPORT_TYPES,
    (void (*)(void))zz9k_ec_keymgmt_export_types },
  { OSSL_FUNC_KEYMGMT_GET_PARAMS,
    (void (*)(void))zz9k_ec_keymgmt_get_params },
  { OSSL_FUNC_KEYMGMT_GETTABLE_PARAMS,
    (void (*)(void))zz9k_ec_keymgmt_gettable_params },
  { OSSL_FUNC_KEYMGMT_SET_PARAMS,
    (void (*)(void))zz9k_ec_keymgmt_set_params },
  { OSSL_FUNC_KEYMGMT_SETTABLE_PARAMS,
    (void (*)(void))zz9k_ec_keymgmt_settable_params },
  { OSSL_FUNC_KEYMGMT_MATCH, (void (*)(void))zz9k_ec_keymgmt_match },
  { OSSL_FUNC_KEYMGMT_DUP, (void (*)(void))zz9k_ec_keymgmt_dup },
  { OSSL_FUNC_KEYMGMT_GEN_INIT, (void (*)(void))zz9k_ec_gen_init },
  { OSSL_FUNC_KEYMGMT_GEN_SET_PARAMS,
    (void (*)(void))zz9k_ec_gen_set_params },
  { OSSL_FUNC_KEYMGMT_GEN_SETTABLE_PARAMS,
    (void (*)(void))zz9k_ec_gen_settable_params },
  { OSSL_FUNC_KEYMGMT_GEN, (void (*)(void))zz9k_ec_gen },
  { OSSL_FUNC_KEYMGMT_GEN_CLEANUP, (void (*)(void))zz9k_ec_gen_cleanup },
  { OSSL_FUNC_KEYMGMT_QUERY_OPERATION_NAME,
    (void (*)(void))zz9k_ec_keymgmt_query_operation_name },
  { 0, NULL }
};

/* ---- ECDH KEYEXCH ---- */

typedef struct {
  ZZ9K_PROV_CTX *provctx;
  ZZ9K_EC_KEY *key;    /* our key (provides the private scalar) */
  ZZ9K_EC_KEY *peer;   /* peer key (provides the public point) */
} ZZ9K_ECDH_EXCH;

static void *zz9k_ecdh_exch_newctx(void *provctx)
{
  ZZ9K_ECDH_EXCH *ctx = OPENSSL_zalloc(sizeof(*ctx));
  if (ctx != NULL) {
    ctx->provctx = provctx;
  }
  return ctx;
}

static int zz9k_ecdh_exch_init(void *vctx, void *provkey,
                               const OSSL_PARAM params[])
{
  ZZ9K_ECDH_EXCH *ctx = vctx;

  (void)params;
  if (ctx == NULL || provkey == NULL) {
    return 0;
  }
  ctx->key = provkey;
  return 1;
}

static int zz9k_ecdh_exch_set_peer(void *vctx, void *provkey)
{
  ZZ9K_ECDH_EXCH *ctx = vctx;

  if (ctx == NULL || provkey == NULL) {
    return 0;
  }
  ctx->peer = provkey;
  return 1;
}

static int zz9k_ecdh_exch_derive(void *vctx, unsigned char *secret,
                                 size_t *secretlen, size_t outlen)
{
  ZZ9K_ECDH_EXCH *ctx = vctx;

  if (ctx == NULL || ctx->key == NULL || ctx->peer == NULL) {
    return 0;
  }
  /* Only P-256 x P-256 ever reaches here: the provider declares just the
   * secp256r1 TLS group, so libssl never fetches this KEYEXCH for a
   * delegated (non-P256) key. */
  if (!ctx->key->is_p256 || !ctx->peer->is_p256 ||
      !ctx->key->has_priv || !ctx->peer->has_pub) {
    return 0;
  }
  if (secret == NULL) {
    *secretlen = ZZ9K_P256_SCALAR_LEN;
    return 1;
  }
  if (outlen < ZZ9K_P256_SCALAR_LEN) {
    return 0;
  }
  if (!zz9k_prov_p256_derive(secret, ctx->key->priv, ctx->peer->point,
                             ctx->provctx)) {
    return 0;
  }
  *secretlen = ZZ9K_P256_SCALAR_LEN;
  return 1;
}

static void zz9k_ecdh_exch_freectx(void *vctx)
{
  OPENSSL_free(vctx);
}

const OSSL_DISPATCH zz9k_ecdh_keyexch_functions[] = {
  { OSSL_FUNC_KEYEXCH_NEWCTX, (void (*)(void))zz9k_ecdh_exch_newctx },
  { OSSL_FUNC_KEYEXCH_INIT, (void (*)(void))zz9k_ecdh_exch_init },
  { OSSL_FUNC_KEYEXCH_SET_PEER, (void (*)(void))zz9k_ecdh_exch_set_peer },
  { OSSL_FUNC_KEYEXCH_DERIVE, (void (*)(void))zz9k_ecdh_exch_derive },
  { OSSL_FUNC_KEYEXCH_FREECTX, (void (*)(void))zz9k_ecdh_exch_freectx },
  { 0, NULL }
};

/* ---- ECDSA SIGNATURE (verify) ---- */

typedef struct {
  ZZ9K_PROV_CTX *provctx;
  ZZ9K_EC_KEY *key;
  EVP_MD_CTX *mdctx;   /* only used by the DigestVerify{Init,Update,Final} trio */
} ZZ9K_ECDSA_CTX;

static void *zz9k_ecdsa_newctx(void *provctx, const char *propq)
{
  ZZ9K_ECDSA_CTX *ctx = OPENSSL_zalloc(sizeof(*ctx));
  (void)propq;
  if (ctx != NULL) {
    ctx->provctx = provctx;
  }
  return ctx;
}

static void zz9k_ecdsa_freectx(void *vctx)
{
  ZZ9K_ECDSA_CTX *ctx = vctx;
  if (ctx != NULL) {
    EVP_MD_CTX_free(ctx->mdctx);
  }
  OPENSSL_free(vctx);
}

static int zz9k_ecdsa_verify_init(void *vctx, void *provkey,
                                  const OSSL_PARAM params[])
{
  ZZ9K_ECDSA_CTX *ctx = vctx;

  (void)params;
  if (ctx == NULL || provkey == NULL) {
    return 0;
  }
  ctx->key = provkey;
  return 1;
}

/* Delegated (non-P256) verify: forward to the shadow EVP_PKEY built at
 * import time against the default provider. Fails closed (returns 0) on any
 * error, a missing shadow, or an invalid signature — never returns 1 except
 * on a confirmed-valid EVP_PKEY_verify result. */
static int zz9k_ecdsa_verify_delegated(ZZ9K_ECDSA_CTX *ctx,
                                       const unsigned char *sig, size_t siglen,
                                       const unsigned char *tbs, size_t tbslen)
{
  EVP_PKEY_CTX *pctx;
  int rc;

  if (ctx->key->shadow == NULL) {
    return 0;
  }
  /* MUST force "provider=default" here (not NULL/inherited). The client
   * libctx's default property query is "?provider=zz9000" (preferred, not
   * required, per amissl's setup) — with a NULL/inherited propquery, this
   * fetch would still prefer OUR "ECDSA" op even though the shadow's keydata
   * belongs to the default provider. EVP then auto-bridges the mismatched
   * key (export via our KEYMGMT, re-import via ours again, since it's the
   * preferred match), landing right back in this same delegated path with a
   * freshly rebuilt shadow — infinite recursion. Requiring "provider=default"
   * pins both the op AND the key to the same, correct provider. */
  pctx = EVP_PKEY_CTX_new_from_pkey(
      (OSSL_LIB_CTX *)(ctx->provctx != NULL ? ctx->provctx->libctx : NULL),
      ctx->key->shadow, "provider=default");
  if (pctx == NULL) {
    return 0;
  }
  rc = EVP_PKEY_verify_init(pctx);
  if (rc > 0) {
    rc = EVP_PKEY_verify(pctx, sig, siglen, tbs, tbslen);
  }
  EVP_PKEY_CTX_free(pctx);
  return rc == 1 ? 1 : 0;
}

/* Accelerated (P-256) key whose digest the hardware/software reference cannot
 * take: only a 32-byte (SHA-256) digest is accelerated, but X.509 permits a
 * P-256 issuer key to sign with SHA-384/512 (rare, but the default provider
 * handles it, so we must not regress). Build a one-shot default-provider P-256
 * key from our point and verify through it — fail-closed. */
static int zz9k_ecdsa_verify_p256_via_default(ZZ9K_ECDSA_CTX *ctx,
                                              const unsigned char *sig,
                                              size_t siglen,
                                              const unsigned char *tbs,
                                              size_t tbslen)
{
  OSSL_LIB_CTX *libctx =
      (OSSL_LIB_CTX *)(ctx->provctx != NULL ? ctx->provctx->libctx : NULL);
  EVP_PKEY_CTX *kctx;
  EVP_PKEY_CTX *pctx;
  EVP_PKEY *pkey = NULL;
  OSSL_PARAM params[3];
  int n = 0;
  int rc = 0;

  ZZ9K_PARAM_UTF8(&params[n], OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1");
  n++;
  ZZ9K_PARAM_OCTET(&params[n], OSSL_PKEY_PARAM_PUB_KEY, ctx->key->point,
                   ZZ9K_P256_POINT_LEN);
  n++;
  ZZ9K_PARAM_END(&params[n]);

  kctx = EVP_PKEY_CTX_new_from_name(libctx, "EC", "provider=default");
  if (kctx == NULL) {
    return 0;
  }
  if (EVP_PKEY_fromdata_init(kctx) <= 0 ||
      EVP_PKEY_fromdata(kctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
    EVP_PKEY_CTX_free(kctx);
    return 0;
  }
  EVP_PKEY_CTX_free(kctx);

  pctx = EVP_PKEY_CTX_new_from_pkey(libctx, pkey, "provider=default");
  if (pctx != NULL && EVP_PKEY_verify_init(pctx) > 0) {
    rc = EVP_PKEY_verify(pctx, sig, siglen, tbs, tbslen) == 1 ? 1 : 0;
  }
  EVP_PKEY_CTX_free(pctx);
  EVP_PKEY_free(pkey);
  return rc;
}

static int zz9k_ecdsa_verify(void *vctx, const unsigned char *sig,
                             size_t siglen, const unsigned char *tbs,
                             size_t tbslen)
{
  ZZ9K_ECDSA_CTX *ctx = vctx;
  unsigned char r[32];
  unsigned char s[32];

  if (ctx == NULL || ctx->key == NULL) {
    return 0;
  }
  if (!ctx->key->is_p256) {
    return zz9k_ecdsa_verify_delegated(ctx, sig, siglen, tbs, tbslen);
  }
  if (!ctx->key->has_pub) {
    return 0;
  }
  if (tbslen != 32) {
    /* P-256 key, non-SHA-256 digest: not accelerable — delegate to default
     * rather than rejecting a valid signature. */
    return zz9k_ecdsa_verify_p256_via_default(ctx, sig, siglen, tbs, tbslen);
  }
  if (!zz9k_decode_ecdsa_sig(sig, siglen, r, s)) {
    return 0;
  }
  {
    int v = zz9k_prov_ecdsa_verify(r, s, tbs, ctx->key->point, ctx->provctx);
    if (v >= 0) {
      return v;
    }
  }
  /* Board could not verify (capability absent, or offload timed out under
   * display contention): fall back to the default provider rather than the
   * ~19x-slower reference — the same offload-or-fallback posture the P-256 key
   * exchange uses. */
  return zz9k_ecdsa_verify_p256_via_default(ctx, sig, siglen, tbs, tbslen);
}

static int zz9k_ecdsa_set_ctx_params(void *vctx, const OSSL_PARAM params[])
{
  /* Digest name/padding params (set e.g. via EVP_PKEY_CTX_set_signature_md,
   * or implicitly by DigestVerifyInit below) are accepted and ignored: the
   * accelerated path is pinned to SHA-256 (tbslen checked at verify time)
   * and the delegated path forwards whatever digest was actually computed
   * to the shadow, whatever its length. */
  (void)vctx;
  (void)params;
  return 1;
}

static const OSSL_PARAM *zz9k_ecdsa_settable_ctx_params(void *vctx,
                                                        void *provctx)
{
  static const OSSL_PARAM types[] = { OSSL_PARAM_END };
  (void)vctx;
  (void)provctx;
  return types;
}

/* ---- DigestVerify (EVP_DigestVerify* / X509_verify / TLS CertificateVerify) ----
 *
 * A provider signature implementation that only offers plain VERIFY (raw,
 * pre-hashed input) does not get digest hashing for free: EVP_DigestVerifyInit
 * requires DIGEST_VERIFY_INIT to be present, or initialisation fails outright
 * (there is no generic "hash externally, then call plain verify" fallback for
 * provider-native signatures). X509_verify and libssl's TLS1.3
 * CertificateVerify both go through EVP_DigestVerify*, so this trio is
 * required for both the accelerated and delegated verify paths — it hashes
 * with a plain EVP_MD_CTX (independent of any provider) and hands the
 * resulting digest to the same zz9k_ecdsa_verify() used by the raw path. */
/* libssl hands the DigestVerify/Sign init entry points the OpenSSL-3 canonical
 * digest name (e.g. "SHA2-256"). EVP_get_digestbyname's legacy name table is
 * incomplete for those aliases on some OpenSSL builds — notably 3.0.13, where
 * "SHA2-256" resolves to NULL, so every SHA-256 TLS signature verify fails to
 * initialise and the handshake aborts. Fetch through the library context
 * instead: it resolves both canonical and legacy names on every OpenSSL 3.x.
 * Returns a fetched EVP_MD the caller must EVP_MD_free() (EVP_DigestInit_ex
 * takes its own reference, so freeing immediately after init is safe); a
 * NULL/empty name defaults to SHA-256. */
static EVP_MD *zz9k_ecdsa_fetch_md(ZZ9K_ECDSA_CTX *ctx, const char *mdname)
{
  OSSL_LIB_CTX *libctx =
      (OSSL_LIB_CTX *)(ctx->provctx != NULL ? ctx->provctx->libctx : NULL);
  return EVP_MD_fetch(
      libctx, (mdname != NULL && mdname[0] != '\0') ? mdname : "SHA2-256", NULL);
}

static int zz9k_ecdsa_digest_verify_init(void *vctx, const char *mdname,
                                         void *provkey,
                                         const OSSL_PARAM params[])
{
  ZZ9K_ECDSA_CTX *ctx = vctx;
  EVP_MD *md;

  if (ctx == NULL || provkey == NULL) {
    return 0;
  }
  ctx->key = provkey;
  if (ctx->mdctx == NULL) {
    ctx->mdctx = EVP_MD_CTX_new();
    if (ctx->mdctx == NULL) {
      return 0;
    }
  }
  md = zz9k_ecdsa_fetch_md(ctx, mdname);
  if (md == NULL || !EVP_DigestInit_ex(ctx->mdctx, md, NULL)) {
    EVP_MD_free(md);
    return 0;
  }
  EVP_MD_free(md);
  return zz9k_ecdsa_set_ctx_params(vctx, params);
}

static int zz9k_ecdsa_digest_verify_update(void *vctx,
                                           const unsigned char *data,
                                           size_t datalen)
{
  ZZ9K_ECDSA_CTX *ctx = vctx;

  if (ctx == NULL || ctx->mdctx == NULL) {
    return 0;
  }
  return EVP_DigestUpdate(ctx->mdctx, data, datalen);
}

static int zz9k_ecdsa_digest_verify_final(void *vctx, const unsigned char *sig,
                                          size_t siglen)
{
  ZZ9K_ECDSA_CTX *ctx = vctx;
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digestlen = 0;

  if (ctx == NULL || ctx->mdctx == NULL) {
    return 0;
  }
  if (!EVP_DigestFinal_ex(ctx->mdctx, digest, &digestlen)) {
    return 0;
  }
  return zz9k_ecdsa_verify(vctx, sig, siglen, digest, digestlen);
}

/* One-shot EVP_DigestVerify(): libssl's TLS1.2/1.3 signature checks (and
 * X509_verify) call this single entry point rather than manual
 * Init+Update+Final, and EVP_DigestVerify() calls it directly in place of
 * the Update+Final pair above when a provider offers it (both are kept —
 * some other caller may still stream via manual Init+Update+Final). Reuses
 * the same ctx->mdctx set up by digest_verify_init. */
static int zz9k_ecdsa_digest_verify_oneshot(void *vctx, const unsigned char *sig,
                                            size_t siglen,
                                            const unsigned char *tbs,
                                            size_t tbslen)
{
  ZZ9K_ECDSA_CTX *ctx = vctx;
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digestlen = 0;

  if (ctx == NULL || ctx->mdctx == NULL) {
    return 0;
  }
  if (!EVP_DigestUpdate(ctx->mdctx, tbs, tbslen) ||
      !EVP_DigestFinal_ex(ctx->mdctx, digest, &digestlen)) {
    return 0;
  }
  return zz9k_ecdsa_verify(vctx, sig, siglen, digest, digestlen);
}

/* ---- ECDSA sign (always delegated) ----
 *
 * The ZZ9000 never accelerates ECDSA *signing* (no firmware primitive), but
 * owning the "EC" keytype means a client-certificate private key routes its
 * CertificateVerify signature here. So sign delegates to the default provider,
 * fed the private key: a delegated (non-P256) key already carries a shadow
 * EVP_PKEY with its private material; an accelerated P-256 key carries only
 * the raw 32-byte scalar (+ point), so a one-shot default keypair is built
 * from it on demand. As with the verify path, "provider=default" is forced on
 * the delegated ctx to keep the fetch from bridging back into this provider. */
static EVP_PKEY *zz9k_ec_p256_signing_pkey(ZZ9K_ECDSA_CTX *ctx)
{
  OSSL_LIB_CTX *libctx =
      (OSSL_LIB_CTX *)(ctx->provctx != NULL ? ctx->provctx->libctx : NULL);
  EVP_PKEY_CTX *kctx = NULL;
  EVP_PKEY *pkey = NULL;
  OSSL_PARAM_BLD *bld = NULL;
  OSSL_PARAM *params = NULL;
  BIGNUM *priv = NULL;

  if (!ctx->key->has_priv) {
    return NULL;
  }
  priv = BN_bin2bn(ctx->key->priv, ZZ9K_P256_SCALAR_LEN, NULL);
  bld = OSSL_PARAM_BLD_new();
  if (priv == NULL || bld == NULL) {
    goto done;
  }
  if (!OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME,
                                       "prime256v1", 0) ||
      !OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, priv)) {
    goto done;
  }
  if (ctx->key->has_pub &&
      !OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY,
                                        ctx->key->point,
                                        ZZ9K_P256_POINT_LEN)) {
    goto done;
  }
  params = OSSL_PARAM_BLD_to_param(bld);
  if (params == NULL) {
    goto done;
  }
  kctx = EVP_PKEY_CTX_new_from_name(libctx, "EC", "provider=default");
  if (kctx == NULL || EVP_PKEY_fromdata_init(kctx) <= 0 ||
      EVP_PKEY_fromdata(kctx, &pkey, EVP_PKEY_KEYPAIR, params) <= 0) {
    EVP_PKEY_free(pkey);
    pkey = NULL;
  }
done:
  OSSL_PARAM_free(params);
  OSSL_PARAM_BLD_free(bld);
  BN_clear_free(priv);
  EVP_PKEY_CTX_free(kctx);
  return pkey;
}

static int zz9k_ecdsa_sign_delegated(ZZ9K_ECDSA_CTX *ctx, unsigned char *sig,
                                     size_t *siglen, size_t sigsize,
                                     const unsigned char *tbs, size_t tbslen)
{
  OSSL_LIB_CTX *libctx =
      (OSSL_LIB_CTX *)(ctx->provctx != NULL ? ctx->provctx->libctx : NULL);
  EVP_PKEY *owned = NULL;
  EVP_PKEY *pkey;
  EVP_PKEY_CTX *pctx = NULL;
  int rc = 0;

  (void)sigsize;
  if (ctx->key == NULL) {
    return 0;
  }
  if (ctx->key->is_p256) {
    owned = zz9k_ec_p256_signing_pkey(ctx);
    pkey = owned;
  } else {
    pkey = ctx->key->shadow;   /* built at import; holds the private key */
  }
  if (pkey == NULL) {
    return 0;
  }
  pctx = EVP_PKEY_CTX_new_from_pkey(libctx, pkey, "provider=default");
  if (pctx != NULL && EVP_PKEY_sign_init(pctx) > 0) {
    rc = EVP_PKEY_sign(pctx, sig, siglen, tbs, tbslen) > 0 ? 1 : 0;
  }
  EVP_PKEY_CTX_free(pctx);
  EVP_PKEY_free(owned);
  return rc;
}

/* Upper bound on the DER signature size, reported for the sig==NULL size
 * query so EVP can size its output buffer without consuming digest state. */
static int zz9k_ecdsa_sign_maxsize(ZZ9K_ECDSA_CTX *ctx, size_t *siglen)
{
  if (ctx->key == NULL) {
    return 0;
  }
  if (ctx->key->is_p256) {
    *siglen = 72;   /* max DER ECDSA-Sig-Value for P-256 */
    return 1;
  }
  if (ctx->key->shadow != NULL) {
    *siglen = (size_t)EVP_PKEY_get_size(ctx->key->shadow);
    return 1;
  }
  return 0;
}

static int zz9k_ecdsa_sign_init(void *vctx, void *provkey,
                                const OSSL_PARAM params[])
{
  ZZ9K_ECDSA_CTX *ctx = vctx;

  if (ctx == NULL || provkey == NULL) {
    return 0;
  }
  ctx->key = provkey;
  return zz9k_ecdsa_set_ctx_params(vctx, params);
}

static int zz9k_ecdsa_sign(void *vctx, unsigned char *sig, size_t *siglen,
                           size_t sigsize, const unsigned char *tbs,
                           size_t tbslen)
{
  ZZ9K_ECDSA_CTX *ctx = vctx;

  if (ctx == NULL || ctx->key == NULL || siglen == NULL) {
    return 0;
  }
  if (sig == NULL) {
    return zz9k_ecdsa_sign_maxsize(ctx, siglen);
  }
  return zz9k_ecdsa_sign_delegated(ctx, sig, siglen, sigsize, tbs, tbslen);
}

static int zz9k_ecdsa_digest_sign_init(void *vctx, const char *mdname,
                                       void *provkey,
                                       const OSSL_PARAM params[])
{
  ZZ9K_ECDSA_CTX *ctx = vctx;
  EVP_MD *md;

  if (ctx == NULL || provkey == NULL) {
    return 0;
  }
  ctx->key = provkey;
  if (ctx->mdctx == NULL) {
    ctx->mdctx = EVP_MD_CTX_new();
    if (ctx->mdctx == NULL) {
      return 0;
    }
  }
  md = zz9k_ecdsa_fetch_md(ctx, mdname);
  if (md == NULL || !EVP_DigestInit_ex(ctx->mdctx, md, NULL)) {
    EVP_MD_free(md);
    return 0;
  }
  EVP_MD_free(md);
  return zz9k_ecdsa_set_ctx_params(vctx, params);
}

static int zz9k_ecdsa_digest_sign_update(void *vctx, const unsigned char *data,
                                         size_t datalen)
{
  ZZ9K_ECDSA_CTX *ctx = vctx;

  if (ctx == NULL || ctx->mdctx == NULL) {
    return 0;
  }
  return EVP_DigestUpdate(ctx->mdctx, data, datalen);
}

static int zz9k_ecdsa_digest_sign_final(void *vctx, unsigned char *sig,
                                        size_t *siglen, size_t sigsize)
{
  ZZ9K_ECDSA_CTX *ctx = vctx;
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digestlen = 0;

  if (ctx == NULL || ctx->mdctx == NULL || siglen == NULL) {
    return 0;
  }
  if (sig == NULL) {
    return zz9k_ecdsa_sign_maxsize(ctx, siglen);
  }
  if (!EVP_DigestFinal_ex(ctx->mdctx, digest, &digestlen)) {
    return 0;
  }
  return zz9k_ecdsa_sign_delegated(ctx, sig, siglen, sigsize, digest,
                                   digestlen);
}

static int zz9k_ecdsa_digest_sign_oneshot(void *vctx, unsigned char *sig,
                                          size_t *siglen, size_t sigsize,
                                          const unsigned char *tbs,
                                          size_t tbslen)
{
  ZZ9K_ECDSA_CTX *ctx = vctx;
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digestlen = 0;

  if (ctx == NULL || ctx->mdctx == NULL || siglen == NULL) {
    return 0;
  }
  if (sig == NULL) {
    return zz9k_ecdsa_sign_maxsize(ctx, siglen);
  }
  if (!EVP_DigestUpdate(ctx->mdctx, tbs, tbslen) ||
      !EVP_DigestFinal_ex(ctx->mdctx, digest, &digestlen)) {
    return 0;
  }
  return zz9k_ecdsa_sign_delegated(ctx, sig, siglen, sigsize, digest,
                                   digestlen);
}

const OSSL_DISPATCH zz9k_ecdsa_signature_functions[] = {
  { OSSL_FUNC_SIGNATURE_NEWCTX, (void (*)(void))zz9k_ecdsa_newctx },
  { OSSL_FUNC_SIGNATURE_FREECTX, (void (*)(void))zz9k_ecdsa_freectx },
  { OSSL_FUNC_SIGNATURE_VERIFY_INIT, (void (*)(void))zz9k_ecdsa_verify_init },
  { OSSL_FUNC_SIGNATURE_VERIFY, (void (*)(void))zz9k_ecdsa_verify },
  { OSSL_FUNC_SIGNATURE_SIGN_INIT, (void (*)(void))zz9k_ecdsa_sign_init },
  { OSSL_FUNC_SIGNATURE_SIGN, (void (*)(void))zz9k_ecdsa_sign },
  { OSSL_FUNC_SIGNATURE_DIGEST_SIGN_INIT,
    (void (*)(void))zz9k_ecdsa_digest_sign_init },
  { OSSL_FUNC_SIGNATURE_DIGEST_SIGN_UPDATE,
    (void (*)(void))zz9k_ecdsa_digest_sign_update },
  { OSSL_FUNC_SIGNATURE_DIGEST_SIGN_FINAL,
    (void (*)(void))zz9k_ecdsa_digest_sign_final },
  { OSSL_FUNC_SIGNATURE_DIGEST_SIGN,
    (void (*)(void))zz9k_ecdsa_digest_sign_oneshot },
  { OSSL_FUNC_SIGNATURE_DIGEST_VERIFY_INIT,
    (void (*)(void))zz9k_ecdsa_digest_verify_init },
  { OSSL_FUNC_SIGNATURE_DIGEST_VERIFY_UPDATE,
    (void (*)(void))zz9k_ecdsa_digest_verify_update },
  { OSSL_FUNC_SIGNATURE_DIGEST_VERIFY_FINAL,
    (void (*)(void))zz9k_ecdsa_digest_verify_final },
  { OSSL_FUNC_SIGNATURE_DIGEST_VERIFY,
    (void (*)(void))zz9k_ecdsa_digest_verify_oneshot },
  { OSSL_FUNC_SIGNATURE_SET_CTX_PARAMS,
    (void (*)(void))zz9k_ecdsa_set_ctx_params },
  { OSSL_FUNC_SIGNATURE_SETTABLE_CTX_PARAMS,
    (void (*)(void))zz9k_ecdsa_settable_ctx_params },
  { 0, NULL }
};
/* The SIGNATURE OSSL_ALGORITHM table (ECDSA + RSA) is assembled in
 * zz9k_algorithms.c; the KEYMGMT/KEYEXCH tables (which now include EC/ECDH)
 * are assembled there too. */
