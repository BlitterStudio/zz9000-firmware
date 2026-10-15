/*
 * NEON IDCT and motion compensation for libmpeg2 0.5.1.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_MPEG2_NEON_H
#define SDK_MPEG2_NEON_H

#include <stdint.h>

/* Swaps libmpeg2's IDCT and motion compensation for the NEON kernels once
 * a one-off check on this CPU found them bit-exact with libmpeg2's own C
 * code; otherwise libmpeg2 keeps its C code. Call after mpeg2_init(),
 * which installs the C code (and permutes the scan tables for it) on its
 * first call. Idempotent; a no-op on builds without NEON. */
void sdk_mpeg2_neon_install(void);

/* 1 when the NEON kernels are installed. */
int sdk_mpeg2_neon_active(void);

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
/* The kernels, for tests that compare them with libmpeg2's C code. Same
 * contracts as libmpeg2's mpeg2_idct_copy/mpeg2_idct_add (coefficients in
 * the C IDCT's permuted scan order; the block is left zeroed) and its
 * motion-compensation table (put/avg x o/x/y/xy x 16/8 wide). */
void sdk_mpeg2_neon_idct_copy(int16_t *block, uint8_t *dest, int stride);
void sdk_mpeg2_neon_idct_add(int last, int16_t *block, uint8_t *dest,
                             int stride);
typedef void sdk_mpeg2_neon_mc_fct(uint8_t *dest, const uint8_t *ref,
                                   int stride, int height);
/* Index: (avg ? 8 : 0) + (width 8 ? 4 : 0) + (y half ? 2 : 0) +
 * (x half ? 1 : 0), libmpeg2's mpeg2_mc_t layout. */
extern sdk_mpeg2_neon_mc_fct *const sdk_mpeg2_neon_mc[16];
#endif

#endif /* SDK_MPEG2_NEON_H */
