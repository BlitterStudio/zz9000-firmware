/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Small SDK SHA-1/SHA-256/HMAC implementation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_crypto.h"
#include "bearssl/bearssl_ec.h"
#include "bearssl/bearssl_rsa.h"
#include "bearssl/bearssl_block.h"
#include "bearssl/bearssl_aead.h"
#include "bearssl/bearssl_hash.h"
#include <string.h>

typedef struct SDKSHA256Context {
	uint32_t state[8];
	uint64_t bit_length;
	uint8_t buffer[SDK_SHA256_BLOCK_SIZE];
	uint32_t buffer_length;
} SDKSHA256Context;

typedef struct SDKSHA1Context {
	uint32_t state[5];
	uint64_t bit_length;
	uint8_t buffer[SDK_SHA1_BLOCK_SIZE];
	uint32_t buffer_length;
} SDKSHA1Context;

typedef struct SDKPoly1305Context {
	uint32_t r0;
	uint32_t r1;
	uint32_t r2;
	uint32_t r3;
	uint32_t r4;
	uint32_t s1;
	uint32_t s2;
	uint32_t s3;
	uint32_t s4;
	uint32_t h0;
	uint32_t h1;
	uint32_t h2;
	uint32_t h3;
	uint32_t h4;
	uint8_t pad[16];
	uint8_t buffer[16];
	uint32_t buffer_length;
} SDKPoly1305Context;

static const uint32_t sha256_k[64] = {
	0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
	0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
	0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
	0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
	0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
	0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
	0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
	0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
	0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
	0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
	0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
	0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
	0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
	0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
	0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
	0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

static uint32_t rotr32(uint32_t value, uint32_t bits)
{
	return (value >> bits) | (value << (32U - bits));
}

static uint32_t rotl32(uint32_t value, uint32_t bits)
{
	return (value << bits) | (value >> (32U - bits));
}

static uint32_t load_be32(const uint8_t *data)
{
	return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
	       ((uint32_t)data[2] << 8) | data[3];
}

static uint32_t load_le32(const uint8_t *data)
{
	return ((uint32_t)data[0]) | ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void store_be32(uint8_t *data, uint32_t value)
{
	data[0] = (uint8_t)(value >> 24);
	data[1] = (uint8_t)(value >> 16);
	data[2] = (uint8_t)(value >> 8);
	data[3] = (uint8_t)value;
}

static void store_le32(uint8_t *data, uint32_t value)
{
	data[0] = (uint8_t)value;
	data[1] = (uint8_t)(value >> 8);
	data[2] = (uint8_t)(value >> 16);
	data[3] = (uint8_t)(value >> 24);
}

static void store_be64(uint8_t *data, uint64_t value)
{
	uint32_t i;

	for (i = 0; i < 8U; i++)
		data[i] = (uint8_t)(value >> (56U - (i * 8U)));
}

static void sha256_init(SDKSHA256Context *ctx)
{
	ctx->state[0] = 0x6a09e667UL;
	ctx->state[1] = 0xbb67ae85UL;
	ctx->state[2] = 0x3c6ef372UL;
	ctx->state[3] = 0xa54ff53aUL;
	ctx->state[4] = 0x510e527fUL;
	ctx->state[5] = 0x9b05688cUL;
	ctx->state[6] = 0x1f83d9abUL;
	ctx->state[7] = 0x5be0cd19UL;
	ctx->bit_length = 0;
	ctx->buffer_length = 0;
}

static void sha1_init(SDKSHA1Context *ctx)
{
	ctx->state[0] = 0x67452301UL;
	ctx->state[1] = 0xefcdab89UL;
	ctx->state[2] = 0x98badcfeUL;
	ctx->state[3] = 0x10325476UL;
	ctx->state[4] = 0xc3d2e1f0UL;
	ctx->bit_length = 0;
	ctx->buffer_length = 0;
}

static void sha1_transform(SDKSHA1Context *ctx, const uint8_t block[64])
{
	uint32_t w[80];
	uint32_t a;
	uint32_t b;
	uint32_t c;
	uint32_t d;
	uint32_t e;
	uint32_t i;

	for (i = 0; i < 16U; i++)
		w[i] = load_be32(block + (i * 4U));
	for (i = 16U; i < 80U; i++)
		w[i] = rotl32(w[i - 3U] ^ w[i - 8U] ^
		              w[i - 14U] ^ w[i - 16U], 1U);

	a = ctx->state[0];
	b = ctx->state[1];
	c = ctx->state[2];
	d = ctx->state[3];
	e = ctx->state[4];

	for (i = 0; i < 80U; i++) {
		uint32_t f;
		uint32_t k;
		uint32_t temp;

		if (i < 20U) {
			f = (b & c) | ((~b) & d);
			k = 0x5a827999UL;
		} else if (i < 40U) {
			f = b ^ c ^ d;
			k = 0x6ed9eba1UL;
		} else if (i < 60U) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8f1bbcdcUL;
		} else {
			f = b ^ c ^ d;
			k = 0xca62c1d6UL;
		}

		temp = rotl32(a, 5U) + f + e + k + w[i];
		e = d;
		d = c;
		c = rotl32(b, 30U);
		b = a;
		a = temp;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
}

static void sha1_update(SDKSHA1Context *ctx, const uint8_t *data,
                        uint32_t length)
{
	uint32_t used;

	ctx->bit_length += ((uint64_t)length * 8U);
	while (length != 0U) {
		used = SDK_SHA1_BLOCK_SIZE - ctx->buffer_length;
		if (used > length)
			used = length;

		memcpy(ctx->buffer + ctx->buffer_length, data, used);
		ctx->buffer_length += used;
		data += used;
		length -= used;

		if (ctx->buffer_length == SDK_SHA1_BLOCK_SIZE) {
			sha1_transform(ctx, ctx->buffer);
			ctx->buffer_length = 0;
		}
	}
}

static void sha1_final(SDKSHA1Context *ctx,
                       uint8_t digest[SDK_SHA1_DIGEST_SIZE])
{
	uint32_t i;

	ctx->buffer[ctx->buffer_length++] = 0x80U;
	if (ctx->buffer_length > 56U) {
		while (ctx->buffer_length < SDK_SHA1_BLOCK_SIZE)
			ctx->buffer[ctx->buffer_length++] = 0;
		sha1_transform(ctx, ctx->buffer);
		ctx->buffer_length = 0;
	}

	while (ctx->buffer_length < 56U)
		ctx->buffer[ctx->buffer_length++] = 0;

	store_be64(ctx->buffer + 56U, ctx->bit_length);
	sha1_transform(ctx, ctx->buffer);

	for (i = 0; i < 5U; i++)
		store_be32(digest + (i * 4U), ctx->state[i]);
}

static void sha256_transform(SDKSHA256Context *ctx, const uint8_t block[64])
{
	uint32_t w[64];
	uint32_t a;
	uint32_t b;
	uint32_t c;
	uint32_t d;
	uint32_t e;
	uint32_t f;
	uint32_t g;
	uint32_t h;
	uint32_t i;

	for (i = 0; i < 16U; i++)
		w[i] = load_be32(block + (i * 4U));
	for (i = 16U; i < 64U; i++) {
		uint32_t s0 = rotr32(w[i - 15U], 7U) ^
		              rotr32(w[i - 15U], 18U) ^ (w[i - 15U] >> 3);
		uint32_t s1 = rotr32(w[i - 2U], 17U) ^
		              rotr32(w[i - 2U], 19U) ^ (w[i - 2U] >> 10);
		w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
	}

	a = ctx->state[0];
	b = ctx->state[1];
	c = ctx->state[2];
	d = ctx->state[3];
	e = ctx->state[4];
	f = ctx->state[5];
	g = ctx->state[6];
	h = ctx->state[7];

	for (i = 0; i < 64U; i++) {
		uint32_t s1 = rotr32(e, 6U) ^ rotr32(e, 11U) ^ rotr32(e, 25U);
		uint32_t ch = (e & f) ^ ((~e) & g);
		uint32_t temp1 = h + s1 + ch + sha256_k[i] + w[i];
		uint32_t s0 = rotr32(a, 2U) ^ rotr32(a, 13U) ^ rotr32(a, 22U);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t temp2 = s0 + maj;

		h = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

static void sha256_update(SDKSHA256Context *ctx, const uint8_t *data,
                          uint32_t length)
{
	uint32_t used;

	ctx->bit_length += ((uint64_t)length * 8U);
	while (length != 0U) {
		used = SDK_SHA256_BLOCK_SIZE - ctx->buffer_length;
		if (used > length)
			used = length;

		memcpy(ctx->buffer + ctx->buffer_length, data, used);
		ctx->buffer_length += used;
		data += used;
		length -= used;

		if (ctx->buffer_length == SDK_SHA256_BLOCK_SIZE) {
			sha256_transform(ctx, ctx->buffer);
			ctx->buffer_length = 0;
		}
	}
}

static void sha256_final(SDKSHA256Context *ctx,
                         uint8_t digest[SDK_SHA256_DIGEST_SIZE])
{
	uint32_t i;

	ctx->buffer[ctx->buffer_length++] = 0x80U;
	if (ctx->buffer_length > 56U) {
		while (ctx->buffer_length < SDK_SHA256_BLOCK_SIZE)
			ctx->buffer[ctx->buffer_length++] = 0;
		sha256_transform(ctx, ctx->buffer);
		ctx->buffer_length = 0;
	}

	while (ctx->buffer_length < 56U)
		ctx->buffer[ctx->buffer_length++] = 0;

	store_be64(ctx->buffer + 56U, ctx->bit_length);
	sha256_transform(ctx, ctx->buffer);

	for (i = 0; i < 8U; i++)
		store_be32(digest + (i * 4U), ctx->state[i]);
}

void sdk_sha256(const uint8_t *data, uint32_t length,
                uint8_t digest[SDK_SHA256_DIGEST_SIZE])
{
	SDKSHA256Context ctx;

	sha256_init(&ctx);
	if (length != 0U)
		sha256_update(&ctx, data, length);
	sha256_final(&ctx, digest);
	memset(&ctx, 0, sizeof(ctx));
}

void sdk_sha1(const uint8_t *data, uint32_t length,
              uint8_t digest[SDK_SHA1_DIGEST_SIZE])
{
	SDKSHA1Context ctx;

	sha1_init(&ctx);
	if (length != 0U)
		sha1_update(&ctx, data, length);
	sha1_final(&ctx, digest);
	memset(&ctx, 0, sizeof(ctx));
}

void sdk_hmac_sha1(const uint8_t *key, uint32_t key_length,
                   const uint8_t *data, uint32_t data_length,
                   uint8_t digest[SDK_SHA1_DIGEST_SIZE])
{
	uint8_t key_block[SDK_SHA1_BLOCK_SIZE];
	uint8_t inner_digest[SDK_SHA1_DIGEST_SIZE];
	uint32_t i;
	SDKSHA1Context ctx;

	memset(key_block, 0, sizeof(key_block));
	if (key_length > SDK_SHA1_BLOCK_SIZE) {
		sdk_sha1(key, key_length, key_block);
	} else if (key_length != 0U) {
		memcpy(key_block, key, key_length);
	}

	for (i = 0; i < SDK_SHA1_BLOCK_SIZE; i++)
		key_block[i] ^= 0x36U;
	sha1_init(&ctx);
	sha1_update(&ctx, key_block, SDK_SHA1_BLOCK_SIZE);
	if (data_length != 0U)
		sha1_update(&ctx, data, data_length);
	sha1_final(&ctx, inner_digest);

	for (i = 0; i < SDK_SHA1_BLOCK_SIZE; i++)
		key_block[i] ^= (0x36U ^ 0x5cU);
	sha1_init(&ctx);
	sha1_update(&ctx, key_block, SDK_SHA1_BLOCK_SIZE);
	sha1_update(&ctx, inner_digest, SDK_SHA1_DIGEST_SIZE);
	sha1_final(&ctx, digest);

	memset(&ctx, 0, sizeof(ctx));
	memset(key_block, 0, sizeof(key_block));
	memset(inner_digest, 0, sizeof(inner_digest));
}

void sdk_hmac_sha256(const uint8_t *key, uint32_t key_length,
                     const uint8_t *data, uint32_t data_length,
                     uint8_t digest[SDK_SHA256_DIGEST_SIZE])
{
	uint8_t key_block[SDK_SHA256_BLOCK_SIZE];
	uint8_t inner_digest[SDK_SHA256_DIGEST_SIZE];
	uint32_t i;
	SDKSHA256Context ctx;

	memset(key_block, 0, sizeof(key_block));
	if (key_length > SDK_SHA256_BLOCK_SIZE) {
		sdk_sha256(key, key_length, key_block);
	} else if (key_length != 0U) {
		memcpy(key_block, key, key_length);
	}

	for (i = 0; i < SDK_SHA256_BLOCK_SIZE; i++)
		key_block[i] ^= 0x36U;
	sha256_init(&ctx);
	sha256_update(&ctx, key_block, SDK_SHA256_BLOCK_SIZE);
	if (data_length != 0U)
		sha256_update(&ctx, data, data_length);
	sha256_final(&ctx, inner_digest);

	for (i = 0; i < SDK_SHA256_BLOCK_SIZE; i++)
		key_block[i] ^= (0x36U ^ 0x5cU);
	sha256_init(&ctx);
	sha256_update(&ctx, key_block, SDK_SHA256_BLOCK_SIZE);
	sha256_update(&ctx, inner_digest, SDK_SHA256_DIGEST_SIZE);
	sha256_final(&ctx, digest);

	memset(&ctx, 0, sizeof(ctx));
	memset(key_block, 0, sizeof(key_block));
	memset(inner_digest, 0, sizeof(inner_digest));
}

static void chacha20_quarter_round(uint32_t state[16], uint32_t a,
                                   uint32_t b, uint32_t c, uint32_t d)
{
	state[a] += state[b];
	state[d] = rotl32(state[d] ^ state[a], 16U);
	state[c] += state[d];
	state[b] = rotl32(state[b] ^ state[c], 12U);
	state[a] += state[b];
	state[d] = rotl32(state[d] ^ state[a], 8U);
	state[c] += state[d];
	state[b] = rotl32(state[b] ^ state[c], 7U);
}

static void chacha20_block(const uint8_t key[SDK_CHACHA20_KEY_SIZE],
                           const uint8_t nonce[SDK_CHACHA20_NONCE_SIZE],
                           uint32_t counter,
                           uint8_t output[SDK_CHACHA20_BLOCK_SIZE])
{
	uint32_t state[16];
	uint32_t working[16];
	uint32_t i;

	state[0] = 0x61707865UL;
	state[1] = 0x3320646eUL;
	state[2] = 0x79622d32UL;
	state[3] = 0x6b206574UL;
	for (i = 0; i < 8U; i++)
		state[4U + i] = load_le32(key + (i * 4U));
	state[12] = counter;
	state[13] = load_le32(nonce);
	state[14] = load_le32(nonce + 4U);
	state[15] = load_le32(nonce + 8U);

	memcpy(working, state, sizeof(working));
	for (i = 0; i < 10U; i++) {
		chacha20_quarter_round(working, 0U, 4U, 8U, 12U);
		chacha20_quarter_round(working, 1U, 5U, 9U, 13U);
		chacha20_quarter_round(working, 2U, 6U, 10U, 14U);
		chacha20_quarter_round(working, 3U, 7U, 11U, 15U);
		chacha20_quarter_round(working, 0U, 5U, 10U, 15U);
		chacha20_quarter_round(working, 1U, 6U, 11U, 12U);
		chacha20_quarter_round(working, 2U, 7U, 8U, 13U);
		chacha20_quarter_round(working, 3U, 4U, 9U, 14U);
	}

	for (i = 0; i < 16U; i++)
		store_le32(output + (i * 4U), working[i] + state[i]);

	memset(state, 0, sizeof(state));
	memset(working, 0, sizeof(working));
}

void sdk_chacha20_xor(const uint8_t key[SDK_CHACHA20_KEY_SIZE],
                      const uint8_t nonce[SDK_CHACHA20_NONCE_SIZE],
                      uint32_t counter,
                      const uint8_t *input,
                      uint8_t *output,
                      uint32_t length)
{
	uint8_t block[SDK_CHACHA20_BLOCK_SIZE];
	uint32_t offset;
	uint32_t used;
	uint32_t i;

	offset = 0;
	while (offset < length) {
		chacha20_block(key, nonce, counter, block);
		counter++;
		used = length - offset;
		if (used > SDK_CHACHA20_BLOCK_SIZE)
			used = SDK_CHACHA20_BLOCK_SIZE;
		for (i = 0; i < used; i++)
			output[offset + i] = input[offset + i] ^ block[i];
		offset += used;
	}

	memset(block, 0, sizeof(block));
}

static void poly1305_init(SDKPoly1305Context *ctx,
                          const uint8_t key[SDK_POLY1305_KEY_SIZE])
{
	ctx->r0 = load_le32(key) & 0x03ffffffUL;
	ctx->r1 = (load_le32(key + 3U) >> 2) & 0x03ffff03UL;
	ctx->r2 = (load_le32(key + 6U) >> 4) & 0x03ffc0ffUL;
	ctx->r3 = (load_le32(key + 9U) >> 6) & 0x03f03fffUL;
	ctx->r4 = (load_le32(key + 12U) >> 8) & 0x000fffffUL;
	ctx->s1 = ctx->r1 * 5U;
	ctx->s2 = ctx->r2 * 5U;
	ctx->s3 = ctx->r3 * 5U;
	ctx->s4 = ctx->r4 * 5U;
	ctx->h0 = 0;
	ctx->h1 = 0;
	ctx->h2 = 0;
	ctx->h3 = 0;
	ctx->h4 = 0;
	memcpy(ctx->pad, key + 16U, sizeof(ctx->pad));
	memset(ctx->buffer, 0, sizeof(ctx->buffer));
	ctx->buffer_length = 0;
}

static void poly1305_process_block(SDKPoly1305Context *ctx,
                                   const uint8_t *data,
                                   uint32_t length)
{
	const uint32_t mask26 = 0x03ffffffUL;
	uint8_t block[16];
	uint32_t hibit;
	uint64_t d0;
	uint64_t d1;
	uint64_t d2;
	uint64_t d3;
	uint64_t d4;
	uint32_t carry;

	memset(block, 0, sizeof(block));
	memcpy(block, data, length);
	hibit = 0;
	if (length < 16U) {
		block[length] = 1U;
	} else {
		hibit = 1UL << 24;
	}

	ctx->h0 += load_le32(block) & mask26;
	ctx->h1 += (load_le32(block + 3U) >> 2) & mask26;
	ctx->h2 += (load_le32(block + 6U) >> 4) & mask26;
	ctx->h3 += (load_le32(block + 9U) >> 6) & mask26;
	ctx->h4 += ((load_le32(block + 12U) >> 8) & mask26) | hibit;

	d0 = ((uint64_t)ctx->h0 * ctx->r0) +
	     ((uint64_t)ctx->h1 * ctx->s4) +
	     ((uint64_t)ctx->h2 * ctx->s3) +
	     ((uint64_t)ctx->h3 * ctx->s2) +
	     ((uint64_t)ctx->h4 * ctx->s1);
	d1 = ((uint64_t)ctx->h0 * ctx->r1) +
	     ((uint64_t)ctx->h1 * ctx->r0) +
	     ((uint64_t)ctx->h2 * ctx->s4) +
	     ((uint64_t)ctx->h3 * ctx->s3) +
	     ((uint64_t)ctx->h4 * ctx->s2);
	d2 = ((uint64_t)ctx->h0 * ctx->r2) +
	     ((uint64_t)ctx->h1 * ctx->r1) +
	     ((uint64_t)ctx->h2 * ctx->r0) +
	     ((uint64_t)ctx->h3 * ctx->s4) +
	     ((uint64_t)ctx->h4 * ctx->s3);
	d3 = ((uint64_t)ctx->h0 * ctx->r3) +
	     ((uint64_t)ctx->h1 * ctx->r2) +
	     ((uint64_t)ctx->h2 * ctx->r1) +
	     ((uint64_t)ctx->h3 * ctx->r0) +
	     ((uint64_t)ctx->h4 * ctx->s4);
	d4 = ((uint64_t)ctx->h0 * ctx->r4) +
	     ((uint64_t)ctx->h1 * ctx->r3) +
	     ((uint64_t)ctx->h2 * ctx->r2) +
	     ((uint64_t)ctx->h3 * ctx->r1) +
	     ((uint64_t)ctx->h4 * ctx->r0);

	carry = (uint32_t)(d0 >> 26);
	ctx->h0 = (uint32_t)d0 & mask26;
	d1 += carry;
	carry = (uint32_t)(d1 >> 26);
	ctx->h1 = (uint32_t)d1 & mask26;
	d2 += carry;
	carry = (uint32_t)(d2 >> 26);
	ctx->h2 = (uint32_t)d2 & mask26;
	d3 += carry;
	carry = (uint32_t)(d3 >> 26);
	ctx->h3 = (uint32_t)d3 & mask26;
	d4 += carry;
	carry = (uint32_t)(d4 >> 26);
	ctx->h4 = (uint32_t)d4 & mask26;
	ctx->h0 += carry * 5U;
	carry = ctx->h0 >> 26;
	ctx->h0 &= mask26;
	ctx->h1 += carry;
	memset(block, 0, sizeof(block));
}

static void poly1305_update(SDKPoly1305Context *ctx, const uint8_t *data,
                            uint32_t length)
{
	if (length == 0U)
		return;

	if (ctx->buffer_length != 0U) {
		uint32_t needed = 16U - ctx->buffer_length;
		if (needed > length)
			needed = length;
		memcpy(ctx->buffer + ctx->buffer_length, data, needed);
		ctx->buffer_length += needed;
		data += needed;
		length -= needed;
		if (ctx->buffer_length == 16U) {
			poly1305_process_block(ctx, ctx->buffer, 16U);
			memset(ctx->buffer, 0, sizeof(ctx->buffer));
			ctx->buffer_length = 0;
		}
	}

	while (length >= 16U) {
		poly1305_process_block(ctx, data, 16U);
		data += 16U;
		length -= 16U;
	}

	if (length != 0U) {
		memcpy(ctx->buffer, data, length);
		ctx->buffer_length = length;
	}
}

static void poly1305_final(SDKPoly1305Context *ctx,
                           uint8_t tag[SDK_POLY1305_TAG_SIZE])
{
	const uint32_t mask26 = 0x03ffffffUL;
	uint32_t g0;
	uint32_t g1;
	uint32_t g2;
	uint32_t g3;
	uint32_t g4;
	uint32_t h0;
	uint32_t h1;
	uint32_t h2;
	uint32_t h3;
	uint32_t h4;
	uint32_t select;
	uint64_t f;
	uint64_t carry64;

	if (ctx->buffer_length != 0U) {
		poly1305_process_block(ctx, ctx->buffer, ctx->buffer_length);
		memset(ctx->buffer, 0, sizeof(ctx->buffer));
		ctx->buffer_length = 0;
	}

	g0 = ctx->h0;
	g1 = ctx->h1;
	g2 = ctx->h2;
	g3 = ctx->h3;
	g4 = ctx->h4;
	g1 += g0 >> 26;
	g0 &= mask26;
	g2 += g1 >> 26;
	g1 &= mask26;
	g3 += g2 >> 26;
	g2 &= mask26;
	g4 += g3 >> 26;
	g3 &= mask26;
	g0 += (g4 >> 26) * 5U;
	g4 &= mask26;
	g1 += g0 >> 26;
	g0 &= mask26;

	h0 = g0 + 5U;
	h1 = g1 + (h0 >> 26);
	h0 &= mask26;
	h2 = g2 + (h1 >> 26);
	h1 &= mask26;
	h3 = g3 + (h2 >> 26);
	h2 &= mask26;
	h4 = g4 + (h3 >> 26);
	h3 &= mask26;
	h4 -= 1UL << 26;

	select = (h4 >> 31) - 1U;
	h0 = (g0 & ~select) | (h0 & select);
	h1 = (g1 & ~select) | (h1 & select);
	h2 = (g2 & ~select) | (h2 & select);
	h3 = (g3 & ~select) | (h3 & select);
	h4 = (g4 & ~select) | (h4 & select);

	f = (uint32_t)(((uint64_t)h0) | ((uint64_t)h1 << 26));
	f += load_le32(ctx->pad);
	store_le32(tag, (uint32_t)f);
	carry64 = f >> 32;
	f = (uint32_t)(((uint64_t)h1 >> 6) | ((uint64_t)h2 << 20));
	f += load_le32(ctx->pad + 4U) + carry64;
	store_le32(tag + 4U, (uint32_t)f);
	carry64 = f >> 32;
	f = (uint32_t)(((uint64_t)h2 >> 12) | ((uint64_t)h3 << 14));
	f += load_le32(ctx->pad + 8U) + carry64;
	store_le32(tag + 8U, (uint32_t)f);
	carry64 = f >> 32;
	f = (uint32_t)(((uint64_t)h3 >> 18) | ((uint64_t)h4 << 8));
	f += load_le32(ctx->pad + 12U) + carry64;
	store_le32(tag + 12U, (uint32_t)f);
}

void sdk_poly1305(const uint8_t key[SDK_POLY1305_KEY_SIZE],
                  const uint8_t *data,
                  uint32_t length,
                  uint8_t tag[SDK_POLY1305_TAG_SIZE])
{
	SDKPoly1305Context ctx;

	poly1305_init(&ctx, key);
	poly1305_update(&ctx, data, length);
	poly1305_final(&ctx, tag);
	memset(&ctx, 0, sizeof(ctx));
}

static void poly1305_update_padded(SDKPoly1305Context *ctx,
                                   const uint8_t *data,
                                   uint32_t length)
{
	static const uint8_t zero[16] = {0};
	uint32_t pad;

	if (length != 0U)
		poly1305_update(ctx, data, length);
	pad = length & 15U;
	if (pad != 0U)
		poly1305_update(ctx, zero, 16U - pad);
}

static void store_le64_from_u32(uint8_t data[8], uint32_t value)
{
	store_le32(data, value);
	store_le32(data + 4U, 0);
}

static void chacha20_poly1305_tag(
	const uint8_t key[SDK_CHACHA20_KEY_SIZE],
	const uint8_t nonce[SDK_CHACHA20_NONCE_SIZE],
	const uint8_t *aad,
	uint32_t aad_length,
	const uint8_t *ciphertext,
	uint32_t ciphertext_length,
	uint8_t tag[SDK_POLY1305_TAG_SIZE])
{
	uint8_t block[SDK_CHACHA20_BLOCK_SIZE];
	uint8_t poly_key[SDK_POLY1305_KEY_SIZE];
	uint8_t lengths[16];
	SDKPoly1305Context ctx;

	memset(block, 0, sizeof(block));
	chacha20_block(key, nonce, 0U, block);
	memcpy(poly_key, block, sizeof(poly_key));

	poly1305_init(&ctx, poly_key);
	poly1305_update_padded(&ctx, aad, aad_length);
	poly1305_update_padded(&ctx, ciphertext, ciphertext_length);
	store_le64_from_u32(lengths, aad_length);
	store_le64_from_u32(lengths + 8U, ciphertext_length);
	poly1305_update(&ctx, lengths, sizeof(lengths));
	poly1305_final(&ctx, tag);

	memset(&ctx, 0, sizeof(ctx));
	memset(block, 0, sizeof(block));
	memset(poly_key, 0, sizeof(poly_key));
	memset(lengths, 0, sizeof(lengths));
}

void sdk_chacha20_poly1305_encrypt(
	const uint8_t key[SDK_CHACHA20_KEY_SIZE],
	const uint8_t nonce[SDK_CHACHA20_NONCE_SIZE],
	const uint8_t *aad,
	uint32_t aad_length,
	const uint8_t *plaintext,
	uint32_t plaintext_length,
	uint8_t *ciphertext,
	uint8_t tag[SDK_POLY1305_TAG_SIZE])
{
	sdk_chacha20_xor(key, nonce, 1U, plaintext, ciphertext,
	                 plaintext_length);
	chacha20_poly1305_tag(key, nonce, aad, aad_length, ciphertext,
	                      plaintext_length, tag);
}

static int tag_equal(const uint8_t *a, const uint8_t *b)
{
	uint8_t diff = 0;
	uint32_t i;

	for (i = 0; i < SDK_POLY1305_TAG_SIZE; i++)
		diff |= a[i] ^ b[i];
	return diff == 0;
}

int sdk_chacha20_poly1305_decrypt(
	const uint8_t key[SDK_CHACHA20_KEY_SIZE],
	const uint8_t nonce[SDK_CHACHA20_NONCE_SIZE],
	const uint8_t *aad,
	uint32_t aad_length,
	const uint8_t *ciphertext,
	uint32_t ciphertext_length,
	const uint8_t tag[SDK_POLY1305_TAG_SIZE],
	uint8_t *plaintext)
{
	uint8_t expected[SDK_POLY1305_TAG_SIZE];

	chacha20_poly1305_tag(key, nonce, aad, aad_length, ciphertext,
	                      ciphertext_length, expected);
	if (!tag_equal(expected, tag)) {
		memset(expected, 0, sizeof(expected));
		return 0;
	}

	sdk_chacha20_xor(key, nonce, 1U, ciphertext, plaintext,
	                 ciphertext_length);
	memset(expected, 0, sizeof(expected));
	return 1;
}

int sdk_x25519(const uint8_t scalar[32], const uint8_t point[32],
               uint8_t shared[32])
{
	const br_ec_impl *ec = &br_ec_c25519_m31;
	uint8_t buf[32];
	uint32_t ok;
	uint32_t nonzero;
	unsigned i;

	memcpy(buf, point, 32);
	/* BearSSL mul: scalar*point in place on buf; curve id 29 = Curve25519.
	 * BearSSL does not reject small-order points itself (its mul returns 0
	 * only for bad lengths), so check for the all-zero result here per
	 * RFC 7748 section 6.1. */
	ok = ec->mul(buf, 32, scalar, 32, BR_EC_curve25519);
	memcpy(shared, buf, 32);
	memset(buf, 0, sizeof(buf));
	nonzero = 0;
	for (i = 0; i < 32; i++)
		nonzero |= shared[i];
	return ok != 0 && nonzero != 0;
}

int sdk_p256_ecdh(const uint8_t scalar[32], const uint8_t peer_point[65],
                   uint8_t shared[32])
{
	const br_ec_impl *ec = &br_ec_p256_m31;
	uint8_t buf[65];  /* working copy of the peer point */
	size_t xoff;
	size_t xlen;
	uint32_t ok;

	memcpy(buf, peer_point, 65);
	/* BearSSL mul: scalar*point in place on buf; curve id 23 = secp256r1.
	 * Returns 0 if the point is not valid on the curve. */
	ok = ec->mul(buf, 65, scalar, 32, BR_EC_secp256r1);
	/* Extract X coordinate from the result point (offset/length via xoff()) */
	xoff = ec->xoff(BR_EC_secp256r1, &xlen);
	memcpy(shared, buf + xoff, xlen);  /* xlen == 32 for P-256 */
	memset(buf, 0, sizeof(buf));  /* scrub stack copy */
	return ok != 0;
}

int sdk_p256_keygen(const uint8_t scalar[32], uint8_t pub[65])
{
	const br_ec_impl *ec = &br_ec_p256_m31;
	const unsigned char *order;
	size_t order_len;
	size_t len;
	unsigned i;
	int nonzero = 0;
	int lt = 0, decided = 0;

	/* BearSSL mulgen requires the multiplier be non-zero and < the group
	 * order; enforce that here (mirrors zz9k_soft_p256_keygen) so a bad
	 * scalar is rejected instead of producing an undefined point. */
	order = ec->order(BR_EC_secp256r1, &order_len);
	if (order_len != SDK_P256_KEY_SIZE)
		return 0;
	for (i = 0; i < SDK_P256_KEY_SIZE; i++) {
		nonzero |= scalar[i];
		if (!decided) {
			if (scalar[i] < order[i]) { lt = 1; decided = 1; }
			else if (scalar[i] > order[i]) { lt = 0; decided = 1; }
		}
	}
	if (!nonzero || !lt)          /* zero, or scalar >= order */
		return 0;

	/* scalar*G -> full uncompressed point (0x04 || X || Y). */
	len = ec->mulgen(pub, scalar, SDK_P256_KEY_SIZE, BR_EC_secp256r1);
	return len == SDK_P256_POINT_SIZE;
}

int sdk_ecdsa_verify_p256(const uint8_t pubkey[SDK_P256_ECDSA_POINT_SIZE],
                           const uint8_t signature[SDK_P256_ECDSA_SIG_SIZE],
                           const uint8_t hash[SDK_SHA256_DIGEST_SIZE])
{
	const br_ec_impl *ec = &br_ec_p256_m31;
	br_ec_public_key pk;
	uint8_t pub_buf[SDK_P256_ECDSA_POINT_SIZE];
	uint8_t sig_buf[SDK_P256_ECDSA_SIG_SIZE];
	uint32_t ok;

	memcpy(pub_buf, pubkey, sizeof(pub_buf));
	memcpy(sig_buf, signature, sizeof(sig_buf));

	pk.curve = BR_EC_secp256r1;
	pk.q = pub_buf;
	pk.qlen = sizeof(pub_buf);

	ok = br_ecdsa_i31_vrfy_raw(ec, hash, SDK_SHA256_DIGEST_SIZE, &pk, sig_buf,
	                           sizeof(sig_buf));

	memset(pub_buf, 0, sizeof(pub_buf));
	memset(sig_buf, 0, sizeof(sig_buf));
	return ok != 0;
}

int sdk_rsa_verify_pkcs1_sha256(const uint8_t *modulus,
                                 size_t modulus_len,
                                 const uint8_t *exponent,
                                 size_t exponent_len,
                                 const uint8_t *signature,
                                 size_t signature_len,
                                 const uint8_t hash[SDK_SHA256_DIGEST_SIZE])
{
	br_rsa_public_key pk;
	uint8_t mod_buf[SDK_RSA_MAX_KEY_BYTES];
	uint8_t exp_buf[4];
	uint8_t sig_buf[SDK_RSA_MAX_KEY_BYTES];
	uint8_t hash_out[SDK_SHA256_DIGEST_SIZE];
	uint32_t ok;

	if (modulus_len == 0 || modulus_len > sizeof(mod_buf) ||
	    signature_len == 0 || signature_len > sizeof(sig_buf) ||
	    exponent_len == 0 || exponent_len > sizeof(exp_buf))
		return 0;
	memcpy(mod_buf, modulus, modulus_len);
	memcpy(exp_buf, exponent, exponent_len);
	memcpy(sig_buf, signature, signature_len);

	pk.n = mod_buf;
	pk.nlen = modulus_len;
	pk.e = exp_buf;
	pk.elen = (size_t)exponent_len;

	ok = br_rsa_i31_pkcs1_vrfy(sig_buf, signature_len, BR_HASH_OID_SHA256,
	                            SDK_SHA256_DIGEST_SIZE, &pk, hash_out);

	if (!ok) {
		memset(mod_buf, 0, sizeof(mod_buf));
		memset(exp_buf, 0, sizeof(exp_buf));
		memset(sig_buf, 0, sizeof(sig_buf));
		return 0;
	}

	/* BearSSL extracted the hash from PKCS#1 padding into hash_out.
	 * Compare it with the caller's pre-computed SHA-256 digest in constant time. */
	{
		uint8_t diff = 0;
		unsigned i;
		for (i = 0; i < SDK_SHA256_DIGEST_SIZE; i++)
			diff |= hash_out[i] ^ hash[i];
		memset(mod_buf, 0, sizeof(mod_buf));
		memset(exp_buf, 0, sizeof(exp_buf));
		memset(sig_buf, 0, sizeof(sig_buf));
		memset(hash_out, 0, sizeof(hash_out));
		return diff == 0;
	}
}

void sdk_aes_gcm_encrypt(const uint8_t *key, uint32_t key_length,
                         const uint8_t nonce[SDK_AES_GCM_NONCE_SIZE],
                         const uint8_t *aad, uint32_t aad_length,
                         const uint8_t *plaintext, uint32_t plaintext_length,
                         uint8_t *ciphertext,
                         uint8_t tag[SDK_AES_GCM_TAG_SIZE])
{
	br_aes_ct_ctr_keys bc;
	br_gcm_context gc;

	br_aes_ct_ctr_init(&bc, key, key_length);
	br_gcm_init(&gc, &bc.vtable, br_ghash_ctmul);
	br_gcm_reset(&gc, nonce, SDK_AES_GCM_NONCE_SIZE);
	if (aad_length != 0U)
		br_gcm_aad_inject(&gc, aad, aad_length);
	br_gcm_flip(&gc);
	/* br_gcm_run works in place; produce ciphertext from plaintext first. */
	if (ciphertext != plaintext)
		memcpy(ciphertext, plaintext, plaintext_length);
	br_gcm_run(&gc, 1, ciphertext, plaintext_length);
	br_gcm_get_tag(&gc, tag);
}

int sdk_aes_gcm_decrypt(const uint8_t *key, uint32_t key_length,
                        const uint8_t nonce[SDK_AES_GCM_NONCE_SIZE],
                        const uint8_t *aad, uint32_t aad_length,
                        const uint8_t *ciphertext, uint32_t ciphertext_length,
                        const uint8_t tag[SDK_AES_GCM_TAG_SIZE],
                        uint8_t *plaintext)
{
	br_aes_ct_ctr_keys bc;
	br_gcm_context gc;

	br_aes_ct_ctr_init(&bc, key, key_length);
	br_gcm_init(&gc, &bc.vtable, br_ghash_ctmul);
	br_gcm_reset(&gc, nonce, SDK_AES_GCM_NONCE_SIZE);
	if (aad_length != 0U)
		br_gcm_aad_inject(&gc, aad, aad_length);
	br_gcm_flip(&gc);
	/* GHASH covers the ciphertext; run in place after copying it across. */
	if (plaintext != ciphertext)
		memcpy(plaintext, ciphertext, ciphertext_length);
	br_gcm_run(&gc, 0, plaintext, ciphertext_length);
	return br_gcm_check_tag(&gc, tag) != 0 ? 1 : 0;
}
