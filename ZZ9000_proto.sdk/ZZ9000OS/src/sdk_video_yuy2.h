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

/* Interlaced frames: the same pack with a linear-blend deinterlace fused
 * in (U2). Output row r is the rounded average of luma rows r and r+1 and,
 * within the chroma plane, of chroma rows (r>>1) and (r>>1)+1 (4:2:0
 * interlaced chroma rows alternate fields too); the last luma and chroma
 * rows are copied, having no successor. One pass reads the planes and
 * writes YUY2 directly - a separate blend before the pack pays the blend
 * again in memory traffic (A4000 bench: 2.0 ms pack + 2.3 ms blend per
 * 720x576 frame; the fused pass costs about the pack's 2 ms). */
int sdk_video_yuv420_to_yuy2_blend(uint8_t *dst, uint32_t dst_pitch,
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

/* The blend exactness oracle, independently callable so tests can compare
 * against it. */
int sdk_video_yuv420_to_yuy2_blend_scalar(uint8_t *dst, uint32_t dst_pitch,
                                          uint32_t width, uint32_t height,
                                          const uint8_t *y, uint32_t y_pitch,
                                          const uint8_t *cb,
                                          const uint8_t *cr,
                                          uint32_t chroma_pitch,
                                          uint32_t *bytes_written);

/* Packs the frame at half width and half height ((width / 2) x
 * (height / 2)), for a source too large for the overlay to fetch in time.
 * Output pixel (r, x) is source luma (2r, 2x); each output pair in row r
 * takes the chroma of source row 2r at column 2p. Needs width and height
 * of at least 2. */
int sdk_video_yuv420_to_yuy2_half(uint8_t *dst, uint32_t dst_pitch,
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
int sdk_video_yuv420_to_yuy2_blend_neon(uint8_t *dst, uint32_t dst_pitch,
                                        uint32_t width, uint32_t height,
                                        const uint8_t *y, uint32_t y_pitch,
                                        const uint8_t *cb,
                                        const uint8_t *cr,
                                        uint32_t chroma_pitch,
                                        uint32_t *bytes_written);
#endif

/* Re-arm both self checks (session begin), and report whether a NEON
 * kernel is live so the profiling output can say so rather than leaving
 * it to be assumed. */
void sdk_video_yuy2_reset_dispatch(void);
int sdk_video_yuy2_neon_active(void);

#endif /* SDK_VIDEO_YUY2_H */
