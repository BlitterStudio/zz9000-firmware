/* Bounded DVD LPCM (48 kHz/16-bit) unpacker for the U11 experiment.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SDK_DVD_LPCM_H
#define SDK_DVD_LPCM_H

#include <stdint.h>

/* Bytes of the private_stream_1 LPCM substream header this unpacker skips. */
#define SDK_DVD_LPCM_HEADER_BYTES 7U
#define SDK_DVD_LPCM_SAMPLE_RATE_HZ 48000U

struct SDKDVDLPCM {
	uint32_t produced_bytes;
	uint32_t frames;
	uint8_t channels;
	uint8_t quantization;
};

/* Parse the 7-byte LPCM substream header that starts every PES payload.
 * Returns 0 and leaves the stream unconfigured on an out-of-scope header
 * (the deliberately-unclaimed 20/24-bit or non-48 kHz variants). */
int sdk_dvd_lpcm_header(struct SDKDVDLPCM *lpcm, const uint8_t *header);

/* Copy one LPCM payload (header already skipped) into a big-endian s16 PCM
 * destination, converting DVD LPCM byte order (L:L:R:R groups) to the
 * firmware PCM ring's interleaved S16BE frame order. Returns bytes
 * produced, bounded by capacity. */
uint32_t sdk_dvd_lpcm_unpack(struct SDKDVDLPCM *lpcm, const uint8_t *src,
                             uint32_t length, uint8_t *dst,
                             uint32_t capacity);

#endif
