/*
 * ZZ9000 OpenSSL 3.x provider — public entry point.
 *
 * A built-in (no-DSO) provider that routes selected crypto operations to the
 * ZZ9000 coprocessor via the SDK, with software fallback. It is registered
 * with an OpenSSL library context through OSSL_PROVIDER_add_builtin():
 *
 *   OSSL_PROVIDER_add_builtin(libctx, "zz9000", zz9k_provider_init);
 *   OSSL_PROVIDER_load(libctx, "zz9000");
 *   EVP_set_default_properties(libctx, "?provider=zz9000");
 *
 * The same source builds against host OpenSSL 3 (for the unit tests) and
 * against AmiSSL 5.x (OpenSSL 3.6) on the Amiga, which exports the provider
 * registration API through amisslext_lib.fd.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_PROVIDER_H
#define ZZ9K_PROVIDER_H

#include <openssl/core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZZ9K_PROVIDER_NAME "zz9000"

/* Built-in provider init function passed to OSSL_PROVIDER_add_builtin(). */
OSSL_provider_init_fn zz9k_provider_init;

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_PROVIDER_H */
