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
	uint64_t media_pts;
	uint64_t raw_pts;
	uint32_t media_flags;
};

#define SDK_VIDEO_MEDIA_NO_PTS UINT64_C(0xffffffffffffffff)
#define SDK_VIDEO_MEDIA_AUDIO_NONE 0U
#define SDK_VIDEO_MEDIA_AUDIO_MP2 1U
#define SDK_VIDEO_MEDIA_SAMPLE_S16BE 2U
#define SDK_VIDEO_MEDIA_PCM_FRAME_BYTES 4U
#define SDK_VIDEO_MEDIA_MAX_PCM_RING (256U * 1024U)

#define SDK_VIDEO_MEDIA_FLAG_AUDIO_READY   (1U << 0)
#define SDK_VIDEO_MEDIA_FLAG_BACKPRESSURE  (1U << 1)
#define SDK_VIDEO_MEDIA_FLAG_DERIVED_TIME  (1U << 2)
#define SDK_VIDEO_MEDIA_FLAG_DISCONTINUITY (1U << 3)
#define SDK_VIDEO_MEDIA_FLAG_REBASED       (1U << 4)
#define SDK_VIDEO_MEDIA_FLAG_AUDIO_DONE    (1U << 5)

struct SDKVideoMediaConfig {
	uint32_t audio_codec;
	uint8_t *pcm_ring;
	uint32_t pcm_ring_capacity;
	uint32_t pcm_low_water_bytes;
	uint32_t pcm_high_water_bytes;
};

struct SDKVideoMediaInfo {
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t sample_format;
	uint64_t pcm_produced;
	uint64_t pcm_acknowledged;
	uint64_t audio_pts;
	uint64_t current_audio_pts;
	uint64_t first_audio_pts;
	uint64_t pts_origin;
	uint64_t raw_pts;
	uint32_t audio_frames;
	uint32_t backpressure_events;
	uint32_t flags;
};

enum sdk_video_backend_decode_result {
	SDK_VIDEO_BACKEND_ERROR = -1,
	SDK_VIDEO_BACKEND_NEED_INPUT = 0,
	SDK_VIDEO_BACKEND_FRAME = 1,
	SDK_VIDEO_BACKEND_DONE = 2,
	SDK_VIDEO_BACKEND_PROGRESS = 3,
	SDK_VIDEO_BACKEND_BACKPRESSURE = 4,
	SDK_VIDEO_BACKEND_UNSUPPORTED = 5,
};

enum sdk_video_backend_write_result {
	SDK_VIDEO_BACKEND_WRITE_ERROR = 0,
	SDK_VIDEO_BACKEND_WRITE_OK = 1,
	SDK_VIDEO_BACKEND_WRITE_BACKPRESSURE = 2,
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
	int (*configure_media)(
		void *decoder, const struct SDKVideoMediaConfig *config);
	int (*get_media_info)(
		void *decoder, struct SDKVideoMediaInfo *info);
	int (*ack_media)(void *decoder, uint64_t acknowledged);
};

const struct SDKVideoDecoderOps *sdk_video_backend_find(uint32_t codec,
	                                                     uint32_t container);

#ifdef SDK_VIDEO_HOST_TEST
int sdk_video_plmpeg_test_boundary_mp2_probe(void);
int sdk_video_plmpeg_test_reserved_mp2_bitrate(void);
int sdk_video_plmpeg_test_rate(
	uint32_t frame_rate_milli, uint64_t *units, uint32_t *per_second);
#endif

#endif /* SDK_VIDEO_BACKEND_H */
