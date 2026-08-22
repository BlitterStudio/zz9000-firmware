/*
 * Pure gain-control mappings shared by the firmware and host tests.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AUDIO_DSP_GAIN_H
#define AUDIO_DSP_GAIN_H

#include <math.h>

/* dB value to linear gain, the scale every ADAU1701 gain control in
 * this tree uses (audio_scene.c staging included). */
static inline double audio_db_to_linear(double decibels)
{
	return pow(10.0, decibels / 20.0);
}

static inline double audio_adau_prefactor_gain(int pre)
{
	double decibels;

	if (pre > 100)
		pre = 100;
	if (pre < 0)
		pre = 0;

	decibels = ((double)pre - 50.0) * 12.0 / 50.0;
	return audio_db_to_linear(decibels);
}

#endif /* AUDIO_DSP_GAIN_H */
