/*
 * ZZ9000 AmiSSL crypto-path profiler.
 *
 * A standalone AmigaOS 3 (m68k) program that drives the TLS handshake crypto
 * primitives through the *ordinary AmiSSL EVP/SSL API* — the exact path a real
 * browser handshake uses — and times each one on the ZZ9000 offload path versus
 * the m68k software path, side by side.
 *
 * This is deliberately different from zz9k-cryptobench, which calls the ZZ9000
 * SDK crypto API directly and so bypasses the OpenSSL provider entirely. A slow
 * handshake that cryptobench cannot reproduce lives *in the provider path*, and
 * only a tool that goes through amissl (like this one) can expose it. Two
 * OSSL_LIB_CTX values isolate the paths cleanly, exactly as the in-tree TLS
 * handshake test does:
 *
 *   - offload context = the default library context, where the drop-in
 *     amissl.library has (at InitAmiSSL) loaded the ZZ9000 provider and set
 *     "?provider=zz9000"; ops route to the board (falling back to software only
 *     when it declines). This is the exact context an unmodified browser uses.
 *   - software context = a fresh OSSL_LIB_CTX with only the default provider;
 *     every op runs in AmiSSL software.
 *
 * Per-op micro-benchmarks (keygen / derive / verify / AEAD) localise a slow
 * primitive; the full in-memory TLS handshake (client<->server over memory
 * BIOs, no network) reproduces the real sequence and its op interactions. All
 * timing uses the timer.device EClock at microsecond resolution, like
 * zz9k-cryptobench.
 *
 * Build it exactly like zz9k_amissl_selftest.c (see docs/zz9k-amissl-provider.md):
 * compile the provider objects plus this file with -DZZ9K_PROVIDER_OFFLOAD and
 * -include proto/amissl.h, link the pure-SDK objects, no -lcrypto/-lssl.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/socket.h>
#include <proto/timer.h>
#include <proto/amissl.h>
#include <proto/amisslmaster.h>

#include <devices/timer.h>
#include <exec/types.h>
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <amissl/amissl.h>

#include <openssl/evp.h>
#include <openssl/provider.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/err.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/param_build.h>
#include <openssl/bn.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

/* This is a PURE AmiSSL client: it does not compile in or register the ZZ9000
 * provider. The installed drop-in amissl.library already registers it at
 * InitAmiSSL and sets "?provider=zz9000" on the default context, so the default
 * (NULL) context IS the offload path a real browser uses. A fresh OSSL_LIB_CTX
 * with only the default provider is the software path. Testing the installed
 * library's provider — not a private copy — is the whole point. */

/* Library bases. proto/amissl.h externs AmiSSLBase/AmiSSLExtBase; this
 * translation unit provides their single definition (as the self-test does). */
struct Library *AmiSSLMasterBase, *AmiSSLBase, *AmiSSLExtBase, *SocketBase;
struct Device  *TimerBase;

static int amissl_open = 0;

/* ---------------------------------------------------------------------------
 * Timing — timer.device UNIT_MICROHZ EClock, mirroring zz9k-cryptobench.
 * ------------------------------------------------------------------------- */

typedef uint64_t Tick;

static struct MsgPort      *g_timer_port;
static struct timerequest  *g_timer_req;
static uint32_t             g_ticks_per_second;
static int                  g_timer_hires;

static void timer_close(void)
{
  if (g_timer_req) {
    if (g_timer_hires) {
      CloseDevice((struct IORequest *)g_timer_req);
    }
    DeleteIORequest((struct IORequest *)g_timer_req);
    g_timer_req = NULL;
  }
  if (g_timer_port) {
    DeleteMsgPort(g_timer_port);
    g_timer_port = NULL;
  }
  TimerBase = NULL;
  g_timer_hires = 0;
}

static void timer_open(void)
{
  g_timer_port = CreateMsgPort();
  if (g_timer_port) {
    g_timer_req = (struct timerequest *)CreateIORequest(
        g_timer_port, sizeof(*g_timer_req));
  }
  if (g_timer_req &&
      OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                 (struct IORequest *)g_timer_req, 0) == 0) {
    struct EClockVal value;
    TimerBase = (struct Device *)g_timer_req->tr_node.io_Device;
    g_ticks_per_second = ReadEClock(&value);
    if (g_ticks_per_second != 0) {
      g_timer_hires = 1;
      return;
    }
    CloseDevice((struct IORequest *)g_timer_req);
  }
  timer_close();
  g_ticks_per_second = 50U;   /* graceful degrade; results will be coarse */
}

static Tick timer_now(void)
{
  if (g_timer_hires) {
    struct EClockVal value;
    ReadEClock(&value);
    return ((Tick)value.ev_hi << 32) | value.ev_lo;
  }
  return 0;
}

/* Elapsed ticks over `count` ops -> milliseconds * 100 (fixed-point hundredths),
 * so a 1.23 ms op prints as 123. 0 on timer error or empty measurement. */
static uint32_t ms_x100_per_op(Tick elapsed, uint32_t count)
{
  uint64_t numerator;
  if (elapsed == 0 || count == 0 || g_ticks_per_second == 0) {
    return 0U;
  }
  numerator = (uint64_t)elapsed * 100000ULL;
  return (uint32_t)(numerator / ((uint64_t)count * (uint64_t)g_ticks_per_second));
}

/* ---------------------------------------------------------------------------
 * Iteration counts. Kept modest so a *pathologically slow* offload path (the
 * thing we are hunting) still finishes in a few seconds per row.
 * ------------------------------------------------------------------------- */

#define N_X25519_KEYGEN 30U
#define N_X25519_DERIVE 30U
#define N_P256_KEYGEN   20U
#define N_P256_DERIVE   20U
#define N_ECDSA_VERIFY  20U
#define N_RSA_VERIFY    10U
#define N_AEAD          50U
#define N_HANDSHAKE      3U

#define AEAD_MAX_RECORD 16384U  /* static scratch; up to one full TLS record */

static const unsigned char g_msg[32] = {
  0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,
  0x0f,0x1e,0x2d,0x3c,0x4b,0x5a,0x69,0x78,0x87,0x96,0xa5,0xb4,0xc3,0xd2,0xe1,0xf0
};

/* Fixed RSA-2048 public key + known-valid PKCS#1-SHA256 and PSS-SHA256
 * signatures over g_msg, generated by correct OpenSSL (scratchpad/genvec.c).
 * Lets the RSA verify KAT run with NO keygen (minutes on a 68k) and gives a
 * deterministic correctness check of the delegated PSS verify path. */
#include "rsa_kat_vector.h"

static void print_errors(const char *where)
{
  unsigned long e;
  while ((e = ERR_get_error()) != 0) {
    char buf[160];
    ERR_error_string_n(e, buf, sizeof(buf));
    printf("    [%s] %s\n", where, buf);
  }
}

/* ---------------------------------------------------------------------------
 * Per-op micro-benchmarks. Each takes the library context that decides the
 * provider (offload = default NULL ctx with zz9k; software = default-only ctx)
 * and returns ms*100 per op, or 0 on failure (with an explanatory line).
 * ------------------------------------------------------------------------- */

/* Generate a keypair in `libctx` (its providers decide offload vs software).
 * For EC pass the group name (e.g. "P-256"); for RSA pass rsa_bits; X25519
 * needs neither. Uses the explicit EVP_PKEY_CTX API — AmiSSL exposes the
 * variadic EVP_PKEY_Q_keygen only as a fixed-arity macro. */
static EVP_PKEY *keygen_pkey(OSSL_LIB_CTX *libctx, const char *type,
                             const char *ec_group, size_t rsa_bits)
{
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(libctx, type, NULL);
  EVP_PKEY *pkey = NULL;
  OSSL_PARAM params[2];
  size_t bits = rsa_bits;
  int n = 0;

  if (ctx == NULL) return NULL;
  if (EVP_PKEY_keygen_init(ctx) <= 0) goto done;
  if (ec_group != NULL) {
    params[n++] = OSSL_PARAM_construct_utf8_string(
        OSSL_PKEY_PARAM_GROUP_NAME, (char *)ec_group, 0);
  } else if (rsa_bits != 0U) {
    params[n++] = OSSL_PARAM_construct_size_t(OSSL_PKEY_PARAM_RSA_BITS, &bits);
  }
  if (n > 0) {
    params[n] = OSSL_PARAM_construct_end();
    if (EVP_PKEY_CTX_set_params(ctx, params) <= 0) goto done;
  }
  if (EVP_PKEY_generate(ctx, &pkey) <= 0) pkey = NULL;

done:
  EVP_PKEY_CTX_free(ctx);
  return pkey;
}

static uint32_t bench_keygen(OSSL_LIB_CTX *libctx, const char *label,
                             const char *type, const char *group,
                             size_t rsa_bits, uint32_t count)
{
  Tick start, elapsed;
  uint32_t i;

  start = timer_now();
  for (i = 0; i < count; i++) {
    EVP_PKEY *k = keygen_pkey(libctx, type, group, rsa_bits);
    if (k == NULL) {
      printf("  %-22s keygen FAILED on iter %lu\n", label,
             (unsigned long)(i + 1U));
      print_errors("keygen");
      return 0U;
    }
    EVP_PKEY_free(k);
  }
  elapsed = timer_now() - start;
  return ms_x100_per_op(elapsed, count);
}

static uint32_t bench_derive(OSSL_LIB_CTX *libctx, const char *label,
                             const char *type, const char *group,
                             uint32_t count)
{
  EVP_PKEY *a = keygen_pkey(libctx, type, group, 0);
  EVP_PKEY *b = keygen_pkey(libctx, type, group, 0);
  Tick start, elapsed;
  uint32_t i;
  uint32_t rc = 0U;

  if (a == NULL || b == NULL) {
    printf("  %-22s derive setup keygen FAILED\n", label);
    print_errors("derive-setup");
    goto out;
  }
  start = timer_now();
  for (i = 0; i < count; i++) {
    EVP_PKEY_CTX *dctx = EVP_PKEY_CTX_new_from_pkey(libctx, a, NULL);
    unsigned char shared[64];
    size_t sharedlen = sizeof(shared);
    int ok = dctx != NULL &&
             EVP_PKEY_derive_init(dctx) > 0 &&
             EVP_PKEY_derive_set_peer(dctx, b) > 0 &&
             EVP_PKEY_derive(dctx, shared, &sharedlen) > 0;
    EVP_PKEY_CTX_free(dctx);
    if (!ok) {
      printf("  %-22s derive FAILED on iter %lu\n", label,
             (unsigned long)(i + 1U));
      print_errors("derive");
      goto out;
    }
  }
  elapsed = timer_now() - start;
  rc = ms_x100_per_op(elapsed, count);

out:
  EVP_PKEY_free(a);
  EVP_PKEY_free(b);
  return rc;
}

static uint32_t bench_verify(OSSL_LIB_CTX *libctx, const char *label,
                             const char *type, const char *group,
                             size_t rsa_bits, uint32_t count)
{
  EVP_PKEY *key = keygen_pkey(libctx, type, group, rsa_bits);
  EVP_MD_CTX *md = NULL;
  unsigned char sig[600];   /* RSA-2048 sig = 256 bytes; ECDSA-P256 < 80 */
  size_t siglen = sizeof(sig);
  Tick start, elapsed;
  uint32_t i;
  uint32_t rc = 0U;

  if (key == NULL) {
    printf("  %-22s verify setup keygen FAILED\n", label);
    print_errors("verify-keygen");
    goto out;
  }
  /* Produce one genuine signature to verify repeatedly. Signing itself is not
   * offloaded (the provider delegates sign to the default provider), which is
   * exactly the real handshake shape: the client only ever *verifies*. */
  md = EVP_MD_CTX_new();
  if (md == NULL ||
      EVP_DigestSignInit_ex(md, NULL, "SHA256", libctx, NULL, key, NULL) <= 0 ||
      EVP_DigestSign(md, sig, &siglen, g_msg, sizeof(g_msg)) <= 0) {
    printf("  %-22s verify setup sign FAILED\n", label);
    print_errors("verify-sign");
    goto out;
  }
  EVP_MD_CTX_free(md);
  md = NULL;

  start = timer_now();
  for (i = 0; i < count; i++) {
    EVP_MD_CTX *v = EVP_MD_CTX_new();
    int ok = v != NULL &&
             EVP_DigestVerifyInit_ex(v, NULL, "SHA256", libctx, NULL, key,
                                     NULL) > 0 &&
             EVP_DigestVerify(v, sig, siglen, g_msg, sizeof(g_msg)) == 1;
    EVP_MD_CTX_free(v);
    if (!ok) {
      printf("  %-22s verify FAILED on iter %lu\n", label,
             (unsigned long)(i + 1U));
      print_errors("verify");
      goto out;
    }
  }
  elapsed = timer_now() - start;
  rc = ms_x100_per_op(elapsed, count);

out:
  EVP_MD_CTX_free(md);
  EVP_PKEY_free(key);
  return rc;
}

static uint32_t bench_aead(OSSL_LIB_CTX *libctx, const char *label,
                           const char *alg, unsigned int record_bytes,
                           uint32_t count)
{
  EVP_CIPHER *cipher = EVP_CIPHER_fetch(libctx, alg, NULL);
  static unsigned char in[AEAD_MAX_RECORD];
  static unsigned char out[AEAD_MAX_RECORD];
  unsigned char tag[16];
  unsigned int n = record_bytes > AEAD_MAX_RECORD ? AEAD_MAX_RECORD
                                                  : record_bytes;
  static const unsigned char key32[32] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
  };
  static const unsigned char iv12[12] = {
    0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad,0xde,0xca,0xf8,0x88
  };
  Tick start, elapsed;
  uint32_t i;
  uint32_t rc = 0U;

  if (cipher == NULL) {
    printf("  %-22s cipher fetch FAILED\n", label);
    print_errors("aead-fetch");
    return 0U;
  }
  memset(in, 0xa5, n);

  start = timer_now();
  for (i = 0; i < count; i++) {
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int outl = 0, finl = 0;
    OSSL_PARAM tagp[2];
    int ok = c != NULL &&
             EVP_EncryptInit_ex2(c, cipher, key32, iv12, NULL) > 0 &&
             EVP_EncryptUpdate(c, out, &outl, in, (int)n) > 0 &&
             EVP_EncryptFinal_ex(c, out + outl, &finl) > 0;
    if (ok) {
      tagp[0] = OSSL_PARAM_construct_octet_string(
          OSSL_CIPHER_PARAM_AEAD_TAG, tag, sizeof(tag));
      tagp[1] = OSSL_PARAM_construct_end();
      ok = EVP_CIPHER_CTX_get_params(c, tagp) > 0;
    }
    EVP_CIPHER_CTX_free(c);
    if (!ok) {
      printf("  %-22s AEAD FAILED on iter %lu\n", label,
             (unsigned long)(i + 1U));
      print_errors("aead");
      goto out;
    }
  }
  elapsed = timer_now() - start;
  rc = ms_x100_per_op(elapsed, count);

out:
  EVP_CIPHER_free(cipher);
  return rc;
}

/* ---------------------------------------------------------------------------
 * Full in-memory TLS 1.3 handshake, adapted from tests/provider_tls_handshake_
 * test.c. The client context under test does the ephemeral keygen, the ECDH
 * derive, and the certificate-chain verify; the server is always software (the
 * remote peer). This reproduces the exact op sequence of a real handshake with
 * zero network.
 * ------------------------------------------------------------------------- */

static int make_cert(OSSL_LIB_CTX *libctx, const char *keytype,
                     EVP_PKEY **out_key, X509 **out_cert)
{
  EVP_PKEY *pkey = NULL;
  X509 *x = NULL;
  X509_NAME *name;
  int ok = 0;

  if (strcmp(keytype, "EC") == 0) {
    pkey = keygen_pkey(libctx, "EC", "P-256", 0);
  } else {
    pkey = keygen_pkey(libctx, "RSA", NULL, (size_t)2048);
  }
  if (pkey == NULL) goto end;
  x = X509_new_ex(libctx, NULL);
  if (x == NULL) goto end;
  if (!X509_set_version(x, 2) ||
      !ASN1_INTEGER_set(X509_get_serialNumber(x), 1) ||
      X509_gmtime_adj(X509_getm_notBefore(x), -3600) == NULL ||
      X509_gmtime_adj(X509_getm_notAfter(x), 86400L * 365) == NULL ||
      !X509_set_pubkey(x, pkey)) {
    goto end;
  }
  name = X509_get_subject_name(x);
  if (!X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                  (const unsigned char *)"zz9k-profile", -1,
                                  -1, 0) ||
      !X509_set_issuer_name(x, name) ||
      X509_sign(x, pkey, EVP_sha256()) <= 0) {
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

static int run_connection(SSL_CTX *cctx, SSL_CTX *sctx)
{
  SSL *client = SSL_new(cctx);
  SSL *server = SSL_new(sctx);
  BIO *c_to_s = BIO_new(BIO_s_mem());
  BIO *s_to_c = BIO_new(BIO_s_mem());
  int done = 0, guard = 0;
  int ok = 0;

  if (!client || !server || !c_to_s || !s_to_c) goto end;
  BIO_set_mem_eof_return(c_to_s, -1);
  BIO_set_mem_eof_return(s_to_c, -1);
  /* Each mem BIO is shared by both SSLs (client wbio + server rbio, and vice
   * versa) AND kept for the explicit BIO_free below: 3 refs total = 2 consumed
   * by the two SSL_set_bio, 1 left for us. One up_ref short here double-frees
   * on cleanup and Gurus the machine. */
  BIO_up_ref(c_to_s);
  BIO_up_ref(c_to_s);
  BIO_up_ref(s_to_c);
  BIO_up_ref(s_to_c);
  SSL_set_bio(client, s_to_c, c_to_s);
  SSL_set_bio(server, c_to_s, s_to_c);
  SSL_set_connect_state(client);
  SSL_set_accept_state(server);

  while (!done && guard++ < 50) {
    int rc = SSL_do_handshake(client);
    if (rc != 1) {
      int err = SSL_get_error(client, rc);
      if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
        print_errors("client-handshake");
        goto end;
      }
    }
    rc = SSL_do_handshake(server);
    if (rc != 1) {
      int err = SSL_get_error(server, rc);
      if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
        print_errors("server-handshake");
        goto end;
      }
    }
    if (SSL_is_init_finished(client) && SSL_is_init_finished(server)) {
      done = 1;
    }
  }
  ok = done;

end:
  SSL_free(client);
  SSL_free(server);
  BIO_free(c_to_s);
  BIO_free(s_to_c);
  return ok;
}

static int handshake_once(OSSL_LIB_CTX *client_ctx, OSSL_LIB_CTX *server_ctx,
                          EVP_PKEY *key, X509 *cert, const char *groups)
{
  SSL_CTX *cctx = SSL_CTX_new_ex(client_ctx, NULL, TLS_client_method());
  SSL_CTX *sctx = SSL_CTX_new_ex(server_ctx, NULL, TLS_server_method());
  int ok = 0;

  if (cctx == NULL || sctx == NULL) { print_errors("ctx"); goto end; }
  if (!SSL_CTX_set_min_proto_version(cctx, TLS1_3_VERSION) ||
      !SSL_CTX_set_max_proto_version(cctx, TLS1_3_VERSION) ||
      !SSL_CTX_set_min_proto_version(sctx, TLS1_3_VERSION) ||
      !SSL_CTX_set_max_proto_version(sctx, TLS1_3_VERSION)) {
    goto end;
  }
  if (SSL_CTX_use_certificate(sctx, cert) <= 0 ||
      SSL_CTX_use_PrivateKey(sctx, key) <= 0) {
    print_errors("server-cert");
    goto end;
  }
  if (!X509_STORE_add_cert(SSL_CTX_get_cert_store(cctx), cert)) goto end;
  SSL_CTX_set_verify(cctx, SSL_VERIFY_PEER, NULL);
  if (groups != NULL &&
      (!SSL_CTX_set1_groups_list(cctx, groups) ||
       !SSL_CTX_set1_groups_list(sctx, groups))) {
    print_errors("groups");
    goto end;
  }
  ok = run_connection(cctx, sctx);

end:
  SSL_CTX_free(cctx);
  SSL_CTX_free(sctx);
  return ok;
}

/* Time `count` full handshakes for one (cert, group) case. cert+key are made in
 * the server (software) context and shared; only the client context varies. */
static uint32_t bench_handshake(OSSL_LIB_CTX *client_ctx,
                                OSSL_LIB_CTX *server_ctx, const char *label,
                                EVP_PKEY *key, X509 *cert, const char *groups,
                                uint32_t count)
{
  Tick start, elapsed;
  uint32_t i;

  start = timer_now();
  for (i = 0; i < count; i++) {
    if (!handshake_once(client_ctx, server_ctx, key, cert, groups)) {
      printf("  %-22s handshake FAILED on iter %lu\n", label,
             (unsigned long)(i + 1U));
      return 0U;
    }
  }
  elapsed = timer_now() - start;
  return ms_x100_per_op(elapsed, count);
}

/* ---------------------------------------------------------------------------
 * Reporting
 * ------------------------------------------------------------------------- */

static void row(const char *name, uint32_t off_x100, uint32_t soft_x100)
{
  printf("  %-22s ", name);
  if (off_x100)  printf("%6lu.%02lu", (unsigned long)(off_x100 / 100U),
                        (unsigned long)(off_x100 % 100U));
  else           printf("     n/a");
  printf("  ");
  if (soft_x100) printf("%6lu.%02lu", (unsigned long)(soft_x100 / 100U),
                        (unsigned long)(soft_x100 % 100U));
  else           printf("     n/a");
  printf("  ");
  if (off_x100 && soft_x100) {
    /* ratio offload/software x100: >100 means offload is SLOWER than software */
    unsigned long r = (unsigned long)(((uint64_t)off_x100 * 100ULL) / soft_x100);
    printf("%3lu.%02lux%s\n", r / 100U, r % 100U,
           (r > 100U) ? "  <== offload slower" : "");
  } else {
    printf("   -\n");
  }
}

/* ---------------------------------------------------------------------------
 * AmiSSL lifecycle (identical shape to zz9k_amissl_selftest.c)
 * ------------------------------------------------------------------------- */

static int init_amissl(void)
{
  if (!(SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4))) {
    printf("Couldn't open bsdsocket.library v4\n");
    return 0;
  }
  if (!(AmiSSLMasterBase = OpenLibrary((STRPTR)"amisslmaster.library",
                                       AMISSLMASTER_MIN_VERSION))) {
    printf("Couldn't open amisslmaster.library\n");
    return 0;
  }
  if (OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
                     AmiSSL_UsesOpenSSLStructs, TRUE,
                     AmiSSL_GetAmiSSLBase, (ULONG)&AmiSSLBase,
                     AmiSSL_GetAmiSSLExtBase, (ULONG)&AmiSSLExtBase,
                     AmiSSL_SocketBase, (ULONG)SocketBase,
                     AmiSSL_ErrNoPtr, (ULONG)&errno,
                     TAG_DONE) != 0) {
    printf("Couldn't open and initialise AmiSSL\n");
    return 0;
  }
  amissl_open = 1;
  return 1;
}

/* Name the provider that actually serves each class of op on the default
 * (offload) context. "zz9000" proves offload is live; "default" means the
 * library has no ZZ9000 provider or the board/service is absent, in which case
 * the offload column will simply match software. */
static void report_offload_provider(void)
{
  EVP_CIPHER *aead = EVP_CIPHER_fetch(NULL, "AES-256-GCM", NULL);
  EVP_KEYMGMT *ec = EVP_KEYMGMT_fetch(NULL, "EC", NULL);
  const OSSL_PROVIDER *pa = aead ? EVP_CIPHER_get0_provider(aead) : NULL;
  const OSSL_PROVIDER *pe = ec ? EVP_KEYMGMT_get0_provider(ec) : NULL;

  printf("Offload provider: AES-GCM -> %s, EC -> %s\n",
         pa ? OSSL_PROVIDER_get0_name(pa) : "(none)",
         pe ? OSSL_PROVIDER_get0_name(pe) : "(none)");
  EVP_CIPHER_free(aead);
  EVP_KEYMGMT_free(ec);
}

static void cleanup(void)
{
  if (amissl_open) {
    CloseAmiSSL();
    amissl_open = 0;
  }
  if (AmiSSLMasterBase) { CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL; }
  if (SocketBase) { CloseLibrary(SocketBase); SocketBase = NULL; }
}

/* Record-AEAD size sweep. Every TLS record currently offloads (zz9k_aead.c:
 * "every record offloads", no size gate), each paying a ~fixed mailbox
 * round-trip. The crossover size where offload finally beats software is the
 * break-even the size gate should use. AES-GCM swept in full; ChaCha at the
 * extremes to confirm it tracks. Cheap — no asymmetric keygen — so this is the
 * whole job in AEAD-only mode. */
static void run_aead_sweep(OSSL_LIB_CTX *soft)
{
  static const unsigned int sizes[] = {64U, 256U, 1024U, 4096U, 8192U, 16384U};
  unsigned int si;

  for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
    char lbl[32];
    sprintf(lbl, "AES-256-GCM %5uB", sizes[si]);
    row(lbl, bench_aead(NULL, lbl, "AES-256-GCM", sizes[si], N_AEAD),
        bench_aead(soft, lbl, "AES-256-GCM", sizes[si], N_AEAD));
  }
  row("ChaCha20-Poly1305   64B",
      bench_aead(NULL, "ChaCha20", "ChaCha20-Poly1305", 64U, N_AEAD),
      bench_aead(soft, "ChaCha20", "ChaCha20-Poly1305", 64U, N_AEAD));
  row("ChaCha20-Poly1305 16KiB",
      bench_aead(NULL, "ChaCha20", "ChaCha20-Poly1305", 16384U, N_AEAD),
      bench_aead(soft, "ChaCha20", "ChaCha20-Poly1305", 16384U, N_AEAD));
}

/* ---------------------------------------------------------------------------
 * RSA-2048 verify KAT (deterministic, keygen-free). Verifies fixed, known-valid
 * signatures from rsa_kat_vector.h. This isolates the path a real browser runs
 * for an RSA server certificate: the TLS 1.3 RSA CertificateVerify is *PSS*,
 * which the zz9000 provider cannot offload and routes through
 * zz9k_rsa_sig_verify_delegated (the default-provider shadow). A rejected
 * known-valid signature is a correctness bug; a large time is the stall we hunt.
 * ------------------------------------------------------------------------- */

/* Build the KAT RSA-2048 public key in `libctx`. Its default propquery decides
 * the keymgmt provider: NULL ctx -> zz9000 delegating RSA (the TLS path);
 * `soft` ctx -> default provider directly. */
static EVP_PKEY *kat_rsa_pubkey(OSSL_LIB_CTX *libctx)
{
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(libctx, "RSA", NULL);
  EVP_PKEY *pkey = NULL;
  BIGNUM *n = BN_bin2bn(kat_rsa_n, (int)sizeof(kat_rsa_n), NULL);
  BIGNUM *e = BN_bin2bn(kat_rsa_e, (int)sizeof(kat_rsa_e), NULL);
  OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
  OSSL_PARAM *params = NULL;

  if (ctx != NULL && n != NULL && e != NULL && bld != NULL &&
      OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n) &&
      OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e) &&
      (params = OSSL_PARAM_BLD_to_param(bld)) != NULL &&
      EVP_PKEY_fromdata_init(ctx) > 0) {
    if (EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
      pkey = NULL;
    }
  }
  OSSL_PARAM_free(params);
  OSSL_PARAM_BLD_free(bld);
  BN_free(n);
  BN_free(e);
  EVP_PKEY_CTX_free(ctx);
  return pkey;
}

/* Verify the known-valid signature `count` times through `key` (whose keymgmt
 * provider `libctx` selected). `pss` selects the padding TLS 1.3 uses for RSA.
 * Returns ms*100 per op, or 0 if ANY verify fails — a known-valid signature
 * MUST verify, so a failure is a correctness bug, not a timing result. */
static uint32_t kat_rsa_verify(OSSL_LIB_CTX *libctx, EVP_PKEY *key,
                               const char *label, const unsigned char *sig,
                               size_t siglen, int pss, uint32_t count)
{
  OSSL_PARAM pp[4];
  const OSSL_PARAM *params = NULL;
  Tick start, elapsed;
  uint32_t i;
  int m = 0;

  if (key == NULL) {
    return 0U;
  }
  if (pss) {
    pp[m++] = OSSL_PARAM_construct_utf8_string(
        OSSL_SIGNATURE_PARAM_PAD_MODE, (char *)"pss", 0);
    pp[m++] = OSSL_PARAM_construct_utf8_string(
        OSSL_SIGNATURE_PARAM_MGF1_DIGEST, (char *)"SHA256", 0);
    pp[m++] = OSSL_PARAM_construct_utf8_string(
        OSSL_SIGNATURE_PARAM_PSS_SALTLEN, (char *)"digest", 0);
    pp[m] = OSSL_PARAM_construct_end();
    params = pp;
  }
  start = timer_now();
  for (i = 0; i < count; i++) {
    EVP_MD_CTX *v = EVP_MD_CTX_new();
    int ok = v != NULL &&
             EVP_DigestVerifyInit_ex(v, NULL, "SHA256", libctx, NULL, key,
                                     params) > 0 &&
             EVP_DigestVerify(v, sig, siglen, g_msg, sizeof(g_msg)) == 1;
    EVP_MD_CTX_free(v);
    if (!ok) {
      printf("  %-26s KNOWN-VALID SIG REJECTED at iter %lu (correctness bug!)\n",
             label, (unsigned long)(i + 1U));
      print_errors("kat-verify");
      return 0U;
    }
  }
  elapsed = timer_now() - start;
  return ms_x100_per_op(elapsed, count);
}

static void run_rsa_kat(OSSL_LIB_CTX *soft)
{
  EVP_PKEY *k_off = kat_rsa_pubkey(NULL);   /* zz9000 delegating RSA keymgmt */
  EVP_PKEY *k_sw = kat_rsa_pubkey(soft);    /* default provider only */

  printf("RSA-2048 verify KAT (known-valid sigs, no keygen). PSS is the TLS 1.3\n");
  printf("RSA CertificateVerify a browser runs on an RSA cert; the zz9000 provider\n");
  printf("delegates it to software. offload col = ?provider=zz9000 default query.\n");
  if (k_off == NULL || k_sw == NULL) {
    printf("  RSA KAT pubkey build FAILED (offload=%p software=%p)\n",
           (void *)k_off, (void *)k_sw);
    print_errors("kat-pubkey");
  } else {
    row("RSA-2048 PKCS1 verify",
        kat_rsa_verify(NULL, k_off, "RSA-2048 PKCS1 verify",
                       kat_rsa_sig_pkcs1, sizeof(kat_rsa_sig_pkcs1), 0,
                       N_RSA_VERIFY),
        kat_rsa_verify(soft, k_sw, "RSA-2048 PKCS1 verify",
                       kat_rsa_sig_pkcs1, sizeof(kat_rsa_sig_pkcs1), 0,
                       N_RSA_VERIFY));
    row("RSA-2048 PSS verify",
        kat_rsa_verify(NULL, k_off, "RSA-2048 PSS verify",
                       kat_rsa_sig_pss, sizeof(kat_rsa_sig_pss), 1,
                       N_RSA_VERIFY),
        kat_rsa_verify(soft, k_sw, "RSA-2048 PSS verify",
                       kat_rsa_sig_pss, sizeof(kat_rsa_sig_pss), 1,
                       N_RSA_VERIFY));
  }
  EVP_PKEY_free(k_off);
  EVP_PKEY_free(k_sw);
}

static void run_profile(OSSL_LIB_CTX *soft)
{
  EVP_PKEY *ec_key = NULL, *rsa_key = NULL;
  X509 *ec_cert = NULL, *rsa_cert = NULL;

  printf("Timings are milliseconds per operation (offload vs m68k software).\n");
  printf("%-24s %9s  %9s  %s\n", "  operation", "offload", "software",
         "ratio");
  printf("  ------------------------------------------------------------\n");

  /* Key exchange primitives */
  row("X25519 keygen",
      bench_keygen(NULL, "X25519 keygen", "X25519", NULL, 0, N_X25519_KEYGEN),
      bench_keygen(soft, "X25519 keygen", "X25519", NULL, 0, N_X25519_KEYGEN));
  row("X25519 derive",
      bench_derive(NULL, "X25519 derive", "X25519", NULL, N_X25519_DERIVE),
      bench_derive(soft, "X25519 derive", "X25519", NULL, N_X25519_DERIVE));
  row("P-256 keygen",
      bench_keygen(NULL, "P-256 keygen", "EC", "P-256", 0, N_P256_KEYGEN),
      bench_keygen(soft, "P-256 keygen", "EC", "P-256", 0, N_P256_KEYGEN));
  row("P-256 derive",
      bench_derive(NULL, "P-256 derive", "EC", "P-256", N_P256_DERIVE),
      bench_derive(soft, "P-256 derive", "EC", "P-256", N_P256_DERIVE));

  /* Certificate verification */
  row("ECDSA-P256 verify",
      bench_verify(NULL, "ECDSA-P256 verify", "EC", "P-256", 0, N_ECDSA_VERIFY),
      bench_verify(soft, "ECDSA-P256 verify", "EC", "P-256", 0, N_ECDSA_VERIFY));
  row("RSA-2048 verify",
      bench_verify(NULL, "RSA-2048 verify", "RSA", NULL, 2048, N_RSA_VERIFY),
      bench_verify(soft, "RSA-2048 verify", "RSA", NULL, 2048, N_RSA_VERIFY));

  run_rsa_kat(soft);

  run_aead_sweep(soft);

  printf("  ------------------------------------------------------------\n");
  printf("Full TLS 1.3 handshakes (client offload vs client software; the\n");
  printf("server side is always software). This is the real handshake shape.\n");

  /* The certs/keys are the server's; make them once in the software context. */
  if (make_cert(soft, "EC", &ec_key, &ec_cert)) {
    row("handshake EC/P-256",
        bench_handshake(NULL, soft, "handshake EC/P-256", ec_key, ec_cert,
                        "P-256", N_HANDSHAKE),
        bench_handshake(soft, soft, "handshake EC/P-256", ec_key, ec_cert,
                        "P-256", N_HANDSHAKE));
  } else {
    printf("  handshake EC/P-256     cert setup FAILED\n");
  }
  if (make_cert(soft, "RSA", &rsa_key, &rsa_cert)) {
    row("handshake RSA/P-256",
        bench_handshake(NULL, soft, "handshake RSA/P-256", rsa_key, rsa_cert,
                        "P-256", N_HANDSHAKE),
        bench_handshake(soft, soft, "handshake RSA/P-256", rsa_key, rsa_cert,
                        "P-256", N_HANDSHAKE));
  } else {
    printf("  handshake RSA/P-256    cert setup FAILED\n");
  }

  EVP_PKEY_free(ec_key);
  EVP_PKEY_free(rsa_key);
  X509_free(ec_cert);
  X509_free(rsa_cert);
}

int main(int argc, char **argv)
{
  OSSL_LIB_CTX *soft = NULL;
  int rc = 20;
  /* Mode select from argv[1], so a targeted probe skips the MINUTES of untimed
   * software RSA/ECDSA keygen the full profile spends on a 68k:
   *   "aead" / a  -> only the AEAD size sweep
   *   "rsa"  / r  -> only the RSA-2048 verify KAT (keygen-free; the delegated
   *                  PSS path is the TLS 1.3 RSA CertificateVerify)
   *   (none)      -> full profile */
  char mode = (argc > 1 && argv[1] != NULL) ? argv[1][0] : '\0';
  int aead_only = (mode == 'a' || mode == 'A');
  int rsa_only = (mode == 'r' || mode == 'R');

  if (!init_amissl()) {
    goto out;
  }
  timer_open();

  printf("ZZ9000 AmiSSL crypto-path profiler%s%s\n",
         aead_only ? " (AEAD sweep only)" : "",
         rsa_only ? " (RSA verify KAT only)" : "");
  printf("Library: %s | %s\n", OpenSSL_version(OPENSSL_VERSION),
         OpenSSL_version(OPENSSL_BUILT_ON));
  printf("Timer: %s (%lu ticks/s)\n",
         g_timer_hires ? "timer.device MICROHZ EClock" : "COARSE (50 Hz)",
         (unsigned long)g_ticks_per_second);
  report_offload_provider();
  printf("--------------------------------------------------------------\n");

  /* Software reference context: a private library context with only the
   * default provider, so nothing routes to the board. The offload context is
   * the default (NULL) context where the drop-in amissl.library set
   * ?provider=zz9000 at InitAmiSSL. */
  soft = OSSL_LIB_CTX_new();
  if (soft == NULL || OSSL_PROVIDER_load(soft, "default") == NULL) {
    printf("Couldn't create the software reference context\n");
    print_errors("soft-ctx");
    goto out;
  }

  if (aead_only) {
    run_aead_sweep(soft);
  } else if (rsa_only) {
    run_rsa_kat(soft);
  } else {
    run_profile(soft);
  }
  rc = 0;

out:
  if (soft) OSSL_LIB_CTX_free(soft);
  timer_close();
  cleanup();
  return rc;
}
