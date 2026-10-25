/*
 * Private ABI between the resident image-session glue and the WEBP ZZM.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SDK_WEBP_MODULE_H
#define SDK_WEBP_MODULE_H

#include <stdint.h>
#include "sdk_image_stream.h"

#define ZZM_WEBP_OPS_VERSION 1U
#define ZZM_WEBP_ARENA_MAX_REGIONS 16U

struct zzm_webp_arena {
	uintptr_t region[ZZM_WEBP_ARENA_MAX_REGIONS];
	uint32_t region_bytes[ZZM_WEBP_ARENA_MAX_REGIONS];
	uint32_t region_used[ZZM_WEBP_ARENA_MAX_REGIONS];
	uint32_t region_count;
	uint32_t region_size;
	uint32_t limit;
	uint32_t held;
	uint32_t owner;
};

struct zzm_webp_session {
	uint32_t session;
	uint32_t output_mode;
	uint32_t dst_x;
	uint32_t dst_y;
	uint32_t dst_width;
	uint32_t dst_height;
	uint32_t output_format;
	uint32_t tile_stride;
	uint32_t tile_rows;
	uintptr_t dst_address;
	uint32_t dst_pitch;
	uint32_t dst_length;
	uintptr_t tile_address;
	uint32_t tile_length;
	uintptr_t input;
	uint32_t input_length;
	uint32_t input_capacity;
	uint8_t header[12];
	uint8_t header_length;
	uintptr_t canvas;
	uintptr_t anim_decoder;
	uint32_t tile_next_y;
	uint32_t image_width;
	uint32_t image_height;
	uint8_t decoded;
	uint8_t output_complete;
	uint8_t direct_arm_local;
	uint8_t is_animation;
	uint8_t failed;
	uint32_t anim_state;
	uint32_t anim_loop_count;
	uint32_t anim_loop_index;
	uint32_t anim_frame_index;
	uint32_t anim_token;
	uint32_t anim_token_seq;
	uint8_t anim_token_outstanding;
	uint8_t anim_presented;
	uint8_t anim_ended;
	int32_t anim_prev_timestamp;
	struct zzm_webp_arena arena;
};

struct zzm_webp_ops {
	uint32_t version;
	uint32_t size;
	uint16_t (*begin)(struct zzm_webp_session *session, uint32_t session_id,
	                  const struct SDKImageStreamBegin *begin,
	                  struct SDKImageStreamResult *result);
	uint16_t (*feed)(struct zzm_webp_session *session, const uint8_t *src,
	                 uint32_t src_length, uint32_t input_eof,
	                 struct SDKImageStreamResult *result);
	void (*close)(struct zzm_webp_session *session);
	int (*complete_arm_local_output)(const struct zzm_webp_session *session,
	                                 uintptr_t *address, uint32_t *length);
	int (*presented_canvas)(const struct zzm_webp_session *session,
	                        uintptr_t *address, uint32_t *pitch,
	                        uint32_t *width, uint32_t *height);
	void (*poison)(struct zzm_webp_session *session);
	uint16_t (*frame_next)(struct zzm_webp_session *session, uint32_t flags,
	                       struct SDKImageAnimationFrameResult *result);
	uint16_t (*frame_present)(struct zzm_webp_session *session,
	                          uint32_t frame_token, uint32_t flags,
	                          struct SDKImageAnimationFrameResult *result);
	uint16_t (*frame_retire)(struct zzm_webp_session *session,
	                         uint32_t frame_token, uint32_t flags,
	                         struct SDKImageAnimationFrameResult *result);
	uint16_t (*restart)(struct zzm_webp_session *session, uint32_t flags,
	                    struct SDKImageAnimationFrameResult *result);
};

#endif /* SDK_WEBP_MODULE_H */
