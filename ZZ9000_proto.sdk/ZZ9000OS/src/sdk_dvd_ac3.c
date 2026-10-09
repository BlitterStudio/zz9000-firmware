/* Deterministic AC-3 (A/52) stereo decode wrapper for the U11 experiment.
 *
 * Downmix policy: liba52's own coefficient set, driven with the fixed
 * output configuration A52_STEREO, unity gain, zero bias and no dynamic
 * range processing. The float-to-int16 conversion clamps and rounds with a
 * fixed expression so the byte stream is a pure function of the encoded
 * frame on one target (deterministic across repeated runs; cross-arch
 * float rounding is covered by the U11 tolerance policy, not rescaled).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdk_dvd_ac3.h"

#include "a52.h"

#include <string.h>

static int16_t ac3_to_s16(float x)
{
	float scaled;

	if (x > 1.0f)
		x = 1.0f;
	else if (x < -1.0f)
		x = -1.0f;
	scaled = x * 32767.0f;
	/* Round half away from zero: fixed semantics, no libc rounding. */
	if (scaled >= 0.0f)
		scaled += 0.5f;
	else
		scaled -= 0.5f;
	return (int16_t)scaled;
}

static uint32_t ac3_source_channels(int flags)
{
	if (flags & A52_LFE)
		return 6U;
	switch (flags & A52_CHANNEL_MASK) {
	case A52_2F2R:
		return 4U;
	case A52_3F:
	case A52_3F2R:
		return 5U;
	case A52_MONO:
	case A52_CHANNEL:
	case A52_STEREO:
	case A52_DOLBY:
	default:
		return 2U;
	}
}

static void drop_front(struct SDKDVDAC3 *ac3, uint32_t bytes)
{
	ac3->es_staged -= bytes;
	memmove(ac3->es, ac3->es + bytes, ac3->es_staged);
}

static uint32_t resync(struct SDKDVDAC3 *ac3)
{
	uint32_t i;

	for (i = 0U; i + 2U <= ac3->es_staged; i++) {
		if (ac3->es[i] == 0x0bU && ac3->es[i + 1U] == 0x77U) {
			if (i != 0U) {
				ac3->malformed += i;
				drop_front(ac3, i);
			}
			return 1U;
		}
	}
	/* No sync word: keep only the final byte (a possible sync start). */
	if (ac3->es_staged > 1U) {
		ac3->malformed += ac3->es_staged - 1U;
		drop_front(ac3, ac3->es_staged - 1U);
	}
	return 0U;
}

void sdk_dvd_ac3_init(struct SDKDVDAC3 *ac3)
{
	if (!ac3)
		return;
	memset(ac3, 0, sizeof(*ac3));
	ac3->state = a52_init(0U);
}

void sdk_dvd_ac3_reset(struct SDKDVDAC3 *ac3)
{
	if (ac3)
		ac3->es_staged = 0U;
}

void sdk_dvd_ac3_destroy(struct SDKDVDAC3 *ac3)
{
	if (!ac3)
		return;
	if (ac3->state) {
		a52_free((a52_state_t *)ac3->state);
		ac3->state = 0;
	}
	ac3->es_staged = 0U;
}

uint32_t sdk_dvd_ac3_decode(struct SDKDVDAC3 *ac3, const uint8_t *src,
                            uint32_t length, uint8_t *dst,
                            uint32_t capacity)
{
	uint32_t produced = 0U;

	if (!ac3 || !ac3->state || (!src && length != 0U) || !dst)
		return 0U;
	if (length != 0U) {
		if (length > SDK_DVD_AC3_ES_CAPACITY)
			length = SDK_DVD_AC3_ES_CAPACITY;
		if (ac3->es_staged + length > SDK_DVD_AC3_ES_CAPACITY) {
			/* Bounded staging: drop everything and resync on the
			 * next frame boundary rather than grow. A valid AC-3
			 * frame (<= 3840 bytes) always fits the buffer. */
			ac3->malformed += ac3->es_staged;
			ac3->es_staged = 0U;
		}
		memcpy(ac3->es + ac3->es_staged, src, length);
		ac3->es_staged += length;
	}

	while (resync(ac3)) {
		int source_flags = 0;
		int sample_rate = 0;
		int bit_rate = 0;
		int output_flags = A52_STEREO;
		sample_t level = 1.0f;
		uint32_t frame_length;
		uint32_t block;

		frame_length = (uint32_t)a52_syncinfo(
			ac3->es, &source_flags, &sample_rate, &bit_rate);
		if (frame_length < 8U) {
			ac3->malformed++;
			drop_front(ac3, 1U);
			continue;
		}
		if (ac3->es_staged < frame_length)
			break;
		if (a52_frame(ac3->state, ac3->es, &output_flags, &level,
		              0.0f) != 0) {
			ac3->malformed++;
			drop_front(ac3, frame_length);
			continue;
		}
		ac3->sample_rate = (uint32_t)sample_rate;
		ac3->source_channels = ac3_source_channels(source_flags);
		ac3->channels = 2U;
		if (capacity - produced <
		    SDK_DVD_AC3_BLOCKS_PER_FRAME *
			    SDK_DVD_AC3_SAMPLES_PER_BLOCK * 4U) {
			/* Ring full: keep the frame staged for the next
			 * call after the client acknowledges. */
			break;
		}
		for (block = 0U; block < SDK_DVD_AC3_BLOCKS_PER_FRAME;
		     block++) {
			sample_t *samples;
			uint32_t i;

			if (a52_block(ac3->state) != 0) {
				ac3->malformed++;
				break;
			}
			samples = a52_samples(ac3->state);
			if (!samples)
				break;
			for (i = 0U; i < SDK_DVD_AC3_SAMPLES_PER_BLOCK;
			     i++) {
				int16_t left = ac3_to_s16(samples[i]);
				int16_t right = ac3_to_s16(
					samples[i +
					        SDK_DVD_AC3_SAMPLES_PER_BLOCK]);
				uint16_t l = (uint16_t)left;
				uint16_t r = (uint16_t)right;

				dst[produced++] = (uint8_t)(l >> 8);
				dst[produced++] = (uint8_t)(l & 0xffU);
				dst[produced++] = (uint8_t)(r >> 8);
				dst[produced++] = (uint8_t)(r & 0xffU);
			}
		}
		ac3->frames++;
		drop_front(ac3, frame_length);
	}
	ac3->produced_bytes += produced;
	return produced;
}
