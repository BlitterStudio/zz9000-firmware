/*
 * NEON IDCT and motion compensation for libmpeg2 0.5.1.
 *
 * libmpeg2's C IDCT and motion compensation are about half of the card's
 * MPEG decode time. These kernels compute exactly what the C code computes
 * (same integer arithmetic, same rounding, same row shortcut), so decoded
 * pictures do not change. libmpeg2's C code stays the exactness oracle
 * (the KTD8 rule of the YUY2 pack): the kernels are installed only after a
 * one-off comparison with it on the card agrees.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_mpeg2_neon.h"

#ifdef SDK_VIDEO_MPEG2_MODULE
#include "module.h"
#else
#include <stdio.h>
#endif
#include <string.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>

/* libmpeg2 0.5.1 internals (mpeg2_internal.h, motion_comp.c, idct.c),
 * pinned with the dependency build. */
typedef void mpeg2_mc_fct(uint8_t *, const uint8_t *, int, int);
typedef struct {
	mpeg2_mc_fct *put[8];
	mpeg2_mc_fct *avg[8];
} mpeg2_mc_t;
extern mpeg2_mc_t mpeg2_mc;
extern void (*mpeg2_idct_copy)(int16_t *block, uint8_t *dest, int stride);
extern void (*mpeg2_idct_add)(int last, int16_t *block, uint8_t *dest,
                              int stride);

/* idct.c's constants: 2048 * sqrt (2) * cos (k * pi / 16). */
#define W1 2841
#define W2 2676
#define W3 2408
#define W5 1609
#define W6 1108
#define W7 565

/* 8x8 transpose of int16 rows: r[k] becomes column k. */
static inline void transpose8(int16x8_t r[8])
{
	int16x8x2_t t0 = vtrnq_s16(r[0], r[1]);
	int16x8x2_t t1 = vtrnq_s16(r[2], r[3]);
	int16x8x2_t t2 = vtrnq_s16(r[4], r[5]);
	int16x8x2_t t3 = vtrnq_s16(r[6], r[7]);
	int32x4x2_t u0 = vtrnq_s32(vreinterpretq_s32_s16(t0.val[0]),
	                           vreinterpretq_s32_s16(t1.val[0]));
	int32x4x2_t u1 = vtrnq_s32(vreinterpretq_s32_s16(t0.val[1]),
	                           vreinterpretq_s32_s16(t1.val[1]));
	int32x4x2_t u2 = vtrnq_s32(vreinterpretq_s32_s16(t2.val[0]),
	                           vreinterpretq_s32_s16(t3.val[0]));
	int32x4x2_t u3 = vtrnq_s32(vreinterpretq_s32_s16(t2.val[1]),
	                           vreinterpretq_s32_s16(t3.val[1]));

	r[0] = vreinterpretq_s16_s32(vcombine_s32(vget_low_s32(u0.val[0]),
	                                          vget_low_s32(u2.val[0])));
	r[4] = vreinterpretq_s16_s32(vcombine_s32(vget_high_s32(u0.val[0]),
	                                          vget_high_s32(u2.val[0])));
	r[1] = vreinterpretq_s16_s32(vcombine_s32(vget_low_s32(u1.val[0]),
	                                          vget_low_s32(u3.val[0])));
	r[5] = vreinterpretq_s16_s32(vcombine_s32(vget_high_s32(u1.val[0]),
	                                          vget_high_s32(u3.val[0])));
	r[2] = vreinterpretq_s16_s32(vcombine_s32(vget_low_s32(u0.val[1]),
	                                          vget_low_s32(u2.val[1])));
	r[6] = vreinterpretq_s16_s32(vcombine_s32(vget_high_s32(u0.val[1]),
	                                          vget_high_s32(u2.val[1])));
	r[3] = vreinterpretq_s16_s32(vcombine_s32(vget_low_s32(u1.val[1]),
	                                          vget_low_s32(u3.val[1])));
	r[7] = vreinterpretq_s16_s32(vcombine_s32(vget_high_s32(u1.val[1]),
	                                          vget_high_s32(u3.val[1])));
}

/* idct_row/idct_col on four lanes, one row (or column) per lane. c[k] is
 * input k in the C IDCT's permuted order (x0 x2 x4 x6 x1 x3 x5 x7); the
 * results are narrowed to int16 the way C stores them. bias/shift are
 * 2048/12 for rows and 65536/17 for columns. */
#define IDCT4(name, BIAS, SHIFT)                                             \
static inline void name(const int32x4_t c[8], int16x4_t out[8])            \
{                                                                          \
	int32x4_t d0 = vaddq_s32(vshlq_n_s32(c[0], 11), vdupq_n_s32(BIAS));\
	int32x4_t d2 = vshlq_n_s32(c[2], 11);                              \
	int32x4_t t0 = vaddq_s32(d0, d2);                                  \
	int32x4_t t1 = vsubq_s32(d0, d2);                                  \
	int32x4_t tmp = vmulq_n_s32(vaddq_s32(c[3], c[1]), W6);            \
	int32x4_t t2 = vmlaq_n_s32(tmp, c[1], W2 - W6);                    \
	int32x4_t t3 = vmlsq_n_s32(tmp, c[3], W2 + W6);                    \
	int32x4_t a0 = vaddq_s32(t0, t2);                                  \
	int32x4_t a1 = vaddq_s32(t1, t3);                                  \
	int32x4_t a2 = vsubq_s32(t1, t3);                                  \
	int32x4_t a3 = vsubq_s32(t0, t2);                                  \
	int32x4_t b0, b1, b2, b3;                                          \
                                                                           \
	tmp = vmulq_n_s32(vaddq_s32(c[7], c[4]), W7);                      \
	t0 = vmlaq_n_s32(tmp, c[4], W1 - W7);                              \
	t1 = vmlsq_n_s32(tmp, c[7], W1 + W7);                              \
	tmp = vmulq_n_s32(vaddq_s32(c[5], c[6]), W3);                      \
	t2 = vmlaq_n_s32(tmp, c[6], W5 - W3);                              \
	t3 = vmlsq_n_s32(tmp, c[5], W5 + W3);                              \
	b0 = vaddq_s32(t0, t2);                                            \
	b3 = vaddq_s32(t1, t3);                                            \
	t0 = vsubq_s32(t0, t2);                                            \
	t1 = vsubq_s32(t1, t3);                                            \
	b1 = vmulq_n_s32(vshrq_n_s32(vaddq_s32(t0, t1), 8), 181);          \
	b2 = vmulq_n_s32(vshrq_n_s32(vsubq_s32(t0, t1), 8), 181);          \
                                                                           \
	out[0] = vmovn_s32(vshrq_n_s32(vaddq_s32(a0, b0), SHIFT));         \
	out[1] = vmovn_s32(vshrq_n_s32(vaddq_s32(a1, b1), SHIFT));         \
	out[2] = vmovn_s32(vshrq_n_s32(vaddq_s32(a2, b2), SHIFT));         \
	out[3] = vmovn_s32(vshrq_n_s32(vaddq_s32(a3, b3), SHIFT));         \
	out[4] = vmovn_s32(vshrq_n_s32(vsubq_s32(a3, b3), SHIFT));         \
	out[5] = vmovn_s32(vshrq_n_s32(vsubq_s32(a2, b2), SHIFT));         \
	out[6] = vmovn_s32(vshrq_n_s32(vsubq_s32(a1, b1), SHIFT));         \
	out[7] = vmovn_s32(vshrq_n_s32(vsubq_s32(a0, b0), SHIFT));         \
}

IDCT4(idct_rows4, 2048, 12)
IDCT4(idct_cols4, 65536, 17)

/* One pass over eight lanes: c[k] holds input k of each lane. */
#define IDCT8(name, kernel)                                                  \
static inline void name(int16x8_t c[8])                                    \
{                                                                          \
	int32x4_t lo[8], hi[8];                                            \
	int16x4_t olo[8], ohi[8];                                          \
	int k;                                                             \
                                                                           \
	for (k = 0; k < 8; k++) {                                          \
		lo[k] = vmovl_s16(vget_low_s16(c[k]));                     \
		hi[k] = vmovl_s16(vget_high_s16(c[k]));                    \
	}                                                                  \
	kernel(lo, olo);                                                   \
	kernel(hi, ohi);                                                   \
	for (k = 0; k < 8; k++)                                            \
		c[k] = vcombine_s16(olo[k], ohi[k]);                       \
}

IDCT8(idct_rows8, idct_rows4)
IDCT8(idct_cols8, idct_cols4)

/* The full 2-D IDCT. On return r[y] is output row y, lane x column x. */
static inline void idct8x8(const int16_t *block, int16x8_t r[8])
{
	int16x8_t shortcut;
	uint16x8_t dc_only;
	int16x8_t row[8];
	int k;

	for (k = 0; k < 8; k++)
		r[k] = vld1q_s16(block + 8 * k);
	transpose8(r);
	/* idct_row's shortcut: a row whose coefficients 1..7 are all zero
	 * becomes block[0] >> 1 throughout (not the full formula's rounding,
	 * which would differ for odd values). */
	dc_only = vceqq_s16(vorrq_s16(vorrq_s16(vorrq_s16(r[1], r[2]),
	                                        vorrq_s16(r[3], r[4])),
	                              vorrq_s16(vorrq_s16(r[5], r[6]), r[7])),
	                    vdupq_n_s16(0));
	shortcut = vshrq_n_s16(r[0], 1);
	for (k = 0; k < 8; k++)
		row[k] = r[k];
	idct_rows8(row);
	for (k = 0; k < 8; k++)
		r[k] = vbslq_s16(dc_only, shortcut, row[k]);
	transpose8(r);
	idct_cols8(r);
}

static inline void zero_block(int16_t *block)
{
	int16x8_t zero = vdupq_n_s16(0);
	int k;

	for (k = 0; k < 8; k++)
		vst1q_s16(block + 8 * k, zero);
}

void sdk_mpeg2_neon_idct_copy(int16_t *block, uint8_t *dest, int stride)
{
	int16x8_t r[8];
	int y;

	idct8x8(block, r);
	for (y = 0; y < 8; y++, dest += stride)
		vst1_u8(dest, vqmovun_s16(r[y]));
	zero_block(block);
}

static inline uint8x8_t add_clip(int16x8_t v, const uint8_t *dest)
{
	return vqmovun_s16(vaddq_s16(
		v, vreinterpretq_s16_u16(vmovl_u8(vld1_u8(dest)))));
}

void sdk_mpeg2_neon_idct_add(int last, int16_t *block, uint8_t *dest,
                             int stride)
{
	int y;

	if (last != 129 || (block[0] & (7 << 4)) == (4 << 4)) {
		int16x8_t r[8];

		idct8x8(block, r);
		for (y = 0; y < 8; y++, dest += stride)
			vst1_u8(dest, add_clip(r[y], dest));
		zero_block(block);
	} else {
		/* DC only: the C code adds the rounded DC and clears just the
		 * two coefficients it can have set. */
		int16x8_t dc = vdupq_n_s16((int16_t)((block[0] + 64) >> 7));

		block[0] = block[63] = 0;
		for (y = 0; y < 8; y++, dest += stride)
			vst1_u8(dest, add_clip(dc, dest));
	}
}

/* Motion compensation: the prediction of one row, as motion_comp.c's
 * predict_o/x/y/xy (rounding averages; xy is (a+b+c+d+2)>>2). Each row
 * reads exactly the bytes the C code reads. */
static inline uint8x16_t pred16_o(const uint8_t *ref, int stride)
{
	(void)stride;
	return vld1q_u8(ref);
}

static inline uint8x16_t pred16_x(const uint8_t *ref, int stride)
{
	(void)stride;
	return vrhaddq_u8(vld1q_u8(ref), vld1q_u8(ref + 1));
}

static inline uint8x16_t pred16_y(const uint8_t *ref, int stride)
{
	return vrhaddq_u8(vld1q_u8(ref), vld1q_u8(ref + stride));
}

static inline uint8x16_t pred16_xy(const uint8_t *ref, int stride)
{
	uint8x16_t a = vld1q_u8(ref);
	uint8x16_t b = vld1q_u8(ref + 1);
	uint8x16_t c = vld1q_u8(ref + stride);
	uint8x16_t d = vld1q_u8(ref + stride + 1);
	uint16x8_t lo = vaddq_u16(vaddl_u8(vget_low_u8(a), vget_low_u8(b)),
	                          vaddl_u8(vget_low_u8(c), vget_low_u8(d)));
	uint16x8_t hi = vaddq_u16(vaddl_u8(vget_high_u8(a), vget_high_u8(b)),
	                          vaddl_u8(vget_high_u8(c), vget_high_u8(d)));

	return vcombine_u8(vrshrn_n_u16(lo, 2), vrshrn_n_u16(hi, 2));
}

static inline uint8x8_t pred8_o(const uint8_t *ref, int stride)
{
	(void)stride;
	return vld1_u8(ref);
}

static inline uint8x8_t pred8_x(const uint8_t *ref, int stride)
{
	(void)stride;
	return vrhadd_u8(vld1_u8(ref), vld1_u8(ref + 1));
}

static inline uint8x8_t pred8_y(const uint8_t *ref, int stride)
{
	return vrhadd_u8(vld1_u8(ref), vld1_u8(ref + stride));
}

static inline uint8x8_t pred8_xy(const uint8_t *ref, int stride)
{
	uint16x8_t sum = vaddq_u16(
		vaddl_u8(vld1_u8(ref), vld1_u8(ref + 1)),
		vaddl_u8(vld1_u8(ref + stride), vld1_u8(ref + stride + 1)));

	return vrshrn_n_u16(sum, 2);
}

#define MC_FUNCS(xy)                                                         \
static void mc_put_##xy##_16(uint8_t *dest, const uint8_t *ref,            \
                             int stride, int height)                        \
{                                                                          \
	do {                                                               \
		vst1q_u8(dest, pred16_##xy(ref, stride));                  \
		ref += stride;                                             \
		dest += stride;                                            \
	} while (--height);                                                \
}                                                                          \
static void mc_avg_##xy##_16(uint8_t *dest, const uint8_t *ref,            \
                             int stride, int height)                        \
{                                                                          \
	do {                                                               \
		vst1q_u8(dest, vrhaddq_u8(pred16_##xy(ref, stride),        \
		                          vld1q_u8(dest)));                \
		ref += stride;                                             \
		dest += stride;                                            \
	} while (--height);                                                \
}                                                                          \
static void mc_put_##xy##_8(uint8_t *dest, const uint8_t *ref,             \
                            int stride, int height)                         \
{                                                                          \
	do {                                                               \
		vst1_u8(dest, pred8_##xy(ref, stride));                    \
		ref += stride;                                             \
		dest += stride;                                            \
	} while (--height);                                                \
}                                                                          \
static void mc_avg_##xy##_8(uint8_t *dest, const uint8_t *ref,             \
                            int stride, int height)                         \
{                                                                          \
	do {                                                               \
		vst1_u8(dest, vrhadd_u8(pred8_##xy(ref, stride),           \
		                        vld1_u8(dest)));                   \
		ref += stride;                                             \
		dest += stride;                                            \
	} while (--height);                                                \
}

MC_FUNCS(o)
MC_FUNCS(x)
MC_FUNCS(y)
MC_FUNCS(xy)

sdk_mpeg2_neon_mc_fct *const sdk_mpeg2_neon_mc[16] = {
	mc_put_o_16, mc_put_x_16, mc_put_y_16, mc_put_xy_16,
	mc_put_o_8, mc_put_x_8, mc_put_y_8, mc_put_xy_8,
	mc_avg_o_16, mc_avg_x_16, mc_avg_y_16, mc_avg_xy_16,
	mc_avg_o_8, mc_avg_x_8, mc_avg_y_8, mc_avg_xy_8,
};

/* The self check: deterministic data through libmpeg2's installed C code
 * and through the kernels. */
static uint32_t check_seed;

static uint32_t check_rand(void)
{
	check_seed = check_seed * 1664525U + 1013904223U;
	return check_seed >> 8;
}

/* Coefficients as libmpeg2 produces them (dequantised, -2048..2047): a
 * dense block, a sparse one, one with DC-only rows, a DC-only block for
 * idct_add's shortcut and one whose DC takes the full path anyway. */
static void check_block(int16_t *block, int kind)
{
	int i;

	memset(block, 0, 64 * sizeof(*block));
	switch (kind) {
	case 0:
		for (i = 0; i < 64; i++)
			block[i] = (int16_t)((int)(check_rand() & 4095U) - 2048);
		break;
	case 1:
		for (i = 0; i < 6; i++)
			block[check_rand() & 63U] =
				(int16_t)((int)(check_rand() & 511U) - 256);
		break;
	case 2:
		for (i = 0; i < 64; i += 8)
			block[i] = (int16_t)((int)(check_rand() & 2047U) - 1024);
		block[19] = 7;
		break;
	case 3:
		block[0] = (int16_t)((int)(check_rand() & 2047U) - 1024);
		block[0] = (int16_t)((block[0] & ~(7 << 4)) | (1 << 4));
		break;
	default:
		block[0] = (int16_t)((4 << 4) | 5);
		block[63] = 1;
		break;
	}
}

#define CHECK_STRIDE 40
#define CHECK_ROWS 18

static int check_idct(void)
{
	static int16_t want_block[64] __attribute__((aligned(16)));
	static int16_t got_block[64] __attribute__((aligned(16)));
	static uint8_t base[CHECK_STRIDE * 8];
	static uint8_t want[CHECK_STRIDE * 8];
	static uint8_t got[CHECK_STRIDE * 8];
	int kind;
	int round;
	int i;

	for (round = 0; round < 4; round++) {
		for (kind = 0; kind < 5; kind++) {
			for (i = 0; i < (int)sizeof(base); i++)
				base[i] = (uint8_t)check_rand();
			check_block(want_block, kind);
			memcpy(got_block, want_block, sizeof(got_block));
			memcpy(want, base, sizeof(want));
			memcpy(got, base, sizeof(got));
			if (kind < 3) {
				mpeg2_idct_copy(want_block, want + 3,
				                CHECK_STRIDE);
				sdk_mpeg2_neon_idct_copy(got_block, got + 3,
				                         CHECK_STRIDE);
			} else {
				mpeg2_idct_add(129, want_block, want + 3,
				               CHECK_STRIDE);
				sdk_mpeg2_neon_idct_add(129, got_block, got + 3,
				                        CHECK_STRIDE);
			}
			if (memcmp(want, got, sizeof(want)) != 0 ||
			    memcmp(want_block, got_block,
			           sizeof(want_block)) != 0)
				return 0;
			if (kind < 3) {
				/* The same block added, not copied. */
				check_block(want_block, kind);
				memcpy(got_block, want_block, sizeof(got_block));
				mpeg2_idct_add(40, want_block, want + 3,
				               CHECK_STRIDE);
				sdk_mpeg2_neon_idct_add(40, got_block, got + 3,
				                        CHECK_STRIDE);
				if (memcmp(want, got, sizeof(want)) != 0 ||
				    memcmp(want_block, got_block,
				           sizeof(want_block)) != 0)
					return 0;
			}
		}
	}
	return 1;
}

static int check_mc(void)
{
	static uint8_t ref[CHECK_STRIDE * CHECK_ROWS];
	static uint8_t base[CHECK_STRIDE * CHECK_ROWS];
	static uint8_t want[CHECK_STRIDE * CHECK_ROWS];
	static uint8_t got[CHECK_STRIDE * CHECK_ROWS];
	int f;
	int i;

	for (f = 0; f < 16; f++) {
		mpeg2_mc_fct *c_fn = f < 8 ? mpeg2_mc.put[f] : mpeg2_mc.avg[f - 8];
		int height = (f & 4) ? 8 : 16;
		int shift;

		/* Odd offsets so no load is aligned, and both heights the
		 * decoder uses for each width (field halves too). */
		for (shift = 0; shift < 2; shift++) {
			int h = shift ? height / 2 : height;

			for (i = 0; i < (int)sizeof(ref); i++) {
				ref[i] = (uint8_t)check_rand();
				base[i] = (uint8_t)check_rand();
			}
			memcpy(want, base, sizeof(want));
			memcpy(got, base, sizeof(got));
			c_fn(want + 5, ref + 1 + shift, CHECK_STRIDE, h);
			sdk_mpeg2_neon_mc[f](got + 5, ref + 1 + shift,
			                     CHECK_STRIDE, h);
			if (memcmp(want, got, sizeof(want)) != 0)
				return 0;
		}
	}
	return 1;
}

static int neon_state; /* 0 unchecked, 1 installed, -1 refused */

void sdk_mpeg2_neon_install(void)
{
	int f;

	if (neon_state != 0)
		return;
	check_seed = 0x2b992ddfU;
	if (!mpeg2_idct_copy || !mpeg2_idct_add || !check_idct() ||
	    !check_mc()) {
#ifdef SDK_VIDEO_MPEG2_MODULE
		if (mpeg2_module_services()->log)
			mpeg2_module_services()->log(
				"mpeg2 NEON IDCT/MC self check failed; C kept");
#else
		printf("[mpeg2] NEON IDCT/MC self check failed; C kept\n");
#endif
		neon_state = -1;
		return;
	}
	mpeg2_idct_copy = sdk_mpeg2_neon_idct_copy;
	mpeg2_idct_add = sdk_mpeg2_neon_idct_add;
	for (f = 0; f < 8; f++) {
		mpeg2_mc.put[f] = sdk_mpeg2_neon_mc[f];
		mpeg2_mc.avg[f] = sdk_mpeg2_neon_mc[f + 8];
	}
	neon_state = 1;
#ifdef SDK_VIDEO_MPEG2_MODULE
	if (mpeg2_module_services()->log)
		mpeg2_module_services()->log("mpeg2 NEON IDCT/MC installed");
#else
	printf("[mpeg2] NEON IDCT/MC installed\n");
#endif
}

int sdk_mpeg2_neon_active(void)
{
	return neon_state > 0;
}

#else /* no NEON */

void sdk_mpeg2_neon_install(void)
{
}

int sdk_mpeg2_neon_active(void)
{
	return 0;
}

#endif
