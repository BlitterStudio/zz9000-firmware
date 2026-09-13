/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZ_CUSTOM_MODE_H
#define ZZ_CUSTOM_MODE_H

#include <stdint.h>

/* Mirrored in zz9000-drivers/include/zz_custom_mode.h. All bus writes are
 * 16-bit on both Zorro buses. Select starts a fresh transaction; commit
 * carries slot | (color << 8), with no scaling. Read commit for status.
 * Frequencies are derived from the PLL tuple, never sent through a word. */
#define ZZ_FW_CAP_CUSTOM_MODE (1U << 7)
#define ZZ_CUSTOM_MODE_SLOT 20U
#define ZZ_CUSTOM_REG_PARAM 0x52U
#define ZZ_CUSTOM_REG_VALUE 0x54U
#define ZZ_CUSTOM_REG_SELECT 0x56U
#define ZZ_CUSTOM_REG_COMMIT 0x58U

#define ZZ_CUSTOM_STATUS_IDLE 0U
#define ZZ_CUSTOM_STATUS_OK 1U
#define ZZ_CUSTOM_STATUS_INVALID 2U
#define ZZ_CUSTOM_STATUS_CLOCK_FAILED 3U

/* Existing firmware parameter identities. Clock/refresh metadata are
 * deliberately omitted: firmware computes them from the validated PLL. */
#define ZZ_CUSTOM_HRES 0U
#define ZZ_CUSTOM_VRES 1U
#define ZZ_CUSTOM_HSTART 2U
#define ZZ_CUSTOM_HEND 3U
#define ZZ_CUSTOM_HTOTAL 4U
#define ZZ_CUSTOM_VSTART 5U
#define ZZ_CUSTOM_VEND 6U
#define ZZ_CUSTOM_VTOTAL 7U
#define ZZ_CUSTOM_POLARITY 8U
#define ZZ_CUSTOM_MUL 13U
#define ZZ_CUSTOM_DIV 14U
#define ZZ_CUSTOM_DIV2 15U
#define ZZ_CUSTOM_REQUIRED_FIELDS 0xe1ffU

#define ZZ_CUSTOM_MIN_WIDTH 320U
#define ZZ_CUSTOM_MIN_HEIGHT 200U
#define ZZ_CUSTOM_MAX_WIDTH 2560U
#define ZZ_CUSTOM_MAX_TOTAL 4095U
/* Eight-pixel rows are 64-bit DMA aligned even in indexed color. */
#define ZZ_CUSTOM_WIDTH_ALIGNMENT 8U
#define ZZ_CUSTOM_MIN_CLOCK_HZ 25000000U
#define ZZ_CUSTOM_MAX_CLOCK_HZ 165000000U

struct zz_custom_mode {
	uint16_t width, height;
	uint16_t hsync_start, hsync_end, htotal;
	uint16_t vsync_start, vsync_end, vtotal;
	uint16_t polarity;
	uint16_t mul, div, div2;
};

/* clk_wiz_0 is a PLLE2 with a 100 MHz reference (zz9000_project.tcl).
 * Use a conservative subset of the -1 part's PLL range: 20..100 MHz
 * phase detector, 1000..1500 MHz VCO, integer output divider 1..128.
 * This restriction applies to new modelines, not the existing presets. */
static inline uint32_t zz_custom_clock_hz(uint16_t mul, uint16_t div,
		uint16_t div2)
{
	uint32_t denominator;
	uint64_t numerator;
	if (mul < 2U || mul > 64U || div < 1U || div > 5U ||
	    div2 < 1U || div2 > 128U ||
	    100U * mul < 1000U * div || 100U * mul > 1500U * div)
		return 0;
	denominator = (uint32_t)div * div2;
	numerator = (uint64_t)100000000U * mul;
	return (uint32_t)((numerator + denominator / 2U) / denominator);
}

static inline int zz_custom_geometry_valid(const struct zz_custom_mode *mode)
{
	return mode->width >= ZZ_CUSTOM_MIN_WIDTH &&
		mode->width <= ZZ_CUSTOM_MAX_WIDTH &&
		mode->width % ZZ_CUSTOM_WIDTH_ALIGNMENT == 0U &&
		mode->height >= ZZ_CUSTOM_MIN_HEIGHT &&
		mode->width < mode->hsync_start &&
		mode->hsync_start < mode->hsync_end &&
		mode->hsync_end < mode->htotal &&
		mode->htotal <= ZZ_CUSTOM_MAX_TOTAL &&
		mode->height < mode->vsync_start &&
		mode->vsync_start < mode->vsync_end &&
		mode->vsync_end < mode->vtotal &&
		mode->vtotal <= ZZ_CUSTOM_MAX_TOTAL && mode->polarity <= 1U;
}

static inline int zz_custom_mode_valid(const struct zz_custom_mode *mode)
{
	uint32_t hz = zz_custom_clock_hz(mode->mul, mode->div, mode->div2);
	return zz_custom_geometry_valid(mode) &&
		hz >= ZZ_CUSTOM_MIN_CLOCK_HZ && hz <= ZZ_CUSTOM_MAX_CLOCK_HZ;
}

/* Resolve the nearest clock over 69 legal feedback pairs. Only adjacent
 * output dividers can minimize error; no exhaustive divider search and
 * no floating point are needed on the 68020. Ties keep the first tuple.
 * Reject errors over 0.5%; P96 receives the achieved clock, not a promise. */
static inline uint32_t zz_custom_resolve_clock(uint32_t requested,
		struct zz_custom_mode *mode)
{
	uint32_t best_error = UINT32_MAX, best_hz = 0;
	uint16_t mul, div, best_mul = 0, best_div = 0, best_div2 = 0;
	if (requested < ZZ_CUSTOM_MIN_CLOCK_HZ ||
	    requested > ZZ_CUSTOM_MAX_CLOCK_HZ)
		return 0;
	for (div = 1; div <= 5 && best_error != 0U; div++) {
		for (mul = 10U * div;
		     mul <= 64U && mul <= 15U * div && best_error != 0U; mul++) {
			uint64_t numerator;
			uint32_t output_div, candidate;
			numerator = (uint64_t)100000000U * mul;
			output_div = (uint32_t)(numerator / ((uint64_t)requested * div));
			for (candidate = output_div;
			     candidate <= output_div + 1U && best_error != 0U; candidate++) {
				uint32_t hz, error;
				if (candidate < 1U || candidate > 128U)
					continue;
				hz = zz_custom_clock_hz(mul, div, (uint16_t)candidate);
				if (hz < ZZ_CUSTOM_MIN_CLOCK_HZ || hz > ZZ_CUSTOM_MAX_CLOCK_HZ)
					continue;
				error = hz > requested ? hz - requested : requested - hz;
				if (error < best_error) {
					best_error = error;
					best_hz = hz;
					best_mul = mul;
					best_div = div;
					best_div2 = (uint16_t)candidate;
				}
			}
		}
	}
	if (!best_hz || best_error > requested / 200U)
		return 0;
	mode->mul = best_mul;
	mode->div = best_div;
	mode->div2 = best_div2;
	return best_hz;
}

#endif
