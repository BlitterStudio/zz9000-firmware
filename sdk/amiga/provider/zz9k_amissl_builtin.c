/*
 * ZZ9000 OpenSSL provider — in-library (built into AmiSSL) registration.
 *
 * Compiled into amissl.library next to the OpenSSL core (and the provider
 * objects + zz9k_offload.c + the SDK host implementation). AmiSSL calls
 * zz9k_provider_register_builtin() from InitAmiSSL for each application, so the
 * offload is available library-wide without the application registering it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k_amissl_builtin.h"
#include "zz9k_provider.h"

#include <openssl/provider.h>
#include <openssl/evp.h>

int zz9k_provider_register_builtin(OSSL_LIB_CTX *libctx)
{
  OSSL_PROVIDER *deflt;
  OSSL_PROVIDER *zz9k;

  /* Idempotent: AmiSSL may call this for every application against a library
   * context that is already set up, so do nothing if we are already loaded. */
  if (OSSL_PROVIDER_available(libctx, ZZ9K_PROVIDER_NAME)) {
    return 1;
  }
  if (!OSSL_PROVIDER_add_builtin(libctx, ZZ9K_PROVIDER_NAME,
                                 zz9k_provider_init)) {
    return 0;
  }
  /* Keep the default provider loaded for fallback (algorithms the ZZ9000 does
   * not implement, or declines at run time). Loading it again when AmiSSL has
   * already done so simply bumps its reference count. */
  deflt = OSSL_PROVIDER_load(libctx, "default");
  if (deflt == NULL) {
    return 0;
  }
  zz9k = OSSL_PROVIDER_load(libctx, ZZ9K_PROVIDER_NAME);
  if (zz9k == NULL) {
    OSSL_PROVIDER_unload(deflt);
    return 0;
  }
  /* Prefer the ZZ9000, but allow fallback to any other loaded provider for
   * operations it does not advertise. */
  if (!EVP_set_default_properties(libctx, "?provider=" ZZ9K_PROVIDER_NAME)) {
    OSSL_PROVIDER_unload(zz9k);
    OSSL_PROVIDER_unload(deflt);
    return 0;
  }
  return 1;
}
