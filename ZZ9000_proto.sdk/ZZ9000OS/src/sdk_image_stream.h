/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Streaming image session state for SDK image services.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_IMAGE_STREAM_H
#define SDK_IMAGE_STREAM_H

#include <stdint.h>
#include "sdk_mailbox.h"

#define SDK_IMAGE_STREAM_PNG_MAX_FEED_BYTES (256U * 1024U)

struct SDKImageStreamBegin {
	uint32_t codec;
	uint32_t output_mode;
	uint32_t dst_surface;
	uint32_t dst_x;
	uint32_t dst_y;
	uint32_t dst_width;
	uint32_t dst_height;
	uint32_t output_format;
	uint32_t tile_handle;
	uint32_t tile_stride;
	uint32_t tile_rows;
	uint32_t flags;
	uintptr_t dst_address;
	uint32_t dst_pitch;
	uint32_t dst_length;
	uintptr_t tile_address;
	uint32_t tile_length;
	/* Session affinity, fixed for the session's whole life: nonzero =
	 * feeds/closes run on the core-1 worker (set at begin when the
	 * scheduler is available), zero = fully inline on core 0. The codec
	 * heap objects live in the owning core's cache, so affinity can
	 * never change mid-session. */
	uint32_t core1_affine;
};

struct SDKImageStreamFeed {
	uint32_t session;
	uint32_t src_handle;
	uint32_t src_offset;
	uint32_t src_length;
	uint32_t flags;
};

struct SDKImageStreamResult {
	uint32_t session;
	uint32_t state;
	uint32_t image_width;
	uint32_t image_height;
	uint32_t output_format;
	uint32_t tile_x;
	uint32_t tile_y;
	uint32_t tile_width;
	uint32_t tile_height;
	uint32_t bytes_consumed;
	uint32_t bytes_written;
	uint32_t flags;
	uintptr_t flush_address;
	uint32_t flush_length;
};

void sdk_image_stream_init(void);
uint32_t sdk_image_stream_active_count(void);
/* Session affinity lookup: 1 = core-1-affine, 0 = core-0, -1 = not found. */
int sdk_image_stream_session_core1(uint32_t session);
/* Nonzero when any open session is core-1-affine (mailbox reset gating). */
int sdk_image_stream_has_core1_sessions(void);
/* After a core-1 fault: drop core-1-affine sessions' dangling codec
 * references (their heap blocks were already freed by the decode-reclaim
 * pass) WITHOUT running destructors, and mark the sessions failed. */
void sdk_image_stream_poison_core1_sessions(void);
uint16_t sdk_image_stream_begin(const struct SDKImageStreamBegin *begin,
                                struct SDKImageStreamResult *result);
uint16_t sdk_image_stream_feed(const struct SDKImageStreamFeed *feed,
                               const uint8_t *src,
                               struct SDKImageStreamResult *result);
uint16_t sdk_image_stream_close(uint32_t session);

#endif /* SDK_IMAGE_STREAM_H */
