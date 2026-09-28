/*
 * ZZ9000 OpenSSL 3.x provider — skeleton.
 *
 * This first cut implements only the provider-level dispatch (parameters,
 * teardown, and an empty operation query). It registers and loads cleanly but
 * advertises no algorithms yet; the key-exchange, cipher and signature
 * operations are layered on in later steps. Keeping this stage isolated lets
 * the registration boundary be validated on its own against host OpenSSL 3,
 * which is the same provider API AmiSSL 5.x (OpenSSL 3.6) exposes on the Amiga.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k_provider.h"
#include "zz9k_prov_local.h"

#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/crypto.h>
#include <openssl/prov_ssl.h>   /* TLS1_VERSION for the TLS-GROUP capability */

#include <string.h>

#ifdef ZZ9K_PROVIDER_OFFLOAD
#include "zz9k_offload.h"
#endif

#define ZZ9K_PROVIDER_VERSION "1.0.0"

static const OSSL_PARAM zz9k_param_types[] = {
  OSSL_PARAM_DEFN(OSSL_PROV_PARAM_NAME, OSSL_PARAM_UTF8_PTR, NULL, 0),
  OSSL_PARAM_DEFN(OSSL_PROV_PARAM_VERSION, OSSL_PARAM_UTF8_PTR, NULL, 0),
  OSSL_PARAM_DEFN(OSSL_PROV_PARAM_BUILDINFO, OSSL_PARAM_UTF8_PTR, NULL, 0),
  OSSL_PARAM_DEFN(OSSL_PROV_PARAM_STATUS, OSSL_PARAM_INTEGER, NULL, 0),
  OSSL_PARAM_END
};

static const OSSL_PARAM *zz9k_gettable_params(void *provctx)
{
  (void)provctx;
  return zz9k_param_types;
}

static int zz9k_get_params(void *provctx, OSSL_PARAM params[])
{
  OSSL_PARAM *p;

  (void)provctx;
  p = OSSL_PARAM_locate(params, OSSL_PROV_PARAM_NAME);
  if (p != NULL &&
      !OSSL_PARAM_set_utf8_ptr(p, "ZZ9000 crypto offload provider")) {
    return 0;
  }
  p = OSSL_PARAM_locate(params, OSSL_PROV_PARAM_VERSION);
  if (p != NULL && !OSSL_PARAM_set_utf8_ptr(p, ZZ9K_PROVIDER_VERSION)) {
    return 0;
  }
  p = OSSL_PARAM_locate(params, OSSL_PROV_PARAM_BUILDINFO);
  if (p != NULL && !OSSL_PARAM_set_utf8_ptr(p, ZZ9K_PROVIDER_VERSION)) {
    return 0;
  }
  p = OSSL_PARAM_locate(params, OSSL_PROV_PARAM_STATUS);
  if (p != NULL && !OSSL_PARAM_set_int(p, 1)) {   /* 1 = operational */
    return 0;
  }
  return 1;
}

/* Advertise the operations the provider implements. Anything not listed here
 * falls through to the default (software) provider via the "?provider=zz9000"
 * property query. */
static const OSSL_ALGORITHM *zz9k_query_operation(void *provctx,
                                                  int operation_id,
                                                  int *no_store)
{
  (void)provctx;
  *no_store = 0;
#if defined(ZZ9K_PROVIDER_OFFLOAD) && !defined(ZZ9K_PROVIDER_TEST_ALL)
  /* On the Amiga, algorithms are advertised only when the board can actually
   * serve them; everything else falls through to AmiSSL's default provider at
   * FETCH time, so the provider's portable software paths never execute
   * inside amissl.library. That is deliberate: the in-library (gcc 2.95,
   * base-relative) build of the software ChaCha tag path computes wrong tags
   * on real hardware even though the same code passes its KATs in every
   * standalone build — root cause undetermined — and a software fallback
   * would be no faster than the default provider anyway. Host builds keep
   * every table unconditional so the parity tests cover the software code. */
  {
    ZZ9K_PROV_CTX *ctx = (ZZ9K_PROV_CTX *)provctx;
    int have_board = (ctx != NULL && ctx->sdk_ctx != NULL);
    /* EC is gated on ECDSA-P256 verify OR P-256 keygen, independent of
     * X25519: either capability alone makes owning the "EC" keytype (P-256
     * accelerated, everything else delegated — see zz9k_ecdsa.c) worthwhile,
     * and the delegation model means an EC key never regresses even when
     * only one of the two is present (e.g. old firmware: derive but no
     * keygen, so ECDHE keygen delegates while cert verify still offloads). */
    int want_x25519 = have_board &&
        (ctx->service_flags & ZZ9K_SERVICE_FLAG_CRYPTO_X25519) != 0U;
    int want_ec = have_board &&
        (ctx->service_flags & (ZZ9K_SERVICE_FLAG_CRYPTO_ECDSA_P256 |
                               ZZ9K_SERVICE_FLAG_CRYPTO_P256_KEYGEN)) != 0U;
    int want_rsa = have_board &&
        (ctx->service_flags & ZZ9K_SERVICE_FLAG_CRYPTO_RSA_2048) != 0U;

    switch (operation_id) {
    case OSSL_OP_KEYMGMT:
      /* Any of the three keytypes may be independently present or absent
       * depending on the firmware's advertised capability flags, so every
       * non-empty subset needs its own table (zz9k_algorithms.c) — the RSA
       * KEYMGMT is what makes an RSA key ever belong to this provider at
       * all, which is what the RSA entry in zz9k_signature_algorithms below
       * relies on to stay inert when RSA_2048 is absent. */
      if (want_x25519 && want_ec && want_rsa) {
        return zz9k_keymgmt_algorithms;
      }
      if (want_x25519 && want_ec) {
        return zz9k_keymgmt_algorithms_x25519_ec;
      }
      if (want_x25519 && want_rsa) {
        return zz9k_keymgmt_algorithms_x25519_rsa;
      }
      if (want_ec && want_rsa) {
        return zz9k_keymgmt_algorithms_ec_rsa;
      }
      if (want_x25519) {
        return zz9k_keymgmt_algorithms_x25519_only;
      }
      if (want_ec) {
        return zz9k_keymgmt_algorithms_ec_only;
      }
      if (want_rsa) {
        return zz9k_keymgmt_algorithms_rsa_only;
      }
      return NULL;
    case OSSL_OP_KEYEXCH:
      /* The combined table holds both X25519 and ECDH; each is only ever
       * reachable through a key created by its own (independently gated)
       * KEYMGMT, so returning the whole table when either capability
       * applies is safe — see the comment on zz9k_keyexch_algorithms.
       * (RSA has no KEYEXCH — verify-only.) */
      return (want_x25519 || want_ec) ? zz9k_keyexch_algorithms : NULL;
    case OSSL_OP_CIPHER:
      if (!have_board) {
        return NULL;
      }
      /* ChaCha20-Poly1305 is the firmware's legacy AEAD default (available
       * whenever the crypto service is); AES-GCM additionally needs its
       * capability flag. */
      return ((ctx->service_flags & ZZ9K_SERVICE_FLAG_CRYPTO_AES_GCM) != 0U)
                 ? zz9k_cipher_algorithms
                 : zz9k_cipher_algorithms_chacha_only;
    case OSSL_OP_SIGNATURE:
      /* ECDSA verify gates specifically on ECDSA_P256 (not P256_KEYGEN —
       * that flag only buys accelerated ECDHE keygen, not verify), RSA verify
       * on RSA_2048; since they share one table (zz9k_algorithms.c), return
       * it whenever EITHER applies — the entry for the absent one is
       * unreachable (no key of that type was ever created under this
       * provider, see the KEYMGMT gating above), not unsafe. */
      return (have_board &&
              (ctx->service_flags & (ZZ9K_SERVICE_FLAG_CRYPTO_ECDSA_P256 |
                                     ZZ9K_SERVICE_FLAG_CRYPTO_RSA_2048)) != 0U)
                 ? zz9k_signature_algorithms
                 : NULL;
    default:
      return NULL;
    }
  }
#else
  switch (operation_id) {
  case OSSL_OP_KEYMGMT:
    return zz9k_keymgmt_algorithms;
  case OSSL_OP_KEYEXCH:
    return zz9k_keyexch_algorithms;
  case OSSL_OP_CIPHER:
    return zz9k_cipher_algorithms;
  case OSSL_OP_SIGNATURE:
    /* ECDSA is always present (see zz9k_algorithms.c); RSA is added only
     * under ZZ9K_PROVIDER_TEST_ALL for the host cross-provider verify test
     * (no delegating RSA keymgmt yet — Phase C). */
    return zz9k_signature_algorithms;
  default:
    return NULL;
  }
#endif
}

/* TLS-GROUP capabilities. libssl only keeps a TLS group when the keymgmt
 * fetch for the group's algorithm resolves to the SAME provider that declared
 * the group — and with the "?provider=zz9000" default property query, fetches
 * for the key types we advertise resolve to us. So every group whose key type
 * this provider shadows must be declared here too, or it silently vanishes
 * from the application's group list (no X25519 key shares at all). Values
 * mirror the default provider's table. */
static const unsigned int zz9k_group_id_x25519 = 29;           /* 0x001d */
static const unsigned int zz9k_group_secbits_x25519 = 128;
static const int zz9k_group_min_tls = TLS1_VERSION;
static const int zz9k_group_max_tls = 0;                       /* no max */
static const int zz9k_group_no_dtls = -1;                      /* disabled */
static const int zz9k_group_is_not_kem = 0;

static const OSSL_PARAM zz9k_tls_group_x25519[] = {
  OSSL_PARAM_utf8_string(OSSL_CAPABILITY_TLS_GROUP_NAME, "x25519", 7),
  OSSL_PARAM_utf8_string(OSSL_CAPABILITY_TLS_GROUP_NAME_INTERNAL, "X25519", 7),
  OSSL_PARAM_utf8_string(OSSL_CAPABILITY_TLS_GROUP_ALG, "X25519", 7),
  OSSL_PARAM_uint(OSSL_CAPABILITY_TLS_GROUP_ID,
                  (unsigned int *)&zz9k_group_id_x25519),
  OSSL_PARAM_uint(OSSL_CAPABILITY_TLS_GROUP_SECURITY_BITS,
                  (unsigned int *)&zz9k_group_secbits_x25519),
  OSSL_PARAM_int(OSSL_CAPABILITY_TLS_GROUP_MIN_TLS,
                 (int *)&zz9k_group_min_tls),
  OSSL_PARAM_int(OSSL_CAPABILITY_TLS_GROUP_MAX_TLS,
                 (int *)&zz9k_group_max_tls),
  OSSL_PARAM_int(OSSL_CAPABILITY_TLS_GROUP_MIN_DTLS,
                 (int *)&zz9k_group_no_dtls),
  OSSL_PARAM_int(OSSL_CAPABILITY_TLS_GROUP_MAX_DTLS,
                 (int *)&zz9k_group_no_dtls),
  OSSL_PARAM_int(OSSL_CAPABILITY_TLS_GROUP_IS_KEM,
                 (int *)&zz9k_group_is_not_kem),
  OSSL_PARAM_END
};

/* secp256r1 (P-256). Only ever declared alongside the EC KEYMGMT: owning the
 * group without owning at least P-256 ECDSA verify or P-256 keygen would be
 * pointless (see zz9k_query_operation's want_ec). */
static const unsigned int zz9k_group_id_secp256r1 = 23;        /* 0x0017 */
static const unsigned int zz9k_group_secbits_secp256r1 = 128;

static const OSSL_PARAM zz9k_tls_group_secp256r1[] = {
  /* "P-256" (not "secp256r1"): this is the name the default provider itself
   * advertises for group id 23, and once our capability entry is the one
   * that wins for that id, it is the only name libssl's group-name lookup
   * (SSL_CTX_set1_groups_list / gid_cb) will accept. */
  OSSL_PARAM_utf8_string(OSSL_CAPABILITY_TLS_GROUP_NAME, "P-256", 6),
  OSSL_PARAM_utf8_string(OSSL_CAPABILITY_TLS_GROUP_NAME_INTERNAL,
                         "prime256v1", 11),
  OSSL_PARAM_utf8_string(OSSL_CAPABILITY_TLS_GROUP_ALG, "EC", 3),
  OSSL_PARAM_uint(OSSL_CAPABILITY_TLS_GROUP_ID,
                  (unsigned int *)&zz9k_group_id_secp256r1),
  OSSL_PARAM_uint(OSSL_CAPABILITY_TLS_GROUP_SECURITY_BITS,
                  (unsigned int *)&zz9k_group_secbits_secp256r1),
  OSSL_PARAM_int(OSSL_CAPABILITY_TLS_GROUP_MIN_TLS,
                 (int *)&zz9k_group_min_tls),
  OSSL_PARAM_int(OSSL_CAPABILITY_TLS_GROUP_MAX_TLS,
                 (int *)&zz9k_group_max_tls),
  OSSL_PARAM_int(OSSL_CAPABILITY_TLS_GROUP_MIN_DTLS,
                 (int *)&zz9k_group_no_dtls),
  OSSL_PARAM_int(OSSL_CAPABILITY_TLS_GROUP_MAX_DTLS,
                 (int *)&zz9k_group_no_dtls),
  OSSL_PARAM_int(OSSL_CAPABILITY_TLS_GROUP_IS_KEM,
                 (int *)&zz9k_group_is_not_kem),
  OSSL_PARAM_END
};

static int zz9k_get_capabilities(void *provctx, const char *capability,
                                 OSSL_CALLBACK *cb, void *arg)
{
  if (strcmp(capability, "TLS-GROUP") != 0) {
    return 1;   /* unknown capability: nothing to add, not an error */
  }
  if (!cb(zz9k_tls_group_x25519, arg)) {
    return 0;
  }
#if defined(ZZ9K_PROVIDER_OFFLOAD) && !defined(ZZ9K_PROVIDER_TEST_ALL)
  {
    ZZ9K_PROV_CTX *ctx = (ZZ9K_PROV_CTX *)provctx;
    int have_board = (ctx != NULL && ctx->sdk_ctx != NULL);
    int want_ec = have_board &&
        (ctx->service_flags & (ZZ9K_SERVICE_FLAG_CRYPTO_ECDSA_P256 |
                               ZZ9K_SERVICE_FLAG_CRYPTO_P256_KEYGEN)) != 0U;

    if (want_ec && !cb(zz9k_tls_group_secp256r1, arg)) {
      return 0;
    }
  }
#else
  /* Host / ZZ9K_PROVIDER_TEST_ALL: EC keymgmt is unconditionally present
   * (see zz9k_query_operation's #else branch), so the group always applies. */
  (void)provctx;
  if (!cb(zz9k_tls_group_secp256r1, arg)) {
    return 0;
  }
#endif
  return 1;
}

static void zz9k_teardown(void *provctx)
{
#ifdef ZZ9K_PROVIDER_OFFLOAD
  ZZ9K_PROV_CTX *ctx = (ZZ9K_PROV_CTX *)provctx;
  if (ctx != NULL && ctx->sdk_ctx != NULL) {
    zz9k_offload_close(ctx->sdk_ctx);
    ctx->sdk_ctx = NULL;
  }
#endif
  OPENSSL_free(provctx);
}

static const OSSL_DISPATCH zz9k_dispatch_table[] = {
  { OSSL_FUNC_PROVIDER_TEARDOWN, (void (*)(void))zz9k_teardown },
  { OSSL_FUNC_PROVIDER_GETTABLE_PARAMS, (void (*)(void))zz9k_gettable_params },
  { OSSL_FUNC_PROVIDER_GET_PARAMS, (void (*)(void))zz9k_get_params },
  { OSSL_FUNC_PROVIDER_QUERY_OPERATION, (void (*)(void))zz9k_query_operation },
  { OSSL_FUNC_PROVIDER_GET_CAPABILITIES,
    (void (*)(void))zz9k_get_capabilities },
  { 0, NULL }   /* OSSL_DISPATCH terminator (portable across OpenSSL 3.0-3.6) */
};

int zz9k_provider_init(const OSSL_CORE_HANDLE *handle, const OSSL_DISPATCH *in,
                       const OSSL_DISPATCH **out, void **provctx)
{
  ZZ9K_PROV_CTX *ctx;

  ctx = OPENSSL_zalloc(sizeof(*ctx));
  if (ctx == NULL) {
    return 0;
  }
  ctx->handle = handle;
  /* The application's library context, used to reach the default provider
   * when an operation must be delegated. For a built-in (non-FIPS) provider
   * the core context IS the library context. */
  for (; in != NULL && in->function_id != 0; in++) {
    if (in->function_id == OSSL_FUNC_CORE_GET_LIBCTX) {
      OSSL_FUNC_core_get_libctx_fn *get_libctx =
          OSSL_FUNC_core_get_libctx(in);
      ctx->libctx = (void *)get_libctx(handle);
      break;
    }
  }
#ifdef ZZ9K_PROVIDER_OFFLOAD
  /* Open the ZZ9000 once for the provider's lifetime and remember which crypto
   * services the firmware advertises. zz9k_offload_open returns NULL when the
   * board is absent or its firmware lacks the crypto service, in which case the
   * provider still loads and every operation transparently uses its software
   * reference. The per-op hooks additionally gate each algorithm on its
   * ZZ9K_SERVICE_FLAG_CRYPTO_* bit (see zz9k_prov_local.h). */
  ctx->sdk_ctx = zz9k_offload_open(&ctx->service_flags);
#endif
  *provctx = ctx;
  *out = zz9k_dispatch_table;
  return 1;
}
