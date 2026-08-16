/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Host regression for dynamically programmed clk_wiz_0 PLL presets.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>

#include "zz_video_modes.h"

#define PLL_INPUT_MHZ 100.0
#define PLL_PFD_MIN_MHZ 19.0
#define PLL_PFD_MAX_MHZ 450.0
#define PLL_VCO_MIN_MHZ 800.0
#define PLL_VCO_MAX_MHZ 1600.0
#define FULL_NATIVE_SCALE_FACTOR 4

static int check_mode(const char *name, enum zz_video_modes index,
			      double expected_refresh_hz)
{
	const struct zz_video_mode *mode = &preset_video_modes[index];
	double pfd_mhz;
	double vco_mhz;
	double pixel_clock_hz;
	double refresh_hz;
	int ok = 1;

	if (mode->mul < 2 || mode->mul > 64 ||
	    mode->div < 1 || mode->div > 56 ||
	    mode->div2 < 1 || mode->div2 > 128) {
		fprintf(stderr, "%s PLL divider tuple %d/%d/%d is out of range\n",
			name, mode->mul, mode->div, mode->div2);
		return 0;
	}

	pfd_mhz = PLL_INPUT_MHZ / mode->div;
	vco_mhz = pfd_mhz * mode->mul;
	pixel_clock_hz = vco_mhz * 1000000.0 / mode->div2;
	refresh_hz = pixel_clock_hz / ((double)mode->hmax * mode->vmax);

	if (pfd_mhz < PLL_PFD_MIN_MHZ || pfd_mhz > PLL_PFD_MAX_MHZ) {
		fprintf(stderr, "%s PLL PFD %.3f MHz is outside %.0f-%.0f MHz\n",
			name, pfd_mhz, PLL_PFD_MIN_MHZ, PLL_PFD_MAX_MHZ);
		ok = 0;
	}
	if (vco_mhz < PLL_VCO_MIN_MHZ || vco_mhz > PLL_VCO_MAX_MHZ) {
		fprintf(stderr, "%s PLL VCO %.3f MHz is outside %.0f-%.0f MHz\n",
			name, vco_mhz, PLL_VCO_MIN_MHZ, PLL_VCO_MAX_MHZ);
		ok = 0;
	}
	if (fabs(pixel_clock_hz - mode->phz) > 1.0) {
		fprintf(stderr,
			"%s pixel clock %.3f Hz does not match table value %d Hz\n",
			name, pixel_clock_hz, mode->phz);
		ok = 0;
	}
	if (fabs(refresh_hz - expected_refresh_hz) > 0.03) {
		fprintf(stderr,
			"%s refresh %.5f Hz is not within 0.03 Hz of %.5f Hz\n",
			name, refresh_hz, expected_refresh_hz);
		ok = 0;
	}

	if (ok)
		printf("%s: %.3f MHz pixel clock, %.5f Hz refresh, %.1f MHz VCO\n",
			name, pixel_clock_hz / 1000000.0, refresh_hz, vco_mhz);

	return ok;
}

static int check_full_native_vsync(const char *name,
				   enum zz_video_modes index)
{
	const struct zz_video_mode *mode = &preset_video_modes[index];
	int active_video_end = mode->vres + FULL_NATIVE_SCALE_FACTOR;

	if (mode->vstart < active_video_end) {
		fprintf(stderr,
			"%s VSync starts at line %d while x4 active video ends at line %d\n",
			name, mode->vstart, active_video_end);
		return 0;
	}

	return 1;
}

static int check_centered_1080p_timing(void)
{
	const struct zz_video_mode *mode =
		&preset_video_modes[ZZVMODE_1920x1080_60];

	if (mode->hres != 1920 || mode->vres != 1080 ||
	    mode->hmax != 2200 || mode->vmax != 1125 ||
	    mode->phz != 150000000 || mode->mul != 15 ||
	    mode->div != 1 || mode->div2 != 10) {
		fprintf(stderr,
			"centered 1080p must reuse mode 5's 150 MHz 2200x1125 timing\n");
		return 0;
	}

	return 1;
}

int main(void)
{
	int ok = 1;

	ok &= check_mode("1280x1024 PAL exact-refresh",
		ZZVMODE_1280x1024_NS_PAL, 49.92226);
	ok &= check_mode("1280x1024 NTSC exact-refresh",
		ZZVMODE_1280x1024_NS_NTSC, 59.93257);
	ok &= check_mode("1280x1024 native standard-refresh",
		ZZVMODE_1280x1024_NATIVE_60, 60.01995);
	ok &= check_mode("centered 1920x1080 standard-refresh",
		ZZVMODE_1920x1080_60, 60.60606);
	ok &= check_centered_1080p_timing();
	ok &= check_full_native_vsync("1280x1024 PAL exact-refresh",
		ZZVMODE_1280x1024_NS_PAL);
	ok &= check_full_native_vsync("1280x1024 NTSC exact-refresh",
		ZZVMODE_1280x1024_NS_NTSC);
	ok &= check_full_native_vsync("1280x1024 native standard-refresh",
		ZZVMODE_1280x1024_NATIVE_60);

	if (!ok)
		return 1;

	puts("video_mode_pll_test: PASS");
	return 0;
}
