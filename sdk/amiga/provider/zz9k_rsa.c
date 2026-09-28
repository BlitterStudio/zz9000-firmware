/*
 * ZZ9000 OpenSSL provider — RSA key management and RSA signature verification.
 *
 * The provider owns the OpenSSL "RSA" keytype so RSA-2048 certificate-chain
 * signature verification can run through it. Acceleration is decided
 * per-VERIFY rather than per-key: the SAME imported RSA key verifies PKCS#1
 * v1.5/SHA-256 signatures (offloadable — chain/TLS1.2 signatures) and
 * RSA-PSS signatures (NOT offloadable — the firmware/software reference only
 * does PKCS#1 v1.5; TLS1.3 uses PSS for RSA certificate/CertificateVerify
 * signatures, so this is the common case). The KEYMGMT `import` therefore:
 *
 *   - always builds a "shadow" EVP_PKEY against the DEFAULT provider (via
 *     EVP_PKEY_fromdata with "provider=default") from the same n/e, lazily at
 *     import time. Any well-formed RSA public key imports successfully this
 *     way, whatever its size.
 *   - additionally, when the key fits the accelerable constraints (<=
 *     4096-bit modulus that is a whole number of 32-bit limbs, exponent <=
 *     32 bits — what zz9k_prov_rsa_verify can offload), also keeps n/e
 *     directly in the key object and marks it `accel`.
 *
 * The SIGNATURE verify then picks per-call: PKCS#1 v1.5 + SHA-256 + an
 * `accel` key + matching signature/digest lengths -> zz9k_prov_rsa_verify
 * (ZZ9000 offload, software reference on the host); anything else (PSS,
 * other digests, non-accelerable keys/sizes) -> forward to the shadow via
 * EVP_PKEY_verify, replaying the captured padding/digest/MGF1/salt-length
 * parameters. Fails closed (never returns valid) on any error, NULL, or
 * missing shadow — mirrors zz9k_ecdsa.c's delegation pattern for EC.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k_prov_local.h"

#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/crypto.h>
#include <openssl/bn.h>
#include <openssl/evp.h>

#include "zz9k-crypto-soft.h"
#include "zz9k_offload.h"

#include <string.h>
#include <stdlib.h>

#define ZZ9K_RSA_MAX_BYTES 512         /* 4096-bit modulus */
#define ZZ9K_RSA_PKCS1_PADDING 1       /* mirrors RSA_PKCS1_PADDING */
#define ZZ9K_RSA_PSS_PADDING 6         /* mirrors RSA_PKCS1_PSS_PADDING */
#define ZZ9K_RSA_MDNAME_MAX 32

static int zz9k_prov_rsa_verify(const unsigned char *sig, uint32_t siglen,
                                const unsigned char *hash,
                                const unsigned char *n, uint32_t nbits,
                                uint32_t e, ZZ9K_PROV_CTX *provctx)
{
#ifdef ZZ9K_PROVIDER_OFFLOAD
  /* Verify on the ZZ9000 (zz9k_crypto_verify) when the firmware advertises
   * RSA verify. Current firmware accepts RSA-2048/3072/4096 (BearSSL is
   * size-agnostic up to 4096-bit) under the single RSA_PKCS1 algorithm id.
   * The key is marshalled as modulus || exponent, the exponent in 4 big-endian
   * bytes. On an offload miss the caller falls back to the default provider
   * (fast) via the key's shadow; an "invalid" verdict for moduli wider than
   * 2048 bits is treated as a miss too, in case the deployed firmware predates
   * the wider sizes and bound-checks rather than errors. */
  if (ZZ9K_PROV_CAN_OFFLOAD(provctx, ZZ9K_SERVICE_FLAG_CRYPTO_RSA_2048)) {
    unsigned int nbytes = nbits / 8U;
    if (nbytes <= ZZ9K_RSA_MAX_BYTES) {
      unsigned char key[ZZ9K_RSA_MAX_BYTES + 4];
      int valid = 0;
      memcpy(key, n, nbytes);
      key[nbytes + 0] = (unsigned char)(e >> 24);
      key[nbytes + 1] = (unsigned char)(e >> 16);
      key[nbytes + 2] = (unsigned char)(e >> 8);
      key[nbytes + 3] = (unsigned char)(e);
      if (zz9k_offload_verify(provctx->sdk_ctx, ZZ9K_OFFLOAD_VERIFY_RSA_PKCS1,
                              hash, sig, siglen, key, nbytes + 4U, &valid) >= 0 &&
          (valid || nbytes <= 256U)) {
        return valid;
      }
    }
  }
  /* Capability absent, or the offload timed out because the ZZ9000 is busy
   * driving the display: return -1 so the caller falls back to the default
   * provider (fast) instead of the in-tree software reference (used only on the
   * host build below). */
  return -1;
#elif defined(ZZ9K_TEST_DEFAULT_FALLBACK)
  (void)sig;
  (void)siglen;
  (void)hash;
  (void)n;
  (void)nbits;
  (void)e;
  (void)provctx;
  return -1;   /* force the caller to exercise the default-provider fallback */
#else
  (void)provctx;
  return zz9k_soft_rsa_verify_pkcs1_sha256(sig, siglen, hash, n, nbits, e);
#endif
}

/* ---- RSA KEYMGMT ---- */

typedef struct {
  ZZ9K_PROV_CTX *provctx;
  unsigned char n[ZZ9K_RSA_MAX_BYTES];
  uint32_t nbytes;
  uint32_t e;
  int has_pub;
  int accel;          /* n/e populated: eligible for the PKCS#1 fast path */
  EVP_PKEY *shadow;    /* default-provider shadow, built at import */
} ZZ9K_RSA_KEY;

static void *zz9k_rsa_keymgmt_new(void *provctx)
{
  ZZ9K_RSA_KEY *key = OPENSSL_zalloc(sizeof(*key));
  if (key != NULL) {
    key->provctx = provctx;
  }
  return key;
}

static void zz9k_rsa_keymgmt_free(void *keydata)
{
  ZZ9K_RSA_KEY *key = keydata;
  if (key != NULL) {
    EVP_PKEY_free(key->shadow);
    OPENSSL_free(key);
  }
}

static int zz9k_rsa_keymgmt_has(const void *keydata, int selection)
{
  const ZZ9K_RSA_KEY *key = keydata;

  if (key == NULL || key->shadow == NULL) {
    return 0;
  }
  if ((selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) != 0 && !key->has_pub) {
    return 0;
  }
  return 1;
}

static int zz9k_rsa_keymgmt_import(void *keydata, int selection,
                                   const OSSL_PARAM params[])
{
  ZZ9K_RSA_KEY *key = keydata;
  ZZ9K_PROV_CTX *pc;
  const OSSL_PARAM *pn;
  const OSSL_PARAM *pe;
  EVP_PKEY_CTX *kctx;
  EVP_PKEY *shadow = NULL;
  BIGNUM *n = NULL;
  BIGNUM *e = NULL;
  int nbytes;

  if (key == NULL || (selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) == 0) {
    return 0;
  }
  pn = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_RSA_N);
  pe = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_RSA_E);
  if (pn == NULL || pe == NULL) {
    return 0;
  }
  pc = key->provctx;
  if (pc == NULL) {
    return 0;
  }

  /* Build the shadow default-provider key from the SAME import params FIRST.
   * This is what makes import succeed for every well-formed RSA public key,
   * not just the accelerable sizes handled below — declining here (as the
   * old verify-only code did for oversized/odd keys) would break cert-chain
   * verification for those keys outright, defeating the point of
   * delegation. */
  kctx = EVP_PKEY_CTX_new_from_name((OSSL_LIB_CTX *)pc->libctx, "RSA",
                                    "provider=default");
  if (kctx == NULL) {
    return 0;
  }
  if (EVP_PKEY_fromdata_init(kctx) <= 0 ||
      /* OSSL_PARAM params[] is non-const in the public API even though
       * fromdata only reads it; params[] itself is caller-owned. */
      EVP_PKEY_fromdata(kctx, &shadow, selection, (OSSL_PARAM *)params) <= 0) {
    EVP_PKEY_CTX_free(kctx);
    return 0;
  }
  EVP_PKEY_CTX_free(kctx);

  EVP_PKEY_free(key->shadow);
  key->shadow = shadow;
  key->has_pub = 1;
  key->accel = 0;
  key->nbytes = 0;
  key->e = 0;

  /* Separately, populate n/e for the accelerated PKCS#1 path when the key
   * fits the sizes the software/firmware reference can verify: <= 4096-bit
   * modulus that is a whole number of 32-bit limbs, and a public exponent
   * that fits in 32 bits. A key outside these bounds still imported fine
   * above — it simply always delegates (see zz9k_rsa_sig_accel_eligible). */
  if (OSSL_PARAM_get_BN(pn, &n) && OSSL_PARAM_get_BN(pe, &e)) {
    nbytes = BN_num_bytes(n);
    if (nbytes > 0 && nbytes <= ZZ9K_RSA_MAX_BYTES && (nbytes % 4) == 0 &&
        BN_num_bits(e) <= 32 && BN_bn2binpad(n, key->n, nbytes) == nbytes) {
      key->nbytes = (uint32_t)nbytes;
      key->e = (uint32_t)BN_get_word(e);
      key->accel = 1;
    }
  }
  BN_free(n);
  BN_free(e);
  return 1;
}

static const OSSL_PARAM *zz9k_rsa_keymgmt_import_types(int selection)
{
  static const OSSL_PARAM types[] = {
    OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_N, NULL, 0),
    OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_E, NULL, 0),
    OSSL_PARAM_END
  };
  if ((selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) != 0) {
    return types;
  }
  return NULL;
}

static const OSSL_PARAM *zz9k_rsa_keymgmt_export_types(int selection)
{
  return zz9k_rsa_keymgmt_import_types(selection);
}

static int zz9k_rsa_keymgmt_export(void *keydata, int selection,
                                   OSSL_CALLBACK *param_cb, void *cbarg)
{
  ZZ9K_RSA_KEY *key = keydata;

  /* Delegated through the shadow so a caller sees the same result whichever
   * provider ends up doing the exporting (mirrors zz9k_ec_keymgmt_export). */
  if (key == NULL || key->shadow == NULL) {
    return 0;
  }
  return EVP_PKEY_export(key->shadow, selection, param_cb, cbarg);
}

static int zz9k_rsa_keymgmt_get_params(void *keydata, OSSL_PARAM params[])
{
  ZZ9K_RSA_KEY *key = keydata;
  OSSL_PARAM *p;
  int bits = 0;
  int secbits = 0;
  int maxsize = 0;

  if (key == NULL) {
    return 0;
  }
  /* Forward the numeric params from the shadow (always present after a
   * successful import) so introspection reports the real key's size, whether
   * or not it is accel-eligible — mirrors zz9k_ec_keymgmt_get_params. */
  if (key->shadow != NULL) {
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
  return 1;
}

static const OSSL_PARAM *zz9k_rsa_keymgmt_gettable_params(void *provctx)
{
  static const OSSL_PARAM types[] = {
    OSSL_PARAM_int(OSSL_PKEY_PARAM_BITS, NULL),
    OSSL_PARAM_int(OSSL_PKEY_PARAM_SECURITY_BITS, NULL),
    OSSL_PARAM_int(OSSL_PKEY_PARAM_MAX_SIZE, NULL),
    OSSL_PARAM_END
  };
  (void)provctx;
  return types;
}

static int zz9k_rsa_keymgmt_match(const void *keydata1, const void *keydata2,
                                  int selection)
{
  const ZZ9K_RSA_KEY *a = keydata1;
  const ZZ9K_RSA_KEY *b = keydata2;

  (void)selection;
  if (a == NULL || b == NULL || a->shadow == NULL || b->shadow == NULL) {
    return 0;
  }
  return EVP_PKEY_eq(a->shadow, b->shadow) == 1;
}

static void *zz9k_rsa_keymgmt_dup(const void *keydata_from, int selection)
{
  const ZZ9K_RSA_KEY *from = keydata_from;
  ZZ9K_RSA_KEY *to;

  (void)selection;
  if (from == NULL) {
    return NULL;
  }
  to = OPENSSL_zalloc(sizeof(*to));
  if (to == NULL) {
    return NULL;
  }
  to->provctx = from->provctx;
  if (from->shadow != NULL) {
    to->shadow = EVP_PKEY_dup(from->shadow);
    if (to->shadow == NULL) {
      OPENSSL_free(to);
      return NULL;
    }
  }
  to->has_pub = from->has_pub;
  to->accel = from->accel;
  to->nbytes = from->nbytes;
  to->e = from->e;
  memcpy(to->n, from->n, sizeof(to->n));
  return to;
}

static const char *zz9k_rsa_keymgmt_query_operation_name(int operation_id)
{
  if (operation_id == OSSL_OP_SIGNATURE) {
    return "RSA";
  }
  return NULL;
}

/* ---- RSA key generation (always delegated) ----
 *
 * The ZZ9000 has no RSA keygen primitive, but owning the "RSA" keytype means
 * app-level RSA keygen routes here and must not regress versus v2.2.0 (which
 * left RSA to the default provider). So generation delegates wholesale to the
 * default provider, replaying the caller's gen params (bits, public exponent,
 * prime count), and the result is wrapped as our key object: the shadow is the
 * generated EVP_PKEY (which carries the private key, so signing can delegate
 * to it), and n/e are additionally captured for the accelerated PKCS#1 verify
 * fast path when the size fits — exactly as import does. */
typedef struct {
  ZZ9K_PROV_CTX *provctx;
  int selection;
  OSSL_PARAM *params;   /* dup of the gen params, replayed onto the default ctx */
} ZZ9K_RSA_GEN;

static void *zz9k_rsa_gen_init(void *provctx, int selection,
                               const OSSL_PARAM params[])
{
  ZZ9K_RSA_GEN *gen;

  /* RSA has no domain parameters — only keypair generation is meaningful. */
  if ((selection & OSSL_KEYMGMT_SELECT_KEYPAIR) == 0) {
    return NULL;
  }
  gen = OPENSSL_zalloc(sizeof(*gen));
  if (gen != NULL) {
    gen->provctx = provctx;
    gen->selection = selection;
    if (params != NULL) {
      gen->params = OSSL_PARAM_dup(params);
    }
  }
  return gen;
}

static int zz9k_rsa_gen_set_params(void *genctx, const OSSL_PARAM params[])
{
  ZZ9K_RSA_GEN *gen = genctx;

  if (gen == NULL) {
    return 0;
  }
  if (params == NULL) {
    return 1;
  }
  /* The keygen bits/exponent arrive here (EVP_PKEY_CTX_set_rsa_keygen_*),
   * replaced wholesale so the delegated default keygen ctx sees them verbatim. */
  OSSL_PARAM_free(gen->params);
  gen->params = OSSL_PARAM_dup(params);
  return 1;
}

static const OSSL_PARAM *zz9k_rsa_gen_settable_params(void *genctx,
                                                      void *provctx)
{
  static const OSSL_PARAM types[] = {
    OSSL_PARAM_size_t(OSSL_PKEY_PARAM_RSA_BITS, NULL),
    OSSL_PARAM_size_t(OSSL_PKEY_PARAM_RSA_PRIMES, NULL),
    OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_E, NULL, 0),
    OSSL_PARAM_END
  };
  (void)genctx;
  (void)provctx;
  return types;
}

static void *zz9k_rsa_gen(void *genctx, OSSL_CALLBACK *cb, void *cbarg)
{
  ZZ9K_RSA_GEN *gen = genctx;
  OSSL_LIB_CTX *libctx;
  EVP_PKEY_CTX *dctx;
  EVP_PKEY *pk = NULL;
  ZZ9K_RSA_KEY *key;
  BIGNUM *n = NULL;
  BIGNUM *e = NULL;
  int nbytes;

  (void)cb;
  (void)cbarg;
  if (gen == NULL) {
    return NULL;
  }
  libctx = (OSSL_LIB_CTX *)(gen->provctx != NULL ? gen->provctx->libctx : NULL);
  dctx = EVP_PKEY_CTX_new_from_name(libctx, "RSA", "provider=default");
  if (dctx == NULL) {
    return NULL;
  }
  if (EVP_PKEY_keygen_init(dctx) <= 0 ||
      (gen->params != NULL && EVP_PKEY_CTX_set_params(dctx, gen->params) <= 0) ||
      EVP_PKEY_generate(dctx, &pk) <= 0) {
    EVP_PKEY_CTX_free(dctx);
    EVP_PKEY_free(pk);
    return NULL;
  }
  EVP_PKEY_CTX_free(dctx);

  key = OPENSSL_zalloc(sizeof(*key));
  if (key == NULL) {
    EVP_PKEY_free(pk);
    return NULL;
  }
  key->provctx = gen->provctx;
  key->shadow = pk;
  key->has_pub = 1;
  /* Populate n/e for the accelerated PKCS#1 fast path when the size fits,
   * mirroring zz9k_rsa_keymgmt_import. */
  if (EVP_PKEY_get_bn_param(pk, OSSL_PKEY_PARAM_RSA_N, &n) &&
      EVP_PKEY_get_bn_param(pk, OSSL_PKEY_PARAM_RSA_E, &e)) {
    nbytes = BN_num_bytes(n);
    if (nbytes > 0 && nbytes <= ZZ9K_RSA_MAX_BYTES && (nbytes % 4) == 0 &&
        BN_num_bits(e) <= 32 && BN_bn2binpad(n, key->n, nbytes) == nbytes) {
      key->nbytes = (uint32_t)nbytes;
      key->e = (uint32_t)BN_get_word(e);
      key->accel = 1;
    }
  }
  BN_free(n);
  BN_free(e);
  return key;
}

static void zz9k_rsa_gen_cleanup(void *genctx)
{
  ZZ9K_RSA_GEN *gen = genctx;
  if (gen != NULL) {
    OSSL_PARAM_free(gen->params);
  }
  OPENSSL_free(genctx);
}

const OSSL_DISPATCH zz9k_rsa_keymgmt_functions[] = {
  { OSSL_FUNC_KEYMGMT_NEW, (void (*)(void))zz9k_rsa_keymgmt_new },
  { OSSL_FUNC_KEYMGMT_FREE, (void (*)(void))zz9k_rsa_keymgmt_free },
  { OSSL_FUNC_KEYMGMT_HAS, (void (*)(void))zz9k_rsa_keymgmt_has },
  { OSSL_FUNC_KEYMGMT_IMPORT, (void (*)(void))zz9k_rsa_keymgmt_import },
  { OSSL_FUNC_KEYMGMT_IMPORT_TYPES,
    (void (*)(void))zz9k_rsa_keymgmt_import_types },
  { OSSL_FUNC_KEYMGMT_EXPORT, (void (*)(void))zz9k_rsa_keymgmt_export },
  { OSSL_FUNC_KEYMGMT_EXPORT_TYPES,
    (void (*)(void))zz9k_rsa_keymgmt_export_types },
  { OSSL_FUNC_KEYMGMT_GET_PARAMS,
    (void (*)(void))zz9k_rsa_keymgmt_get_params },
  { OSSL_FUNC_KEYMGMT_GETTABLE_PARAMS,
    (void (*)(void))zz9k_rsa_keymgmt_gettable_params },
  { OSSL_FUNC_KEYMGMT_MATCH, (void (*)(void))zz9k_rsa_keymgmt_match },
  { OSSL_FUNC_KEYMGMT_DUP, (void (*)(void))zz9k_rsa_keymgmt_dup },
  { OSSL_FUNC_KEYMGMT_GEN_INIT, (void (*)(void))zz9k_rsa_gen_init },
  { OSSL_FUNC_KEYMGMT_GEN_SET_PARAMS,
    (void (*)(void))zz9k_rsa_gen_set_params },
  { OSSL_FUNC_KEYMGMT_GEN_SETTABLE_PARAMS,
    (void (*)(void))zz9k_rsa_gen_settable_params },
  { OSSL_FUNC_KEYMGMT_GEN, (void (*)(void))zz9k_rsa_gen },
  { OSSL_FUNC_KEYMGMT_GEN_CLEANUP, (void (*)(void))zz9k_rsa_gen_cleanup },
  { OSSL_FUNC_KEYMGMT_QUERY_OPERATION_NAME,
    (void (*)(void))zz9k_rsa_keymgmt_query_operation_name },
  { 0, NULL }
};

/* ---- RSA SIGNATURE (verify) ---- */

typedef struct {
  ZZ9K_PROV_CTX *provctx;
  ZZ9K_RSA_KEY *key;
  int pad_mode;                          /* ZZ9K_RSA_{PKCS1,PSS}_PADDING */
  char mdname[ZZ9K_RSA_MDNAME_MAX];      /* '\0' = unset */
  char mgf1mdname[ZZ9K_RSA_MDNAME_MAX];  /* '\0' = unset (PSS only) */
  int saltlen;                           /* meaningful only if has_saltlen */
  int has_saltlen;
  EVP_MD_CTX *mdctx;   /* only used by the DigestVerify{Init,Update,Final} trio */
} ZZ9K_RSA_SIG_CTX;

static void *zz9k_rsa_sig_newctx(void *provctx, const char *propq)
{
  ZZ9K_RSA_SIG_CTX *ctx = OPENSSL_zalloc(sizeof(*ctx));
  (void)propq;
  if (ctx != NULL) {
    ctx->provctx = provctx;
    ctx->pad_mode = ZZ9K_RSA_PKCS1_PADDING;   /* matches the default provider */
  }
  return ctx;
}

static void zz9k_rsa_sig_freectx(void *vctx)
{
  ZZ9K_RSA_SIG_CTX *ctx = vctx;
  if (ctx != NULL) {
    EVP_MD_CTX_free(ctx->mdctx);
  }
  OPENSSL_free(vctx);
}

static int zz9k_rsa_sig_set_ctx_params(void *vctx, const OSSL_PARAM params[]);

static int zz9k_rsa_sig_verify_init(void *vctx, void *provkey,
                                    const OSSL_PARAM params[])
{
  ZZ9K_RSA_SIG_CTX *ctx = vctx;

  if (ctx == NULL || provkey == NULL) {
    return 0;
  }
  ctx->key = provkey;
  return zz9k_rsa_sig_set_ctx_params(ctx, params);
}

/* PKCS#1 v1.5 + SHA-256 + an `accel` key + matching lengths is the only
 * shape zz9k_prov_rsa_verify can serve; everything else must delegate. An
 * unset digest name (mdname[0] == '\0') is treated as SHA-256 like the
 * pre-delegation code always implicitly did for the raw (non-DigestVerify)
 * verify path, which only ever checked tbslen == 32. */
static int zz9k_rsa_sig_accel_eligible(const ZZ9K_RSA_SIG_CTX *ctx,
                                       size_t siglen, size_t tbslen)
{
  if (ctx->pad_mode != ZZ9K_RSA_PKCS1_PADDING) {
    return 0;
  }
  if (ctx->mdname[0] != '\0' && strcmp(ctx->mdname, "SHA256") != 0 &&
      strcmp(ctx->mdname, "SHA2-256") != 0) {
    return 0;
  }
  if (ctx->key == NULL || !ctx->key->accel) {
    return 0;
  }
  if (tbslen != 32 || siglen != ctx->key->nbytes) {
    return 0;
  }
  return 1;
}

/* Delegated verify: forward to the shadow EVP_PKEY built at import time
 * against the default provider, replaying the captured padding/digest/MGF1/
 * salt-length so PSS (and any other non-accelerated shape) verifies
 * correctly. Fails closed on any error, a missing shadow, or an invalid
 * signature — never returns 1 except on a confirmed-valid EVP_PKEY_verify
 * result. */
static int zz9k_rsa_sig_verify_delegated(ZZ9K_RSA_SIG_CTX *ctx,
                                         const unsigned char *sig,
                                         size_t siglen,
                                         const unsigned char *tbs,
                                         size_t tbslen)
{
  EVP_PKEY_CTX *pctx;
  OSSL_PARAM params[5];
  int n = 0;
  int rc;

  if (ctx->key == NULL || ctx->key->shadow == NULL) {
    return 0;
  }
  /* MUST force "provider=default" here (not NULL/inherited) — see the
   * identical comment in zz9k_ecdsa_verify_delegated: with an inherited
   * "?provider=zz9000" propquery this fetch would still prefer OUR "RSA" op
   * even though the shadow's keydata belongs to the default provider, and
   * EVP would bridge it right back into this same delegated path (infinite
   * recursion). Requiring "provider=default" pins both the op and the key to
   * the same, correct provider. */
  pctx = EVP_PKEY_CTX_new_from_pkey(
      (OSSL_LIB_CTX *)(ctx->provctx != NULL ? ctx->provctx->libctx : NULL),
      ctx->key->shadow, "provider=default");
  if (pctx == NULL) {
    return 0;
  }
  if (EVP_PKEY_verify_init(pctx) <= 0) {
    EVP_PKEY_CTX_free(pctx);
    return 0;
  }

  ZZ9K_PARAM_INT(&params[n], OSSL_SIGNATURE_PARAM_PAD_MODE, &ctx->pad_mode);
  n++;
  if (ctx->mdname[0] != '\0') {
    ZZ9K_PARAM_UTF8(&params[n], OSSL_SIGNATURE_PARAM_DIGEST, ctx->mdname);
    n++;
  }
  if (ctx->pad_mode == ZZ9K_RSA_PSS_PADDING) {
    if (ctx->mgf1mdname[0] != '\0') {
      ZZ9K_PARAM_UTF8(&params[n], OSSL_SIGNATURE_PARAM_MGF1_DIGEST,
                      ctx->mgf1mdname);
      n++;
    }
    if (ctx->has_saltlen) {
      ZZ9K_PARAM_INT(&params[n], OSSL_SIGNATURE_PARAM_PSS_SALTLEN,
                    &ctx->saltlen);
      n++;
    }
  }
  ZZ9K_PARAM_END(&params[n]);

  if (EVP_PKEY_CTX_set_params(pctx, params) <= 0) {
    EVP_PKEY_CTX_free(pctx);
    return 0;
  }
  rc = EVP_PKEY_verify(pctx, sig, siglen, tbs, tbslen);
  EVP_PKEY_CTX_free(pctx);
  return rc == 1 ? 1 : 0;
}

/* Fallback for the accelerated PKCS#1 v1.5 / SHA-256 path when the board cannot
 * serve the offload (capability absent, or a mailbox timeout under display
 * contention): verify through the key's default-provider shadow rather than the
 * ~1.4x-slower in-tree software reference. The accelerated path is always
 * SHA-256 PKCS#1 (the firmware/software reference assume SHA-256 and
 * zz9k_rsa_sig_accel_eligible enforces a 32-byte digest), so force that digest
 * here instead of replaying ctx->mdname — which accel_eligible permits to be
 * empty. Same provider=default recursion guard as zz9k_rsa_sig_verify_delegated. */
static int zz9k_rsa_verify_pkcs1_sha256_via_default(ZZ9K_RSA_SIG_CTX *ctx,
                                                    const unsigned char *sig,
                                                    size_t siglen,
                                                    const unsigned char *tbs,
                                                    size_t tbslen)
{
  EVP_PKEY_CTX *pctx;
  OSSL_PARAM params[3];
  int pad = ZZ9K_RSA_PKCS1_PADDING;
  int n = 0;
  int rc;

  if (ctx->key == NULL || ctx->key->shadow == NULL) {
    return 0;
  }
  pctx = EVP_PKEY_CTX_new_from_pkey(
      (OSSL_LIB_CTX *)(ctx->provctx != NULL ? ctx->provctx->libctx : NULL),
      ctx->key->shadow, "provider=default");
  if (pctx == NULL) {
    return 0;
  }
  if (EVP_PKEY_verify_init(pctx) <= 0) {
    EVP_PKEY_CTX_free(pctx);
    return 0;
  }
  ZZ9K_PARAM_INT(&params[n], OSSL_SIGNATURE_PARAM_PAD_MODE, &pad);
  n++;
  ZZ9K_PARAM_UTF8(&params[n], OSSL_SIGNATURE_PARAM_DIGEST, "SHA256");
  n++;
  ZZ9K_PARAM_END(&params[n]);
  if (EVP_PKEY_CTX_set_params(pctx, params) <= 0) {
    EVP_PKEY_CTX_free(pctx);
    return 0;
  }
  rc = EVP_PKEY_verify(pctx, sig, siglen, tbs, tbslen);
  EVP_PKEY_CTX_free(pctx);
  return rc == 1 ? 1 : 0;
}

/* RSA-2048 verify runs in SOFTWARE by default. The board offload saves only
 * ~90 ms (29 ms vs 119 ms in the default-provider shadow, measured by
 * tools/zz9k-cryptoprofile.c "rsa" KAT) but a mailbox round trip can stall for
 * MINUTES under display contention — observed on hardware: a page whose TLS
 * cert triggers this verify (rsa2048.badssl.com, whose leaf cert is
 * RSA-2048/PKCS#1-SHA256) hung >2.5 min with the offload and connected in ~9 s
 * with it off. The offload does fall back to the shadow on a *returned* miss,
 * but under contention the wait blocks rather than returning a timeout, so the
 * fallback never fires. Unlike ECDSA-P256 verify (990->22 ms) and the derives
 * (400-1300->8-14 ms), offloading a ~120 ms RSA verify is a net loss — the same
 * conclusion the AEAD size gate reaches for small records. ENV:
 * ZZ9K_RSA_VERIFY_OFFLOAD re-enables the board path for testing. Read once. */
static int zz9k_rsa_verify_offload_enabled(void)
{
  static int cached = -1;
  if (cached < 0) {
    cached = (getenv("ZZ9K_RSA_VERIFY_OFFLOAD") != NULL) ? 1 : 0;
  }
  return cached;
}

static int zz9k_rsa_sig_verify(void *vctx, const unsigned char *sig,
                               size_t siglen, const unsigned char *tbs,
                               size_t tbslen)
{
  ZZ9K_RSA_SIG_CTX *ctx = vctx;

  if (ctx == NULL || ctx->key == NULL || !ctx->key->has_pub) {
    return 0;
  }
  if (zz9k_rsa_sig_accel_eligible(ctx, siglen, tbslen)) {
    if (zz9k_rsa_verify_offload_enabled()) {
      int v = zz9k_prov_rsa_verify(sig, (uint32_t)siglen, tbs, ctx->key->n,
                                   ctx->key->nbytes * 8U, ctx->key->e,
                                   ctx->provctx);
      if (v >= 0) {
        return v;
      }
    }
    /* Default path (and offload-miss fallback): verify via the default-provider
     * shadow — fast, correct, and no mailbox round trip to stall under load. */
    return zz9k_rsa_verify_pkcs1_sha256_via_default(ctx, sig, siglen, tbs,
                                                    tbslen);
  }
  return zz9k_rsa_sig_verify_delegated(ctx, sig, siglen, tbs, tbslen);
}

/* Capture the verify parameters instead of declining anything but plain
 * PKCS#1/SHA-256 (the old verify-only behaviour): the accel-vs-delegate
 * decision is made per-verify by zz9k_rsa_sig_accel_eligible, not here — an
 * outright decline of e.g. PSS would break TLS1.3 RSA signature verification
 * (rsa_pss_rsae_sha256 and friends) instead of correctly delegating it. */
static int zz9k_rsa_sig_set_ctx_params(void *vctx, const OSSL_PARAM params[])
{
  ZZ9K_RSA_SIG_CTX *ctx = vctx;
  const OSSL_PARAM *p;

  if (ctx == NULL) {
    return 0;
  }
  if (params == NULL) {
    return 1;
  }
  p = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_PAD_MODE);
  if (p != NULL) {
    if (p->data_type == OSSL_PARAM_INTEGER) {
      if (!OSSL_PARAM_get_int(p, &ctx->pad_mode)) {
        return 0;
      }
    } else if (p->data_type == OSSL_PARAM_UTF8_STRING) {
      if (p->data == NULL) {
        return 0;
      }
      if (strcmp((const char *)p->data, "pkcs1") == 0) {
        ctx->pad_mode = ZZ9K_RSA_PKCS1_PADDING;
      } else if (strcmp((const char *)p->data, "pss") == 0) {
        ctx->pad_mode = ZZ9K_RSA_PSS_PADDING;
      } else {
        return 0;   /* not a padding this provider can accelerate or forward */
      }
    } else {
      return 0;
    }
    if (ctx->pad_mode != ZZ9K_RSA_PKCS1_PADDING &&
        ctx->pad_mode != ZZ9K_RSA_PSS_PADDING) {
      return 0;
    }
  }
  /* Any digest name is accepted (not just SHA-256): accel-eligibility is
   * checked at verify time, so e.g. PSS-SHA384 or even PKCS#1-SHA384 (neither
   * accelerable) is still recorded here and simply delegates. */
  p = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_DIGEST);
  if (p != NULL) {
    char *mp = ctx->mdname;
    if (!OSSL_PARAM_get_utf8_string(p, &mp, sizeof(ctx->mdname))) {
      return 0;
    }
  }
  p = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_MGF1_DIGEST);
  if (p != NULL) {
    char *mp = ctx->mgf1mdname;
    if (!OSSL_PARAM_get_utf8_string(p, &mp, sizeof(ctx->mgf1mdname))) {
      return 0;
    }
  }
  p = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_PSS_SALTLEN);
  if (p != NULL) {
    if (p->data_type == OSSL_PARAM_INTEGER) {
      if (!OSSL_PARAM_get_int(p, &ctx->saltlen)) {
        return 0;
      }
      ctx->has_saltlen = 1;
    } else if (p->data_type == OSSL_PARAM_UTF8_STRING) {
      char saltstr[16];
      char *sp = saltstr;
      char *end = NULL;
      long v;

      if (!OSSL_PARAM_get_utf8_string(p, &sp, sizeof(saltstr))) {
        return 0;
      }
      if (strcmp(saltstr, "digest") == 0) {
        ctx->saltlen = -1;               /* RSA_PSS_SALTLEN_DIGEST */
      } else if (strcmp(saltstr, "auto") == 0) {
        ctx->saltlen = -2;               /* RSA_PSS_SALTLEN_AUTO */
      } else if (strcmp(saltstr, "max") == 0) {
        ctx->saltlen = -3;               /* RSA_PSS_SALTLEN_MAX */
      } else if (strcmp(saltstr, "auto-digest-max") == 0) {
        ctx->saltlen = -4;               /* RSA_PSS_SALTLEN_AUTO_DIGEST_MAX */
      } else {
        v = strtol(saltstr, &end, 10);
        if (end == saltstr || *end != '\0') {
          return 0;
        }
        ctx->saltlen = (int)v;
      }
      ctx->has_saltlen = 1;
    } else {
      return 0;
    }
  }
  return 1;
}

static const OSSL_PARAM *zz9k_rsa_sig_settable_ctx_params(void *vctx,
                                                          void *provctx)
{
  static const OSSL_PARAM types[] = {
    OSSL_PARAM_int(OSSL_SIGNATURE_PARAM_PAD_MODE, NULL),
    OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_DIGEST, NULL, 0),
    OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_MGF1_DIGEST, NULL, 0),
    OSSL_PARAM_int(OSSL_SIGNATURE_PARAM_PSS_SALTLEN, NULL),
    OSSL_PARAM_END
  };
  (void)vctx;
  (void)provctx;
  return types;
}

/* ---- DigestVerify (EVP_DigestVerify* / X509_verify / TLS CertificateVerify) ----
 *
 * Required for the same reason as zz9k_ecdsa.c's trio: a provider signature
 * implementation that only offers plain VERIFY does not get digest hashing
 * for free, and X509_verify / libssl's TLS1.2/1.3 signature checks both go
 * through EVP_DigestVerify*. Hashes with a plain EVP_MD_CTX (independent of
 * any provider) and hands the digest to the same zz9k_rsa_sig_verify() used
 * by the raw path — for the delegated PSS path this is what hands the shadow
 * the externally computed digest with the PSS params replayed. */
/* libssl hands the DigestVerify/Sign init entry points the OpenSSL-3 canonical
 * digest name (e.g. "SHA2-256"). EVP_get_digestbyname's legacy name table is
 * incomplete for those aliases on some OpenSSL builds — notably 3.0.13, where
 * "SHA2-256" resolves to NULL, so every SHA-256 TLS signature verify fails to
 * initialise and the handshake aborts. Fetch through the library context
 * instead: it resolves both canonical and legacy names on every OpenSSL 3.x.
 * Returns a fetched EVP_MD the caller must EVP_MD_free() (EVP_DigestInit_ex
 * takes its own reference, so freeing immediately after init is safe); a
 * NULL/empty name defaults to SHA-256. */
static EVP_MD *zz9k_rsa_sig_fetch_md(ZZ9K_RSA_SIG_CTX *ctx, const char *mdname)
{
  OSSL_LIB_CTX *libctx =
      (OSSL_LIB_CTX *)(ctx->provctx != NULL ? ctx->provctx->libctx : NULL);
  return EVP_MD_fetch(
      libctx, (mdname != NULL && mdname[0] != '\0') ? mdname : "SHA2-256", NULL);
}

static int zz9k_rsa_sig_digest_verify_init(void *vctx, const char *mdname,
                                           void *provkey,
                                           const OSSL_PARAM params[])
{
  ZZ9K_RSA_SIG_CTX *ctx = vctx;
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
  md = zz9k_rsa_sig_fetch_md(ctx, mdname);
  if (md == NULL || !EVP_DigestInit_ex(ctx->mdctx, md, NULL)) {
    EVP_MD_free(md);
    return 0;
  }
  /* DigestVerifyInit hands the digest by name directly rather than via an
   * OSSL_PARAM, so it must be captured here too (for the accel decision and
   * the delegated replay) — mirrors what set_ctx_params(...DIGEST) does for
   * the raw-verify path. Use the canonical name so it matches what
   * zz9k_rsa_sig_accel_eligible compares against. */
  {
    const char *canon = EVP_MD_get0_name(md);
    if (canon != NULL) {
      OPENSSL_strlcpy(ctx->mdname, canon, sizeof(ctx->mdname));
    }
  }
  EVP_MD_free(md);
  return zz9k_rsa_sig_set_ctx_params(vctx, params);
}

static int zz9k_rsa_sig_digest_verify_update(void *vctx,
                                             const unsigned char *data,
                                             size_t datalen)
{
  ZZ9K_RSA_SIG_CTX *ctx = vctx;

  if (ctx == NULL || ctx->mdctx == NULL) {
    return 0;
  }
  return EVP_DigestUpdate(ctx->mdctx, data, datalen);
}

static int zz9k_rsa_sig_digest_verify_final(void *vctx,
                                            const unsigned char *sig,
                                            size_t siglen)
{
  ZZ9K_RSA_SIG_CTX *ctx = vctx;
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digestlen = 0;

  if (ctx == NULL || ctx->mdctx == NULL) {
    return 0;
  }
  if (!EVP_DigestFinal_ex(ctx->mdctx, digest, &digestlen)) {
    return 0;
  }
  return zz9k_rsa_sig_verify(vctx, sig, siglen, digest, digestlen);
}

/* One-shot EVP_DigestVerify(): libssl's TLS1.2/1.3 signature checks (and
 * X509_verify) call this single entry point rather than manual
 * Init+Update+Final. Reuses the same ctx->mdctx set up by
 * digest_verify_init. */
static int zz9k_rsa_sig_digest_verify_oneshot(void *vctx,
                                              const unsigned char *sig,
                                              size_t siglen,
                                              const unsigned char *tbs,
                                              size_t tbslen)
{
  ZZ9K_RSA_SIG_CTX *ctx = vctx;
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digestlen = 0;

  if (ctx == NULL || ctx->mdctx == NULL) {
    return 0;
  }
  if (!EVP_DigestUpdate(ctx->mdctx, tbs, tbslen) ||
      !EVP_DigestFinal_ex(ctx->mdctx, digest, &digestlen)) {
    return 0;
  }
  return zz9k_rsa_sig_verify(vctx, sig, siglen, digest, digestlen);
}

/* ---- RSA sign (always delegated) ----
 *
 * The ZZ9000 never accelerates RSA *signing* (no firmware primitive), but
 * owning the "RSA" keytype means a client-certificate RSA private key routes
 * its CertificateVerify signature here. Sign delegates to the shadow EVP_PKEY
 * (which carries the private key for imported private keys and for keys we
 * generated), replaying the captured padding/digest/MGF1/salt-length exactly
 * as the verify-delegation path does, and forcing "provider=default" to avoid
 * bridging back into this provider. */
static int zz9k_rsa_sig_sign_delegated(ZZ9K_RSA_SIG_CTX *ctx,
                                       unsigned char *sig, size_t *siglen,
                                       size_t sigsize,
                                       const unsigned char *tbs, size_t tbslen)
{
  EVP_PKEY_CTX *pctx;
  OSSL_PARAM params[5];
  int n = 0;
  int rc = 0;

  (void)sigsize;
  if (ctx->key == NULL || ctx->key->shadow == NULL) {
    return 0;
  }
  pctx = EVP_PKEY_CTX_new_from_pkey(
      (OSSL_LIB_CTX *)(ctx->provctx != NULL ? ctx->provctx->libctx : NULL),
      ctx->key->shadow, "provider=default");
  if (pctx == NULL) {
    return 0;
  }
  if (EVP_PKEY_sign_init(pctx) <= 0) {
    EVP_PKEY_CTX_free(pctx);
    return 0;
  }
  ZZ9K_PARAM_INT(&params[n], OSSL_SIGNATURE_PARAM_PAD_MODE, &ctx->pad_mode);
  n++;
  if (ctx->mdname[0] != '\0') {
    ZZ9K_PARAM_UTF8(&params[n], OSSL_SIGNATURE_PARAM_DIGEST, ctx->mdname);
    n++;
  }
  if (ctx->pad_mode == ZZ9K_RSA_PSS_PADDING) {
    if (ctx->mgf1mdname[0] != '\0') {
      ZZ9K_PARAM_UTF8(&params[n], OSSL_SIGNATURE_PARAM_MGF1_DIGEST,
                      ctx->mgf1mdname);
      n++;
    }
    if (ctx->has_saltlen) {
      ZZ9K_PARAM_INT(&params[n], OSSL_SIGNATURE_PARAM_PSS_SALTLEN,
                     &ctx->saltlen);
      n++;
    }
  }
  ZZ9K_PARAM_END(&params[n]);
  if (EVP_PKEY_CTX_set_params(pctx, params) <= 0) {
    EVP_PKEY_CTX_free(pctx);
    return 0;
  }
  rc = EVP_PKEY_sign(pctx, sig, siglen, tbs, tbslen) > 0 ? 1 : 0;
  EVP_PKEY_CTX_free(pctx);
  return rc;
}

static int zz9k_rsa_sig_maxsize(ZZ9K_RSA_SIG_CTX *ctx, size_t *siglen)
{
  if (ctx->key == NULL || ctx->key->shadow == NULL) {
    return 0;
  }
  *siglen = (size_t)EVP_PKEY_get_size(ctx->key->shadow);
  return 1;
}

static int zz9k_rsa_sig_sign_init(void *vctx, void *provkey,
                                  const OSSL_PARAM params[])
{
  ZZ9K_RSA_SIG_CTX *ctx = vctx;

  if (ctx == NULL || provkey == NULL) {
    return 0;
  }
  ctx->key = provkey;
  return zz9k_rsa_sig_set_ctx_params(ctx, params);
}

static int zz9k_rsa_sig_sign(void *vctx, unsigned char *sig, size_t *siglen,
                             size_t sigsize, const unsigned char *tbs,
                             size_t tbslen)
{
  ZZ9K_RSA_SIG_CTX *ctx = vctx;

  if (ctx == NULL || ctx->key == NULL || siglen == NULL) {
    return 0;
  }
  if (sig == NULL) {
    return zz9k_rsa_sig_maxsize(ctx, siglen);
  }
  return zz9k_rsa_sig_sign_delegated(ctx, sig, siglen, sigsize, tbs, tbslen);
}

static int zz9k_rsa_sig_digest_sign_init(void *vctx, const char *mdname,
                                         void *provkey,
                                         const OSSL_PARAM params[])
{
  ZZ9K_RSA_SIG_CTX *ctx = vctx;
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
  md = zz9k_rsa_sig_fetch_md(ctx, mdname);
  if (md == NULL || !EVP_DigestInit_ex(ctx->mdctx, md, NULL)) {
    EVP_MD_free(md);
    return 0;
  }
  {
    const char *canon = EVP_MD_get0_name(md);
    if (canon != NULL) {
      OPENSSL_strlcpy(ctx->mdname, canon, sizeof(ctx->mdname));
    }
  }
  EVP_MD_free(md);
  return zz9k_rsa_sig_set_ctx_params(vctx, params);
}

static int zz9k_rsa_sig_digest_sign_update(void *vctx,
                                           const unsigned char *data,
                                           size_t datalen)
{
  ZZ9K_RSA_SIG_CTX *ctx = vctx;

  if (ctx == NULL || ctx->mdctx == NULL) {
    return 0;
  }
  return EVP_DigestUpdate(ctx->mdctx, data, datalen);
}

static int zz9k_rsa_sig_digest_sign_final(void *vctx, unsigned char *sig,
                                          size_t *siglen, size_t sigsize)
{
  ZZ9K_RSA_SIG_CTX *ctx = vctx;
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digestlen = 0;

  if (ctx == NULL || ctx->mdctx == NULL || siglen == NULL) {
    return 0;
  }
  if (sig == NULL) {
    return zz9k_rsa_sig_maxsize(ctx, siglen);
  }
  if (!EVP_DigestFinal_ex(ctx->mdctx, digest, &digestlen)) {
    return 0;
  }
  return zz9k_rsa_sig_sign_delegated(ctx, sig, siglen, sigsize, digest,
                                     digestlen);
}

static int zz9k_rsa_sig_digest_sign_oneshot(void *vctx, unsigned char *sig,
                                            size_t *siglen, size_t sigsize,
                                            const unsigned char *tbs,
                                            size_t tbslen)
{
  ZZ9K_RSA_SIG_CTX *ctx = vctx;
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digestlen = 0;

  if (ctx == NULL || ctx->mdctx == NULL || siglen == NULL) {
    return 0;
  }
  if (sig == NULL) {
    return zz9k_rsa_sig_maxsize(ctx, siglen);
  }
  if (!EVP_DigestUpdate(ctx->mdctx, tbs, tbslen) ||
      !EVP_DigestFinal_ex(ctx->mdctx, digest, &digestlen)) {
    return 0;
  }
  return zz9k_rsa_sig_sign_delegated(ctx, sig, siglen, sigsize, digest,
                                     digestlen);
}

const OSSL_DISPATCH zz9k_rsa_signature_functions[] = {
  { OSSL_FUNC_SIGNATURE_NEWCTX, (void (*)(void))zz9k_rsa_sig_newctx },
  { OSSL_FUNC_SIGNATURE_FREECTX, (void (*)(void))zz9k_rsa_sig_freectx },
  { OSSL_FUNC_SIGNATURE_VERIFY_INIT,
    (void (*)(void))zz9k_rsa_sig_verify_init },
  { OSSL_FUNC_SIGNATURE_VERIFY, (void (*)(void))zz9k_rsa_sig_verify },
  { OSSL_FUNC_SIGNATURE_SIGN_INIT, (void (*)(void))zz9k_rsa_sig_sign_init },
  { OSSL_FUNC_SIGNATURE_SIGN, (void (*)(void))zz9k_rsa_sig_sign },
  { OSSL_FUNC_SIGNATURE_DIGEST_SIGN_INIT,
    (void (*)(void))zz9k_rsa_sig_digest_sign_init },
  { OSSL_FUNC_SIGNATURE_DIGEST_SIGN_UPDATE,
    (void (*)(void))zz9k_rsa_sig_digest_sign_update },
  { OSSL_FUNC_SIGNATURE_DIGEST_SIGN_FINAL,
    (void (*)(void))zz9k_rsa_sig_digest_sign_final },
  { OSSL_FUNC_SIGNATURE_DIGEST_SIGN,
    (void (*)(void))zz9k_rsa_sig_digest_sign_oneshot },
  { OSSL_FUNC_SIGNATURE_DIGEST_VERIFY_INIT,
    (void (*)(void))zz9k_rsa_sig_digest_verify_init },
  { OSSL_FUNC_SIGNATURE_DIGEST_VERIFY_UPDATE,
    (void (*)(void))zz9k_rsa_sig_digest_verify_update },
  { OSSL_FUNC_SIGNATURE_DIGEST_VERIFY_FINAL,
    (void (*)(void))zz9k_rsa_sig_digest_verify_final },
  { OSSL_FUNC_SIGNATURE_DIGEST_VERIFY,
    (void (*)(void))zz9k_rsa_sig_digest_verify_oneshot },
  { OSSL_FUNC_SIGNATURE_SET_CTX_PARAMS,
    (void (*)(void))zz9k_rsa_sig_set_ctx_params },
  { OSSL_FUNC_SIGNATURE_SETTABLE_CTX_PARAMS,
    (void (*)(void))zz9k_rsa_sig_settable_ctx_params },
  { 0, NULL }
};
/* The SIGNATURE OSSL_ALGORITHM table (ECDSA + RSA) is assembled in
 * zz9k_algorithms.c; the KEYMGMT tables (which now include RSA) are
 * assembled there too. */
