/*
 * NEON IDCT and motion compensation against libmpeg2's own C code.
 *
 * Runs on an ARM NEON target (run.sh: armhf under qemu-arm). The C code is
 * the oracle: libmpeg2 installs it on the first mpeg2_init(), the test
 * keeps those pointers, and every comparison is bit for bit - kernel by
 * kernel on random data, then whole streams decoded through the MPEG
 * backend with each set installed in turn.
 *
 * Usage: mpeg2_neon_test [extra.mpg ...]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_mpeg2_neon.h"
#include "sdk_video_backend.h"
#include "mpeg2.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../video_codec/mpeg1_ps_fixture.inc"
#include "../video_codec/mpeg1_mp2_ps_fixture.inc"
#include "../video_codec/dvd_pal_25p_fixture.inc"
#include "../video_codec/dvd_pal_tff_fixture.inc"
#include "../video_codec/dvd_ntsc_30_fixture.inc"
#include "../video_codec/dvd_ntsc_film_32_fixture.inc"

#if !defined(__ARM_NEON) && !defined(__ARM_NEON__)
#error "mpeg2_neon_test needs an ARM NEON target (see run.sh)"
#endif

/* libmpeg2 0.5.1 internals, as in sdk_mpeg2_neon.c. */
typedef void mpeg2_mc_fct(uint8_t *, const uint8_t *, int, int);
typedef struct {
	mpeg2_mc_fct *put[8];
	mpeg2_mc_fct *avg[8];
} mpeg2_mc_t;
extern mpeg2_mc_t mpeg2_mc;
extern void (*mpeg2_idct_copy)(int16_t *block, uint8_t *dest, int stride);
extern void (*mpeg2_idct_add)(int last, int16_t *block, uint8_t *dest,
                              int stride);

static void (*c_idct_copy)(int16_t *, uint8_t *, int);
static void (*c_idct_add)(int, int16_t *, uint8_t *, int);
static mpeg2_mc_t c_mc;
static void (*neon_idct_copy)(int16_t *, uint8_t *, int);
static void (*neon_idct_add)(int, int16_t *, uint8_t *, int);
static mpeg2_mc_t neon_mc;

static uint32_t seed = 1U;

static uint32_t rnd(void)
{
	seed = seed * 1103515245U + 12345U;
	return seed >> 7;
}

static int rnd_range(int lo, int hi)
{
	return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

/* Coefficient blocks in the shapes the decoder produces, plus extremes:
 * dense, sparse, DC-only rows, a single coefficient, and saturating
 * magnitudes that drive the output past 0..255. */
static void make_block(int16_t *block, int kind)
{
	int i;
	int n;

	memset(block, 0, 64 * sizeof(*block));
	switch (kind % 6) {
	case 0:
		for (i = 0; i < 64; i++)
			block[i] = (int16_t)rnd_range(-2048, 2047);
		break;
	case 1:
		n = rnd_range(1, 8);
		for (i = 0; i < n; i++)
			block[rnd() & 63U] = (int16_t)rnd_range(-300, 300);
		break;
	case 2:
		for (i = 0; i < 64; i += 8)
			block[i] = (int16_t)rnd_range(-2048, 2047);
		if (rnd() & 1U)
			block[rnd() & 63U] = (int16_t)rnd_range(-20, 20);
		break;
	case 3:
		block[rnd() & 63U] = (int16_t)rnd_range(-2048, 2047);
		break;
	case 4:
		for (i = 0; i < 64; i++)
			block[i] = (rnd() & 1U) ? 2047 : -2048;
		break;
	default:
		block[0] = (int16_t)rnd_range(-2048, 2047);
		block[63] = (int16_t)(rnd() & 1U);
		break;
	}
}

#define STRIDE 48

static int test_idct(void)
{
	static int16_t cb[64] __attribute__((aligned(16)));
	static int16_t nb[64] __attribute__((aligned(16)));
	static uint8_t cd[STRIDE * 10];
	static uint8_t nd[STRIDE * 10];
	int round;

	for (round = 0; round < 200000; round++) {
		int kind = round % 6;
		int op = (round / 6) % 3;
		int off = 1 + (int)(rnd() % 8U);
		int i;

		for (i = 0; i < (int)sizeof(cd); i++)
			cd[i] = (uint8_t)rnd();
		memcpy(nd, cd, sizeof(nd));
		make_block(cb, kind);
		memcpy(nb, cb, sizeof(nb));
		if (op == 0) {
			c_idct_copy(cb, cd + off, STRIDE);
			neon_idct_copy(nb, nd + off, STRIDE);
		} else {
			/* 129 is slice.c's DC-only marker; other values take
			 * the full path. */
			int last = op == 1 ? 129 : rnd_range(0, 63);

			c_idct_add(last, cb, cd + off, STRIDE);
			neon_idct_add(last, nb, nd + off, STRIDE);
		}
		if (memcmp(cd, nd, sizeof(cd)) != 0 ||
		    memcmp(cb, nb, sizeof(cb)) != 0) {
			printf("idct mismatch: round %d kind %d op %d\n",
			       round, kind, op);
			return 0;
		}
	}
	return 1;
}

static int test_mc(void)
{
	static uint8_t ref[STRIDE * 20];
	static uint8_t cd[STRIDE * 20];
	static uint8_t nd[STRIDE * 20];
	int round;

	for (round = 0; round < 64000; round++) {
		int f = round & 15;
		int wide = (f & 4) == 0;
		int heights[3] = { wide ? 16 : 8, wide ? 8 : 4, 1 };
		int h = heights[(round >> 4) % 3];
		int roff = (int)(rnd() % 16U);
		int doff = (int)(rnd() % 16U);
		mpeg2_mc_fct *cf = f < 8 ? c_mc.put[f] : c_mc.avg[f - 8];
		mpeg2_mc_fct *nf = f < 8 ? neon_mc.put[f] : neon_mc.avg[f - 8];
		int i;

		for (i = 0; i < (int)sizeof(ref); i++) {
			ref[i] = (uint8_t)rnd();
			cd[i] = (uint8_t)rnd();
		}
		memcpy(nd, cd, sizeof(nd));
		cf(cd + doff, ref + roff, STRIDE, h);
		nf(nd + doff, ref + roff, STRIDE, h);
		if (memcmp(cd, nd, sizeof(cd)) != 0) {
			printf("mc mismatch: round %d function %d height %d\n",
			       round, f, h);
			return 0;
		}
	}
	return 1;
}

static uint64_t fnv(uint64_t h, const uint8_t *p, uint32_t n)
{
	while (n--) {
		h ^= *p++;
		h *= 0x100000001b3ULL;
	}
	return h;
}

static uint64_t hash_frame(uint64_t h, const struct SDKVideoDecodedFrame *f)
{
	uint32_t y;

	for (y = 0U; y < f->height; y++)
		h = fnv(h, f->y + y * f->y_pitch, f->width);
	for (y = 0U; y < (f->height + 1U) / 2U; y++) {
		h = fnv(h, f->cb + y * f->chroma_pitch, (f->width + 1U) / 2U);
		h = fnv(h, f->cr + y * f->chroma_pitch, (f->width + 1U) / 2U);
	}
	return h;
}

/* Decodes a whole Program Stream; 0 frames on failure. */
static uint32_t decode_stream(uint32_t codec, const uint8_t *data,
                              uint32_t length, uint64_t *hash)
{
	const struct SDKVideoDecoderOps *ops =
		sdk_video_backend_find(codec, SDK_VIDEO_CONTAINER_MPEG_PS);
	struct SDKVideoDecodedFrame frame;
	uint32_t offset = 0U;
	uint32_t frames = 0U;
	uint32_t guard = 0U;
	void *decoder;
	int eof_sent = 0;

	*hash = 0xcbf29ce484222325ULL;
	if (!ops || !(decoder = ops->create(0U)))
		return 0U;
	while (guard++ < 10000000U) {
		int r = ops->decode(decoder, &frame);

		if (r == SDK_VIDEO_BACKEND_FRAME) {
			*hash = hash_frame(*hash, &frame);
			frames++;
		} else if (r == SDK_VIDEO_BACKEND_DONE) {
			break;
		} else if (r == SDK_VIDEO_BACKEND_ERROR) {
			frames = 0U;
			break;
		} else if (offset < length) {
			uint32_t chunk = length - offset > 16384U
				? 16384U : length - offset;
			uint32_t accepted = 0U;

			if (ops->write(decoder, data + offset, chunk,
			               offset + chunk == length, &accepted) ==
			    SDK_VIDEO_BACKEND_WRITE_ERROR) {
				frames = 0U;
				break;
			}
			offset += accepted;
			eof_sent = offset == length;
		} else if (!eof_sent) {
			uint32_t accepted = 0U;

			(void)ops->write(decoder, 0, 0U, 1, &accepted);
			eof_sent = 1;
		}
	}
	ops->destroy(decoder);
	return frames;
}

static void use_c(void)
{
	mpeg2_idct_copy = c_idct_copy;
	mpeg2_idct_add = c_idct_add;
	mpeg2_mc = c_mc;
}

static void use_neon(void)
{
	mpeg2_idct_copy = neon_idct_copy;
	mpeg2_idct_add = neon_idct_add;
	mpeg2_mc = neon_mc;
}

static int compare_stream(const char *name, uint32_t codec,
                          const uint8_t *data, uint32_t length)
{
	uint64_t c_hash;
	uint64_t n_hash;
	uint32_t c_frames;
	uint32_t n_frames;

	use_c();
	c_frames = decode_stream(codec, data, length, &c_hash);
	use_neon();
	n_frames = decode_stream(codec, data, length, &n_hash);
	printf("%-28s frames=%u C=%016llx NEON=%016llx %s\n", name,
	       (unsigned)c_frames, (unsigned long long)c_hash,
	       (unsigned long long)n_hash,
	       c_frames != 0U && c_frames == n_frames && c_hash == n_hash
	           ? "ok" : "MISMATCH");
	return c_frames != 0U && c_frames == n_frames && c_hash == n_hash;
}

static int compare_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	uint8_t *data;
	long size;
	int ok;
	uint32_t codec;

	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = (uint8_t *)malloc((size_t)size);
	if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
		fclose(f);
		free(data);
		return 0;
	}
	fclose(f);
	/* An MPEG-2 pack header starts 01xxxxxx, MPEG-1's 0010xxxx. */
	codec = size > 4 && (data[4] & 0xc0U) == 0x40U
		? SDK_VIDEO_CODEC_MPEG2 : SDK_VIDEO_CODEC_MPEG1;
	ok = compare_stream(path, codec, data, (uint32_t)size);
	free(data);
	return ok;
}

int main(int argc, char **argv)
{
	mpeg2dec_t *probe;
	int ok = 1;
	int i;

	/* The first mpeg2_init() installs libmpeg2's C code: keep it. */
	probe = mpeg2_init();
	if (!probe) {
		printf("mpeg2_init failed\n");
		return 1;
	}
	mpeg2_close(probe);
	c_idct_copy = mpeg2_idct_copy;
	c_idct_add = mpeg2_idct_add;
	c_mc = mpeg2_mc;

	sdk_mpeg2_neon_install();
	if (!sdk_mpeg2_neon_active() ||
	    mpeg2_idct_copy != sdk_mpeg2_neon_idct_copy) {
		printf("the NEON kernels refused to install (self check)\n");
		return 1;
	}
	neon_idct_copy = mpeg2_idct_copy;
	neon_idct_add = mpeg2_idct_add;
	neon_mc = mpeg2_mc;

	if (!test_idct()) {
		ok = 0;
	} else {
		printf("idct: 200000 blocks ok\n");
	}
	if (!test_mc()) {
		ok = 0;
	} else {
		printf("mc: 64000 calls ok\n");
	}
	ok &= compare_stream("mpeg1_ps_fixture", SDK_VIDEO_CODEC_MPEG1,
	                     zz9k_mpeg1_ps_fixture,
	                     zz9k_mpeg1_ps_fixture_len);
	ok &= compare_stream("mpeg1_mp2_ps_fixture", SDK_VIDEO_CODEC_MPEG1,
	                     zz9k_mpeg1_mp2_ps_fixture,
	                     zz9k_mpeg1_mp2_ps_fixture_len);
	ok &= compare_stream("dvd_pal_25p_fixture", SDK_VIDEO_CODEC_MPEG2,
	                     zz9k_dvd_pal_25p_fixture,
	                     zz9k_dvd_pal_25p_fixture_len);
	ok &= compare_stream("dvd_pal_tff_fixture", SDK_VIDEO_CODEC_MPEG2,
	                     zz9k_dvd_pal_tff_fixture,
	                     zz9k_dvd_pal_tff_fixture_len);
	ok &= compare_stream("dvd_ntsc_30_fixture", SDK_VIDEO_CODEC_MPEG2,
	                     zz9k_dvd_ntsc_30_fixture,
	                     zz9k_dvd_ntsc_30_fixture_len);
	ok &= compare_stream("dvd_ntsc_film_32_fixture", SDK_VIDEO_CODEC_MPEG2,
	                     zz9k_dvd_ntsc_film_32_fixture,
	                     zz9k_dvd_ntsc_film_32_fixture_len);
	for (i = 1; i < argc; i++)
		ok &= compare_file(argv[i]);
	printf("%s\n", ok ? "ALL NEON MPEG CHECKS PASSED" : "NEON MPEG CHECKS FAILED");
	return ok ? 0 : 1;
}
