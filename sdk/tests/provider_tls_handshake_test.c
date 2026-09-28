/*
 * Full TLS handshake test for the ZZ9000 provider — the test that emulates a
 * browser on the Amiga talking to a stock server. The CLIENT runs in a library
 * context with the zz9000 provider registered and the "?provider=zz9000"
 * default property query (exactly what the patched amissl.library sets up per
 * application); the SERVER runs in a separate, clean library context (the
 * remote peer). The two sides are pumped through memory BIOs.
 *
 * The client verifies the server certificate (X509 chain verification through
 * whatever provider the property query selects), so this exercises the entire
 * client surface: X25519/EC key-share generation, key exchange, signature
 * verification (PKCS#1, PSS, ECDSA), record ciphers, and all the EVP_PKEY
 * introspection libssl performs along the way.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k_provider.h"

#include <openssl/provider.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/err.h>
#include <openssl/bio.h>

#include <stdio.h>
#include <string.h>

static void print_errors(const char *label)
{
  printf("    [%s] error stack:\n", label);
  ERR_print_errors_fp(stdout);
}

/* Self-signed certificate + key, created entirely in `libctx` (the server's
 * clean context). keytype is "EC" (P-256) or "RSA" (2048, PKCS#1 self-sig). */
static int make_cert(OSSL_LIB_CTX *libctx, const char *keytype,
                     EVP_PKEY **out_key, X509 **out_cert)
{
  EVP_PKEY *pkey = NULL;
  X509 *x = NULL;
  X509_NAME *name = NULL;
  int ok = 0;

  if (strcmp(keytype, "EC") == 0) {
    pkey = EVP_PKEY_Q_keygen(libctx, NULL, "EC", "P-256");
  } else if (strcmp(keytype, "EC-P384") == 0) {
    pkey = EVP_PKEY_Q_keygen(libctx, NULL, "EC", "P-384");
  } else {
    pkey = EVP_PKEY_Q_keygen(libctx, NULL, "RSA", (size_t)2048);
  }
  if (pkey == NULL) {
    goto end;
  }
  x = X509_new_ex(libctx, NULL);
  if (x == NULL) {
    goto end;
  }
  if (!X509_set_version(x, 2) ||
      !ASN1_INTEGER_set(X509_get_serialNumber(x), 1) ||
      X509_gmtime_adj(X509_getm_notBefore(x), -3600) == NULL ||
      X509_gmtime_adj(X509_getm_notAfter(x), 86400L * 365) == NULL ||
      !X509_set_pubkey(x, pkey)) {
    goto end;
  }
  name = X509_get_subject_name(x);
  if (!X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                  (const unsigned char *)"zz9k-test", -1, -1,
                                  0) ||
      !X509_set_issuer_name(x, name)) {
    goto end;
  }
  if (X509_sign(x, pkey, EVP_sha256()) <= 0) {
    goto end;
  }
  ok = 1;

end:
  if (!ok) {
    print_errors("make_cert");
    EVP_PKEY_free(pkey);
    X509_free(x);
    return 0;
  }
  *out_key = pkey;
  *out_cert = x;
  return 1;
}

/* Pump the handshake and one round of application data between client and
 * server through memory BIOs. Returns 1 on full success. */
static int run_connection(SSL_CTX *cctx, SSL_CTX *sctx, const char *label)
{
  SSL *client = NULL, *server = NULL;
  BIO *c_to_s = NULL, *s_to_c = NULL;
  static const char ping[] = "ZZ9000 ping";
  static const char pong[] = "ZZ9000 pong";
  char buf[64];
  int i, ok = 0;

  client = SSL_new(cctx);
  server = SSL_new(sctx);
  c_to_s = BIO_new(BIO_s_mem());
  s_to_c = BIO_new(BIO_s_mem());
  if (client == NULL || server == NULL || c_to_s == NULL || s_to_c == NULL) {
    goto end;
  }
  BIO_set_mem_eof_return(c_to_s, -1);
  BIO_set_mem_eof_return(s_to_c, -1);
  /* Each BIO is shared by both SSLs (client wbio + server rbio, and vice
   * versa) and we also keep our own reference for the explicit free below,
   * so each needs two extra refs on top of creation (3 total: 2 consumed by
   * SSL_set_bio, 1 left for us). */
  BIO_up_ref(c_to_s);
  BIO_up_ref(c_to_s);
  BIO_up_ref(s_to_c);
  BIO_up_ref(s_to_c);
  SSL_set_bio(client, s_to_c, c_to_s);
  SSL_set_bio(server, c_to_s, s_to_c);
  SSL_set_connect_state(client);
  SSL_set_accept_state(server);

  for (i = 0; i < 100; i++) {
    int rc = SSL_do_handshake(client);
    int rs;
    if (rc <= 0) {
      int err = SSL_get_error(client, rc);
      if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
        printf("  %-34s FAIL (client handshake, ssl err %d)\n", label, err);
        print_errors("client");
        goto end;
      }
    }
    rs = SSL_do_handshake(server);
    if (rs <= 0) {
      int err = SSL_get_error(server, rs);
      if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
        printf("  %-34s FAIL (server handshake, ssl err %d)\n", label, err);
        print_errors("server");
        goto end;
      }
    }
    if (SSL_is_init_finished(client) && SSL_is_init_finished(server)) {
      break;
    }
  }
  if (!SSL_is_init_finished(client) || !SSL_is_init_finished(server)) {
    printf("  %-34s FAIL (handshake did not complete)\n", label);
    goto end;
  }

  /* One round trip of application data in each direction. */
  if (SSL_write(client, ping, (int)sizeof(ping)) != (int)sizeof(ping) ||
      SSL_read(server, buf, (int)sizeof(buf)) != (int)sizeof(ping) ||
      memcmp(buf, ping, sizeof(ping)) != 0) {
    printf("  %-34s FAIL (client->server data)\n", label);
    print_errors("data c->s");
    goto end;
  }
  if (SSL_write(server, pong, (int)sizeof(pong)) != (int)sizeof(pong) ||
      SSL_read(client, buf, (int)sizeof(buf)) != (int)sizeof(pong) ||
      memcmp(buf, pong, sizeof(pong)) != 0) {
    printf("  %-34s FAIL (server->client data)\n", label);
    print_errors("data s->c");
    goto end;
  }
  printf("  %-34s ok (%s, %s)\n", label, SSL_get_version(client),
         SSL_get_cipher_name(client));
  ok = 1;

end:
  SSL_free(client);
  SSL_free(server);
  BIO_free(c_to_s);
  BIO_free(s_to_c);
  return ok;
}

/* One configuration: TLS version bounds, server certificate type, client
 * group list, optional cipher list (TLS 1.2) / ciphersuites (TLS 1.3). */
static int run_case(OSSL_LIB_CTX *client_libctx, OSSL_LIB_CTX *server_libctx,
                    const char *label, int min_proto, int max_proto,
                    EVP_PKEY *key, X509 *cert, const char *groups,
                    const char *cipher_list, const char *ciphersuites)
{
  SSL_CTX *cctx = NULL, *sctx = NULL;
  int ok = 0;

  cctx = SSL_CTX_new_ex(client_libctx, NULL, TLS_client_method());
  sctx = SSL_CTX_new_ex(server_libctx, NULL, TLS_server_method());
  if (cctx == NULL || sctx == NULL) {
    printf("  %-34s FAIL (SSL_CTX creation)\n", label);
    print_errors("ctx");
    goto end;
  }
  if (!SSL_CTX_set_min_proto_version(cctx, min_proto) ||
      !SSL_CTX_set_max_proto_version(cctx, max_proto) ||
      !SSL_CTX_set_min_proto_version(sctx, min_proto) ||
      !SSL_CTX_set_max_proto_version(sctx, max_proto)) {
    goto end;
  }
  if (SSL_CTX_use_certificate(sctx, cert) <= 0 ||
      SSL_CTX_use_PrivateKey(sctx, key) <= 0) {
    printf("  %-34s FAIL (server cert/key)\n", label);
    print_errors("server cert");
    goto end;
  }
  /* The client fully verifies the chain (forces signature verification
   * through the property-selected provider). */
  if (!X509_STORE_add_cert(SSL_CTX_get_cert_store(cctx), cert)) {
    goto end;
  }
  SSL_CTX_set_verify(cctx, SSL_VERIFY_PEER, NULL);
  if (groups != NULL) {
    if (!SSL_CTX_set1_groups_list(cctx, groups)) {
      printf("  %-34s FAIL (client groups '%s')\n", label, groups);
      print_errors("client groups");
      goto end;
    }
    if (!SSL_CTX_set1_groups_list(sctx, groups)) {
      printf("  %-34s FAIL (server groups '%s')\n", label, groups);
      print_errors("server groups");
      goto end;
    }
  }
  if (cipher_list != NULL && !SSL_CTX_set_cipher_list(cctx, cipher_list)) {
    goto end;
  }
  if (ciphersuites != NULL && !SSL_CTX_set_ciphersuites(cctx, ciphersuites)) {
    goto end;
  }
  ok = run_connection(cctx, sctx, label);

end:
  SSL_CTX_free(cctx);
  SSL_CTX_free(sctx);
  return ok;
}

int main(void)
{
  OSSL_LIB_CTX *client_libctx = OSSL_LIB_CTX_new();
  OSSL_LIB_CTX *server_libctx = OSSL_LIB_CTX_new();
  EVP_PKEY *ec_key = NULL, *rsa_key = NULL, *ec384_key = NULL;
  X509 *ec_cert = NULL, *rsa_cert = NULL, *ec384_cert = NULL;
  int rc = 0;

  if (client_libctx == NULL || server_libctx == NULL) {
    return 1;
  }
  /* Server side: clean context, default provider only (the remote peer). */
  if (OSSL_PROVIDER_load(server_libctx, "default") == NULL) {
    printf("FAIL: server provider setup\n");
    return 1;
  }
  if (!make_cert(server_libctx, "EC", &ec_key, &ec_cert) ||
      !make_cert(server_libctx, "RSA", &rsa_key, &rsa_cert) ||
      !make_cert(server_libctx, "EC-P384", &ec384_key, &ec384_cert)) {
    printf("FAIL: certificate creation\n");
    return 1;
  }
  /* Client side: what the patched amissl.library gives every application. */
  if (!OSSL_PROVIDER_add_builtin(client_libctx, ZZ9K_PROVIDER_NAME,
                                 zz9k_provider_init) ||
      OSSL_PROVIDER_load(client_libctx, "default") == NULL ||
      OSSL_PROVIDER_load(client_libctx, ZZ9K_PROVIDER_NAME) == NULL ||
      !EVP_set_default_properties(client_libctx, "?provider=zz9000")) {
    printf("FAIL: client provider setup\n");
    return 1;
  }

  printf("ZZ9000 provider TLS handshake matrix (client = zz9000 libctx)\n");

  rc |= !run_case(client_libctx, server_libctx,
                  "TLS1.3 ECDSA-P256 x25519", TLS1_3_VERSION, TLS1_3_VERSION,
                  ec_key, ec_cert, "X25519", NULL, NULL);
  rc |= !run_case(client_libctx, server_libctx,
                  "TLS1.3 RSA-2048 x25519 (PSS)", TLS1_3_VERSION,
                  TLS1_3_VERSION, rsa_key, rsa_cert, "X25519", NULL, NULL);
  rc |= !run_case(client_libctx, server_libctx,
                  "TLS1.3 ECDSA-P256 chacha", TLS1_3_VERSION, TLS1_3_VERSION,
                  ec_key, ec_cert, "X25519", NULL,
                  "TLS_CHACHA20_POLY1305_SHA256");
  rc |= !run_case(client_libctx, server_libctx,
                  "TLS1.3 ECDSA-P256 group-P256", TLS1_3_VERSION,
                  TLS1_3_VERSION, ec_key, ec_cert, "P-256", NULL, NULL);
  rc |= !run_case(client_libctx, server_libctx,
                  "TLS1.2 ECDSA-P256 aes-gcm P256", TLS1_2_VERSION,
                  TLS1_2_VERSION, ec_key, ec_cert, "P-256",
                  "ECDHE-ECDSA-AES128-GCM-SHA256", NULL);
  rc |= !run_case(client_libctx, server_libctx,
                  "TLS1.2 RSA-2048 chacha P256", TLS1_2_VERSION,
                  TLS1_2_VERSION, rsa_key, rsa_cert, "P-256",
                  "ECDHE-RSA-CHACHA20-POLY1305", NULL);
  rc |= !run_case(client_libctx, server_libctx,
                  "TLS1.2 ECDSA chacha P256", TLS1_2_VERSION, TLS1_2_VERSION,
                  ec_key, ec_cert, "P-256",
                  "ECDHE-ECDSA-CHACHA20-POLY1305", NULL);
  rc |= !run_case(client_libctx, server_libctx,
                  "TLS1.2 RSA aes-gcm P256", TLS1_2_VERSION, TLS1_2_VERSION,
                  rsa_key, rsa_cert, "P-256",
                  "ECDHE-RSA-AES128-GCM-SHA256", NULL);
  rc |= !run_case(client_libctx, server_libctx,
                  "TLS1.2 RSA aes-gcm x25519", TLS1_2_VERSION,
                  TLS1_2_VERSION, rsa_key, rsa_cert, "X25519",
                  "ECDHE-RSA-AES256-GCM-SHA384", NULL);
  /* Delegation regression: the client's EC KEYMGMT/ECDSA now own "EC" for
   * every curve, but only P-256 is accelerated. A P-384 server cert forces
   * our EC keymgmt to build a shadow EVP_PKEY (default provider) at import
   * and our ECDSA verify to forward to it — this must still succeed. The
   * handshake's own key exchange uses X25519 (P-384 is never offered as a
   * TLS group), so this isolates cert-chain delegation from key exchange. */
  rc |= !run_case(client_libctx, server_libctx,
                  "TLS1.3 ECDSA-P384 cert (delegated) x25519", TLS1_3_VERSION,
                  TLS1_3_VERSION, ec384_key, ec384_cert, "X25519", NULL, NULL);

  EVP_PKEY_free(ec_key);
  EVP_PKEY_free(rsa_key);
  EVP_PKEY_free(ec384_key);
  X509_free(ec_cert);
  X509_free(rsa_cert);
  X509_free(ec384_cert);
  OSSL_LIB_CTX_free(client_libctx);
  OSSL_LIB_CTX_free(server_libctx);
  if (rc == 0) {
    printf("provider_tls_handshake_test: passed\n");
  }
  return rc;
}
