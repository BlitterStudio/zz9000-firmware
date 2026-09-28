/*
 * ZZ9000 OpenSSL provider — in-library (built into AmiSSL) registration.
 *
 * This is the entry point used when the provider is compiled *inside*
 * amissl.library rather than registered by an application. AmiSSL calls
 * zz9k_provider_register_builtin() once per application during InitAmiSSL, so
 * every program that opens the library transparently gets ZZ9000 offload with
 * no source changes of its own. Unlike zz9k_amissl.c (the application-side
 * helper, which calls OpenSSL through the AmiSSL inline redirection), this unit
 * is part of the OpenSSL build and calls the core directly.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_AMISSL_BUILTIN_H
#define ZZ9K_AMISSL_BUILTIN_H

#include <openssl/types.h>   /* OSSL_LIB_CTX */

#ifdef __cplusplus
extern "C" {
#endif

/* Register and load the ZZ9000 provider in `libctx` (NULL = the application's
 * default context) and set its default property query to "?provider=zz9000".
 * The default provider is also loaded so non-offloaded operations fall back to
 * AmiSSL's software implementation. Returns 1 on success, 0 on failure (the
 * application then simply runs with AmiSSL's software crypto). Safe to call
 * more than once for the same context. */
int zz9k_provider_register_builtin(OSSL_LIB_CTX *libctx);

#ifdef __cplusplus
}
#endif

#endif /* ZZ9K_AMISSL_BUILTIN_H */
