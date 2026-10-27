/*
 * Output gamma table for the ZZ9000 video formatter LUT.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gamma.h"

#include <math.h>

int gamma_table_build(uint8_t lut[GAMMA_LUT_ENTRIES], uint16_t gamma_hundredths)
{
	double exponent;
	uint32_t i;

	if (gamma_hundredths < GAMMA_HUNDREDTHS_MIN ||
	    gamma_hundredths > GAMMA_HUNDREDTHS_MAX)
		return 0;

	/* out = 255 * (in / 255) ^ (1 / gamma): values above 1.0 brighten
	 * the midtones, values below 1.0 darken them, 1.00 is identity. */
	exponent = 100.0 / (double)gamma_hundredths;
	for (i = 0; i < GAMMA_LUT_ENTRIES; i++) {
		double v = 255.0 * pow((double)i / 255.0, exponent);
		long r = lround(v);

		if (r < 0)
			r = 0;
		if (r > 255)
			r = 255;
		lut[i] = (uint8_t)r;
	}

	/* Endpoints are fixed: black stays black and white stays white for
	 * every curve, which also pins the table at both ends. */
	lut[0] = 0;
	lut[GAMMA_LUT_ENTRIES - 1] = 255;
	return 1;
}
