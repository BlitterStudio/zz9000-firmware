/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MINIGL_PRESENT_H
#define MINIGL_PRESENT_H
#include <stdint.h>
#include "minigl_renderer.h"
/* vram starts at board offset 0x10000. Source is top-down numeric RRGGBBAA. */
int minigl_present_copy(const ZZ9KMiniGLPacketPresent *pres,
                        const uint32_t *color_buffer, uint8_t *vram,
                        uint32_t vram_length);
int minigl_present_frame(const ZZ9KMiniGLPacketPresent *pres,
                         const uint32_t *color_buffer);
int minigl_present_copy_rgb565(const ZZ9KMiniGLPacketPresent *pres,
                               const uint16_t *color_buffer, uint8_t *vram,
                               uint32_t vram_length);
/* RasterIX's target is packed RGB565; callers must have retired its fence. */
int minigl_present_frame_rgb565(const ZZ9KMiniGLPacketPresent *pres,
                                const uint16_t *color_buffer);
#endif
