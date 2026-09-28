/*
 * ZZ9000 OpenSSL provider — internal definitions shared across the provider
 * translation units (provider core + per-operation implementations).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_PROV_LOCAL_H
#define ZZ9K_PROV_LOCAL_H

#include <openssl/core.h>
#include <openssl/core_dispatch.h>

#include "zz9k/abi.h"   /* ZZ9K_SERVICE_FLAG_CRYPTO_* for the offload gates */

#ifdef __cplusplus
extern "C" {
#endif

/* Provider-side context. On the Amiga `sdk_ctx` holds an open offload context
 * (zz9k_offload_open, which wraps the SDK board handle plus persistent shared
 * scratch buffers) — but only when the firmware's crypto service responded at
 * provider init — and `service_flags` the crypto service flags it advertised.
 * On the host both stay NULL/0 and everything uses the software reference. */
typedef struct zz9k_prov_ctx_st {
  const OSSL_CORE_HANDLE *handle;
  void *libctx;             /* OSSL_LIB_CTX of the application (via
                               core_get_libctx); used to delegate operations
                               the board does not accelerate to the default
                               provider in the same context */
  void *sdk_ctx;            /* offload context when running on hardware, else NULL */
  unsigned int service_flags;
} ZZ9K_PROV_CTX;

/* Offload gates used by the operation implementations. Every algorithm must
 * check its own ZZ9K_SERVICE_FLAG_CRYPTO_* bit: firmware that implements the
 * crypto service but not a given algorithm either rejects the call (a wasted
 * mailbox round trip per operation) or, for AEAD, silently runs the legacy
 * default algorithm instead (see the AEAD flags-nibble note in zz9k/abi.h).
 * ChaCha20-Poly1305 is that legacy default, available whenever the crypto
 * service itself is, so it gates on the service alone. */
#define ZZ9K_PROV_CAN_OFFLOAD_SERVICE(p) \
  ((p) != NULL && (p)->sdk_ctx != NULL)
#define ZZ9K_PROV_CAN_OFFLOAD(p, flag) \
  (ZZ9K_PROV_CAN_OFFLOAD_SERVICE(p) && ((p)->service_flags & (flag)) != 0U)

/* Hand-rolled OSSL_PARAM construction. The OSSL_PARAM_construct_* helpers
 * return the struct by value, which cannot cross AmiSSL's m68k inline-call
 * boundary in application-side builds (the _amiga out-param variants exist
 * for that); filling the fields directly is equivalent and portable
 * everywhere this code builds. */
#define ZZ9K_PARAM_OCTET(pp, k, d, n)                  \
  do {                                                 \
    (pp)->key = (k);                                   \
    (pp)->data_type = OSSL_PARAM_OCTET_STRING;         \
    (pp)->data = (void *)(d);                          \
    (pp)->data_size = (n);                             \
    (pp)->return_size = OSSL_PARAM_UNMODIFIED;         \
  } while (0)
#define ZZ9K_PARAM_UTF8(pp, k, s)                      \
  do {                                                 \
    (pp)->key = (k);                                   \
    (pp)->data_type = OSSL_PARAM_UTF8_STRING;          \
    (pp)->data = (void *)(s);                          \
    (pp)->data_size = strlen(s);                       \
    (pp)->return_size = OSSL_PARAM_UNMODIFIED;         \
  } while (0)
/* `ip` must be a pointer to an int whose storage outlives the OSSL_PARAM's
 * use (e.g. a local variable or a field in a longer-lived ctx), same caveat
 * as ZZ9K_PARAM_OCTET/UTF8 above. */
#define ZZ9K_PARAM_INT(pp, k, ip)                      \
  do {                                                 \
    (pp)->key = (k);                                   \
    (pp)->data_type = OSSL_PARAM_INTEGER;              \
    (pp)->data = (void *)(ip);                         \
    (pp)->data_size = sizeof(*(ip));                   \
    (pp)->return_size = OSSL_PARAM_UNMODIFIED;         \
  } while (0)
#define ZZ9K_PARAM_END(pp)                             \
  do {                                                 \
    (pp)->key = NULL;                                  \
    (pp)->data_type = 0;                               \
    (pp)->data = NULL;                                 \
    (pp)->data_size = 0;                               \
    (pp)->return_size = 0;                             \
  } while (0)

/* X25519 scalar multiplication used by the key-exchange derive. Routes to the
 * ZZ9000 offload when `provctx` carries a live SDK context with the X25519
 * service flag, otherwise the portable software reference. Returns 1 on
 * success, 0 on failure (e.g. all-zero / small-order result). */
int zz9k_prov_x25519(unsigned char out[32], const unsigned char scalar[32],
                     const unsigned char point[32], ZZ9K_PROV_CTX *provctx);

/* Per-key-type KEYMGMT, per-algorithm KEYEXCH and SIGNATURE dispatch tables,
 * combined into the OSSL_ALGORITHM tables in zz9k_algorithms.c. */
extern const OSSL_DISPATCH zz9k_x25519_keymgmt_functions[];
extern const OSSL_DISPATCH zz9k_ec_keymgmt_functions[];
extern const OSSL_DISPATCH zz9k_rsa_keymgmt_functions[];
extern const OSSL_DISPATCH zz9k_x25519_keyexch_functions[];
extern const OSSL_DISPATCH zz9k_ecdh_keyexch_functions[];
extern const OSSL_DISPATCH zz9k_ecdsa_signature_functions[];
extern const OSSL_DISPATCH zz9k_rsa_signature_functions[];

/* Algorithm tables advertised by the provider's query-operation callback.
 * The KEYMGMT variants let the production Amiga branch advertise any subset
 * of X25519 / EC / RSA (each gated on its own firmware capability flags, see
 * zz9k_provider.c); the full combined table is used when all three apply (or
 * unconditionally on the host / under TEST_ALL). */
extern const OSSL_ALGORITHM zz9k_keymgmt_algorithms[];
extern const OSSL_ALGORITHM zz9k_keymgmt_algorithms_x25519_only[];
extern const OSSL_ALGORITHM zz9k_keymgmt_algorithms_ec_only[];
extern const OSSL_ALGORITHM zz9k_keymgmt_algorithms_rsa_only[];
extern const OSSL_ALGORITHM zz9k_keymgmt_algorithms_x25519_ec[];
extern const OSSL_ALGORITHM zz9k_keymgmt_algorithms_x25519_rsa[];
extern const OSSL_ALGORITHM zz9k_keymgmt_algorithms_ec_rsa[];
extern const OSSL_ALGORITHM zz9k_keyexch_algorithms[];
extern const OSSL_ALGORITHM zz9k_cipher_algorithms[];
extern const OSSL_ALGORITHM zz9k_cipher_algorithms_chacha_only[];
extern const OSSL_ALGORITHM zz9k_signature_algorithms[];

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_PROV_LOCAL_H */
