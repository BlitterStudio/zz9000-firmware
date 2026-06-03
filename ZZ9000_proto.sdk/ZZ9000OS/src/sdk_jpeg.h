/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * JPEG decode helpers for SDK image services.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_JPEG_H
#define SDK_JPEG_H

#include <stdint.h>
#include "sdk_mailbox.h"

#define SDK_JPEG_BACKEND_FLAG_BASELINE    (1U << 0)
#define SDK_JPEG_BACKEND_FLAG_PROGRESSIVE (1U << 1)
#define SDK_JPEG_BACKEND_FLAG_DIRECT_BGRA (1U << 2)
#define SDK_JPEG_BACKEND_FLAG_SCALING     (1U << 3)
#define SDK_JPEG_BACKEND_FLAG_DIRECT_RGB888 (1U << 4)

const char *sdk_jpeg_backend_name(void);
uint32_t sdk_jpeg_backend_flags(void);
uint32_t sdk_jpeg_service_flags(void);

int sdk_jpeg_decode_to_surface(const uint8_t *jpeg, uint32_t jpeg_length,
                               uint8_t *dst, uint32_t dst_width,
                               uint32_t dst_height, uint32_t dst_pitch,
                               uint32_t dst_format, uint32_t dst_x,
                               uint32_t dst_y, uint32_t *out_width,
                               uint32_t *out_height,
                               uint32_t *out_bytes_written);

#endif /* SDK_JPEG_H */
