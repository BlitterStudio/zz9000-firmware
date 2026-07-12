/*
 * Codec/container-neutral backend contract for SDK video sessions.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_VIDEO_BACKEND_H
#define SDK_VIDEO_BACKEND_H

#include <stddef.h>
#include <stdint.h>

struct SDKVideoDecoderInfo {
	uint32_t width;
	uint32_t height;
	uint32_t frame_rate_milli;
};

struct SDKVideoDecodedFrame {
	uint32_t width;
	uint32_t height;
	uint32_t y_pitch;
	uint32_t chroma_pitch;
	const uint8_t *y;
	const uint8_t *cb;
	const uint8_t *cr;
	uint32_t time_millis;
};

enum sdk_video_backend_decode_result {
	SDK_VIDEO_BACKEND_ERROR = -1,
	SDK_VIDEO_BACKEND_NEED_INPUT = 0,
	SDK_VIDEO_BACKEND_FRAME = 1,
	SDK_VIDEO_BACKEND_DONE = 2,
};

struct SDKVideoDecoderOps {
	uint32_t codec;
	uint32_t container;
	const char *name;
	void *(*create)(void);
	void (*destroy)(void *decoder);
	int (*write)(void *decoder, const uint8_t *src, uint32_t length,
	             int eof, uint32_t *accepted);
	int (*get_info)(void *decoder, struct SDKVideoDecoderInfo *info);
	int (*decode)(void *decoder, struct SDKVideoDecodedFrame *frame);
};

const struct SDKVideoDecoderOps *sdk_video_backend_find(uint32_t codec,
	                                                     uint32_t container);

#endif /* SDK_VIDEO_BACKEND_H */
