/*
 * Phase 4.1 host unit test: the ZZ9000 provider registers and loads cleanly
 * against host OpenSSL 3 via OSSL_PROVIDER_add_builtin + OSSL_PROVIDER_load,
 * reports its name/version/status parameters, and tears down. This validates
 * the provider-registration boundary independently of the Amiga/AmiSSL target
 * (same provider API) and of the ZZ9000 hardware.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k_provider.h"

#include <openssl/provider.h>
#include <openssl/core_names.h>
#include <openssl/params.h>

#include <stdio.h>
#include <string.h>

int main(void)
{
  OSSL_LIB_CTX *libctx;
  OSSL_PROVIDER *prov;
  const char *name = NULL;
  int status = 0;
  OSSL_PARAM request[3];

  libctx = OSSL_LIB_CTX_new();
  if (libctx == NULL) {
    printf("FAIL: OSSL_LIB_CTX_new\n");
    return 1;
  }

  if (!OSSL_PROVIDER_add_builtin(libctx, ZZ9K_PROVIDER_NAME,
                                 zz9k_provider_init)) {
    printf("FAIL: OSSL_PROVIDER_add_builtin\n");
    return 2;
  }

  prov = OSSL_PROVIDER_load(libctx, ZZ9K_PROVIDER_NAME);
  if (prov == NULL) {
    printf("FAIL: OSSL_PROVIDER_load\n");
    return 3;
  }

  if (!OSSL_PROVIDER_available(libctx, ZZ9K_PROVIDER_NAME)) {
    printf("FAIL: provider not available after load\n");
    return 4;
  }

  /* The provider must answer its name and report operational status. */
  request[0] = OSSL_PARAM_construct_utf8_ptr(OSSL_PROV_PARAM_NAME,
                                             (char **)&name, 0);
  request[1] = OSSL_PARAM_construct_int(OSSL_PROV_PARAM_STATUS, &status);
  request[2] = OSSL_PARAM_construct_end();
  if (!OSSL_PROVIDER_get_params(prov, request)) {
    printf("FAIL: OSSL_PROVIDER_get_params\n");
    return 5;
  }
  if (name == NULL || strstr(name, "ZZ9000") == NULL) {
    printf("FAIL: provider name not reported (%s)\n", name ? name : "(null)");
    return 6;
  }
  if (status != 1) {
    printf("FAIL: provider status not operational (%d)\n", status);
    return 7;
  }

  if (!OSSL_PROVIDER_unload(prov)) {
    printf("FAIL: OSSL_PROVIDER_unload\n");
    return 8;
  }
  OSSL_LIB_CTX_free(libctx);

  printf("provider_init_test: passed (name=\"%s\", status=%d)\n", name, status);
  return 0;
}
