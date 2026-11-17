/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * JPEG decode helpers for SDK image services.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_JPEG_H
#define SDK_JPEG_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "jpeglib.h"
#include "sdk_mailbox.h"

#define SDK_JPEG_BACKEND_FLAG_BASELINE    (1U << 0)
#define SDK_JPEG_BACKEND_FLAG_PROGRESSIVE (1U << 1)
#define SDK_JPEG_BACKEND_FLAG_DIRECT_BGRA (1U << 2)
#define SDK_JPEG_BACKEND_FLAG_SCALING     (1U << 3)
#define SDK_JPEG_BACKEND_FLAG_DIRECT_RGB888 (1U << 4)

const char *sdk_jpeg_backend_name(void);
uint32_t sdk_jpeg_backend_flags(void);
uint32_t sdk_jpeg_service_flags(void);

struct SDKJpegDecode {
	struct jpeg_decompress_struct cinfo;
	struct jpeg_error_mgr error;
	struct jpeg_source_mgr source;
	struct jpeg_progress_mgr progress;
	jmp_buf jump;
	const uint8_t *jpeg;
	uint8_t *dst;
	uint32_t jpeg_length;
	uint32_t jpeg_offset;
	uint32_t skip_pending;
	uint32_t input_budget;
	uint32_t input_this_step;
	uint32_t monitor_calls;
	uint32_t monitor_suspended;
	uint32_t dst_width;
	uint32_t dst_height;
	uint32_t dst_pitch;
	uint32_t dst_x;
	uint32_t dst_y;
	uint32_t bytes_per_pixel;
	J_COLOR_SPACE color_space;
	uint32_t width;
	uint32_t height;
	uint8_t created;
	uint8_t header_ready;
	uint8_t started;
	uint8_t complete;
};

void sdk_jpeg_decode_init(struct SDKJpegDecode *decode);
int sdk_jpeg_decode_begin(struct SDKJpegDecode *decode, const uint8_t *jpeg,
			  uint32_t jpeg_length, uint8_t *dst,
			  uint32_t dst_width, uint32_t dst_height,
			  uint32_t dst_pitch, uint32_t dst_format,
			  uint32_t dst_x, uint32_t dst_y);
/* Decodes at most max_rows output rows. Returns zero on decode failure. */
int sdk_jpeg_decode_step(struct SDKJpegDecode *decode, uint32_t max_rows,
			 uint32_t *rows_decoded, int *complete);
/* Number of compressed input bytes made visible during the last step. */
uint32_t sdk_jpeg_decode_input_bytes(const struct SDKJpegDecode *decode);
/* Nonzero when the last step ended suspended by the iMCU work bound. */
uint32_t sdk_jpeg_decode_monitor_suspended(const struct SDKJpegDecode *decode);
void sdk_jpeg_decode_destroy(struct SDKJpegDecode *decode);

int sdk_jpeg_decode_to_surface(const uint8_t *jpeg, uint32_t jpeg_length,
                               uint8_t *dst, uint32_t dst_width,
                               uint32_t dst_height, uint32_t dst_pitch,
                               uint32_t dst_format, uint32_t dst_x,
                               uint32_t dst_y, uint32_t *out_width,
                               uint32_t *out_height,
                               uint32_t *out_bytes_written);

#endif /* SDK_JPEG_H */
