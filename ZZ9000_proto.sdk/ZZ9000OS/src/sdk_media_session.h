/*
 * Additive video-first media session layered over sdk_video_stream.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_MEDIA_SESSION_H
#define SDK_MEDIA_SESSION_H

#include <stdint.h>

#include "sdk_mailbox.h"

struct SDKMediaSessionBegin {
	uint32_t video_codec;
	uint32_t container;
	uint32_t width;
	uint32_t height;
	uint32_t output_format;
	uint32_t audio_codec;
	uint32_t pcm_ring_handle;
	uint32_t pcm_ring_capacity;
	uint32_t pcm_low_water_bytes;
	uint32_t pcm_high_water_bytes;
	uint32_t flags;
	uint8_t *pcm_ring;
};

struct SDKMediaSessionWrite {
	uint32_t session;
	const uint8_t *src;
	uint32_t src_length;
	uint32_t flags;
};

struct SDKMediaSessionMainResult {
	uint32_t session;
	uint32_t state;
	uint32_t width;
	uint32_t height;
	uint32_t frame_rate_num;
	uint32_t frame_rate_den;
	uint32_t frame_number;
	uint64_t video_pts;
	uint32_t bytes_accepted;
	uint32_t bytes_written;
	uint32_t flags;
};

struct SDKMediaSessionStatusResult {
	uint32_t session;
	uint32_t state;
	uint32_t page;
	uint32_t flags;
	uint64_t value[4];
};

struct SDKMediaSessionAudioResult {
	uint32_t session;
	uint32_t state;
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t sample_format;
	uint64_t pcm_produced;
	uint64_t pcm_acknowledged;
	uint64_t audio_pts;
	uint32_t flags;
};

void sdk_media_session_init(void);
int sdk_media_session_core1(uint32_t session);
int sdk_media_session_close_known(uint32_t session);
void sdk_media_session_poison_core1(void);
void sdk_media_session_present_queued(uint32_t session);
void sdk_media_session_close_retired(uint32_t session);

uint16_t sdk_media_session_begin(
	const struct SDKMediaSessionBegin *begin,
	struct SDKMediaSessionMainResult *result);
uint16_t sdk_media_session_write(
	const struct SDKMediaSessionWrite *write,
	struct SDKMediaSessionMainResult *result);
uint16_t sdk_media_session_decode(
	uint32_t session, uint32_t flags,
	struct SDKMediaSessionMainResult *result);
uint16_t sdk_media_session_present(
	uint32_t session, uint32_t flags,
	struct SDKMediaSessionMainResult *result);
uint16_t sdk_media_session_discard(
	uint32_t session, uint32_t flags,
	struct SDKMediaSessionMainResult *result);
uint16_t sdk_media_session_status(
	uint32_t session, uint32_t page, uint32_t flags,
	struct SDKMediaSessionStatusResult *result);
uint16_t sdk_media_session_audio_read(
	uint32_t session, uint64_t acknowledged, uint32_t flags,
	struct SDKMediaSessionAudioResult *result);
uint16_t sdk_media_session_close(
	uint32_t session, uint32_t flags,
	struct SDKMediaSessionMainResult *result);

#endif /* SDK_MEDIA_SESSION_H */
