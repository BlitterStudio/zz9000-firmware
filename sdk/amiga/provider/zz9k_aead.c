/*
 * ZZ9000 OpenSSL provider — AEAD record ciphers (AES-128/256-GCM and
 * ChaCha20-Poly1305).
 *
 * These wrap the SDK's one-shot AEAD primitives behind OpenSSL's streaming
 * cipher interface. Because AEAD ciphers report a block size of 1, EVP gives
 * the data update() a full-size output buffer but the final() a zero-size one
 * (see crypto/evp/evp_enc.c), so the whole record is processed in the data
 * update() and final() only completes the tag. AAD arrives in update() calls
 * with out == NULL and is accumulated; the data arrives in a single update()
 * with out != NULL (the TLS record pattern). For decryption the tag must be
 * set (via set_ctx_params) before the data update, which is how the TLS
 * receive path drives an AEAD cipher.
 *
 * zz9k_prov_aead() is the single offload hook: on the Amiga it will route to
 * zz9k_crypto_aead / zz9k_crypto_aead_batch; here and on the host it uses the
 * SDK software reference.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k_prov_local.h"

#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/evp.h>

#include "zz9k-crypto-soft.h"
#include "zz9k_offload.h"

#include <string.h>
#include <stdlib.h>

/* Cipher mode values mirror EVP_CIPH_GCM_MODE / EVP_CIPH_STREAM_CIPHER so the
 * EVP layer recognises the mode without pulling in <openssl/evp.h> here. */
#define ZZ9K_MODE_GCM    0x6
#define ZZ9K_MODE_STREAM 0x0

enum {
  ZZ9K_AEAD_AES128_GCM = 0,
  ZZ9K_AEAD_AES256_GCM,
  ZZ9K_AEAD_CHACHA20_POLY1305
};

#define ZZ9K_AEAD_IVLEN  12
#define ZZ9K_AEAD_TAGLEN 16

/* Record size (bytes) at/above which an AEAD record offloads to the board;
 * smaller records run in the default provider's software. The offload is a flat
 * ~7 ms mailbox round trip that only beats the m68k software above ~1.6 KiB
 * (measured, tools/zz9k-cryptoprofile.c "aead" sweep). 2 KiB is throughput-
 * neutral at the crossover and, by keeping the many small TLS records a page
 * load emits off the doorbell, cuts the round-trip count — and the contention
 * exposure under display load — to just the bulk records offload actually wins.
 * Override with ENV:ZZ9K_AEAD_OFFLOAD_MIN to retune on hardware without a
 * rebuild (see zz9k_aead_offload_min). */
#define ZZ9K_AEAD_OFFLOAD_MIN_BYTES 2048

/* TLS 1.2 record framing (RFC 5288 / RFC 7905). */
#define ZZ9K_TLS_AAD_LEN     13
#define ZZ9K_TLS_EXPLICIT_IV 8
#define ZZ9K_TLS_TAG_LEN     16

typedef struct {
  ZZ9K_PROV_CTX *provctx;
  int alg;
  size_t keylen;
  size_t ivlen;
  size_t taglen;
  unsigned char key[32];
  unsigned char iv[ZZ9K_AEAD_IVLEN];
  unsigned char tag[ZZ9K_AEAD_TAGLEN];
  int enc;
  int have_key;
  int have_iv;
  int tag_set;
  int done;             /* the single data update has been processed */
  unsigned char *aad;
  size_t aadlen;
  size_t aadcap;
  /* TLS 1.2 record-layer state, driven by libssl through
   * EVP_CTRL_AEAD_SET_IV_FIXED / EVP_CTRL_AEAD_TLS1_AAD. For GCM `tls_iv` is
   * the working 12-byte nonce (4-byte salt + 8-byte invocation field,
   * randomised at SET_IV_FIXED and incremented per record); for ChaCha it is
   * the full 12-byte fixed IV, XORed with the record sequence number per
   * record. `tls_aad_set` arms the one-shot record path for the next data
   * update and is cleared after each record. */
  unsigned char tls_aad[ZZ9K_TLS_AAD_LEN];
  unsigned char tls_iv[ZZ9K_AEAD_IVLEN];
  int tls_iv_set;
  int tls_aad_set;
} ZZ9K_AEAD_CTX;

#ifdef ZZ9K_PROVIDER_OFFLOAD
/* EVP algorithm name for a zz9k AEAD id, used to fetch the software cipher. */
static const char *zz9k_aead_evp_name(int alg)
{
  switch (alg) {
  case ZZ9K_AEAD_AES128_GCM:        return "AES-128-GCM";
  case ZZ9K_AEAD_AES256_GCM:        return "AES-256-GCM";
  case ZZ9K_AEAD_CHACHA20_POLY1305: return "ChaCha20-Poly1305";
  default:                          return NULL;
  }
}

/* The record-size offload threshold, read once from ENV:ZZ9K_AEAD_OFFLOAD_MIN
 * (bytes) so the crossover can be retuned on hardware without a rebuild;
 * defaults to the measured ZZ9K_AEAD_OFFLOAD_MIN_BYTES. getenv on AmigaOS hits
 * ENV: so it is cached rather than read per record. */
static size_t zz9k_aead_offload_min(void)
{
  static long cached = -1;   /* -1 = not yet read */
  if (cached < 0) {
    const char *e = getenv("ZZ9K_AEAD_OFFLOAD_MIN");
    long v = (e != NULL) ? atol(e) : (long)ZZ9K_AEAD_OFFLOAD_MIN_BYTES;
    cached = (v < 0) ? 0 : v;
  }
  return (size_t)cached;
}

/* One-shot AEAD through the default (AmiSSL software) provider, pinned with
 * "provider=default" so the fetch cannot bridge back into this provider. This
 * serves both the small-record path (below the offload threshold) and the
 * offload-miss fallback. It is deliberately NOT the zz9k_soft_* reference the
 * host build uses below: that reference miscomputes ChaCha tags inside the
 * base-relative amissl.library (root cause undetermined), whereas the default
 * provider is validated in-lib by zz9k_amissl_selftest's
 * chacha_evp_kat("provider=default"). Same contract as zz9k_prov_aead: `tag` is
 * an output for encrypt and an input for decrypt; the tag length is the AEAD
 * default 16 — the only length the TLS record layer and the board offload use.
 * Returns 1 on success (decrypt: tag verified), 0 otherwise. */
static int zz9k_aead_default_oneshot(int alg, int enc, const unsigned char *key,
                                     size_t keylen, const unsigned char *iv,
                                     const unsigned char *aad, size_t aadlen,
                                     const unsigned char *in, size_t inlen,
                                     unsigned char *out, unsigned char *tag,
                                     ZZ9K_PROV_CTX *provctx)
{
  const char *name = zz9k_aead_evp_name(alg);
  EVP_CIPHER *cipher = NULL;
  EVP_CIPHER_CTX *ctx = NULL;
  int outl = 0, tmpl = 0, ok = 0;

  (void)keylen;   /* the cipher name fixes the key length */
  if (name == NULL) {
    return 0;
  }
  cipher = EVP_CIPHER_fetch(provctx != NULL ? provctx->libctx : NULL, name,
                            "provider=default");
  ctx = EVP_CIPHER_CTX_new();
  if (cipher == NULL || ctx == NULL) {
    goto done;
  }
  if (enc) {
    if (!EVP_EncryptInit_ex2(ctx, cipher, key, iv, NULL) ||
        (aadlen > 0 &&
         !EVP_EncryptUpdate(ctx, NULL, &outl, aad, (int)aadlen)) ||
        !EVP_EncryptUpdate(ctx, out, &outl, in, (int)inlen) ||
        !EVP_EncryptFinal_ex(ctx, out + outl, &tmpl) ||
        !EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, ZZ9K_AEAD_TAGLEN,
                             tag)) {
      goto done;
    }
    ok = 1;
  } else {
    if (!EVP_DecryptInit_ex2(ctx, cipher, key, iv, NULL) ||
        !EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, ZZ9K_AEAD_TAGLEN,
                             tag) ||
        (aadlen > 0 &&
         !EVP_DecryptUpdate(ctx, NULL, &outl, aad, (int)aadlen)) ||
        !EVP_DecryptUpdate(ctx, out, &outl, in, (int)inlen)) {
      goto done;
    }
    /* Final verifies the tag: 1 = authentic, 0 = reject. */
    ok = (EVP_DecryptFinal_ex(ctx, out + outl, &tmpl) == 1);
  }

done:
  EVP_CIPHER_CTX_free(ctx);
  EVP_CIPHER_free(cipher);
  return ok;
}
#endif /* ZZ9K_PROVIDER_OFFLOAD */

/* One-shot AEAD over a whole record. `tag` is an output for encrypt and an
 * input for decrypt. Returns 1 on success (decrypt: tag valid), 0 otherwise. */
static int zz9k_prov_aead(int alg, int enc, const unsigned char *key,
                          size_t keylen, const unsigned char *iv,
                          const unsigned char *aad, size_t aadlen,
                          const unsigned char *in, size_t inlen,
                          unsigned char *out, unsigned char *tag,
                          ZZ9K_PROV_CTX *provctx)
{
#ifdef ZZ9K_PROVIDER_OFFLOAD
  /* On the Amiga the AEAD ciphers are only advertised when the board's crypto
   * service is live (see zz9k_query_operation), and AES-GCM additionally only
   * with its capability flag, because AEAD firmware that predates the algorithm
   * bits in the descriptor flags treats them as don't-care and would run the
   * legacy ChaCha20-Poly1305 while reporting success.
   *
   * Records offload only at/above the size threshold. The offload is a flat
   * ~7 ms mailbox round trip; below ~1.6 KiB the m68k software is faster, and
   * far more importantly, the many small records a page load emits would each
   * be a doorbell round trip — hundreds per page — which under display load
   * (core-0's completion poll starved) balloon and, with no fallback, fail a
   * record into a browser retry/timeout cascade. Small records therefore run
   * in the default provider's software (no mailbox, correct crypto).
   *
   * A failed offload (allocation/mailbox error/timeout, or a decrypt whose tag
   * the board rejected — all return non-positive) also falls back to the
   * default provider rather than failing the record: for a valid record this
   * recovers it, so a display-load stall no longer kills the connection; for a
   * genuinely bad decrypt tag the software path rejects it too, so
   * authentication is preserved. The fallback is the default provider, NOT the
   * zz9k_soft_* reference below — that reference computes wrong ChaCha tags
   * inside the base-relative amissl.library despite passing its KATs in every
   * standalone build (root cause undetermined). The host build (no
   * ZZ9K_PROVIDER_OFFLOAD) runs the software reference below, which the parity
   * tests cover. */
  {
    int aes = (alg != ZZ9K_AEAD_CHACHA20_POLY1305);
    int r;
    if (inlen < zz9k_aead_offload_min() ||
        (aes && !ZZ9K_PROV_CAN_OFFLOAD(provctx,
                                       ZZ9K_SERVICE_FLAG_CRYPTO_AES_GCM)) ||
        !ZZ9K_PROV_CAN_OFFLOAD_SERVICE(provctx)) {
      return zz9k_aead_default_oneshot(alg, enc, key, keylen, iv, aad, aadlen,
                                       in, inlen, out, tag, provctx);
    }
    r = zz9k_offload_aead(provctx->sdk_ctx, aes, (unsigned int)keylen, !enc,
                          key, iv, aad, (unsigned int)aadlen, in,
                          (unsigned int)inlen, out, tag);
    if (r > 0) {
      return 1;
    }
    return zz9k_aead_default_oneshot(alg, enc, key, keylen, iv, aad, aadlen,
                                     in, inlen, out, tag, provctx);
  }
#else
  (void)provctx;
#endif
  switch (alg) {
  case ZZ9K_AEAD_AES128_GCM:
  case ZZ9K_AEAD_AES256_GCM:
    if (enc) {
      return zz9k_soft_aes_gcm_encrypt(out, tag, in, (uint32_t)inlen, aad,
                                       (uint32_t)aadlen, key, (uint32_t)keylen,
                                       iv);
    }
    return zz9k_soft_aes_gcm_decrypt(out, in, (uint32_t)inlen, aad,
                                     (uint32_t)aadlen, tag, key,
                                     (uint32_t)keylen, iv);
  case ZZ9K_AEAD_CHACHA20_POLY1305:
    if (enc) {
      zz9k_soft_chacha20_poly1305_encrypt(out, tag, in, (uint32_t)inlen, aad,
                                          (uint32_t)aadlen, key, iv);
      return 1;
    }
    return zz9k_soft_chacha20_poly1305_decrypt(out, in, (uint32_t)inlen, aad,
                                               (uint32_t)aadlen, tag, key, iv);
  default:
    return 0;
  }
}

/* Increment the 64-bit big-endian invocation field of a GCM TLS nonce. */
static void zz9k_ctr64_inc(unsigned char *counter)
{
  int n = 8;
  while (n > 0) {
    if (++counter[--n] != 0) {
      return;
    }
  }
}

/* One TLS 1.2 record, armed by the EVP_CTRL_AEAD_TLS1_AAD control. Mirrors
 * the default provider's gcm_tls_cipher / chacha20-poly1305 TLS path: GCM
 * records are explicit-IV(8) || payload || tag(16) with the nonce salt(4) ||
 * explicit-IV; ChaCha records are payload || tag(16) with the nonce = fixed IV
 * XOR record sequence number (RFC 7905). Encrypt returns the whole record in
 * *outl, decrypt returns the payload length, written after the explicit IV —
 * exactly the contract libssl's record layer expects. */
static int zz9k_aead_tls_record(ZZ9K_AEAD_CTX *ctx, unsigned char *out,
                                size_t *outl, size_t outsize,
                                const unsigned char *in, size_t inl)
{
  unsigned char nonce[ZZ9K_AEAD_IVLEN];
  unsigned char tag[ZZ9K_TLS_TAG_LEN];
  size_t payload;
  int i;

  ctx->tls_aad_set = 0;            /* one record per AAD control */
  if (!ctx->have_key || out == NULL) {
    return 0;
  }
  if (ctx->alg != ZZ9K_AEAD_CHACHA20_POLY1305) {
    /* GCM: the working nonce (salt + invocation field) is delivered through
     * EVP_CTRL_AEAD_SET_IV_FIXED (-> tls_iv) by the record layer. */
    if (!ctx->tls_iv_set) {
      return 0;
    }
    if (inl < ZZ9K_TLS_EXPLICIT_IV + ZZ9K_TLS_TAG_LEN || outsize < inl) {
      return 0;
    }
    payload = inl - ZZ9K_TLS_EXPLICIT_IV - ZZ9K_TLS_TAG_LEN;
    if (ctx->enc) {
      /* The explicit IV is the current invocation field; bump it after use. */
      memcpy(nonce, ctx->tls_iv, ZZ9K_AEAD_IVLEN);
      memcpy(out, nonce + 4, ZZ9K_TLS_EXPLICIT_IV);
      zz9k_ctr64_inc(ctx->tls_iv + 4);
      if (!zz9k_prov_aead(ctx->alg, 1, ctx->key, ctx->keylen, nonce,
                          ctx->tls_aad, ZZ9K_TLS_AAD_LEN,
                          in + ZZ9K_TLS_EXPLICIT_IV, payload,
                          out + ZZ9K_TLS_EXPLICIT_IV, tag, ctx->provctx)) {
        return 0;
      }
      memcpy(out + ZZ9K_TLS_EXPLICIT_IV + payload, tag, ZZ9K_TLS_TAG_LEN);
      *outl = inl;
      return 1;
    }
    memcpy(nonce, ctx->tls_iv, 4);
    memcpy(nonce + 4, in, ZZ9K_TLS_EXPLICIT_IV);
    memcpy(tag, in + ZZ9K_TLS_EXPLICIT_IV + payload, ZZ9K_TLS_TAG_LEN);
    if (!zz9k_prov_aead(ctx->alg, 0, ctx->key, ctx->keylen, nonce,
                        ctx->tls_aad, ZZ9K_TLS_AAD_LEN,
                        in + ZZ9K_TLS_EXPLICIT_IV, payload,
                        out + ZZ9K_TLS_EXPLICIT_IV, tag, ctx->provctx)) {
      return 0;
    }
    *outl = payload;
    return 1;
  }
  /* ChaCha20-Poly1305 (RFC 7905). Unlike GCM, the record layer delivers the
   * 12-byte fixed IV through the ordinary cipher init (-> ctx->iv), not
   * SET_IV_FIXED, so the per-record nonce is that fixed IV XOR the record
   * sequence number (the first 8 bytes of the TLS AAD). */
  if (!ctx->have_iv) {
    return 0;
  }
  if (inl < ZZ9K_TLS_TAG_LEN || outsize < inl) {
    return 0;
  }
  payload = inl - ZZ9K_TLS_TAG_LEN;
  memcpy(nonce, ctx->iv, ZZ9K_AEAD_IVLEN);
  for (i = 0; i < 8; i++) {
    nonce[4 + i] ^= ctx->tls_aad[i];   /* sequence number */
  }
  if (ctx->enc) {
    if (!zz9k_prov_aead(ctx->alg, 1, ctx->key, ctx->keylen, nonce,
                        ctx->tls_aad, ZZ9K_TLS_AAD_LEN, in, payload, out, tag,
                        ctx->provctx)) {
      return 0;
    }
    memcpy(out + payload, tag, ZZ9K_TLS_TAG_LEN);
    *outl = inl;
    return 1;
  }
  memcpy(tag, in + payload, ZZ9K_TLS_TAG_LEN);
  if (!zz9k_prov_aead(ctx->alg, 0, ctx->key, ctx->keylen, nonce, ctx->tls_aad,
                      ZZ9K_TLS_AAD_LEN, in, payload, out, tag, ctx->provctx)) {
    return 0;
  }
  *outl = payload;
  return 1;
}

/* ---- context lifecycle ---- */

static void *zz9k_aead_newctx(void *provctx, int alg, size_t keylen)
{
  ZZ9K_AEAD_CTX *ctx = OPENSSL_zalloc(sizeof(*ctx));
  if (ctx != NULL) {
    ctx->provctx = provctx;
    ctx->alg = alg;
    ctx->keylen = keylen;
    ctx->ivlen = ZZ9K_AEAD_IVLEN;
    ctx->taglen = ZZ9K_AEAD_TAGLEN;
  }
  return ctx;
}

static void *zz9k_aes128gcm_newctx(void *p)
{
  return zz9k_aead_newctx(p, ZZ9K_AEAD_AES128_GCM, 16);
}
static void *zz9k_aes256gcm_newctx(void *p)
{
  return zz9k_aead_newctx(p, ZZ9K_AEAD_AES256_GCM, 32);
}
static void *zz9k_chachapoly_newctx(void *p)
{
  return zz9k_aead_newctx(p, ZZ9K_AEAD_CHACHA20_POLY1305, 32);
}

static void zz9k_aead_freectx(void *vctx)
{
  ZZ9K_AEAD_CTX *ctx = vctx;
  if (ctx != NULL) {
    OPENSSL_clear_free(ctx->aad, ctx->aadcap);
    OPENSSL_cleanse(ctx, sizeof(*ctx));
    OPENSSL_free(ctx);
  }
}

/* ---- parameters ---- */

static int zz9k_aead_set_ctx_params(void *vctx, const OSSL_PARAM params[]);

static int zz9k_aead_init(void *vctx, const unsigned char *key, size_t keylen,
                          const unsigned char *iv, size_t ivlen,
                          const OSSL_PARAM params[], int enc)
{
  ZZ9K_AEAD_CTX *ctx = vctx;

  if (ctx == NULL) {
    return 0;
  }
  ctx->enc = enc;
  ctx->done = 0;
  ctx->aadlen = 0;
  ctx->tls_aad_set = 0;
  if (iv != NULL) {
    if (ivlen != ctx->ivlen) {
      return 0;
    }
    memcpy(ctx->iv, iv, ivlen);
    ctx->have_iv = 1;
  }
  if (key != NULL) {
    if (keylen != ctx->keylen) {
      return 0;
    }
    memcpy(ctx->key, key, keylen);
    ctx->have_key = 1;
  }
  return zz9k_aead_set_ctx_params(ctx, params);
}

static int zz9k_aead_encrypt_init(void *vctx, const unsigned char *key,
                                  size_t keylen, const unsigned char *iv,
                                  size_t ivlen, const OSSL_PARAM params[])
{
  return zz9k_aead_init(vctx, key, keylen, iv, ivlen, params, 1);
}

static int zz9k_aead_decrypt_init(void *vctx, const unsigned char *key,
                                  size_t keylen, const unsigned char *iv,
                                  size_t ivlen, const OSSL_PARAM params[])
{
  return zz9k_aead_init(vctx, key, keylen, iv, ivlen, params, 0);
}

static int zz9k_aead_update(void *vctx, unsigned char *out, size_t *outl,
                            size_t outsize, const unsigned char *in,
                            size_t inl)
{
  ZZ9K_AEAD_CTX *ctx = vctx;

  if (ctx == NULL) {
    return 0;
  }
  if (out == NULL) {                 /* AAD */
    if (inl > 0) {
      if (ctx->aadlen + inl > ctx->aadcap) {
        size_t ncap = ctx->aadlen + inl;
        unsigned char *na = OPENSSL_realloc(ctx->aad, ncap);
        if (na == NULL) {
          return 0;
        }
        ctx->aad = na;
        ctx->aadcap = ncap;
      }
      memcpy(ctx->aad + ctx->aadlen, in, inl);
      ctx->aadlen += inl;
    }
    *outl = inl;
    return 1;
  }
  /* TLS 1.2 record path: armed by EVP_CTRL_AEAD_TLS1_AAD, one whole record
   * per update, reusable across records without re-init. */
  if (ctx->tls_aad_set) {
    return zz9k_aead_tls_record(ctx, out, outl, outsize, in, inl);
  }
  /* Record data: processed in a single one-shot call. */
  if (ctx->done || !ctx->have_key || !ctx->have_iv || outsize < inl) {
    return 0;
  }
  if (!ctx->enc && !ctx->tag_set) {  /* decrypt needs the tag up front */
    return 0;
  }
  if (!zz9k_prov_aead(ctx->alg, ctx->enc, ctx->key, ctx->keylen, ctx->iv,
                      ctx->aad, ctx->aadlen, in, inl, out, ctx->tag,
                      ctx->provctx)) {
    return 0;
  }
  ctx->done = 1;
  *outl = inl;
  return 1;
}

static int zz9k_aead_final(void *vctx, unsigned char *out, size_t *outl,
                           size_t outsize)
{
  ZZ9K_AEAD_CTX *ctx = vctx;

  (void)out;
  (void)outsize;
  if (ctx == NULL) {
    return 0;
  }
  /* Empty-message AEAD: no data update happened, still authenticate. */
  if (!ctx->done) {
    unsigned char dummy[1] = { 0 };
    if (!ctx->have_key || !ctx->have_iv) {
      return 0;
    }
    if (!ctx->enc && !ctx->tag_set) {
      return 0;
    }
    if (!zz9k_prov_aead(ctx->alg, ctx->enc, ctx->key, ctx->keylen, ctx->iv,
                        ctx->aad, ctx->aadlen, dummy, 0, dummy, ctx->tag,
                        ctx->provctx)) {
      return 0;
    }
    ctx->done = 1;
  }
  *outl = 0;
  return 1;
}

static int zz9k_aead_get_ctx_params(void *vctx, OSSL_PARAM params[])
{
  ZZ9K_AEAD_CTX *ctx = vctx;
  OSSL_PARAM *p;

  p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_AEAD_TAG);
  if (p != NULL) {
    if (!ctx->enc || p->data_size > ctx->taglen) {
      return 0;
    }
    if (!OSSL_PARAM_set_octet_string(p, ctx->tag, p->data_size)) {
      return 0;
    }
  }
  p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_KEYLEN);
  if (p != NULL && !OSSL_PARAM_set_size_t(p, ctx->keylen)) {
    return 0;
  }
  p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_IVLEN);
  if (p != NULL && !OSSL_PARAM_set_size_t(p, ctx->ivlen)) {
    return 0;
  }
  p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_AEAD_TAGLEN);
  if (p != NULL && !OSSL_PARAM_set_size_t(p, ctx->taglen)) {
    return 0;
  }
  /* TLS 1.2: the record layer reads back the pad the TLS1_AAD control implies
   * (the tag appended to every record). */
  p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_AEAD_TLS1_AAD_PAD);
  if (p != NULL && !OSSL_PARAM_set_size_t(p, ZZ9K_TLS_TAG_LEN)) {
    return 0;
  }
  return 1;
}

static int zz9k_aead_set_ctx_params(void *vctx, const OSSL_PARAM params[])
{
  ZZ9K_AEAD_CTX *ctx = vctx;
  const OSSL_PARAM *p;

  if (params == NULL) {
    return 1;
  }
  p = OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_AEAD_TAG);
  if (p != NULL) {
    if (p->data_type != OSSL_PARAM_OCTET_STRING ||
        p->data_size > ZZ9K_AEAD_TAGLEN) {
      return 0;
    }
    memcpy(ctx->tag, p->data, p->data_size);
    ctx->taglen = p->data_size;
    ctx->tag_set = 1;
  }
  p = OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_IVLEN);
  if (p != NULL) {
    size_t len;
    if (!OSSL_PARAM_get_size_t(p, &len) || len != ZZ9K_AEAD_IVLEN) {
      return 0;
    }
    ctx->ivlen = len;
  }
  p = OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_KEYLEN);
  if (p != NULL) {
    size_t len;
    if (!OSSL_PARAM_get_size_t(p, &len) || len != ctx->keylen) {
      return 0;
    }
  }
  /* TLS 1.2 record-layer controls (EVP_CTRL_AEAD_TLS1_AAD /
   * EVP_CTRL_AEAD_SET_IV_FIXED), mirroring the default provider: the 13-byte
   * AAD is stashed with its length field corrected for the explicit IV (GCM)
   * and, on decrypt, the tag; the fixed IV seeds the working nonce (GCM: 4-byte
   * salt plus a randomised invocation field, ChaCha: all 12 bytes). */
  p = OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_AEAD_TLS1_AAD);
  if (p != NULL) {
    size_t len;
    if (p->data_type != OSSL_PARAM_OCTET_STRING ||
        p->data_size != ZZ9K_TLS_AAD_LEN) {
      return 0;
    }
    memcpy(ctx->tls_aad, p->data, ZZ9K_TLS_AAD_LEN);
    len = ((size_t)ctx->tls_aad[ZZ9K_TLS_AAD_LEN - 2] << 8) |
          ctx->tls_aad[ZZ9K_TLS_AAD_LEN - 1];
    if (ctx->alg != ZZ9K_AEAD_CHACHA20_POLY1305) {
      if (len < ZZ9K_TLS_EXPLICIT_IV) {
        return 0;
      }
      len -= ZZ9K_TLS_EXPLICIT_IV;
    }
    if (!ctx->enc) {
      if (len < ZZ9K_TLS_TAG_LEN) {
        return 0;
      }
      len -= ZZ9K_TLS_TAG_LEN;
    }
    ctx->tls_aad[ZZ9K_TLS_AAD_LEN - 2] = (unsigned char)(len >> 8);
    ctx->tls_aad[ZZ9K_TLS_AAD_LEN - 1] = (unsigned char)(len & 0xff);
    ctx->tls_aad_set = 1;
  }
  p = OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_AEAD_TLS1_IV_FIXED);
  if (p != NULL) {
    if (p->data_type != OSSL_PARAM_OCTET_STRING) {
      return 0;
    }
    if (ctx->alg == ZZ9K_AEAD_CHACHA20_POLY1305) {
      if (p->data_size != ZZ9K_AEAD_IVLEN) {
        return 0;
      }
      memcpy(ctx->tls_iv, p->data, ZZ9K_AEAD_IVLEN);
    } else {
      if (p->data_size != 4) {
        return 0;
      }
      memcpy(ctx->tls_iv, p->data, 4);
      if (ctx->enc && RAND_bytes(ctx->tls_iv + 4, ZZ9K_TLS_EXPLICIT_IV) <= 0) {
        return 0;
      }
    }
    ctx->tls_iv_set = 1;
  }
  return 1;
}

static const OSSL_PARAM *zz9k_aead_gettable_ctx_params(void *cctx,
                                                       void *provctx)
{
  static const OSSL_PARAM types[] = {
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_KEYLEN, NULL),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_IVLEN, NULL),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_AEAD_TAGLEN, NULL),
    OSSL_PARAM_octet_string(OSSL_CIPHER_PARAM_AEAD_TAG, NULL, 0),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_AEAD_TLS1_AAD_PAD, NULL),
    OSSL_PARAM_END
  };
  (void)cctx;
  (void)provctx;
  return types;
}

static const OSSL_PARAM *zz9k_aead_settable_ctx_params(void *cctx,
                                                       void *provctx)
{
  static const OSSL_PARAM types[] = {
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_KEYLEN, NULL),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_IVLEN, NULL),
    OSSL_PARAM_octet_string(OSSL_CIPHER_PARAM_AEAD_TAG, NULL, 0),
    OSSL_PARAM_octet_string(OSSL_CIPHER_PARAM_AEAD_TLS1_AAD, NULL, 0),
    OSSL_PARAM_octet_string(OSSL_CIPHER_PARAM_AEAD_TLS1_IV_FIXED, NULL, 0),
    OSSL_PARAM_END
  };
  (void)cctx;
  (void)provctx;
  return types;
}

static int zz9k_aead_get_params_common(OSSL_PARAM params[], size_t keylen,
                                       unsigned int mode)
{
  OSSL_PARAM *p;

  p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_BLOCK_SIZE);
  if (p != NULL && !OSSL_PARAM_set_size_t(p, 1)) {
    return 0;
  }
  p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_KEYLEN);
  if (p != NULL && !OSSL_PARAM_set_size_t(p, keylen)) {
    return 0;
  }
  p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_IVLEN);
  if (p != NULL && !OSSL_PARAM_set_size_t(p, ZZ9K_AEAD_IVLEN)) {
    return 0;
  }
  p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_MODE);
  if (p != NULL && !OSSL_PARAM_set_uint(p, mode)) {
    return 0;
  }
  p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_AEAD);
  if (p != NULL && !OSSL_PARAM_set_int(p, 1)) {
    return 0;
  }
  p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_CUSTOM_IV);
  if (p != NULL && !OSSL_PARAM_set_int(p, 1)) {
    return 0;
  }
  return 1;
}

static int zz9k_aes128gcm_get_params(OSSL_PARAM params[])
{
  return zz9k_aead_get_params_common(params, 16, ZZ9K_MODE_GCM);
}
static int zz9k_aes256gcm_get_params(OSSL_PARAM params[])
{
  return zz9k_aead_get_params_common(params, 32, ZZ9K_MODE_GCM);
}
static int zz9k_chachapoly_get_params(OSSL_PARAM params[])
{
  return zz9k_aead_get_params_common(params, 32, ZZ9K_MODE_STREAM);
}

static const OSSL_PARAM *zz9k_aead_gettable_params(void *provctx)
{
  static const OSSL_PARAM types[] = {
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_BLOCK_SIZE, NULL),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_KEYLEN, NULL),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_IVLEN, NULL),
    OSSL_PARAM_uint(OSSL_CIPHER_PARAM_MODE, NULL),
    OSSL_PARAM_int(OSSL_CIPHER_PARAM_AEAD, NULL),
    OSSL_PARAM_int(OSSL_CIPHER_PARAM_CUSTOM_IV, NULL),
    OSSL_PARAM_END
  };
  (void)provctx;
  return types;
}

/* ---- dispatch tables ---- */

#define ZZ9K_AEAD_COMMON_DISPATCH \
  { OSSL_FUNC_CIPHER_ENCRYPT_INIT, (void (*)(void))zz9k_aead_encrypt_init }, \
  { OSSL_FUNC_CIPHER_DECRYPT_INIT, (void (*)(void))zz9k_aead_decrypt_init }, \
  { OSSL_FUNC_CIPHER_UPDATE, (void (*)(void))zz9k_aead_update }, \
  { OSSL_FUNC_CIPHER_FINAL, (void (*)(void))zz9k_aead_final }, \
  { OSSL_FUNC_CIPHER_FREECTX, (void (*)(void))zz9k_aead_freectx }, \
  { OSSL_FUNC_CIPHER_GET_CTX_PARAMS, \
    (void (*)(void))zz9k_aead_get_ctx_params }, \
  { OSSL_FUNC_CIPHER_SET_CTX_PARAMS, \
    (void (*)(void))zz9k_aead_set_ctx_params }, \
  { OSSL_FUNC_CIPHER_GETTABLE_PARAMS, \
    (void (*)(void))zz9k_aead_gettable_params }, \
  { OSSL_FUNC_CIPHER_GETTABLE_CTX_PARAMS, \
    (void (*)(void))zz9k_aead_gettable_ctx_params }, \
  { OSSL_FUNC_CIPHER_SETTABLE_CTX_PARAMS, \
    (void (*)(void))zz9k_aead_settable_ctx_params }

static const OSSL_DISPATCH zz9k_aes128gcm_functions[] = {
  { OSSL_FUNC_CIPHER_NEWCTX, (void (*)(void))zz9k_aes128gcm_newctx },
  { OSSL_FUNC_CIPHER_GET_PARAMS, (void (*)(void))zz9k_aes128gcm_get_params },
  ZZ9K_AEAD_COMMON_DISPATCH,
  { 0, NULL }
};

static const OSSL_DISPATCH zz9k_aes256gcm_functions[] = {
  { OSSL_FUNC_CIPHER_NEWCTX, (void (*)(void))zz9k_aes256gcm_newctx },
  { OSSL_FUNC_CIPHER_GET_PARAMS, (void (*)(void))zz9k_aes256gcm_get_params },
  ZZ9K_AEAD_COMMON_DISPATCH,
  { 0, NULL }
};

static const OSSL_DISPATCH zz9k_chachapoly_functions[] = {
  { OSSL_FUNC_CIPHER_NEWCTX, (void (*)(void))zz9k_chachapoly_newctx },
  { OSSL_FUNC_CIPHER_GET_PARAMS, (void (*)(void))zz9k_chachapoly_get_params },
  ZZ9K_AEAD_COMMON_DISPATCH,
  { 0, NULL }
};

const OSSL_ALGORITHM zz9k_cipher_algorithms[] = {
  { "AES-128-GCM", "provider=zz9000", zz9k_aes128gcm_functions,
    "ZZ9000 AES-128-GCM" },
  { "AES-256-GCM", "provider=zz9000", zz9k_aes256gcm_functions,
    "ZZ9000 AES-256-GCM" },
  { "ChaCha20-Poly1305", "provider=zz9000", zz9k_chachapoly_functions,
    "ZZ9000 ChaCha20-Poly1305" },
  { NULL, NULL, NULL, NULL }
};

/* Advertised instead of the full table when the firmware has the crypto
 * service but not the AES-GCM capability flag (ChaCha20-Poly1305 is the
 * legacy AEAD default every crypto-service firmware provides). */
const OSSL_ALGORITHM zz9k_cipher_algorithms_chacha_only[] = {
  { "ChaCha20-Poly1305", "provider=zz9000", zz9k_chachapoly_functions,
    "ZZ9000 ChaCha20-Poly1305" },
  { NULL, NULL, NULL, NULL }
};
