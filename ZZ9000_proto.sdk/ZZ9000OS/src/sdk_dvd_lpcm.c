/* Bounded DVD LPCM (48 kHz/16-bit) unpacker for the U11 experiment.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdk_dvd_lpcm.h"

int sdk_dvd_lpcm_header(struct SDKDVDLPCM *lpcm, const uint8_t *header)
{
	uint32_t channels;
	uint32_t quantization;
	uint32_t rate;

	if (!lpcm || !header)
		return 0;
	/* Byte 2: quantization (bits 7-6) and sample rate (bits 3-2) as
	 * written by the DVD-video LPCM substream header; byte 3: channel
	 * count minus one in bits 7-4. Verified against ffmpeg -f dvd
	 * fixtures in test/video_codec. */
	quantization = (header[2] >> 6) & 3U;
	rate = (header[2] >> 2) & 3U;
	channels = ((header[3] >> 4) & 15U) + 1U;
	/* The experiment's deliberately-unclaimed variants stay out of scope:
	 * only 48 kHz/16-bit mono or stereo is accepted. */
	if (quantization != 0U || rate != 0U ||
	    (channels != 1U && channels != 2U))
		return 0;
	if (lpcm->channels == 0U) {
		lpcm->channels = (uint8_t)channels;
		lpcm->quantization = (uint8_t)quantization;
	} else if (lpcm->channels != (uint8_t)channels) {
		return 0;
	}
	return 1;
}

uint32_t sdk_dvd_lpcm_unpack(struct SDKDVDLPCM *lpcm, const uint8_t *src,
                             uint32_t length, uint8_t *dst,
                             uint32_t capacity)
{
	uint32_t frame_bytes;
	uint32_t frames;
	uint32_t bytes;
	uint32_t i;

	if (!lpcm || !src || !dst || length == 0U || lpcm->channels == 0U)
		return 0U;
	frame_bytes = 2U * lpcm->channels;
	/* Whole PCM frames only; a torn tail stays in the payload. */
	frames = length / frame_bytes;
	bytes = frames * frame_bytes;
	if (bytes > capacity)
		bytes = (capacity / frame_bytes) * frame_bytes;
	/* DVD LPCM is already big-endian interleaved s16, the exact frame
	 * format of the firmware PCM ring, so the bounded unpack is a copy. */
	for (i = 0U; i < bytes; i++)
		dst[i] = src[i];
	lpcm->produced_bytes += bytes;
	lpcm->frames += bytes / frame_bytes;
	return bytes;
}
