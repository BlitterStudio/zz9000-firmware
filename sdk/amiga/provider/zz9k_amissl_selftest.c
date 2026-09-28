/*
 * ZZ9000 OpenSSL provider — AmiSSL hardware self-test.
 *
 * A standalone AmigaOS 3 (m68k) program that initialises AmiSSL, registers the
 * ZZ9000 provider, and exercises the offload through the ordinary AmiSSL EVP
 * API. For each AEAD it:
 *
 *   1. fetches the cipher with the default "?provider=zz9000" query and prints
 *      which provider actually serves it (this is the proof the fetch resolved
 *      to the hardware path),
 *   2. encrypts a test message through that cipher (offloaded on the ZZ9000),
 *   3. decrypts the result with a cipher pinned to "provider=default" (AmiSSL's
 *      software implementation), and checks the recovered plaintext and the
 *      authentication tag.
 *
 * Step 3 cross-validates the offloaded encryption against the independent
 * software reference, so a correct round trip means the ZZ9000 produced a
 * standards-conformant ciphertext and tag — no memorised test vector needed.
 *
 * AmiSSL is opened with AmiSSL_UsesOpenSSLStructs = TRUE because the provider
 * allocates non-opaque OpenSSL structures (OSSL_PARAM / OSSL_DISPATCH /
 * OSSL_ALGORITHM) directly. Both the main (AmiSSLBase) and extension
 * (AmiSSLExtBase, which carries OSSL_PROVIDER_*) bases are fetched.
 *
 * Build it together with the provider objects and define ZZ9K_PROVIDER_OFFLOAD;
 * see docs/zz9k-amissl-provider.md for the full command line.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/socket.h>
#include <proto/amissl.h>
#include <proto/amisslmaster.h>

#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <amissl/amissl.h>

#include <openssl/evp.h>
#include <openssl/provider.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "zz9k_amissl.h"

/* Library bases. proto/amissl.h externs AmiSSLBase and AmiSSLExtBase; this
 * translation unit provides their single definition. */
struct Library *AmiSSLMasterBase, *AmiSSLBase, *AmiSSLExtBase, *SocketBase;

static int amissl_open = 0;

static const char *provider_of_cipher(EVP_CIPHER *c)
{
  const OSSL_PROVIDER *p = c ? EVP_CIPHER_get0_provider(c) : NULL;
  return p ? OSSL_PROVIDER_get0_name(p) : "(none)";
}

/* AES-256-GCM / ChaCha20-Poly1305 round trip: encrypt with `enc_props`, decrypt
 * with `dec_props`. Returns 1 if the recovered plaintext matches and the tag
 * verifies. */
static int aead_roundtrip(const char *alg, const char *enc_props,
                          const char *dec_props, unsigned int keylen)
{
  static const unsigned char key32[32] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f
  };
  static const unsigned char iv12[12] = {
    0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad,0xde,0xca,0xf8,0x88
  };
  static const unsigned char aad[16] = {
    0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef
  };
  static const unsigned char pt[40] = {
    'Z','Z','9','0','0','0',' ','A','m','i','S','S','L',' ','o','f',
    'f','l','o','a','d',' ','s','e','l','f','-','t','e','s','t',' ','m','s','g',
    '!','!','!','!','!'
  };
  unsigned char ct[64], rt[64], tag[16];
  EVP_CIPHER *enc = NULL, *dec = NULL;
  EVP_CIPHER_CTX *ectx = NULL, *dctx = NULL;
  int outl = 0, tmpl = 0, ok = 0;

  enc = EVP_CIPHER_fetch(NULL, alg, enc_props);
  dec = EVP_CIPHER_fetch(NULL, alg, dec_props);
  if (enc == NULL || dec == NULL) {
    printf("  %-22s FAIL (fetch: enc=%p dec=%p)\n", alg, (void *)enc,
           (void *)dec);
    goto done;
  }
  printf("  %-22s encrypt via '%s', decrypt via '%s'\n", alg,
         provider_of_cipher(enc), provider_of_cipher(dec));

  /* Encrypt. */
  ectx = EVP_CIPHER_CTX_new();
  if (ectx == NULL ||
      !EVP_EncryptInit_ex2(ectx, enc, key32, iv12, NULL) ||
      !EVP_EncryptUpdate(ectx, NULL, &outl, aad, (int)sizeof(aad)) ||
      !EVP_EncryptUpdate(ectx, ct, &outl, pt, (int)sizeof(pt)) ||
      !EVP_EncryptFinal_ex(ectx, ct + outl, &tmpl) ||
      !EVP_CIPHER_CTX_ctrl(ectx, EVP_CTRL_AEAD_GET_TAG, 16, tag)) {
    printf("  %-22s FAIL (encrypt)\n", alg);
    goto done;
  }

  /* Decrypt with the cross-check provider, driving the AEAD in the SAME order
   * libssl's record layer uses for a received record: set the expected tag
   * immediately after init, BEFORE the AAD/data updates. That is exactly what
   * TLS 1.3 RX does (ssl/record/methods/tls13_meth.c sets EVP_CTRL_AEAD_SET_TAG
   * right after EVP_CipherInit_ex, then updates AAD then data), and it is the
   * order a one-shot offload decrypt needs the tag for. (The documented generic
   * EVP idiom sets the tag AFTER the data update; no TLS version drives decrypt
   * that way, and the provider's one-shot decrypt path deliberately rejects it.)
   * With the tag up front this exercises the board's offload decrypt+verify —
   * the operation a browser performs on every server response. */
  dctx = EVP_CIPHER_CTX_new();
  if (dctx == NULL ||
      !EVP_DecryptInit_ex2(dctx, dec, key32, iv12, NULL) ||
      !EVP_CIPHER_CTX_ctrl(dctx, EVP_CTRL_AEAD_SET_TAG, 16, tag) ||
      !EVP_DecryptUpdate(dctx, NULL, &outl, aad, (int)sizeof(aad)) ||
      !EVP_DecryptUpdate(dctx, rt, &outl, ct, (int)sizeof(pt))) {
    printf("  %-22s FAIL (decrypt setup)\n", alg);
    goto done;
  }
  if (EVP_DecryptFinal_ex(dctx, rt + outl, &tmpl) != 1) {
    printf("  %-22s FAIL (tag rejected)\n", alg);
    goto done;
  }
  if (memcmp(rt, pt, sizeof(pt)) != 0) {
    printf("  %-22s FAIL (plaintext mismatch)\n", alg);
    goto done;
  }
  printf("  %-22s PASS (keylen=%u)\n", alg, keylen);
  ok = 1;

done:
  EVP_CIPHER_CTX_free(ectx);
  EVP_CIPHER_CTX_free(dctx);
  EVP_CIPHER_free(enc);
  EVP_CIPHER_free(dec);
  return ok;
}

/* X25519 key agreement across providers: keypair A through the default
 * "?provider=zz9000" query (keygen + derive offloaded on the board), keypair B
 * pinned to provider=default (AmiSSL software). Both directions must produce
 * the same shared secret, which proves the offloaded keygen/derive — the
 * exact operations a TLS key share uses — are mathematically correct against
 * an independent implementation. */
static int x25519_agreement(void)
{
  EVP_PKEY_CTX *gctx = NULL, *dctx = NULL;
  EVP_PKEY *a = NULL, *b = NULL, *apub = NULL, *bpub = NULL;
  EVP_KEYMGMT *km = NULL;
  unsigned char *aenc = NULL, *benc = NULL;
  unsigned char sec_a[32], sec_b[32];
  size_t alen = 0, blen = 0, na = sizeof(sec_a), nb = sizeof(sec_b);
  int ok = 0;

  km = EVP_KEYMGMT_fetch(NULL, "X25519", NULL);
  printf("  %-22s keymgmt via '%s'\n", "X25519",
         km ? OSSL_PROVIDER_get0_name(EVP_KEYMGMT_get0_provider(km))
            : "(fetch failed)");

  /* A: preferred provider (zz9000 when the board is present). */
  gctx = EVP_PKEY_CTX_new_from_name(NULL, "X25519", NULL);
  if (gctx == NULL || EVP_PKEY_keygen_init(gctx) <= 0 ||
      EVP_PKEY_keygen(gctx, &a) <= 0) {
    printf("  %-22s FAIL (keygen via preferred provider)\n", "X25519");
    goto done;
  }
  EVP_PKEY_CTX_free(gctx);
  /* B: pinned software default. */
  gctx = EVP_PKEY_CTX_new_from_name(NULL, "X25519", "provider=default");
  if (gctx == NULL || EVP_PKEY_keygen_init(gctx) <= 0 ||
      EVP_PKEY_keygen(gctx, &b) <= 0) {
    printf("  %-22s FAIL (keygen via provider=default)\n", "X25519");
    goto done;
  }

  alen = EVP_PKEY_get1_encoded_public_key(a, &aenc);
  blen = EVP_PKEY_get1_encoded_public_key(b, &benc);
  if (alen != 32 || blen != 32) {
    printf("  %-22s FAIL (encoded pub: a=%lu b=%lu)\n", "X25519",
           (unsigned long)alen, (unsigned long)blen);
    goto done;
  }

  /* A derives with the preferred provider against B's public key... */
  bpub = EVP_PKEY_new_raw_public_key_ex(NULL, "X25519", NULL, benc, blen);
  dctx = EVP_PKEY_CTX_new_from_pkey(NULL, a, NULL);
  if (bpub == NULL || dctx == NULL || EVP_PKEY_derive_init(dctx) <= 0 ||
      EVP_PKEY_derive_set_peer(dctx, bpub) <= 0 ||
      EVP_PKEY_derive(dctx, sec_a, &na) <= 0) {
    printf("  %-22s FAIL (derive via preferred provider)\n", "X25519");
    goto done;
  }
  EVP_PKEY_CTX_free(dctx);
  /* ...and B derives with the software default against A's public key. */
  apub = EVP_PKEY_new_raw_public_key_ex(NULL, "X25519", "provider=default",
                                        aenc, alen);
  dctx = EVP_PKEY_CTX_new_from_pkey(NULL, b, "provider=default");
  if (apub == NULL || dctx == NULL || EVP_PKEY_derive_init(dctx) <= 0 ||
      EVP_PKEY_derive_set_peer(dctx, apub) <= 0 ||
      EVP_PKEY_derive(dctx, sec_b, &nb) <= 0) {
    printf("  %-22s FAIL (derive via provider=default)\n", "X25519");
    goto done;
  }
  if (na != 32 || nb != 32 || memcmp(sec_a, sec_b, 32) != 0) {
    printf("  %-22s FAIL (shared secrets DISAGREE - offloaded X25519 is "
           "producing wrong results)\n", "X25519");
    goto done;
  }
  printf("  %-22s PASS (cross-provider key agreement)\n", "X25519");
  ok = 1;

done:
  EVP_PKEY_CTX_free(gctx);
  EVP_PKEY_CTX_free(dctx);
  EVP_KEYMGMT_free(km);
  EVP_PKEY_free(a);
  EVP_PKEY_free(b);
  EVP_PKEY_free(apub);
  EVP_PKEY_free(bpub);
  OPENSSL_free(aenc);
  OPENSSL_free(benc);
  return ok;
}

/* RFC 8439 section 2.8.2 ChaCha20-Poly1305 known-answer vector, run through
 * EVP against a specific provider. Unlike a round trip, this compares the
 * ciphertext and tag byte-for-byte against the RFC, so it tells us WHICH side
 * of a failing round trip deviates from the standard, through the full
 * EVP/library stack on real hardware. */
static int chacha_evp_kat(const char *props)
{
  static const unsigned char kkey[32] = {
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f
  };
  static const unsigned char knonce[12] = {
    0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47
  };
  static const unsigned char kaad[12] = {
    0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7
  };
  static const unsigned char kpt[114] =
    "Ladies and Gentlemen of the class of '99: If I could offer "
    "you only one tip for the future, sunscreen would be it.";
  static const unsigned char kct[114] = {
    0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb,
    0x7b, 0x86, 0xaf, 0xbc, 0x53, 0xef, 0x7e, 0xc2,
    0xa4, 0xad, 0xed, 0x51, 0x29, 0x6e, 0x08, 0xfe,
    0xa9, 0xe2, 0xb5, 0xa7, 0x36, 0xee, 0x62, 0xd6,
    0x3d, 0xbe, 0xa4, 0x5e, 0x8c, 0xa9, 0x67, 0x12,
    0x82, 0xfa, 0xfb, 0x69, 0xda, 0x92, 0x72, 0x8b,
    0x1a, 0x71, 0xde, 0x0a, 0x9e, 0x06, 0x0b, 0x29,
    0x05, 0xd6, 0xa5, 0xb6, 0x7e, 0xcd, 0x3b, 0x36,
    0x92, 0xdd, 0xbd, 0x7f, 0x2d, 0x77, 0x8b, 0x8c,
    0x98, 0x03, 0xae, 0xe3, 0x28, 0x09, 0x1b, 0x58,
    0xfa, 0xb3, 0x24, 0xe4, 0xfa, 0xd6, 0x75, 0x94,
    0x55, 0x85, 0x80, 0x8b, 0x48, 0x31, 0xd7, 0xbc,
    0x3f, 0xf4, 0xde, 0xf0, 0x8e, 0x4b, 0x7a, 0x9d,
    0xe5, 0x76, 0xd2, 0x65, 0x86, 0xce, 0xc6, 0x4b,
    0x61, 0x16
  };
  static const unsigned char ktag[16] = {
    0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a,
    0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91
  };
  unsigned char ct[114], tag[16];
  EVP_CIPHER *c = NULL;
  EVP_CIPHER_CTX *ctx = NULL;
  int outl = 0, tmpl = 0, ok = 0, i;

  c = EVP_CIPHER_fetch(NULL, "ChaCha20-Poly1305", props);
  ctx = EVP_CIPHER_CTX_new();
  if (c == NULL || ctx == NULL) {
    printf("  chacha-kat[%-16s] FAIL (fetch)\n", props);
    goto done;
  }
  if (!EVP_EncryptInit_ex2(ctx, c, kkey, knonce, NULL) ||
      !EVP_EncryptUpdate(ctx, NULL, &outl, kaad, (int)sizeof(kaad)) ||
      !EVP_EncryptUpdate(ctx, ct, &outl, kpt, (int)sizeof(kpt)) ||
      !EVP_EncryptFinal_ex(ctx, ct + outl, &tmpl) ||
      !EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag)) {
    printf("  chacha-kat[%-16s] FAIL (EVP call, served by '%s')\n", props,
           provider_of_cipher(c));
    goto done;
  }
  for (i = 0; i < 114; i++) {
    if (ct[i] != kct[i]) {
      printf("  chacha-kat[%-16s] FAIL (ct first diff @%d: %02x != %02x, "
             "served by '%s')\n", props, i, ct[i], kct[i],
             provider_of_cipher(c));
      goto done;
    }
  }
  if (memcmp(tag, ktag, 16) != 0) {
    printf("  chacha-kat[%-16s] FAIL (ct ok, TAG wrong: "
           "%02x%02x%02x%02x... != %02x%02x%02x%02x..., served by '%s')\n",
           props, tag[0], tag[1], tag[2], tag[3], ktag[0], ktag[1], ktag[2],
           ktag[3], provider_of_cipher(c));
    goto done;
  }
  printf("  chacha-kat[%-16s] PASS (RFC 8439, served by '%s')\n", props,
         provider_of_cipher(c));
  ok = 1;

done:
  EVP_CIPHER_CTX_free(ctx);
  EVP_CIPHER_free(c);
  return ok;
}

static int run_tests(void)
{
  int ok = 1;
  printf("ZZ9000 AmiSSL provider self-test\n");
  /* Identify the EXACT library binary serving this run: the OpenSSL build
   * timestamp is baked into amissl.library at build time, so this line
   * proves whether a newly installed library is actually the one loaded
   * (AmigaOS keeps libraries resident — copying the file is not enough
   * without a reboot or Avail FLUSH). */
  printf("Library: %s | %s\n", OpenSSL_version(OPENSSL_VERSION),
         OpenSSL_version(OPENSSL_BUILT_ON));
  /* The zz9000 provider version is the unambiguous library marker (the
   * OpenSSL build stamp above does not change across incremental relinks). */
  {
    EVP_KEYMGMT *km = EVP_KEYMGMT_fetch(NULL, "X25519", NULL);
    if (km != NULL) {
      const OSSL_PROVIDER *pr = EVP_KEYMGMT_get0_provider(km);
      OSSL_PARAM p[2];
      const char *ver = NULL;
      memset(p, 0, sizeof(p));
      p[0].key = "version";
      p[0].data_type = OSSL_PARAM_UTF8_PTR;
      p[0].data = &ver;
      p[0].data_size = sizeof(ver);
      if (pr != NULL && OSSL_PROVIDER_get_params(pr, p) && ver != NULL) {
        printf("X25519 served by: %s, provider version %s\n",
               OSSL_PROVIDER_get0_name(pr), ver);
      }
      EVP_KEYMGMT_free(km);
    }
  }
  printf("--------------------------------\n");
  /* Key exchange first: this is what every TLS handshake does. */
  ok &= x25519_agreement();
  /* Encrypt via the default "?provider=zz9000" query (offloaded when the board
   * advertises the service), decrypt via the software default for cross-check. */
  ok &= aead_roundtrip("AES-256-GCM", NULL, "provider=default", 32);
  ok &= aead_roundtrip("ChaCha20-Poly1305", NULL, "provider=default", 32);
  /* ChaCha diagnosis: the RFC vector through EVP against each provider tells
   * us which implementation deviates; the 2x2 round-trip matrix localizes a
   * mismatch to the encrypt or decrypt side. */
  ok &= chacha_evp_kat("provider=zz9000");
  ok &= chacha_evp_kat("provider=default");
  ok &= aead_roundtrip("ChaCha20-Poly1305", "provider=zz9000",
                       "provider=zz9000", 32);
  ok &= aead_roundtrip("ChaCha20-Poly1305", "provider=default",
                       "provider=default", 32);
  ok &= aead_roundtrip("ChaCha20-Poly1305", "provider=default",
                       "provider=zz9000", 32);
  printf("--------------------------------\n");
  printf("Result: %s\n", ok ? "ALL PASS" : "FAILURES");
  return ok;
}

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
  /* OpenAmiSSLTags opens + initialises AmiSSL and returns both library bases.
   * UsesOpenSSLStructs TRUE: the provider references non-opaque OpenSSL structs
   * directly. CloseAmiSSL() later undoes all of this. */
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

static void cleanup(void)
{
  zz9k_amissl_unregister();
  if (amissl_open) {
    CloseAmiSSL();   /* also calls CleanupAmiSSL() */
    amissl_open = 0;
  }
  if (AmiSSLMasterBase) { CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL; }
  if (SocketBase) { CloseLibrary(SocketBase); SocketBase = NULL; }
}

int main(void)
{
  int rc = 20;

  if (!init_amissl()) {
    goto out;
  }
  if (!zz9k_amissl_register()) {
    printf("Couldn't register the ZZ9000 provider\n");
    goto out;
  }
  rc = run_tests() ? 0 : 5;

out:
  cleanup();
  return rc;
}
