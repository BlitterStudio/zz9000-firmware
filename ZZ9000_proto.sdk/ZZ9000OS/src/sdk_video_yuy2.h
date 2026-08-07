/*
 * planar YUV 4:2:0 to packed YUY2.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_VIDEO_YUY2_H
#define SDK_VIDEO_YUY2_H

#include <stdint.h>

uint32_t sdk_video_yuy2_row_bytes(uint32_t width);

/* Dispatching entry point: uses NEON where the build has it and the one-off
 * self check agreed with the scalar oracle, and the scalar kernel otherwise. */
int sdk_video_yuv420_to_yuy2(uint8_t *dst, uint32_t dst_pitch,
                             uint32_t width, uint32_t height,
                             const uint8_t *y, uint32_t y_pitch,
                             const uint8_t *cb, const uint8_t *cr,
                             uint32_t chroma_pitch,
                             uint32_t *bytes_written);

/* The exactness oracle (KTD8). Always present, never dispatched away from,
 * and independently callable so tests can compare against it. */
int sdk_video_yuv420_to_yuy2_scalar(uint8_t *dst, uint32_t dst_pitch,
                                    uint32_t width, uint32_t height,
                                    const uint8_t *y, uint32_t y_pitch,
                                    const uint8_t *cb, const uint8_t *cr,
                                    uint32_t chroma_pitch,
                                    uint32_t *bytes_written);

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
int sdk_video_yuv420_to_yuy2_neon(uint8_t *dst, uint32_t dst_pitch,
                                  uint32_t width, uint32_t height,
                                  const uint8_t *y, uint32_t y_pitch,
                                  const uint8_t *cb, const uint8_t *cr,
                                  uint32_t chroma_pitch,
                                  uint32_t *bytes_written);
#endif

/* Re-arm the self check (session begin), and report which kernel is live so
 * the profiling output can say so rather than leaving it to be assumed. */
void sdk_video_yuy2_reset_dispatch(void);
int sdk_video_yuy2_neon_active(void);

#endif /* SDK_VIDEO_YUY2_H */
