/*
 * Portable software reference for ChaCha20, Poly1305, and the
 * ChaCha20-Poly1305 AEAD (RFC 8439).
 *
 * The Poly1305 core uses the well-known radix-2^26 "donna" formulation,
 * chosen because it relies only on 32x32->64 multiplies that an m68k
 * compiler can emit, with no 128-bit intermediate.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-crypto-soft.h"

#include <string.h>

static uint32_t zz9k_soft_load32_le(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void zz9k_soft_store32_le(uint8_t *p, uint32_t value)
{
  p[0] = (uint8_t)(value & 0xffU);
  p[1] = (uint8_t)((value >> 8) & 0xffU);
  p[2] = (uint8_t)((value >> 16) & 0xffU);
  p[3] = (uint8_t)((value >> 24) & 0xffU);
}

static uint32_t zz9k_soft_rotl32(uint32_t value, unsigned int count)
{
  return (value << count) | (value >> (32U - count));
}

#define ZZ9K_SOFT_QUARTERROUND(a, b, c, d) \
  do {                                     \
    a += b; d ^= a; d = zz9k_soft_rotl32(d, 16U); \
    c += d; b ^= c; b = zz9k_soft_rotl32(b, 12U); \
    a += b; d ^= a; d = zz9k_soft_rotl32(d, 8U);  \
    c += d; b ^= c; b = zz9k_soft_rotl32(b, 7U);  \
  } while (0)

static void zz9k_soft_chacha20_block(
    uint8_t out[64],
    const uint8_t key[ZZ9K_SOFT_CHACHA20_KEY_BYTES],
    const uint8_t nonce[ZZ9K_SOFT_CHACHA20_NONCE_BYTES],
    uint32_t counter)
{
  uint32_t state[16];
  uint32_t work[16];
  unsigned int i;

  state[0] = 0x61707865U;
  state[1] = 0x3320646eU;
  state[2] = 0x79622d32U;
  state[3] = 0x6b206574U;
  for (i = 0; i < 8U; i++) {
    state[4U + i] = zz9k_soft_load32_le(key + (i * 4U));
  }
  state[12] = counter;
  for (i = 0; i < 3U; i++) {
    state[13U + i] = zz9k_soft_load32_le(nonce + (i * 4U));
  }

  for (i = 0; i < 16U; i++) {
    work[i] = state[i];
  }

  for (i = 0; i < 10U; i++) {
    ZZ9K_SOFT_QUARTERROUND(work[0], work[4], work[8], work[12]);
    ZZ9K_SOFT_QUARTERROUND(work[1], work[5], work[9], work[13]);
    ZZ9K_SOFT_QUARTERROUND(work[2], work[6], work[10], work[14]);
    ZZ9K_SOFT_QUARTERROUND(work[3], work[7], work[11], work[15]);
    ZZ9K_SOFT_QUARTERROUND(work[0], work[5], work[10], work[15]);
    ZZ9K_SOFT_QUARTERROUND(work[1], work[6], work[11], work[12]);
    ZZ9K_SOFT_QUARTERROUND(work[2], work[7], work[8], work[13]);
    ZZ9K_SOFT_QUARTERROUND(work[3], work[4], work[9], work[14]);
  }

  for (i = 0; i < 16U; i++) {
    zz9k_soft_store32_le(out + (i * 4U), work[i] + state[i]);
  }
}

void zz9k_soft_chacha20_xor(uint8_t *out, const uint8_t *in, uint32_t length,
                            const uint8_t key[ZZ9K_SOFT_CHACHA20_KEY_BYTES],
                            const uint8_t nonce[ZZ9K_SOFT_CHACHA20_NONCE_BYTES],
                            uint32_t counter)
{
  uint8_t block[64];
  uint32_t offset = 0;

  while (offset < length) {
    uint32_t chunk = length - offset;
    uint32_t i;

    if (chunk > 64U) {
      chunk = 64U;
    }
    zz9k_soft_chacha20_block(block, key, nonce, counter);
    for (i = 0; i < chunk; i++) {
      out[offset + i] = (uint8_t)(in[offset + i] ^ block[i]);
    }
    offset += chunk;
    counter++;
  }
}

typedef struct ZZ9KSoftPoly1305 {
  uint32_t r[5];
  uint32_t h[5];
  uint32_t pad[4];
} ZZ9KSoftPoly1305;

static void zz9k_soft_poly1305_init(ZZ9KSoftPoly1305 *st,
                                    const uint8_t key[32])
{
  uint32_t t0 = zz9k_soft_load32_le(key + 0);
  uint32_t t1 = zz9k_soft_load32_le(key + 4);
  uint32_t t2 = zz9k_soft_load32_le(key + 8);
  uint32_t t3 = zz9k_soft_load32_le(key + 12);

  st->r[0] = (t0) & 0x3ffffffU;
  st->r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03U;
  st->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ffU;
  st->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fffU;
  st->r[4] = (t3 >> 8) & 0x00fffffU;

  st->h[0] = 0;
  st->h[1] = 0;
  st->h[2] = 0;
  st->h[3] = 0;
  st->h[4] = 0;

  st->pad[0] = zz9k_soft_load32_le(key + 16);
  st->pad[1] = zz9k_soft_load32_le(key + 20);
  st->pad[2] = zz9k_soft_load32_le(key + 24);
  st->pad[3] = zz9k_soft_load32_le(key + 28);
}

/* Processes one 16-byte block. `hibit` adds the 2^128 bit for full blocks. */
static void zz9k_soft_poly1305_block(ZZ9KSoftPoly1305 *st,
                                     const uint8_t block[16],
                                     uint32_t hibit)
{
  uint32_t r0 = st->r[0];
  uint32_t r1 = st->r[1];
  uint32_t r2 = st->r[2];
  uint32_t r3 = st->r[3];
  uint32_t r4 = st->r[4];
  uint32_t s1 = r1 * 5U;
  uint32_t s2 = r2 * 5U;
  uint32_t s3 = r3 * 5U;
  uint32_t s4 = r4 * 5U;
  uint32_t h0 = st->h[0];
  uint32_t h1 = st->h[1];
  uint32_t h2 = st->h[2];
  uint32_t h3 = st->h[3];
  uint32_t h4 = st->h[4];
  uint32_t t0 = zz9k_soft_load32_le(block + 0);
  uint32_t t1 = zz9k_soft_load32_le(block + 4);
  uint32_t t2 = zz9k_soft_load32_le(block + 8);
  uint32_t t3 = zz9k_soft_load32_le(block + 12);
  uint64_t d0;
  uint64_t d1;
  uint64_t d2;
  uint64_t d3;
  uint64_t d4;
  uint32_t c;

  h0 += (t0) & 0x3ffffffU;
  h1 += ((t0 >> 26) | (t1 << 6)) & 0x3ffffffU;
  h2 += ((t1 >> 20) | (t2 << 12)) & 0x3ffffffU;
  h3 += ((t2 >> 14) | (t3 << 18)) & 0x3ffffffU;
  h4 += (t3 >> 8) | hibit;

  d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 +
       (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
  d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 +
       (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
  d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 +
       (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
  d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 +
       (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
  d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 +
       (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

  c = (uint32_t)(d0 >> 26);
  h0 = (uint32_t)d0 & 0x3ffffffU;
  d1 += c;
  c = (uint32_t)(d1 >> 26);
  h1 = (uint32_t)d1 & 0x3ffffffU;
  d2 += c;
  c = (uint32_t)(d2 >> 26);
  h2 = (uint32_t)d2 & 0x3ffffffU;
  d3 += c;
  c = (uint32_t)(d3 >> 26);
  h3 = (uint32_t)d3 & 0x3ffffffU;
  d4 += c;
  c = (uint32_t)(d4 >> 26);
  h4 = (uint32_t)d4 & 0x3ffffffU;
  h0 += c * 5U;
  c = h0 >> 26;
  h0 = h0 & 0x3ffffffU;
  h1 += c;

  st->h[0] = h0;
  st->h[1] = h1;
  st->h[2] = h2;
  st->h[3] = h3;
  st->h[4] = h4;
}

static void zz9k_soft_poly1305_finish(ZZ9KSoftPoly1305 *st, uint8_t tag[16])
{
  uint32_t h0 = st->h[0];
  uint32_t h1 = st->h[1];
  uint32_t h2 = st->h[2];
  uint32_t h3 = st->h[3];
  uint32_t h4 = st->h[4];
  uint32_t g0;
  uint32_t g1;
  uint32_t g2;
  uint32_t g3;
  uint32_t g4;
  uint32_t c;
  uint32_t mask;
  uint64_t f;

  c = h1 >> 26;
  h1 &= 0x3ffffffU;
  h2 += c;
  c = h2 >> 26;
  h2 &= 0x3ffffffU;
  h3 += c;
  c = h3 >> 26;
  h3 &= 0x3ffffffU;
  h4 += c;
  c = h4 >> 26;
  h4 &= 0x3ffffffU;
  h0 += c * 5U;
  c = h0 >> 26;
  h0 &= 0x3ffffffU;
  h1 += c;

  g0 = h0 + 5U;
  c = g0 >> 26;
  g0 &= 0x3ffffffU;
  g1 = h1 + c;
  c = g1 >> 26;
  g1 &= 0x3ffffffU;
  g2 = h2 + c;
  c = g2 >> 26;
  g2 &= 0x3ffffffU;
  g3 = h3 + c;
  c = g3 >> 26;
  g3 &= 0x3ffffffU;
  g4 = h4 + c - (1U << 26);

  mask = (g4 >> 31) - 1U;
  g0 &= mask;
  g1 &= mask;
  g2 &= mask;
  g3 &= mask;
  g4 &= mask;
  mask = ~mask;
  h0 = (h0 & mask) | g0;
  h1 = (h1 & mask) | g1;
  h2 = (h2 & mask) | g2;
  h3 = (h3 & mask) | g3;
  h4 = (h4 & mask) | g4;

  h0 = ((h0) | (h1 << 26)) & 0xffffffffU;
  h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffffU;
  h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffffU;
  h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffffU;

  f = (uint64_t)h0 + st->pad[0];
  h0 = (uint32_t)f;
  f = (uint64_t)h1 + st->pad[1] + (f >> 32);
  h1 = (uint32_t)f;
  f = (uint64_t)h2 + st->pad[2] + (f >> 32);
  h2 = (uint32_t)f;
  f = (uint64_t)h3 + st->pad[3] + (f >> 32);
  h3 = (uint32_t)f;

  zz9k_soft_store32_le(tag + 0, h0);
  zz9k_soft_store32_le(tag + 4, h1);
  zz9k_soft_store32_le(tag + 8, h2);
  zz9k_soft_store32_le(tag + 12, h3);
}

static void zz9k_soft_poly1305_update(ZZ9KSoftPoly1305 *st,
                                      const uint8_t *message, uint32_t length)
{
  while (length >= 16U) {
    zz9k_soft_poly1305_block(st, message, 1U << 24);
    message += 16U;
    length -= 16U;
  }
  if (length > 0U) {
    uint8_t block[16];

    memset(block, 0, sizeof(block));
    memcpy(block, message, length);
    block[length] = 1U;
    zz9k_soft_poly1305_block(st, block, 0U);
  }
}

void zz9k_soft_poly1305(uint8_t tag[ZZ9K_SOFT_POLY1305_TAG_BYTES],
                        const uint8_t *message, uint32_t length,
                        const uint8_t key[ZZ9K_SOFT_POLY1305_KEY_BYTES])
{
  ZZ9KSoftPoly1305 st;

  zz9k_soft_poly1305_init(&st, key);
  zz9k_soft_poly1305_update(&st, message, length);
  zz9k_soft_poly1305_finish(&st, tag);
}

/*
 * Feeds one AEAD segment (aad or ciphertext) to Poly1305 as whole 16-byte
 * blocks, zero-padding the final partial block. Unlike the standalone
 * Poly1305 message convention, every AEAD block is a full block and takes
 * the 2^128 high bit; the zero pad supplies the pad16() of RFC 8439.
 */
static void zz9k_soft_poly1305_aead_segment(ZZ9KSoftPoly1305 *st,
                                            const uint8_t *data,
                                            uint32_t length)
{
  while (length >= 16U) {
    zz9k_soft_poly1305_block(st, data, 1U << 24);
    data += 16U;
    length -= 16U;
  }
  if (length > 0U) {
    uint8_t block[16];

    memset(block, 0, sizeof(block));
    memcpy(block, data, length);
    zz9k_soft_poly1305_block(st, block, 1U << 24);
  }
}

static void zz9k_soft_chacha20_poly1305_tag(
    uint8_t tag[16],
    const uint8_t *ciphertext, uint32_t length,
    const uint8_t *aad, uint32_t aad_length,
    const uint8_t key[ZZ9K_SOFT_CHACHA20_KEY_BYTES],
    const uint8_t nonce[ZZ9K_SOFT_CHACHA20_NONCE_BYTES])
{
  ZZ9KSoftPoly1305 st;
  uint8_t otk[64];
  uint8_t lengths[16];
  uint8_t zero_block[32];

  /* Explicitly zeroed local, NOT a zero-initialized static: statics with an
   * all-zero initializer are placed in BSS, and inside a base-relative
   * resident library (the AmiSSL Path A build) that BSS is not reliably
   * zeroed — a garbage zero_block poisons the Poly1305 one-time key and
   * produces a random tag over a perfectly good ciphertext. Found the hard
   * way on real hardware. */
  memset(zero_block, 0, sizeof(zero_block));
  zz9k_soft_chacha20_xor(otk, zero_block, 32U, key, nonce, 0U);
  zz9k_soft_poly1305_init(&st, otk);

  zz9k_soft_poly1305_aead_segment(&st, aad, aad_length);
  zz9k_soft_poly1305_aead_segment(&st, ciphertext, length);

  zz9k_soft_store32_le(lengths + 0, aad_length);
  zz9k_soft_store32_le(lengths + 4, 0U);
  zz9k_soft_store32_le(lengths + 8, length);
  zz9k_soft_store32_le(lengths + 12, 0U);
  zz9k_soft_poly1305_block(&st, lengths, 1U << 24);

  zz9k_soft_poly1305_finish(&st, tag);
}

void zz9k_soft_chacha20_poly1305_encrypt(
    uint8_t *ciphertext, uint8_t tag[ZZ9K_SOFT_POLY1305_TAG_BYTES],
    const uint8_t *plaintext, uint32_t length,
    const uint8_t *aad, uint32_t aad_length,
    const uint8_t key[ZZ9K_SOFT_CHACHA20_KEY_BYTES],
    const uint8_t nonce[ZZ9K_SOFT_CHACHA20_NONCE_BYTES])
{
  zz9k_soft_chacha20_xor(ciphertext, plaintext, length, key, nonce, 1U);
  zz9k_soft_chacha20_poly1305_tag(tag, ciphertext, length, aad, aad_length,
                                  key, nonce);
}

/* ---- X25519 (Curve25519 Diffie-Hellman, RFC 7748) ---- */
/* Portable C99 implementation. Based on the public-domain work of
 * Daniel J. Bernstein (SUPERCOP ref10). All arithmetic is mod 2^255-19.
 * Limb layout: alternating 26-bit (even) and 25-bit (odd) limbs.
 *   h[0] = bits   0-25, h[1] = bits  26-50, h[2] = bits  51-76,
 *   h[3] = bits  77-101, h[4] = bits 102-127, h[5] = bits 128-152,
 *   h[6] = bits 153-178, h[7] = bits 179-203, h[8] = bits 204-229,
 *   h[9] = bits 230-254. */

typedef uint32_t fe25519[10];

static void fe25519_0(fe25519 h) {
  int i; for (i = 0; i < 10; i++) h[i] = 0;
}
static void fe25519_1(fe25519 h) {
  fe25519_0(h); h[0] = 1;
}
static void fe25519_copy(fe25519 h, const fe25519 f) {
  int i; for (i = 0; i < 10; i++) h[i] = f[i];
}
static void fe25519_cswap(fe25519 f, fe25519 g, uint32_t b) {
  int i;
  uint32_t mask = (uint32_t)(-(int32_t)b);
  for (i = 0; i < 10; i++) {
    uint32_t t = mask & (f[i] ^ g[i]);
    f[i] ^= t; g[i] ^= t;
  }
}

/*
 * Deserialise a 32-byte little-endian encoding into ten limbs.
 * Limb sizes (bits): h0:26 h1:25 h2:26 h3:25 h4:26 h5:25 h6:26 h7:25 h8:26 h9:25.
 * Bit boundaries (field bit .. field bit):
 *   h0: 0-25, h1: 26-50, h2: 51-76, h3: 77-101, h4: 102-127,
 *   h5: 128-152, h6: 153-178, h7: 179-203, h8: 204-229, h9: 230-254.
 */
static void fe25519_from_bytes(fe25519 h, const uint8_t *s) {
  int64_t b0 =(int64_t)s[ 0],b1 =(int64_t)s[ 1],b2 =(int64_t)s[ 2],b3 =(int64_t)s[ 3];
  int64_t b4 =(int64_t)s[ 4],b5 =(int64_t)s[ 5],b6 =(int64_t)s[ 6];
  int64_t b7 =(int64_t)s[ 7],b8 =(int64_t)s[ 8],b9 =(int64_t)s[ 9];
  int64_t b10=(int64_t)s[10],b11=(int64_t)s[11],b12=(int64_t)s[12];
  int64_t b13=(int64_t)s[13],b14=(int64_t)s[14],b15=(int64_t)s[15];
  int64_t b16=(int64_t)s[16],b17=(int64_t)s[17],b18=(int64_t)s[18],b19=(int64_t)s[19];
  int64_t b20=(int64_t)s[20],b21=(int64_t)s[21],b22=(int64_t)s[22];
  int64_t b23=(int64_t)s[23],b24=(int64_t)s[24],b25=(int64_t)s[25];
  int64_t b26=(int64_t)s[26],b27=(int64_t)s[27],b28=(int64_t)s[28];
  int64_t b29=(int64_t)s[29],b30=(int64_t)s[30],b31=(int64_t)s[31];
  /* h0: field bits 0-25.  Byte 3 contributes bits 24-25 (mask 0x03). */
  h[0]=(uint32_t)(b0|(b1<<8)|(b2<<16)|((b3&0x03)<<24));
  /* h1: field bits 26-50.  Byte 3 bits 2-7, bytes 4-5, byte 6 bits 0-2. */
  h[1]=(uint32_t)((b3>>2)|(b4<<6)|(b5<<14)|((b6&0x07)<<22));
  /* h2: field bits 51-76.  Byte 6 bits 3-7, bytes 7-8, byte 9 bits 0-4. */
  h[2]=(uint32_t)((b6>>3)|(b7<<5)|(b8<<13)|((b9&0x1f)<<21));
  /* h3: field bits 77-101. Byte 9 bits 5-7, bytes 10-11, byte 12 bits 0-5. */
  h[3]=(uint32_t)((b9>>5)|(b10<<3)|(b11<<11)|((b12&0x3f)<<19));
  /* h4: field bits 102-127. Byte 12 bits 6-7, bytes 13-15 (full). */
  h[4]=(uint32_t)((b12>>6)|(b13<<2)|(b14<<10)|(b15<<18));
  /* h5: field bits 128-152. Bytes 16-18, byte 19 bit 0 only. */
  h[5]=(uint32_t)(b16|(b17<<8)|(b18<<16)|((b19&0x01)<<24));
  /* h6: field bits 153-178. Byte 19 bits 1-7, bytes 20-21, byte 22 bits 0-2. */
  h[6]=(uint32_t)((b19>>1)|(b20<<7)|(b21<<15)|((b22&0x07)<<23));
  /* h7: field bits 179-203. Byte 22 bits 3-7, bytes 23-24, byte 25 bits 0-3. */
  h[7]=(uint32_t)((b22>>3)|(b23<<5)|(b24<<13)|((b25&0x0f)<<21));
  /* h8: field bits 204-229. Byte 25 bits 4-7, bytes 26-27, byte 28 bits 0-5. */
  h[8]=(uint32_t)((b25>>4)|(b26<<4)|(b27<<12)|((b28&0x3f)<<20));
  /* h9: field bits 230-254. Byte 28 bits 6-7, bytes 29-30, byte 31 bits 0-6. */
  h[9]=(uint32_t)((b28>>6)|(b29<<2)|(b30<<10)|((b31&0x7f)<<18));
}

static void fe25519_to_bytes(uint8_t *s, fe25519 h) {
  int64_t h0=(int64_t)h[0], h1=(int64_t)h[1], h2=(int64_t)h[2], h3=(int64_t)h[3], h4=(int64_t)h[4];
  int64_t h5=(int64_t)h[5], h6=(int64_t)h[6], h7=(int64_t)h[7], h8=(int64_t)h[8], h9=(int64_t)h[9];
  int64_t q, carry;
  /* Reduce: compute q = floor((h + 19) / 2^255) */
  q = (19*h9 + (((int64_t)1)<<24)) >> 25;
  q = (h0+q) >> 26; q = (h1+q) >> 25; q = (h2+q) >> 26; q = (h3+q) >> 25;
  q = (h4+q) >> 26; q = (h5+q) >> 25; q = (h6+q) >> 26; q = (h7+q) >> 25;
  q = (h8+q) >> 26; q = (h9+q) >> 25;
  h0 += 19*q;
  carry=h0>>26; h1+=carry; h0&=0x3ffffff;
  carry=h1>>25; h2+=carry; h1&=0x1ffffff;
  carry=h2>>26; h3+=carry; h2&=0x3ffffff;
  carry=h3>>25; h4+=carry; h3&=0x1ffffff;
  carry=h4>>26; h5+=carry; h4&=0x3ffffff;
  carry=h5>>25; h6+=carry; h5&=0x1ffffff;
  carry=h6>>26; h7+=carry; h6&=0x3ffffff;
  carry=h7>>25; h8+=carry; h7&=0x1ffffff;
  carry=h8>>26; h9+=carry; h8&=0x3ffffff;
  h9&=0x1ffffff;
  /* Pack 10 limbs into 32 bytes. */
  /* h0:26 h1:25 h2:26 h3:25 h4:26 h5:25 h6:26 h7:25 h8:26 h9:25 */
  s[ 0]=(uint8_t)(h0);
  s[ 1]=(uint8_t)(h0>> 8);
  s[ 2]=(uint8_t)(h0>>16);
  s[ 3]=(uint8_t)((h0>>24)|(h1<<2));   /* h0 bits 24-25, h1 bits 0-5 */
  s[ 4]=(uint8_t)(h1>> 6);
  s[ 5]=(uint8_t)(h1>>14);
  s[ 6]=(uint8_t)((h1>>22)|(h2<<3));   /* h1 bits 22-24, h2 bits 0-4 */
  s[ 7]=(uint8_t)(h2>> 5);
  s[ 8]=(uint8_t)(h2>>13);
  s[ 9]=(uint8_t)((h2>>21)|(h3<<5));   /* h2 bits 21-25, h3 bits 0-2 */
  s[10]=(uint8_t)(h3>> 3);
  s[11]=(uint8_t)(h3>>11);
  s[12]=(uint8_t)((h3>>19)|(h4<<6));   /* h3 bits 19-24, h4 bits 0-1 */
  s[13]=(uint8_t)(h4>> 2);
  s[14]=(uint8_t)(h4>>10);
  s[15]=(uint8_t)(h4>>18);             /* h4 bits 18-25 (8 bits, full byte) */
  s[16]=(uint8_t)(h5);
  s[17]=(uint8_t)(h5>> 8);
  s[18]=(uint8_t)(h5>>16);
  s[19]=(uint8_t)((h5>>24)|(h6<<1));   /* h5 bit 24, h6 bits 0-6 */
  s[20]=(uint8_t)(h6>> 7);
  s[21]=(uint8_t)(h6>>15);
  s[22]=(uint8_t)((h6>>23)|(h7<<3));   /* h6 bits 23-25, h7 bits 0-4 */
  s[23]=(uint8_t)(h7>> 5);
  s[24]=(uint8_t)(h7>>13);
  s[25]=(uint8_t)((h7>>21)|(h8<<4));   /* h7 bits 21-24, h8 bits 0-3 */
  s[26]=(uint8_t)(h8>> 4);
  s[27]=(uint8_t)(h8>>12);
  s[28]=(uint8_t)((h8>>20)|(h9<<6));   /* h8 bits 20-25, h9 bits 0-1 */
  s[29]=(uint8_t)(h9>> 2);
  s[30]=(uint8_t)(h9>>10);
  s[31]=(uint8_t)(h9>>18);
}

static void fe25519_add(fe25519 h, const fe25519 f, const fe25519 g) {
  int i; for (i=0;i<10;i++) h[i]=f[i]+g[i];
}
static void fe25519_sub(fe25519 h, const fe25519 f, const fe25519 g) {
  /* Compute h = f - g + 2p to keep all limbs non-negative.
   * 2p = 2*(2^255-19). Per-limb values of 2p (all limbs fully set minus
   * the low bits of 19): */
  static const uint32_t TWO_P[10] = {
    0x7ffffda, 0x3fffffe, 0x7fffffe, 0x3fffffe, 0x7fffffe,
    0x3fffffe, 0x7fffffe, 0x3fffffe, 0x7fffffe, 0x3fffffe
  };
  /* Use int64_t to avoid uint32_t wrapping when f[i] < g[i]. */
  int i;
  for (i=0;i<10;i++) {
    int64_t diff = (int64_t)f[i] - (int64_t)g[i] + (int64_t)TWO_P[i];
    h[i] = (uint32_t)diff;
  }
}
/*
 * Field multiplication h = f * g mod 2^255-19.
 *
 * Limb weights: w[k] = {0,26,51,77,102,128,153,179,204,230}.
 * For any pair (i,j) with i+j=k, the product f[i]*g[j] has weight
 * w[i]+w[j].  When both i and j are odd, w[i]+w[j] = w[i+j]+1, so the
 * limb product must be scaled by 2.  This is absorbed by pre-doubling
 * odd-indexed f limbs: f1_2=2*f1, f3_2=2*f3, etc.  The same 2x factor
 * applies to wrap-around ODD*ODD terms (i+j = k+10).
 * This is the standard SUPERCOP ref10 formulation, valid for f!=g as well.
 */
static void fe25519_mul(fe25519 h, const fe25519 f, const fe25519 g) {
  int64_t f0=(int64_t)f[0],f1=(int64_t)f[1],f2=(int64_t)f[2],f3=(int64_t)f[3],f4=(int64_t)f[4];
  int64_t f5=(int64_t)f[5],f6=(int64_t)f[6],f7=(int64_t)f[7],f8=(int64_t)f[8],f9=(int64_t)f[9];
  int64_t g0=(int64_t)g[0],g1=(int64_t)g[1],g2=(int64_t)g[2],g3=(int64_t)g[3],g4=(int64_t)g[4];
  int64_t g5=(int64_t)g[5],g6=(int64_t)g[6],g7=(int64_t)g[7],g8=(int64_t)g[8],g9=(int64_t)g[9];
  /* Pre-doubled odd-indexed f limbs for ODD*ODD correction factor. */
  int64_t f1_2=2*f1, f3_2=2*f3, f5_2=2*f5, f7_2=2*f7, f9_2=2*f9;
  /* Precomputed 19x versions of g for wrap-around reduction. */
  int64_t g1_19=19*g1,g2_19=19*g2,g3_19=19*g3,g4_19=19*g4,g5_19=19*g5;
  int64_t g6_19=19*g6,g7_19=19*g7,g8_19=19*g8,g9_19=19*g9;
  int64_t r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,carry;
  /* For each output limb r[k]:
   *   non-wrap terms (i+j=k, i+j<10): use f[i]*g[j], with f1_2 for ODD i
   *   wrap terms (i+j=k+10):          use 19*f[i]*g[j], with f1_2 for ODD i
   * Pairs marked (o) have both indices odd => use f_odd_2 for the f factor. */
  r0=f0*g0     +f1_2*g9_19 +f2*g8_19  +f3_2*g7_19 +f4*g6_19  +f5_2*g5_19 +f6*g4_19  +f7_2*g3_19 +f8*g2_19  +f9_2*g1_19;
  r1=f0*g1     +f1*g0      +f2*g9_19  +f3*g8_19   +f4*g7_19  +f5*g6_19   +f6*g5_19  +f7*g4_19   +f8*g3_19  +f9*g2_19;
  r2=f0*g2     +f1_2*g1    +f2*g0     +f3_2*g9_19 +f4*g8_19  +f5_2*g7_19 +f6*g6_19  +f7_2*g5_19 +f8*g4_19  +f9_2*g3_19;
  r3=f0*g3     +f1*g2      +f2*g1     +f3*g0      +f4*g9_19  +f5*g8_19   +f6*g7_19  +f7*g6_19   +f8*g5_19  +f9*g4_19;
  r4=f0*g4     +f1_2*g3    +f2*g2     +f3_2*g1    +f4*g0     +f5_2*g9_19 +f6*g8_19  +f7_2*g7_19 +f8*g6_19  +f9_2*g5_19;
  r5=f0*g5     +f1*g4      +f2*g3     +f3*g2      +f4*g1     +f5*g0      +f6*g9_19  +f7*g8_19   +f8*g7_19  +f9*g6_19;
  r6=f0*g6     +f1_2*g5    +f2*g4     +f3_2*g3    +f4*g2     +f5_2*g1    +f6*g0     +f7_2*g9_19 +f8*g8_19  +f9_2*g7_19;
  r7=f0*g7     +f1*g6      +f2*g5     +f3*g4      +f4*g3     +f5*g2      +f6*g1     +f7*g0      +f8*g9_19  +f9*g8_19;
  r8=f0*g8     +f1_2*g7    +f2*g6     +f3_2*g5    +f4*g4     +f5_2*g3    +f6*g2     +f7_2*g1    +f8*g0     +f9_2*g9_19;
  r9=f0*g9     +f1*g8      +f2*g7     +f3*g6      +f4*g5     +f5*g4      +f6*g3     +f7*g2      +f8*g1     +f9*g0;
  carry=r0>>26; r1+=carry; r0&=0x3ffffff;
  carry=r1>>25; r2+=carry; r1&=0x1ffffff;
  carry=r2>>26; r3+=carry; r2&=0x3ffffff;
  carry=r3>>25; r4+=carry; r3&=0x1ffffff;
  carry=r4>>26; r5+=carry; r4&=0x3ffffff;
  carry=r5>>25; r6+=carry; r5&=0x1ffffff;
  carry=r6>>26; r7+=carry; r6&=0x3ffffff;
  carry=r7>>25; r8+=carry; r7&=0x1ffffff;
  carry=r8>>26; r9+=carry; r8&=0x3ffffff;
  carry=r9>>25; r0+=19*carry; r9&=0x1ffffff;
  carry=r0>>26; r1+=carry; r0&=0x3ffffff;
  h[0]=(uint32_t)r0;h[1]=(uint32_t)r1;h[2]=(uint32_t)r2;h[3]=(uint32_t)r3;h[4]=(uint32_t)r4;
  h[5]=(uint32_t)r5;h[6]=(uint32_t)r6;h[7]=(uint32_t)r7;h[8]=(uint32_t)r8;h[9]=(uint32_t)r9;
}
static void fe25519_sq(fe25519 h, const fe25519 f) { fe25519_mul(h, f, f); }
/* Multiply by a24 = (486662-2)/4 = 121665, the Montgomery ladder constant.
 * (Not ref10's fe_mul121666 -- that belongs to a differently arranged
 * ladder step; this AA + a24*E formulation uses 121665.) */
static void fe25519_mul_a24(fe25519 h, const fe25519 f) {
  int i; int64_t carry;
  int64_t t[10]; for(i=0;i<10;i++) t[i]=(int64_t)f[i]*121665;
  carry=t[9]>>25; t[0]+=19*carry; t[9]&=0x1ffffff;
  carry=t[0]>>26; t[1]+=carry; t[0]&=0x3ffffff;
  carry=t[1]>>25; t[2]+=carry; t[1]&=0x1ffffff;
  carry=t[2]>>26; t[3]+=carry; t[2]&=0x3ffffff;
  carry=t[3]>>25; t[4]+=carry; t[3]&=0x1ffffff;
  carry=t[4]>>26; t[5]+=carry; t[4]&=0x3ffffff;
  carry=t[5]>>25; t[6]+=carry; t[5]&=0x1ffffff;
  carry=t[6]>>26; t[7]+=carry; t[6]&=0x3ffffff;
  carry=t[7]>>25; t[8]+=carry; t[7]&=0x1ffffff;
  carry=t[8]>>26; t[9]+=carry; t[8]&=0x3ffffff;
  for(i=0;i<10;i++) h[i]=(uint32_t)t[i];
}
static void fe25519_invert(fe25519 out, const fe25519 z) {
  fe25519 t0,t1,t2,t3; int i;
  fe25519_sq(t0,z);
  fe25519_sq(t1,t0); fe25519_sq(t1,t1);
  fe25519_mul(t1,z,t1);
  fe25519_mul(t0,t0,t1);
  fe25519_sq(t2,t0); fe25519_mul(t1,t1,t2);
  fe25519_sq(t2,t1); for(i=1;i<5;i++) fe25519_sq(t2,t2);
  fe25519_mul(t1,t2,t1);
  fe25519_sq(t2,t1); for(i=1;i<10;i++) fe25519_sq(t2,t2);
  fe25519_mul(t2,t2,t1);
  fe25519_sq(t3,t2); for(i=1;i<20;i++) fe25519_sq(t3,t3);
  fe25519_mul(t2,t3,t2);
  fe25519_sq(t2,t2); for(i=1;i<10;i++) fe25519_sq(t2,t2);
  fe25519_mul(t1,t2,t1);
  fe25519_sq(t2,t1); for(i=1;i<50;i++) fe25519_sq(t2,t2);
  fe25519_mul(t2,t2,t1);
  fe25519_sq(t3,t2); for(i=1;i<100;i++) fe25519_sq(t3,t3);
  fe25519_mul(t2,t3,t2);
  fe25519_sq(t2,t2); for(i=1;i<50;i++) fe25519_sq(t2,t2);
  fe25519_mul(t1,t2,t1);
  fe25519_sq(t1,t1); for(i=1;i<5;i++) fe25519_sq(t1,t1);
  fe25519_mul(out,t1,t0);
}

static void x25519_ladder(const uint8_t *k, const uint8_t *u, uint8_t *out)
{
  fe25519 x1,x2,x3,z2,z3,A,AA,B,BB,E,C,D,DA,CB,tmp;
  uint8_t e[32]; int i; uint32_t swap=0, b;
  memcpy(e,k,32);
  e[0] &= 248; e[31] &= 127; e[31] |= 64;
  fe25519_from_bytes(x1,u);
  fe25519_1(x2); fe25519_0(z2);
  fe25519_copy(x3,x1); fe25519_1(z3);
  for (i=254; i>=0; i--) {
    b=(e[i/8]>>(i&7))&1;
    swap^=b; fe25519_cswap(x2,x3,swap); fe25519_cswap(z2,z3,swap); swap=b;
    fe25519_add(A,x2,z2); fe25519_sq(AA,A);
    fe25519_sub(B,x2,z2); fe25519_sq(BB,B);
    fe25519_sub(E,AA,BB);
    fe25519_add(C,x3,z3); fe25519_sub(D,x3,z3);
    fe25519_mul(DA,D,A); fe25519_mul(CB,C,B);
    fe25519_add(tmp,DA,CB); fe25519_sq(x3,tmp);
    fe25519_sub(tmp,DA,CB); fe25519_sq(z3,tmp); fe25519_mul(z3,z3,x1);
    fe25519_mul(x2,AA,BB);
    fe25519_mul_a24(tmp,E); fe25519_add(tmp,AA,tmp);
    fe25519_mul(z2,E,tmp);
  }
  fe25519_cswap(x2,x3,swap); fe25519_cswap(z2,z3,swap);
  fe25519_invert(z2,z2);
  fe25519_mul(x2,x2,z2);
  fe25519_to_bytes(out,x2);
}

int zz9k_soft_x25519(uint8_t out[32], const uint8_t scalar[32],
                     const uint8_t point[32])
{
  uint32_t zero=0; int i;
  x25519_ladder(scalar, point, out);
  for (i=0;i<32;i++) zero|=out[i];
  return zero!=0; /* 0 = all-zero output (small-order point), reject */
}

int zz9k_soft_chacha20_poly1305_decrypt(
    uint8_t *plaintext,
    const uint8_t *ciphertext, uint32_t length,
    const uint8_t *aad, uint32_t aad_length,
    const uint8_t tag[ZZ9K_SOFT_POLY1305_TAG_BYTES],
    const uint8_t key[ZZ9K_SOFT_CHACHA20_KEY_BYTES],
    const uint8_t nonce[ZZ9K_SOFT_CHACHA20_NONCE_BYTES])
{
  uint8_t expected[16];
  uint32_t diff = 0;
  uint32_t i;

  zz9k_soft_chacha20_poly1305_tag(expected, ciphertext, length, aad,
                                  aad_length, key, nonce);
  for (i = 0; i < 16U; i++) {
    diff |= (uint32_t)(expected[i] ^ tag[i]);
  }
  if (diff != 0U) {
    return 0;
  }

  zz9k_soft_chacha20_xor(plaintext, ciphertext, length, key, nonce, 1U);
  return 1;
}

/* ---- P-256 (secp256r1) ECDH / ECDSA verify and RSA PKCS#1 v1.5 verify ----
 *
 * Clean-room standard-form (non-Montgomery) reference, written for obvious
 * correctness rather than speed: a generic little-endian uint32_t big-integer
 * core (schoolbook multiply, binary long-division reduction, square-and-
 * multiply modular exponentiation) underneath P-256 point arithmetic in
 * Jacobian coordinates. Uses only 32x32->64 multiplies so an m68k compiler
 * can emit it. Validated against RFC 7748 / FIPS 186-4 / RFC 8017 known-answer
 * tests and cross-checked against OpenSSL. This is the KAT oracle and software
 * fallback; the ARM firmware uses BearSSL for the real offload path.
 */

/* ===== Generic big-integer core (little-endian uint32_t limbs) ===== */

#define BN_MAX_LIMBS 128           /* up to 4096-bit (RSA-2048/3072/4096) */

/* Compare a and b (len limbs). Returns -1, 0, or 1. */
static int bn_cmp(const uint32_t *a, const uint32_t *b, int len)
{
  int i;
  for (i = len - 1; i >= 0; i--) {
    if (a[i] < b[i]) return -1;
    if (a[i] > b[i]) return 1;
  }
  return 0;
}

/* r = a + b (len limbs). Returns the carry out (0 or 1). */
static uint32_t bn_add(uint32_t *r, const uint32_t *a, const uint32_t *b,
                       int len)
{
  uint32_t carry = 0;
  int i;
  for (i = 0; i < len; i++) {
    uint64_t sum = (uint64_t)a[i] + b[i] + carry;
    r[i] = (uint32_t)sum;
    carry = (uint32_t)(sum >> 32);
  }
  return carry;
}

/* r = a - b (len limbs). Returns the borrow out (0 or 1). */
static uint32_t bn_sub(uint32_t *r, const uint32_t *a, const uint32_t *b,
                       int len)
{
  uint32_t borrow = 0;
  int i;
  for (i = 0; i < len; i++) {
    uint64_t diff = (uint64_t)a[i] - b[i] - borrow;
    r[i] = (uint32_t)diff;
    borrow = (uint32_t)((diff >> 32) & 1U);
  }
  return borrow;
}

/* Full product r[2*len] = a[len] * b[len]. */
static void bn_mul(uint32_t *r, const uint32_t *a, const uint32_t *b, int len)
{
  int i, j;
  for (i = 0; i < 2 * len; i++) r[i] = 0;
  for (i = 0; i < len; i++) {
    uint64_t carry = 0;
    for (j = 0; j < len; j++) {
      carry += (uint64_t)a[i] * b[j] + r[i + j];
      r[i + j] = (uint32_t)carry;
      carry >>= 32;
    }
    r[i + len] = (uint32_t)carry;
  }
}

/* Bit i of a (len limbs); 0 if out of range. */
static int bn_bit(const uint32_t *a, int len, int i)
{
  int w = i >> 5;
  if (w < 0 || w >= len) return 0;
  return (int)((a[w] >> (i & 31)) & 1U);
}

/* Highest set bit position + 1 (bit length); 0 if a == 0. */
static int bn_bitlen(const uint32_t *a, int len)
{
  int i, b;
  for (i = len - 1; i >= 0; i--) {
    if (a[i]) {
      for (b = 31; b >= 0; b--) {
        if ((a[i] >> b) & 1U) return i * 32 + b + 1;
      }
    }
  }
  return 0;
}

/* r[n] = x[xlen] mod m[n], by binary long division. n <= BN_MAX_LIMBS. */
static void bn_mod(uint32_t *r, const uint32_t *x, int xlen,
                   const uint32_t *m, int n)
{
  uint32_t acc[BN_MAX_LIMBS + 1];
  uint32_t mm[BN_MAX_LIMBS + 1];
  int bits = bn_bitlen(x, xlen);
  int i, k;

  for (i = 0; i <= n; i++) {
    acc[i] = 0;
    mm[i] = (i < n) ? m[i] : 0U;
  }

  for (i = bits - 1; i >= 0; i--) {
    uint32_t carry = 0;
    for (k = 0; k <= n; k++) {           /* acc <<= 1 across n+1 limbs */
      uint32_t newcarry = acc[k] >> 31;
      acc[k] = (acc[k] << 1) | carry;
      carry = newcarry;
    }
    acc[0] |= (uint32_t)bn_bit(x, xlen, i);
    if (bn_cmp(acc, mm, n + 1) >= 0) {
      bn_sub(acc, acc, mm, n + 1);
    }
  }
  for (i = 0; i < n; i++) r[i] = acc[i];
}

/* r[n] = a[n] * b[n] mod m[n]. */
static void bn_mulmod(uint32_t *r, const uint32_t *a, const uint32_t *b,
                      const uint32_t *m, int n)
{
  uint32_t prod[2 * BN_MAX_LIMBS];
  bn_mul(prod, a, b, n);
  bn_mod(r, prod, 2 * n, m, n);
}

/* r[n] = base^exp mod m[n]. exp is read as ebits bits from exp[] (LE). */
static void bn_modexp(uint32_t *r, const uint32_t *base, const uint32_t *exp,
                      int ebits, const uint32_t *m, int n)
{
  uint32_t acc[BN_MAX_LIMBS];
  uint32_t b[BN_MAX_LIMBS];
  int explimbs = (ebits + 31) / 32;
  int i;

  for (i = 0; i < n; i++) {
    acc[i] = 0;
    b[i] = base[i];
  }
  acc[0] = 1U;
  bn_mod(b, b, n, m, n);                  /* reduce base in case base >= m */

  for (i = ebits - 1; i >= 0; i--) {
    bn_mulmod(acc, acc, acc, m, n);
    if (bn_bit(exp, explimbs, i)) {
      bn_mulmod(acc, acc, b, m, n);
    }
  }
  for (i = 0; i < n; i++) r[i] = acc[i];
}

/* Load nbytes big-endian into LE limbs (nbytes a multiple of 4). */
static void bn_from_be(uint32_t *r, const uint8_t *be, int nbytes)
{
  int n = nbytes / 4, i;
  for (i = 0; i < n; i++) {
    const uint8_t *p = be + (n - 1 - i) * 4;
    r[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
  }
}

/* Store n LE limbs as big-endian into n*4 bytes. */
static void bn_to_be(uint8_t *be, const uint32_t *a, int n)
{
  int i;
  for (i = 0; i < n; i++) {
    uint32_t v = a[n - 1 - i];
    be[i * 4]     = (uint8_t)(v >> 24);
    be[i * 4 + 1] = (uint8_t)(v >> 16);
    be[i * 4 + 2] = (uint8_t)(v >> 8);
    be[i * 4 + 3] = (uint8_t)v;
  }
}

/* ===== P-256 (secp256r1) ===== */

#define P256_LIMBS 8

/* Field prime p, group order n, curve coefficient b, and generator G
 * (all little-endian uint32_t[8]). The curve coefficient a = -3 is implicit. */
static const uint32_t p256_p[P256_LIMBS] = {
  0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0x00000000U,
  0x00000000U, 0x00000000U, 0x00000001U, 0xFFFFFFFFU
};
static const uint32_t p256_n[P256_LIMBS] = {
  0xFC632551U, 0xF3B9CAC2U, 0xA7179E84U, 0xBCE6FAADU,
  0xFFFFFFFFU, 0xFFFFFFFFU, 0x00000000U, 0xFFFFFFFFU
};
static const uint32_t p256_b[P256_LIMBS] = {
  0x27D2604BU, 0x3BCE3C3EU, 0xCC53B0F6U, 0x651D06B0U,
  0x769886BCU, 0xB3EBBD55U, 0xAA3A93E7U, 0x5AC635D8U
};
static const uint32_t p256_gx[P256_LIMBS] = {
  0xD898C296U, 0xF4A13945U, 0x2DEB33A0U, 0x77037D81U,
  0x63A440F2U, 0xF8BCE6E5U, 0xE12C4247U, 0x6B17D1F2U
};
static const uint32_t p256_gy[P256_LIMBS] = {
  0x37BF51F5U, 0xCBB64068U, 0x6B315ECEU, 0x2BCE3357U,
  0x7C0F9E16U, 0x8EE7EB4AU, 0xFE1A7F9BU, 0x4FE342E2U
};

/* Field arithmetic over F_p (inputs assumed < p). */
static void fp_add(uint32_t *r, const uint32_t *a, const uint32_t *b)
{
  uint32_t t[P256_LIMBS];
  uint32_t c = bn_add(t, a, b, P256_LIMBS);
  if (c || bn_cmp(t, p256_p, P256_LIMBS) >= 0) {
    bn_sub(t, t, p256_p, P256_LIMBS);
  }
  memcpy(r, t, sizeof t);
}

static void fp_sub(uint32_t *r, const uint32_t *a, const uint32_t *b)
{
  uint32_t t[P256_LIMBS];
  if (bn_sub(t, a, b, P256_LIMBS)) {
    bn_add(t, t, p256_p, P256_LIMBS);
  }
  memcpy(r, t, sizeof t);
}

static void fp_mul(uint32_t *r, const uint32_t *a, const uint32_t *b)
{
  bn_mulmod(r, a, b, p256_p, P256_LIMBS);
}

static void fp_sqr(uint32_t *r, const uint32_t *a)
{
  bn_mulmod(r, a, a, p256_p, P256_LIMBS);
}

/* r = a^-1 mod p via Fermat (a^(p-2) mod p). */
static void fp_inv(uint32_t *r, const uint32_t *a)
{
  uint32_t e[P256_LIMBS], two[P256_LIMBS];
  int i;
  for (i = 0; i < P256_LIMBS; i++) two[i] = 0;
  two[0] = 2U;
  bn_sub(e, p256_p, two, P256_LIMBS);      /* p - 2 */
  bn_modexp(r, a, e, 256, p256_p, P256_LIMBS);
}

static int fp_is_zero(const uint32_t *a)
{
  int i;
  uint32_t z = 0;
  for (i = 0; i < P256_LIMBS; i++) z |= a[i];
  return z == 0U;
}

/* Point in Jacobian coordinates: affine = (X/Z^2, Y/Z^3); infinity has Z = 0. */
typedef struct {
  uint32_t X[P256_LIMBS];
  uint32_t Y[P256_LIMBS];
  uint32_t Z[P256_LIMBS];
} p256_pt;

static void p256_set_infinity(p256_pt *r)
{
  int i;
  for (i = 0; i < P256_LIMBS; i++) {
    r->X[i] = 0;
    r->Y[i] = 0;
    r->Z[i] = 0;
  }
  r->X[0] = 1U;
  r->Y[0] = 1U;
}

/* out = 2*P. out may alias P. */
static void p256_double(p256_pt *out, const p256_pt *P)
{
  uint32_t S[P256_LIMBS], M[P256_LIMBS], Y2[P256_LIMBS];
  uint32_t t1[P256_LIMBS], t2[P256_LIMBS];
  uint32_t X3[P256_LIMBS], Y3[P256_LIMBS], Z3[P256_LIMBS];

  if (fp_is_zero(P->Z) || fp_is_zero(P->Y)) {
    p256_set_infinity(out);
    return;
  }
  fp_sqr(Y2, P->Y);                         /* Y^2 */
  fp_mul(S, P->X, Y2);                      /* X*Y^2 */
  fp_add(S, S, S); fp_add(S, S, S);         /* S = 4*X*Y^2 */
  fp_sqr(t1, P->Z);                         /* Z^2 */
  fp_sub(t2, P->X, t1);                     /* X - Z^2 */
  fp_add(t1, P->X, t1);                     /* X + Z^2 */
  fp_mul(M, t2, t1);                        /* (X-Z^2)(X+Z^2) */
  fp_add(t1, M, M); fp_add(M, t1, M);       /* M = 3*(X-Z^2)(X+Z^2) */
  fp_sqr(X3, M); fp_sub(X3, X3, S); fp_sub(X3, X3, S);   /* X3 = M^2 - 2S */
  fp_sub(t1, S, X3); fp_mul(Y3, M, t1);     /* M*(S - X3) */
  fp_sqr(t2, Y2);                           /* Y^4 */
  fp_add(t2, t2, t2); fp_add(t2, t2, t2); fp_add(t2, t2, t2); /* 8*Y^4 */
  fp_sub(Y3, Y3, t2);                       /* Y3 = M*(S-X3) - 8*Y^4 */
  fp_mul(Z3, P->Y, P->Z); fp_add(Z3, Z3, Z3);            /* Z3 = 2*Y*Z */
  memcpy(out->X, X3, sizeof X3);
  memcpy(out->Y, Y3, sizeof Y3);
  memcpy(out->Z, Z3, sizeof Z3);
}

/* out = P + Q. out may alias P or Q. */
static void p256_add(p256_pt *out, const p256_pt *P, const p256_pt *Q)
{
  uint32_t Z1Z1[P256_LIMBS], Z2Z2[P256_LIMBS];
  uint32_t U1[P256_LIMBS], U2[P256_LIMBS], S1[P256_LIMBS], S2[P256_LIMBS];
  uint32_t H[P256_LIMBS], R[P256_LIMBS], H2[P256_LIMBS], H3[P256_LIMBS];
  uint32_t U1H2[P256_LIMBS], t[P256_LIMBS];
  uint32_t X3[P256_LIMBS], Y3[P256_LIMBS], Z3[P256_LIMBS];

  if (fp_is_zero(P->Z)) { *out = *Q; return; }
  if (fp_is_zero(Q->Z)) { *out = *P; return; }

  fp_sqr(Z1Z1, P->Z);
  fp_sqr(Z2Z2, Q->Z);
  fp_mul(U1, P->X, Z2Z2);                   /* U1 = X1*Z2^2 */
  fp_mul(U2, Q->X, Z1Z1);                   /* U2 = X2*Z1^2 */
  fp_mul(S1, P->Y, Q->Z); fp_mul(S1, S1, Z2Z2);   /* S1 = Y1*Z2^3 */
  fp_mul(S2, Q->Y, P->Z); fp_mul(S2, S2, Z1Z1);   /* S2 = Y2*Z1^3 */

  if (bn_cmp(U1, U2, P256_LIMBS) == 0) {
    if (bn_cmp(S1, S2, P256_LIMBS) != 0) {  /* P = -Q -> infinity */
      p256_set_infinity(out);
      return;
    }
    p256_double(out, P);                    /* P = Q -> doubling */
    return;
  }

  fp_sub(H, U2, U1);                        /* H = U2 - U1 */
  fp_sub(R, S2, S1);                        /* R = S2 - S1 */
  fp_sqr(H2, H); fp_mul(H3, H2, H);         /* H^2, H^3 */
  fp_mul(U1H2, U1, H2);                     /* U1*H^2 */
  fp_sqr(X3, R); fp_sub(X3, X3, H3);
  fp_sub(X3, X3, U1H2); fp_sub(X3, X3, U1H2);           /* X3 = R^2-H^3-2U1H^2 */
  fp_sub(t, U1H2, X3); fp_mul(Y3, R, t);
  fp_mul(t, S1, H3); fp_sub(Y3, Y3, t);     /* Y3 = R*(U1H^2-X3) - S1*H^3 */
  fp_mul(Z3, H, P->Z); fp_mul(Z3, Z3, Q->Z);            /* Z3 = H*Z1*Z2 */
  memcpy(out->X, X3, sizeof X3);
  memcpy(out->Y, Y3, sizeof Y3);
  memcpy(out->Z, Z3, sizeof Z3);
}

/* out = k * P, left-to-right double-and-add. k is a 256-bit LE scalar. */
static void p256_scalar(p256_pt *out, const uint32_t k[P256_LIMBS],
                        const p256_pt *P)
{
  p256_pt acc;
  int i;
  p256_set_infinity(&acc);
  for (i = 255; i >= 0; i--) {
    p256_double(&acc, &acc);
    if (bn_bit(k, P256_LIMBS, i)) {
      p256_add(&acc, &acc, P);
    }
  }
  *out = acc;
}

/* Affine x of P into x_out (LE limbs). Returns 0 if P is the infinity point. */
static int p256_affine_x(uint32_t x_out[P256_LIMBS], const p256_pt *P)
{
  uint32_t zinv[P256_LIMBS], zinv2[P256_LIMBS];
  if (fp_is_zero(P->Z)) return 0;
  fp_inv(zinv, P->Z);
  fp_sqr(zinv2, zinv);
  fp_mul(x_out, P->X, zinv2);
  return 1;
}

/* Validate affine (x, y) lies on the curve: y^2 == x^3 - 3x + b (mod p). */
static int p256_point_valid(const uint32_t x[P256_LIMBS],
                            const uint32_t y[P256_LIMBS])
{
  uint32_t lhs[P256_LIMBS], rhs[P256_LIMBS], three_x[P256_LIMBS];
  if (bn_cmp(x, p256_p, P256_LIMBS) >= 0 ||
      bn_cmp(y, p256_p, P256_LIMBS) >= 0) {
    return 0;
  }
  fp_sqr(lhs, y);                           /* y^2 */
  fp_sqr(rhs, x); fp_mul(rhs, rhs, x);      /* x^3 */
  fp_add(three_x, x, x); fp_add(three_x, three_x, x);   /* 3x */
  fp_sub(rhs, rhs, three_x);                /* x^3 - 3x */
  fp_add(rhs, rhs, p256_b);                 /* x^3 - 3x + b */
  return bn_cmp(lhs, rhs, P256_LIMBS) == 0;
}

/* Load an uncompressed (0x04 || X || Y) point into a Jacobian point with Z=1,
 * validating it lies on the curve. Returns 0 on malformed/off-curve input. */
static int p256_load_public(p256_pt *out, const uint8_t public_point[65])
{
  int i;
  if (public_point[0] != 0x04U) return 0;
  bn_from_be(out->X, public_point + 1, 32);
  bn_from_be(out->Y, public_point + 33, 32);
  if (!p256_point_valid(out->X, out->Y)) return 0;
  for (i = 0; i < P256_LIMBS; i++) out->Z[i] = 0;
  out->Z[0] = 1U;
  return 1;
}

int zz9k_soft_p256_ecdh(uint8_t shared_secret[32],
                        const uint8_t private_key[32],
                        const uint8_t public_point[65])
{
  p256_pt Q, R;
  uint32_t d[P256_LIMBS], x[P256_LIMBS];

  if (!p256_load_public(&Q, public_point)) return 0;
  bn_from_be(d, private_key, 32);
  if (fp_is_zero(d) || bn_cmp(d, p256_n, P256_LIMBS) >= 0) return 0; /* d in [1,n) */
  p256_scalar(&R, d, &Q);
  if (!p256_affine_x(x, &R)) return 0;      /* result is infinity -> reject */
  bn_to_be(shared_secret, x, P256_LIMBS);
  return 1;
}

int zz9k_soft_p256_keygen(uint8_t public_point[65],
                          const uint8_t private_key[32])
{
  p256_pt G, R;
  uint32_t d[P256_LIMBS], x[P256_LIMBS], y[P256_LIMBS];
  uint32_t zinv[P256_LIMBS], zinv2[P256_LIMBS], zinv3[P256_LIMBS];
  int i;

  bn_from_be(d, private_key, 32);
  if (fp_is_zero(d) || bn_cmp(d, p256_n, P256_LIMBS) >= 0) return 0; /* d in [1,n) */

  for (i = 0; i < P256_LIMBS; i++) {
    G.X[i] = p256_gx[i];
    G.Y[i] = p256_gy[i];
    G.Z[i] = 0;
  }
  G.Z[0] = 1U;
  p256_scalar(&R, d, &G);                     /* R = d*G */
  if (fp_is_zero(R.Z)) return 0;              /* infinity -> reject */

  /* Jacobian -> affine: x = X/Z^2, y = Y/Z^3. */
  fp_inv(zinv, R.Z);
  fp_sqr(zinv2, zinv);
  fp_mul(zinv3, zinv2, zinv);
  fp_mul(x, R.X, zinv2);
  fp_mul(y, R.Y, zinv3);

  public_point[0] = 0x04U;                    /* uncompressed SEC1 form */
  bn_to_be(public_point + 1, x, P256_LIMBS);
  bn_to_be(public_point + 33, y, P256_LIMBS);
  return 1;
}

int zz9k_soft_ecdsa_verify_p256(const uint8_t signature_r[32],
                                const uint8_t signature_s[32],
                                const uint8_t message_hash[32],
                                const uint8_t public_point[65])
{
  p256_pt G, Q, R1, R2, R;
  uint32_t r[P256_LIMBS], s[P256_LIMBS], z[P256_LIMBS], w[P256_LIMBS];
  uint32_t u1[P256_LIMBS], u2[P256_LIMBS], e[P256_LIMBS], two[P256_LIMBS];
  uint32_t x[P256_LIMBS], v[P256_LIMBS];
  int i;

  bn_from_be(r, signature_r, 32);
  bn_from_be(s, signature_s, 32);
  bn_from_be(z, message_hash, 32);
  if (fp_is_zero(r) || bn_cmp(r, p256_n, P256_LIMBS) >= 0) return 0; /* r in [1,n) */
  if (fp_is_zero(s) || bn_cmp(s, p256_n, P256_LIMBS) >= 0) return 0; /* s in [1,n) */
  if (!p256_load_public(&Q, public_point)) return 0;

  for (i = 0; i < P256_LIMBS; i++) two[i] = 0;
  two[0] = 2U;
  bn_sub(e, p256_n, two, P256_LIMBS);       /* n - 2 */
  bn_modexp(w, s, e, 256, p256_n, P256_LIMBS);          /* w = s^-1 mod n */
  bn_mulmod(u1, z, w, p256_n, P256_LIMBS);  /* u1 = z*w mod n */
  bn_mulmod(u2, r, w, p256_n, P256_LIMBS);  /* u2 = r*w mod n */

  for (i = 0; i < P256_LIMBS; i++) {
    G.X[i] = p256_gx[i];
    G.Y[i] = p256_gy[i];
    G.Z[i] = 0;
  }
  G.Z[0] = 1U;
  p256_scalar(&R1, u1, &G);
  p256_scalar(&R2, u2, &Q);
  p256_add(&R, &R1, &R2);                    /* R = u1*G + u2*Q */
  if (!p256_affine_x(x, &R)) return 0;       /* infinity -> invalid */
  bn_mod(v, x, P256_LIMBS, p256_n, P256_LIMBS);         /* v = R.x mod n */
  return bn_cmp(v, r, P256_LIMBS) == 0;
}

/* ===== RSA PKCS#1 v1.5 (SHA-256) signature verification ===== */

int zz9k_soft_rsa_verify_pkcs1_sha256(const uint8_t *signature,
                                      uint32_t sig_len,
                                      const uint8_t *message_hash,
                                      const uint8_t *n,
                                      uint32_t n_bits,
                                      uint32_t e)
{
  static const uint8_t sha256_digest_info[19] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
    0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
  };
  uint32_t sig[BN_MAX_LIMBS], mod[BN_MAX_LIMBS], m[BN_MAX_LIMBS], ev[1];
  uint8_t em[BN_MAX_LIMBS * 4];
  int limbs = (int)(n_bits / 32U);
  int emlen = limbs * 4;
  int ebits, pos, ps_end, i;

  if (n_bits == 0U || (n_bits % 32U) != 0U || limbs > BN_MAX_LIMBS) return 0;
  if (sig_len != (uint32_t)emlen) return 0;

  bn_from_be(sig, signature, emlen);
  bn_from_be(mod, n, emlen);
  if (bn_cmp(sig, mod, limbs) >= 0) return 0;           /* signature < modulus */

  ev[0] = e;
  ebits = bn_bitlen(ev, 1);
  if (ebits == 0) return 0;
  bn_modexp(m, sig, ev, ebits, mod, limbs);             /* m = sig^e mod n */
  bn_to_be(em, m, limbs);

  /* EM = 0x00 || 0x01 || PS(0xFF..) || 0x00 || DigestInfo || hash. */
  if (em[0] != 0x00U || em[1] != 0x01U) return 0;
  pos = 2;
  while (pos < emlen && em[pos] == 0xFFU) pos++;
  ps_end = pos;
  if (ps_end - 2 < 8) return 0;                         /* PS must be >= 8 */
  if (ps_end >= emlen || em[ps_end] != 0x00U) return 0;
  pos = ps_end + 1;                                     /* start of T */
  if (emlen - pos != 19 + 32) return 0;
  for (i = 0; i < 19; i++) {
    if (em[pos + i] != sha256_digest_info[i]) return 0;
  }
  for (i = 0; i < 32; i++) {
    if (em[pos + 19 + i] != message_hash[i]) return 0;
  }
  return 1;
}

/* ---- AES-128/256-GCM (FIPS-197 + NIST SP 800-38D) ----
 *
 * Compact correctness-first reference: byte-oriented AES (S-box + GF(2^8)
 * MixColumns) and a bitwise GF(2^128) GHASH. The firmware offload uses
 * BearSSL's constant-time aes_ct/ghash_ctmul; this software path is the KAT
 * oracle and m68k fallback, validated against the NIST GCM test vectors.
 */

static const uint8_t zz9k_aes_sbox[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t zz9k_aes_rcon[10] = {
  0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

/* Round keys in column-major form: rk[round][4*col + row]. nr = 10 or 14. */
typedef struct {
  uint8_t rk[15][16];
  int nr;
} zz9k_aes_ctx;

static uint8_t zz9k_aes_gmul(uint8_t a, uint8_t b)
{
  uint8_t p = 0;
  int i;
  for (i = 0; i < 8; i++) {
    if (b & 1U) p ^= a;
    {
      uint8_t hi = (uint8_t)(a & 0x80U);
      a = (uint8_t)(a << 1);
      if (hi) a ^= 0x1bU;
    }
    b = (uint8_t)(b >> 1);
  }
  return p;
}

/* Expand a 16- or 32-byte key. Returns 1 on success, 0 on bad key length. */
static int zz9k_aes_init(zz9k_aes_ctx *c, const uint8_t *key, uint32_t key_len)
{
  uint8_t w[60][4];
  int nk, nr, total, i, r, col, row;

  if (key_len == 16U) { nk = 4; }
  else if (key_len == 32U) { nk = 8; }
  else { return 0; }
  nr = nk + 6;
  total = 4 * (nr + 1);
  c->nr = nr;

  for (i = 0; i < nk; i++) {
    w[i][0] = key[4 * i];
    w[i][1] = key[4 * i + 1];
    w[i][2] = key[4 * i + 2];
    w[i][3] = key[4 * i + 3];
  }
  for (i = nk; i < total; i++) {
    uint8_t t[4];
    t[0] = w[i - 1][0]; t[1] = w[i - 1][1];
    t[2] = w[i - 1][2]; t[3] = w[i - 1][3];
    if ((i % nk) == 0) {
      uint8_t tmp = t[0];                 /* RotWord then SubWord */
      t[0] = (uint8_t)(zz9k_aes_sbox[t[1]] ^ zz9k_aes_rcon[i / nk - 1]);
      t[1] = zz9k_aes_sbox[t[2]];
      t[2] = zz9k_aes_sbox[t[3]];
      t[3] = zz9k_aes_sbox[tmp];
    } else if (nk > 6 && (i % nk) == 4) {
      t[0] = zz9k_aes_sbox[t[0]];
      t[1] = zz9k_aes_sbox[t[1]];
      t[2] = zz9k_aes_sbox[t[2]];
      t[3] = zz9k_aes_sbox[t[3]];
    }
    w[i][0] = (uint8_t)(w[i - nk][0] ^ t[0]);
    w[i][1] = (uint8_t)(w[i - nk][1] ^ t[1]);
    w[i][2] = (uint8_t)(w[i - nk][2] ^ t[2]);
    w[i][3] = (uint8_t)(w[i - nk][3] ^ t[3]);
  }
  for (r = 0; r <= nr; r++) {
    for (col = 0; col < 4; col++) {
      for (row = 0; row < 4; row++) {
        c->rk[r][4 * col + row] = w[4 * r + col][row];
      }
    }
  }
  return 1;
}

static void zz9k_aes_encrypt_block(const zz9k_aes_ctx *c,
                                   const uint8_t in[16], uint8_t out[16])
{
  uint8_t s[16];
  int round, i, col;

  for (i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ c->rk[0][i]);

  for (round = 1; round <= c->nr; round++) {
    uint8_t t[16];
    /* SubBytes */
    for (i = 0; i < 16; i++) s[i] = zz9k_aes_sbox[s[i]];
    /* ShiftRows (column-major: index = 4*col + row) */
    for (col = 0; col < 4; col++) {
      for (i = 0; i < 4; i++) {       /* i = row */
        t[4 * col + i] = s[4 * ((col + i) & 3) + i];
      }
    }
    if (round != c->nr) {
      /* MixColumns */
      for (col = 0; col < 4; col++) {
        uint8_t a0 = t[4 * col + 0], a1 = t[4 * col + 1];
        uint8_t a2 = t[4 * col + 2], a3 = t[4 * col + 3];
        s[4 * col + 0] = (uint8_t)(zz9k_aes_gmul(2, a0) ^ zz9k_aes_gmul(3, a1) ^
                                   a2 ^ a3);
        s[4 * col + 1] = (uint8_t)(a0 ^ zz9k_aes_gmul(2, a1) ^
                                   zz9k_aes_gmul(3, a2) ^ a3);
        s[4 * col + 2] = (uint8_t)(a0 ^ a1 ^ zz9k_aes_gmul(2, a2) ^
                                   zz9k_aes_gmul(3, a3));
        s[4 * col + 3] = (uint8_t)(zz9k_aes_gmul(3, a0) ^ a1 ^ a2 ^
                                   zz9k_aes_gmul(2, a3));
      }
    } else {
      for (i = 0; i < 16; i++) s[i] = t[i];
    }
    /* AddRoundKey */
    for (i = 0; i < 16; i++) s[i] ^= c->rk[round][i];
  }
  for (i = 0; i < 16; i++) out[i] = s[i];
}

/* GF(2^128) multiply per SP 800-38D: x = x . h (both 16-byte, big-endian). */
static void zz9k_ghash_mul(uint8_t x[16], const uint8_t h[16])
{
  uint8_t z[16];
  uint8_t v[16];
  int i, bit;

  for (i = 0; i < 16; i++) { z[i] = 0; v[i] = h[i]; }
  for (i = 0; i < 128; i++) {
    if ((x[i >> 3] >> (7 - (i & 7))) & 1U) {     /* bit i of x, MSB first */
      int j;
      for (j = 0; j < 16; j++) z[j] ^= v[j];
    }
    bit = v[15] & 1U;                            /* LSB of v */
    {
      int j;
      for (j = 15; j > 0; j--) {
        v[j] = (uint8_t)((v[j] >> 1) | (v[j - 1] << 7));
      }
      v[0] = (uint8_t)(v[0] >> 1);
    }
    if (bit) v[0] ^= 0xe1U;                       /* reduction R */
  }
  for (i = 0; i < 16; i++) x[i] = z[i];
}

/* Fold `len` bytes of `data` into the GHASH accumulator `y` (16-byte blocks,
 * zero-padded). */
static void zz9k_ghash_update(uint8_t y[16], const uint8_t h[16],
                              const uint8_t *data, uint32_t len)
{
  uint32_t off = 0;
  while (off < len) {
    uint8_t block[16];
    uint32_t n = len - off;
    uint32_t i;
    if (n > 16U) n = 16U;
    for (i = 0; i < 16U; i++) block[i] = (i < n) ? data[off + i] : 0U;
    for (i = 0; i < 16U; i++) y[i] ^= block[i];
    zz9k_ghash_mul(y, h);
    off += n;
  }
}

static void zz9k_put_be32_local(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Core GCM: derive H, J0, tag mask, run CTR over in->out, compute the tag.
 * `ct` points at the ciphertext for GHASH (== out for encrypt, == in for
 * decrypt), since GHASH is always computed over the ciphertext. */
/* `ghash_first` orders the GHASH-over-ciphertext pass relative to the CTR
 * pass so that in == out (in-place processing, the TLS record pattern) works
 * in both directions: decrypt must GHASH the ciphertext before the CTR pass
 * overwrites it, encrypt must GHASH the ciphertext after producing it. */
static void zz9k_aes_gcm_core(const zz9k_aes_ctx *c,
                              const uint8_t nonce[12],
                              const uint8_t *aad, uint32_t aad_length,
                              const uint8_t *in, uint8_t *out, uint32_t length,
                              const uint8_t *ct, uint8_t tag[16],
                              int ghash_first)
{
  uint8_t h[16];
  uint8_t j0[16];
  uint8_t y[16];
  uint8_t counter[16];
  uint8_t ks[16];
  uint8_t lenblk[16];
  uint8_t zero[16];
  uint32_t off, i;

  for (i = 0; i < 16U; i++) zero[i] = 0;
  zz9k_aes_encrypt_block(c, zero, h);            /* H = E(0) */

  for (i = 0; i < 12U; i++) j0[i] = nonce[i];    /* J0 = IV || 0^31 1 */
  j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;

  /* GHASH(AAD || C || len(AAD)||len(C)); the C part runs before or after the
   * CTR pass per ghash_first. */
  for (i = 0; i < 16U; i++) y[i] = 0;
  zz9k_ghash_update(y, h, aad, aad_length);
  if (ghash_first) {
    zz9k_ghash_update(y, h, ct, length);
  }

  /* CTR encryption starting from inc32(J0). */
  for (i = 0; i < 16U; i++) counter[i] = j0[i];
  off = 0;
  while (off < length) {
    uint32_t ctr = ((uint32_t)counter[12] << 24) | ((uint32_t)counter[13] << 16) |
                   ((uint32_t)counter[14] << 8) | (uint32_t)counter[15];
    uint32_t n = length - off;
    if (n > 16U) n = 16U;
    zz9k_put_be32_local(&counter[12], ctr + 1U);
    zz9k_aes_encrypt_block(c, counter, ks);
    for (i = 0; i < n; i++) out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
    off += n;
  }

  if (!ghash_first) {
    zz9k_ghash_update(y, h, ct, length);
  }
  zz9k_put_be32_local(&lenblk[0], 0U);
  zz9k_put_be32_local(&lenblk[4], aad_length * 8U);
  zz9k_put_be32_local(&lenblk[8], 0U);
  zz9k_put_be32_local(&lenblk[12], length * 8U);
  for (i = 0; i < 16U; i++) y[i] ^= lenblk[i];
  zz9k_ghash_mul(y, h);

  /* Tag = GHASH ^ E(J0). */
  zz9k_aes_encrypt_block(c, j0, ks);
  for (i = 0; i < 16U; i++) tag[i] = (uint8_t)(y[i] ^ ks[i]);
}

int zz9k_soft_aes_gcm_encrypt(uint8_t *ciphertext,
                              uint8_t tag[ZZ9K_SOFT_AES_GCM_TAG_BYTES],
                              const uint8_t *plaintext, uint32_t length,
                              const uint8_t *aad, uint32_t aad_length,
                              const uint8_t *key, uint32_t key_length,
                              const uint8_t nonce[ZZ9K_SOFT_AES_GCM_NONCE_BYTES])
{
  zz9k_aes_ctx c;
  if (!zz9k_aes_init(&c, key, key_length)) return 0;
  zz9k_aes_gcm_core(&c, nonce, aad, aad_length, plaintext, ciphertext, length,
                    ciphertext, tag, 0);
  return 1;
}

int zz9k_soft_aes_gcm_decrypt(uint8_t *plaintext,
                              const uint8_t *ciphertext, uint32_t length,
                              const uint8_t *aad, uint32_t aad_length,
                              const uint8_t tag[ZZ9K_SOFT_AES_GCM_TAG_BYTES],
                              const uint8_t *key, uint32_t key_length,
                              const uint8_t nonce[ZZ9K_SOFT_AES_GCM_NONCE_BYTES])
{
  zz9k_aes_ctx c;
  uint8_t expected[16];
  uint8_t diff = 0;
  uint32_t i;

  if (!zz9k_aes_init(&c, key, key_length)) return 0;
  /* CTR over ciphertext->plaintext; GHASH over the ciphertext input, before
   * the CTR pass so decrypting in place (TLS records) stays correct. */
  zz9k_aes_gcm_core(&c, nonce, aad, aad_length, ciphertext, plaintext, length,
                    ciphertext, expected, 1);
  for (i = 0; i < 16U; i++) diff |= (uint8_t)(expected[i] ^ tag[i]);
  if (diff != 0U) {
    for (i = 0; i < length; i++) plaintext[i] = 0;  /* withhold on failure */
    return 0;
  }
  return 1;
}
