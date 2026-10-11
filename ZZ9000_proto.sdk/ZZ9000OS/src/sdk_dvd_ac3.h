/* Deterministic AC-3 (A/52) stereo decode wrapper for the U11 experiment.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SDK_DVD_AC3_H
#define SDK_DVD_AC3_H

#include <stdint.h>

/* Bytes of the private_stream_1 AC-3 substream header this wrapper skips. */
#define SDK_DVD_AC3_HEADER_BYTES 4U
#define SDK_DVD_AC3_BLOCKS_PER_FRAME 6U
#define SDK_DVD_AC3_SAMPLES_PER_BLOCK 256U
#define SDK_DVD_AC3_MAX_CHANNELS 6U
#define SDK_DVD_AC3_ES_CAPACITY (3840U * 2U)
/* Interleaved S16BE stereo bytes one AC-3 frame decodes to. */
#define SDK_DVD_AC3_FRAME_PCM_BYTES \
	(SDK_DVD_AC3_BLOCKS_PER_FRAME * SDK_DVD_AC3_SAMPLES_PER_BLOCK * 4U)

struct SDKDVDAC3 {
	void *state;             /* a52_state_t */
	uint8_t es[SDK_DVD_AC3_ES_CAPACITY];
	uint32_t es_staged;
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t source_channels;
	uint32_t produced_bytes;
	uint32_t frames;
	uint32_t malformed;
};

void sdk_dvd_ac3_init(struct SDKDVDAC3 *ac3);
void sdk_dvd_ac3_reset(struct SDKDVDAC3 *ac3);
void sdk_dvd_ac3_destroy(struct SDKDVDAC3 *ac3);
/* Stage raw AC-3 bytes (substream header already skipped) without
 * decoding or dropping anything: takes at most the free staging space and
 * returns how many bytes it took. */
uint32_t sdk_dvd_ac3_feed(struct SDKDVDAC3 *ac3, const uint8_t *src,
                          uint32_t length);
/* Resyncs on 0x0B77 and decodes every complete staged frame that fits
 * into dst as interleaved S16BE stereo; frames that do not fit stay staged
 * for the next call. Returns bytes produced, bounded by capacity. */
uint32_t sdk_dvd_ac3_decode(struct SDKDVDAC3 *ac3, uint8_t *dst,
                            uint32_t capacity);
/* 1 when a complete frame is staged at the front (leading garbage is
 * discarded on the way, as decode would). */
int sdk_dvd_ac3_frame_ready(struct SDKDVDAC3 *ac3);

#endif
