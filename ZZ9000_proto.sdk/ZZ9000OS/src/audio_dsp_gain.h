/*
 * Pure gain-control mappings shared by the firmware and host tests.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AUDIO_DSP_GAIN_H
#define AUDIO_DSP_GAIN_H

#include <math.h>
#include <stdint.h>

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

/*
 * Stereo separation (issue #255). The master chain ends in a 2x2
 * crossfeed matrix
 *
 *   L' = own * L + cross * R
 *   R' = own * R + cross * L
 *
 * with own = (1 + s) / 2 and cross = (1 - s) / 2 for a separation
 * s = percent / 100: 100 is the identity (full stereo), 0 mixes both
 * channels to the same mono signal. own + cross is exactly 1, so
 * neither extreme exceeds the larger input peak and the matrix needs
 * no headroom of its own. Amiberry and WinUAE use the same matrix.
 *
 * ADAU1701 parameter words are 5.23 fixed point (0x00800000 = 1.0).
 */
#define AUDIO_SEPARATION_MAX 100U
#define AUDIO_SEPARATION_UNITY_Q23 0x00800000U

static inline uint32_t audio_separation_own_q23(unsigned percent)
{
	if (percent > AUDIO_SEPARATION_MAX)
		percent = AUDIO_SEPARATION_MAX;
	/* (100 + p) / 200 of unity, rounded to nearest; at most
	 * 200 * 2^23 + 100, which fits 32 bits. */
	return ((100U + percent) * AUDIO_SEPARATION_UNITY_Q23 + 100U) / 200U;
}

static inline uint32_t audio_separation_cross_q23(unsigned percent)
{
	return AUDIO_SEPARATION_UNITY_Q23 - audio_separation_own_q23(percent);
}

/* Big-endian parameter-RAM word for a non-negative 5.23 value. */
static inline void audio_q23_to_param_bytes(uint32_t q23, uint8_t out[4])
{
	out[0] = (uint8_t)(q23 >> 24);
	out[1] = (uint8_t)(q23 >> 16);
	out[2] = (uint8_t)(q23 >> 8);
	out[3] = (uint8_t)q23;
}

#endif /* AUDIO_DSP_GAIN_H */
