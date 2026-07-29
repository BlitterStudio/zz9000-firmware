/*
 * Codec-neutral streaming video sessions.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_VIDEO_STREAM_H
#define SDK_VIDEO_STREAM_H

#include <stdint.h>

#include "sdk_mailbox.h"
#include "sdk_video_backend.h"

struct SDKVideoStreamBegin {
	uint32_t codec;
	uint32_t container;
	uint32_t width;
	uint32_t height;
	uint32_t output_format;
	uint32_t flags;
};

struct SDKVideoStreamWrite {
	uint32_t session;
	const uint8_t *src;
	uint32_t src_length;
	uint32_t flags;
};

struct SDKVideoStreamDecode {
	uint32_t session;
};

struct SDKVideoStreamResult {
	uint32_t session;
	uint32_t state;
	uint32_t width;
	uint32_t height;
	uint32_t frame_rate_milli;
	uint32_t frame_number;
	uint32_t frame_time_millis;
	uint32_t bytes_accepted;
	uint32_t bytes_written;
	uint32_t flags;
};

#define SDK_VIDEO_STREAM_OWNER_LEGACY 0U
#define SDK_VIDEO_STREAM_OWNER_MEDIA  1U

void sdk_video_stream_init(void);
uint32_t sdk_video_stream_active_count(void);
int sdk_video_stream_session_core1(uint32_t session);
int sdk_video_stream_session_owner(uint32_t session);
uint32_t sdk_video_stream_session_height(uint32_t session);
int sdk_video_stream_has_core1_sessions(void);
void sdk_video_stream_poison_core1_sessions(void);
int sdk_video_stream_get_direct_frame(uint32_t session,
	                                  struct SDKVideoDecodedFrame *frame);

uint16_t sdk_video_stream_begin(const struct SDKVideoStreamBegin *begin,
	                            struct SDKVideoStreamResult *result);
uint16_t sdk_video_stream_begin_owned(
	const struct SDKVideoStreamBegin *begin, uint32_t owner,
	struct SDKVideoStreamResult *result);
uint16_t sdk_video_stream_write(const struct SDKVideoStreamWrite *write,
	                            struct SDKVideoStreamResult *result);
uint16_t sdk_video_stream_decode(const struct SDKVideoStreamDecode *decode,
	                             struct SDKVideoStreamResult *result);
uint16_t sdk_video_stream_close(uint32_t session,
	                            struct SDKVideoStreamResult *result);

#endif /* SDK_VIDEO_STREAM_H */
