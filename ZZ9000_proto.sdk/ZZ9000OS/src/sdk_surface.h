/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Small SDK surface helpers used by firmware mailbox services.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_SURFACE_H
#define SDK_SURFACE_H

#include <stdint.h>
#include "sdk_mailbox.h"

uint32_t sdk_surface_format_bytes(uint32_t format);

int sdk_surface_fill_rect(uint8_t *surface, uint32_t width, uint32_t height,
                          uint32_t pitch, uint32_t format,
                          uint32_t x, uint32_t y,
                          uint32_t rect_width, uint32_t rect_height,
                          uint32_t color);

int sdk_surface_copy_rect(uint8_t *dst, uint32_t dst_width,
                          uint32_t dst_height, uint32_t dst_pitch,
                          const uint8_t *src, uint32_t src_width,
                          uint32_t src_height, uint32_t src_pitch,
                          uint32_t format, uint32_t src_x,
                          uint32_t src_y, uint32_t dst_x,
                          uint32_t dst_y, uint32_t rect_width,
                          uint32_t rect_height);

int sdk_surface_scale_rect(uint8_t *dst, uint32_t dst_width,
                           uint32_t dst_height, uint32_t dst_pitch,
                           const uint8_t *src, uint32_t src_width,
                           uint32_t src_height, uint32_t src_pitch,
                           uint32_t format, uint32_t src_x,
                           uint32_t src_y, uint32_t src_rect_width,
                           uint32_t src_rect_height, uint32_t dst_x,
                           uint32_t dst_y, uint32_t dst_rect_width,
                           uint32_t dst_rect_height, uint32_t filter);

int sdk_surface_scale_rect_clipped(
	uint8_t *dst, uint32_t dst_width, uint32_t dst_height,
	uint32_t dst_pitch, const uint8_t *src, uint32_t src_width,
	uint32_t src_height, uint32_t src_pitch, uint32_t format,
	uint32_t src_x, uint32_t src_y, uint32_t src_rect_width,
	uint32_t src_rect_height, uint32_t dst_x, uint32_t dst_y,
	uint32_t dst_rect_width, uint32_t dst_rect_height,
	uint32_t clip_x, uint32_t clip_y, uint32_t clip_width,
	uint32_t clip_height, uint32_t filter);

int sdk_surface_scale_formats_supported(uint32_t src_format,
                                        uint32_t dst_format,
                                        uint32_t filter);

int sdk_surface_scale_rect_formats(
	uint8_t *dst, uint32_t dst_width, uint32_t dst_height,
	uint32_t dst_pitch, uint32_t dst_format,
	const uint8_t *src, uint32_t src_width, uint32_t src_height,
	uint32_t src_pitch, uint32_t src_format,
	uint32_t src_x, uint32_t src_y, uint32_t src_rect_width,
	uint32_t src_rect_height, uint32_t dst_x, uint32_t dst_y,
	uint32_t dst_rect_width, uint32_t dst_rect_height,
	uint32_t filter);

int sdk_surface_scale_rect_formats_clipped(
	uint8_t *dst, uint32_t dst_width, uint32_t dst_height,
	uint32_t dst_pitch, uint32_t dst_format,
	const uint8_t *src, uint32_t src_width, uint32_t src_height,
	uint32_t src_pitch, uint32_t src_format,
	uint32_t src_x, uint32_t src_y, uint32_t src_rect_width,
	uint32_t src_rect_height, uint32_t dst_x, uint32_t dst_y,
	uint32_t dst_rect_width, uint32_t dst_rect_height,
	uint32_t clip_x, uint32_t clip_y, uint32_t clip_width,
	uint32_t clip_height, uint32_t filter);

#endif /* SDK_SURFACE_H */
