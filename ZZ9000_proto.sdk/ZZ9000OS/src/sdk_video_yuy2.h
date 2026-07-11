/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef SDK_VIDEO_YUY2_H
#define SDK_VIDEO_YUY2_H

#include <stdint.h>

uint32_t sdk_video_yuy2_row_bytes(uint32_t width);

int sdk_video_yuv420_to_yuy2(uint8_t *dst, uint32_t dst_pitch,
	                         uint32_t width, uint32_t height,
	                         const uint8_t *y, uint32_t y_pitch,
	                         const uint8_t *cb, const uint8_t *cr,
	                         uint32_t chroma_pitch,
	                         uint32_t *bytes_written);

#endif /* SDK_VIDEO_YUY2_H */
