/*
 * ZZ9000 OpenSSL provider — X25519 key management and key exchange.
 *
 * A minimal KEYMGMT holds a 32-byte X25519 key (private scalar and/or public
 * u-coordinate), and a KEYEXCH performs the ECDH derive. The derive routes
 * through zz9k_prov_x25519(), which offloads to the ZZ9000 when available and
 * otherwise uses the SDK's portable software reference (the same code the
 * firmware was validated against). Public keys are derived from the private
 * scalar on import so key objects are self-consistent.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k_prov_local.h"

#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include "zz9k-crypto-soft.h"
#include "zz9k_offload.h"

#include <string.h>

#define ZZ9K_X25519_KEYLEN 32

/* The Curve25519 base point u = 9. */
static const unsigned char zz9k_x25519_basepoint[ZZ9K_X25519_KEYLEN] = { 9 };

int zz9k_prov_x25519(unsigned char out[32], const unsigned char scalar[32],
                     const unsigned char point[32], ZZ9K_PROV_CTX *provctx)
{
#ifdef ZZ9K_PROVIDER_OFFLOAD
  /* On the Amiga the key exchange always runs on the hardware — X25519 is
   * only advertised when the firmware has its capability flag (see
   * zz9k_query_operation). A failed offload FAILS the operation instead of
   * falling back to the portable software reference: that code is not
   * trusted inside the base-relative amissl.library build (see the AEAD hook
   * for the full story), and a failed handshake beats a silently wrong one.
   * On the host ZZ9K_PROVIDER_OFFLOAD is undefined and the software
   * reference below is what the parity tests cover. */
  if (!ZZ9K_PROV_CAN_OFFLOAD(provctx, ZZ9K_SERVICE_FLAG_CRYPTO_X25519)) {
    return 0;
  }
  return zz9k_offload_x25519(provctx->sdk_ctx, out, scalar, point) > 0 ? 1
                                                                       : 0;
#else
  (void)provctx;
  return zz9k_soft_x25519(out, scalar, point);
#endif
}

/* ---- KEYMGMT ---- */

typedef struct {
  ZZ9K_PROV_CTX *provctx;
  unsigned char priv[ZZ9K_X25519_KEYLEN];
  unsigned char pub[ZZ9K_X25519_KEYLEN];
  int has_priv;
  int has_pub;
} ZZ9K_X25519_KEY;

static void *zz9k_x25519_keymgmt_new(void *provctx)
{
  ZZ9K_X25519_KEY *key = OPENSSL_zalloc(sizeof(*key));
  if (key != NULL) {
    key->provctx = provctx;
  }
  return key;
}

static void zz9k_x25519_keymgmt_free(void *keydata)
{
  if (keydata != NULL) {
    OPENSSL_cleanse(keydata, sizeof(ZZ9K_X25519_KEY));
    OPENSSL_free(keydata);
  }
}

static int zz9k_x25519_keymgmt_has(const void *keydata, int selection)
{
  const ZZ9K_X25519_KEY *key = keydata;
  int ok = 1;

  if (key == NULL) {
    return 0;
  }
  if ((selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) != 0) {
    ok = ok && key->has_pub;
  }
  if ((selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) != 0) {
    ok = ok && key->has_priv;
  }
  return ok;
}

static int zz9k_x25519_keymgmt_import(void *keydata, int selection,
                                      const OSSL_PARAM params[])
{
  ZZ9K_X25519_KEY *key = keydata;
  const OSSL_PARAM *p;
  int imported = 0;

  if (key == NULL) {
    return 0;
  }
  if ((selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) != 0) {
    p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_PRIV_KEY);
    if (p != NULL) {
      void *out = key->priv;
      size_t len = 0;
      if (!OSSL_PARAM_get_octet_string(p, &out, ZZ9K_X25519_KEYLEN, &len) ||
          len != ZZ9K_X25519_KEYLEN) {
        return 0;
      }
      key->has_priv = 1;
      /* Derive the public key so the object is a self-consistent keypair. */
      if (zz9k_prov_x25519(key->pub, key->priv, zz9k_x25519_basepoint,
                           key->provctx)) {
        key->has_pub = 1;
      }
      imported = 1;
    }
  }
  if ((selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) != 0) {
    p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_PUB_KEY);
    if (p != NULL) {
      void *out = key->pub;
      size_t len = 0;
      if (!OSSL_PARAM_get_octet_string(p, &out, ZZ9K_X25519_KEYLEN, &len) ||
          len != ZZ9K_X25519_KEYLEN) {
        return 0;
      }
      key->has_pub = 1;
      imported = 1;
    }
  }
  return imported;
}

static const OSSL_PARAM *zz9k_x25519_keymgmt_import_types(int selection)
{
  static const OSSL_PARAM types[] = {
    OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PRIV_KEY, NULL, 0),
    OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PUB_KEY, NULL, 0),
    OSSL_PARAM_END
  };
  if ((selection & OSSL_KEYMGMT_SELECT_KEYPAIR) != 0) {
    return types;
  }
  return NULL;
}

static int zz9k_x25519_keymgmt_get_params(void *keydata, OSSL_PARAM params[])
{
  ZZ9K_X25519_KEY *key = keydata;
  OSSL_PARAM *p;

  p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_BITS);
  if (p != NULL && !OSSL_PARAM_set_int(p, 253)) {
    return 0;
  }
  p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_SECURITY_BITS);
  if (p != NULL && !OSSL_PARAM_set_int(p, 128)) {
    return 0;
  }
  p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_MAX_SIZE);
  if (p != NULL && !OSSL_PARAM_set_int(p, ZZ9K_X25519_KEYLEN)) {
    return 0;
  }
  /* libssl serializes the key share via ENCODED_PUBLIC_KEY (raw u-coord). */
  p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY);
  if (p != NULL) {
    if (!key->has_pub ||
        !OSSL_PARAM_set_octet_string(p, key->pub, ZZ9K_X25519_KEYLEN)) {
      return 0;
    }
  }
  p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_PUB_KEY);
  if (p != NULL) {
    if (!key->has_pub ||
        !OSSL_PARAM_set_octet_string(p, key->pub, ZZ9K_X25519_KEYLEN)) {
      return 0;
    }
  }
  p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_PRIV_KEY);
  if (p != NULL) {
    if (!key->has_priv ||
        !OSSL_PARAM_set_octet_string(p, key->priv, ZZ9K_X25519_KEYLEN)) {
      return 0;
    }
  }
  return 1;
}

static const OSSL_PARAM *zz9k_x25519_keymgmt_gettable_params(void *provctx)
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

/* libssl installs the peer key share with EVP_PKEY_set1_encoded_public_key. */
static int zz9k_x25519_keymgmt_set_params(void *keydata,
                                          const OSSL_PARAM params[])
{
  ZZ9K_X25519_KEY *key = keydata;
  const OSSL_PARAM *p;

  if (key == NULL) {
    return 0;
  }
  p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY);
  if (p != NULL) {
    if (p->data_type != OSSL_PARAM_OCTET_STRING ||
        p->data_size != ZZ9K_X25519_KEYLEN) {
      return 0;
    }
    memcpy(key->pub, p->data, ZZ9K_X25519_KEYLEN);
    key->has_pub = 1;
  }
  return 1;
}

static const OSSL_PARAM *zz9k_x25519_keymgmt_settable_params(void *provctx)
{
  static const OSSL_PARAM types[] = {
    OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, NULL, 0),
    OSSL_PARAM_END
  };
  (void)provctx;
  return types;
}

static int zz9k_x25519_keymgmt_match(const void *keydata1, const void *keydata2,
                                     int selection)
{
  const ZZ9K_X25519_KEY *a = keydata1;
  const ZZ9K_X25519_KEY *b = keydata2;

  if (a == NULL || b == NULL) {
    return 0;
  }
  if ((selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) != 0) {
    if (!a->has_pub || !b->has_pub ||
        memcmp(a->pub, b->pub, ZZ9K_X25519_KEYLEN) != 0) {
      return 0;
    }
  }
  if ((selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) != 0) {
    if (!a->has_priv || !b->has_priv ||
        memcmp(a->priv, b->priv, ZZ9K_X25519_KEYLEN) != 0) {
      return 0;
    }
  }
  return 1;
}

static void *zz9k_x25519_keymgmt_dup(const void *keydata_from, int selection)
{
  const ZZ9K_X25519_KEY *from = keydata_from;
  ZZ9K_X25519_KEY *to;

  if (from == NULL) {
    return NULL;
  }
  to = OPENSSL_zalloc(sizeof(*to));
  if (to == NULL) {
    return NULL;
  }
  to->provctx = from->provctx;
  if ((selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) != 0 && from->has_pub) {
    memcpy(to->pub, from->pub, ZZ9K_X25519_KEYLEN);
    to->has_pub = 1;
  }
  if ((selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) != 0 && from->has_priv) {
    memcpy(to->priv, from->priv, ZZ9K_X25519_KEYLEN);
    to->has_priv = 1;
  }
  return to;
}

static int zz9k_x25519_keymgmt_export(void *keydata, int selection,
                                      OSSL_CALLBACK *param_cb, void *cbarg)
{
  ZZ9K_X25519_KEY *key = keydata;
  OSSL_PARAM params[3];
  int n = 0;

  if (key == NULL) {
    return 0;
  }
  if ((selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) != 0 && key->has_pub) {
    ZZ9K_PARAM_OCTET(&params[n], OSSL_PKEY_PARAM_PUB_KEY, key->pub,
                     ZZ9K_X25519_KEYLEN);
    n++;
  }
  if ((selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) != 0 && key->has_priv) {
    ZZ9K_PARAM_OCTET(&params[n], OSSL_PKEY_PARAM_PRIV_KEY, key->priv,
                     ZZ9K_X25519_KEYLEN);
    n++;
  }
  if (n == 0) {
    return 0;
  }
  ZZ9K_PARAM_END(&params[n]);
  return param_cb(params, cbarg);
}

static const OSSL_PARAM *zz9k_x25519_keymgmt_export_types(int selection)
{
  return zz9k_x25519_keymgmt_import_types(selection);
}

/* ---- key generation (the TLS key share) ---- */

typedef struct {
  ZZ9K_PROV_CTX *provctx;
  int selection;
} ZZ9K_X25519_GEN;

static void *zz9k_x25519_gen_init(void *provctx, int selection,
                                  const OSSL_PARAM params[])
{
  ZZ9K_X25519_GEN *gen;

  (void)params;
  if ((selection & (OSSL_KEYMGMT_SELECT_KEYPAIR |
                    OSSL_KEYMGMT_SELECT_ALL_PARAMETERS)) == 0) {
    return NULL;
  }
  gen = OPENSSL_zalloc(sizeof(*gen));
  if (gen != NULL) {
    gen->provctx = provctx;
    gen->selection = selection;
  }
  return gen;
}

/* libssl calls EVP_PKEY_CTX_set_group_name(ctx, "X25519") on the keygen
 * context (ssl_generate_pkey_group). Like the default provider we accept the
 * group name as long as it is our one algorithm, and ignore the optional KDF
 * property / DHKEM IKM params. Without this the TLS key-share generation
 * fails outright. */
static int zz9k_x25519_gen_set_params(void *genctx, const OSSL_PARAM params[])
{
  const OSSL_PARAM *p;

  (void)genctx;
  if (params == NULL) {
    return 1;
  }
  p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_GROUP_NAME);
  if (p != NULL) {
    if (p->data_type != OSSL_PARAM_UTF8_STRING || p->data == NULL ||
        OPENSSL_strcasecmp(p->data, "x25519") != 0) {
      return 0;
    }
  }
  return 1;
}

static const OSSL_PARAM *zz9k_x25519_gen_settable_params(void *genctx,
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

static void *zz9k_x25519_gen(void *genctx, OSSL_CALLBACK *cb, void *cbarg)
{
  ZZ9K_X25519_GEN *gen = genctx;
  ZZ9K_X25519_KEY *key;

  (void)cb;
  (void)cbarg;
  if (gen == NULL) {
    return NULL;
  }
  key = OPENSSL_zalloc(sizeof(*key));
  if (key == NULL) {
    return NULL;
  }
  key->provctx = gen->provctx;
  /* TLS 1.2 creates a parameter-only X25519 object for the server's
   * ephemeral key before installing its encoded public key via set_params().
   * X25519 has no explicit domain parameters, so the correct representation
   * is a blank provider key object, matching OpenSSL's default provider. */
  if ((gen->selection & OSSL_KEYMGMT_SELECT_KEYPAIR) == 0) {
    return key;
  }
  /* Raw 32 random bytes; RFC 7748 clamping happens inside the scalar
   * multiplication, matching how OpenSSL stores X25519 private keys. */
  if (RAND_priv_bytes(key->priv, ZZ9K_X25519_KEYLEN) <= 0 ||
      !zz9k_prov_x25519(key->pub, key->priv, zz9k_x25519_basepoint,
                        key->provctx)) {
    OPENSSL_clear_free(key, sizeof(*key));
    return NULL;
  }
  key->has_priv = 1;
  key->has_pub = 1;
  return key;
}

static void zz9k_x25519_gen_cleanup(void *genctx)
{
  OPENSSL_free(genctx);
}

static const char *zz9k_x25519_keymgmt_query_operation_name(int operation_id)
{
  if (operation_id == OSSL_OP_KEYEXCH) {
    return "X25519";
  }
  return NULL;
}

const OSSL_DISPATCH zz9k_x25519_keymgmt_functions[] = {
  { OSSL_FUNC_KEYMGMT_NEW, (void (*)(void))zz9k_x25519_keymgmt_new },
  { OSSL_FUNC_KEYMGMT_FREE, (void (*)(void))zz9k_x25519_keymgmt_free },
  { OSSL_FUNC_KEYMGMT_HAS, (void (*)(void))zz9k_x25519_keymgmt_has },
  { OSSL_FUNC_KEYMGMT_IMPORT, (void (*)(void))zz9k_x25519_keymgmt_import },
  { OSSL_FUNC_KEYMGMT_IMPORT_TYPES,
    (void (*)(void))zz9k_x25519_keymgmt_import_types },
  { OSSL_FUNC_KEYMGMT_EXPORT, (void (*)(void))zz9k_x25519_keymgmt_export },
  { OSSL_FUNC_KEYMGMT_EXPORT_TYPES,
    (void (*)(void))zz9k_x25519_keymgmt_export_types },
  { OSSL_FUNC_KEYMGMT_GET_PARAMS,
    (void (*)(void))zz9k_x25519_keymgmt_get_params },
  { OSSL_FUNC_KEYMGMT_GETTABLE_PARAMS,
    (void (*)(void))zz9k_x25519_keymgmt_gettable_params },
  { OSSL_FUNC_KEYMGMT_SET_PARAMS,
    (void (*)(void))zz9k_x25519_keymgmt_set_params },
  { OSSL_FUNC_KEYMGMT_SETTABLE_PARAMS,
    (void (*)(void))zz9k_x25519_keymgmt_settable_params },
  { OSSL_FUNC_KEYMGMT_MATCH, (void (*)(void))zz9k_x25519_keymgmt_match },
  { OSSL_FUNC_KEYMGMT_DUP, (void (*)(void))zz9k_x25519_keymgmt_dup },
  { OSSL_FUNC_KEYMGMT_GEN_INIT, (void (*)(void))zz9k_x25519_gen_init },
  { OSSL_FUNC_KEYMGMT_GEN_SET_PARAMS,
    (void (*)(void))zz9k_x25519_gen_set_params },
  { OSSL_FUNC_KEYMGMT_GEN_SETTABLE_PARAMS,
    (void (*)(void))zz9k_x25519_gen_settable_params },
  { OSSL_FUNC_KEYMGMT_GEN, (void (*)(void))zz9k_x25519_gen },
  { OSSL_FUNC_KEYMGMT_GEN_CLEANUP,
    (void (*)(void))zz9k_x25519_gen_cleanup },
  { OSSL_FUNC_KEYMGMT_QUERY_OPERATION_NAME,
    (void (*)(void))zz9k_x25519_keymgmt_query_operation_name },
  { 0, NULL }
};

/* ---- KEYEXCH ---- */

typedef struct {
  ZZ9K_PROV_CTX *provctx;
  ZZ9K_X25519_KEY *key;   /* our key (provides the private scalar) */
  ZZ9K_X25519_KEY *peer;  /* peer key (provides the public point) */
} ZZ9K_X25519_EXCH;

static void *zz9k_x25519_exch_newctx(void *provctx)
{
  ZZ9K_X25519_EXCH *ctx = OPENSSL_zalloc(sizeof(*ctx));
  if (ctx != NULL) {
    ctx->provctx = provctx;
  }
  return ctx;
}

static int zz9k_x25519_exch_init(void *vctx, void *provkey,
                                 const OSSL_PARAM params[])
{
  ZZ9K_X25519_EXCH *ctx = vctx;

  (void)params;
  if (ctx == NULL || provkey == NULL) {
    return 0;
  }
  ctx->key = provkey;
  return 1;
}

static int zz9k_x25519_exch_set_peer(void *vctx, void *provkey)
{
  ZZ9K_X25519_EXCH *ctx = vctx;

  if (ctx == NULL || provkey == NULL) {
    return 0;
  }
  ctx->peer = provkey;
  return 1;
}

static int zz9k_x25519_exch_derive(void *vctx, unsigned char *secret,
                                   size_t *secretlen, size_t outlen)
{
  ZZ9K_X25519_EXCH *ctx = vctx;

  if (ctx == NULL || ctx->key == NULL || ctx->peer == NULL) {
    return 0;
  }
  if (!ctx->key->has_priv || !ctx->peer->has_pub) {
    return 0;
  }
  if (secret == NULL) {
    *secretlen = ZZ9K_X25519_KEYLEN;
    return 1;
  }
  if (outlen < ZZ9K_X25519_KEYLEN) {
    return 0;
  }
  if (!zz9k_prov_x25519(secret, ctx->key->priv, ctx->peer->pub, ctx->provctx)) {
    return 0;
  }
  *secretlen = ZZ9K_X25519_KEYLEN;
  return 1;
}

static void zz9k_x25519_exch_freectx(void *vctx)
{
  OPENSSL_free(vctx);
}

/* Non-static: the KEYEXCH OSSL_ALGORITHM table (X25519 + ECDH) is assembled
 * centrally in zz9k_algorithms.c since it spans two key-exchange
 * implementations, same as the KEYMGMT table. */
const OSSL_DISPATCH zz9k_x25519_keyexch_functions[] = {
  { OSSL_FUNC_KEYEXCH_NEWCTX, (void (*)(void))zz9k_x25519_exch_newctx },
  { OSSL_FUNC_KEYEXCH_INIT, (void (*)(void))zz9k_x25519_exch_init },
  { OSSL_FUNC_KEYEXCH_SET_PEER, (void (*)(void))zz9k_x25519_exch_set_peer },
  { OSSL_FUNC_KEYEXCH_DERIVE, (void (*)(void))zz9k_x25519_exch_derive },
  { OSSL_FUNC_KEYEXCH_FREECTX, (void (*)(void))zz9k_x25519_exch_freectx },
  { 0, NULL }
};
