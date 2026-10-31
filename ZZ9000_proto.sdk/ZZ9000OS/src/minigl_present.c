/*
 * ZZ9000 MiniGL locked RTG bitmap presentation.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "minigl_present.h"
#include "minigl_platform.h"

#include "memorymap.h"
#include <stddef.h>


static uint32_t pixel_bytes(uint32_t format)
{
    switch (format) {
    case ZZ9K_MGL_PIXFMT_RGBA32:
    case ZZ9K_MGL_PIXFMT_BGRA32:
    case ZZ9K_MGL_PIXFMT_ARGB32:
    case ZZ9K_MGL_PIXFMT_ABGR32: return 4;
    case ZZ9K_MGL_PIXFMT_RGB565_BE:
    case ZZ9K_MGL_PIXFMT_RGB565_LE: return 2;
    case ZZ9K_MGL_PIXFMT_RGB24:
    case ZZ9K_MGL_PIXFMT_BGR24: return 3;
    default: return 0;
    }
}

static int valid_rectangle(const ZZ9KMiniGLPacketPresent *p, uint32_t length)
{
    uint32_t bytes;
    uint64_t row_end, last;
    if (!p || p->target_width == 0 || p->target_height == 0 ||
        p->target_width > ZZ9K_MINIGL_MAX_WIDTH ||
        p->target_height > ZZ9K_MINIGL_MAX_HEIGHT ||
        p->source_x > p->target_width || p->source_y > p->target_height ||
        p->window_w > p->target_width - p->source_x ||
        p->window_h > p->target_height - p->source_y ||
        p->dest_offset < MNT_FB_BASE)
        return 0;
    bytes = pixel_bytes(p->dest_format);
    if (bytes == 0 || p->dest_pitch == 0)
        return 0;
    row_end = ((uint64_t)p->window_x + p->window_w) * bytes;
    if (row_end > p->dest_pitch)
        return 0;
    if (p->window_w == 0 || p->window_h == 0)
        return p->dest_offset - MNT_FB_BASE <= length;
    last = (uint64_t)(p->dest_offset - MNT_FB_BASE) +
           ((uint64_t)p->window_y + p->window_h - 1) * p->dest_pitch + row_end;
    return last <= length;
}

/* Choose the conversion outside the pixel loop. No allocation or scaling;
 * the guest supplies each visible layer clip rectangle while it is locked. */
#define COPY_RECT(BYTES, STORE) do { \
    uint32_t y, x; \
    for (y = 0; y < p->window_h; ++y) { \
        const uint32_t *src = color_buffer + (p->source_y + y) * p->target_width + p->source_x; \
        uint8_t *dst = vram + p->dest_offset - MNT_FB_BASE + \
                      (p->window_y + y) * p->dest_pitch + p->window_x * (BYTES); \
        for (x = 0; x < p->window_w; ++x, dst += (BYTES)) { \
            uint32_t c = src[x]; \
            STORE; \
        } \
    } \
} while (0)

int minigl_present_copy(const ZZ9KMiniGLPacketPresent *p,
                        const uint32_t *color_buffer, uint8_t *vram,
                        uint32_t vram_length)
{
    if (!color_buffer || !vram || !valid_rectangle(p, vram_length))
        return 0;
    switch (p->dest_format) {
    case ZZ9K_MGL_PIXFMT_RGBA32:
        COPY_RECT(4, dst[0]=c>>24; dst[1]=c>>16; dst[2]=c>>8; dst[3]=c);
        break;
    case ZZ9K_MGL_PIXFMT_BGRA32:
        COPY_RECT(4, dst[0]=c>>8; dst[1]=c>>16; dst[2]=c>>24; dst[3]=c);
        break;
    case ZZ9K_MGL_PIXFMT_ARGB32:
        COPY_RECT(4, dst[0]=c; dst[1]=c>>24; dst[2]=c>>16; dst[3]=c>>8);
        break;
    case ZZ9K_MGL_PIXFMT_ABGR32:
        COPY_RECT(4, dst[0]=c; dst[1]=c>>8; dst[2]=c>>16; dst[3]=c>>24);
        break;
    case ZZ9K_MGL_PIXFMT_RGB565_BE:
        COPY_RECT(2, uint32_t s=((c>>16)&0xf800)|((c>>13)&0x7e0)|((c>>11)&31); dst[0]=s>>8; dst[1]=s);
        break;
    case ZZ9K_MGL_PIXFMT_RGB565_LE:
        COPY_RECT(2, uint32_t s=((c>>16)&0xf800)|((c>>13)&0x7e0)|((c>>11)&31); dst[0]=s; dst[1]=s>>8);
        break;
    case ZZ9K_MGL_PIXFMT_RGB24:
        COPY_RECT(3, dst[0]=c>>24; dst[1]=c>>16; dst[2]=c>>8);
        break;
    case ZZ9K_MGL_PIXFMT_BGR24:
        COPY_RECT(3, dst[0]=c>>8; dst[1]=c>>16; dst[2]=c>>24);
        break;
    default: return 0;
    }
    return 1;
}
#undef COPY_RECT
int minigl_present_copy_rgb565(const ZZ9KMiniGLPacketPresent *p,
                               const uint16_t *color_buffer, uint8_t *vram,
                               uint32_t vram_length)
{
    uint32_t y;

    if (!color_buffer || !vram || !valid_rectangle(p, vram_length))
        return 0;
    for (y = 0U; y < p->window_h; ++y) {
        const uint16_t *src = color_buffer + (p->source_y + y) * p->target_width +
                              p->source_x;
        uint8_t *dst = vram + p->dest_offset - MNT_FB_BASE +
                       (p->window_y + y) * p->dest_pitch +
                       p->window_x * pixel_bytes(p->dest_format);
        uint32_t x;

        for (x = 0U; x < p->window_w; ++x) {
            uint16_t value = src[x];
            uint8_t red = (uint8_t)(((value >> 11U) & 0x1fU) * 255U / 31U);
            uint8_t green = (uint8_t)(((value >> 5U) & 0x3fU) * 255U / 63U);
            uint8_t blue = (uint8_t)((value & 0x1fU) * 255U / 31U);

            switch (p->dest_format) {
            case ZZ9K_MGL_PIXFMT_RGB565_LE:
                dst[0] = (uint8_t)value;
                dst[1] = (uint8_t)(value >> 8U);
                dst += 2U;
                break;
            case ZZ9K_MGL_PIXFMT_RGB565_BE:
                dst[0] = (uint8_t)(value >> 8U);
                dst[1] = (uint8_t)value;
                dst += 2U;
                break;
            case ZZ9K_MGL_PIXFMT_RGBA32:
                dst[0] = red; dst[1] = green; dst[2] = blue; dst[3] = 0xffU; dst += 4U;
                break;
            case ZZ9K_MGL_PIXFMT_BGRA32:
                dst[0] = blue; dst[1] = green; dst[2] = red; dst[3] = 0xffU; dst += 4U;
                break;
            case ZZ9K_MGL_PIXFMT_ARGB32:
                dst[0] = 0xffU; dst[1] = red; dst[2] = green; dst[3] = blue; dst += 4U;
                break;
            case ZZ9K_MGL_PIXFMT_ABGR32:
                dst[0] = 0xffU; dst[1] = blue; dst[2] = green; dst[3] = red; dst += 4U;
                break;
            case ZZ9K_MGL_PIXFMT_RGB24:
                dst[0] = red; dst[1] = green; dst[2] = blue; dst += 3U;
                break;
            case ZZ9K_MGL_PIXFMT_BGR24:
                dst[0] = blue; dst[1] = green; dst[2] = red; dst += 3U;
                break;
            default:
                return 0;
            }
        }
    }
    return 1;
}


int minigl_present_frame(const ZZ9KMiniGLPacketPresent *p,
                         const uint32_t *color_buffer)
{
    const uint32_t length = HOST_VISIBLE_HEAP_ADDRESS - FRAMEBUFFER_ADDRESS;
    uint32_t y;
    uint32_t bytes;

    if (!color_buffer || !valid_rectangle(p, length))
        return 0;
    bytes = p->window_w * pixel_bytes(p->dest_format);
    for (y = 0; bytes != 0 && y < p->window_h; ++y) {
        uintptr_t address = (uintptr_t)p->dest_offset + ADDR_ADJ +
                            (p->window_y + y) * p->dest_pitch +
                            p->window_x * pixel_bytes(p->dest_format);
        minigl_platform_cache_invalidate(NULL, address, bytes);
    }
    if (!minigl_present_copy(p, color_buffer,
                             (uint8_t *)(uintptr_t)FRAMEBUFFER_ADDRESS, length))
        return 0;
    for (y = 0; bytes != 0 && y < p->window_h; ++y) {
        uintptr_t address = (uintptr_t)p->dest_offset + ADDR_ADJ +
                            (p->window_y + y) * p->dest_pitch +
                            p->window_x * pixel_bytes(p->dest_format);
        minigl_platform_cache_clean(NULL, address, bytes);
    }
    minigl_platform_barrier(NULL);
    return 1;
}

int minigl_present_frame_rgb565(const ZZ9KMiniGLPacketPresent *p,
                                const uint16_t *color_buffer)
{
    const uint32_t length = HOST_VISIBLE_HEAP_ADDRESS - FRAMEBUFFER_ADDRESS;
    uint32_t y;
    uint32_t bytes;

    if (!color_buffer || !valid_rectangle(p, length))
        return 0;
    bytes = p->window_w * pixel_bytes(p->dest_format);
    for (y = 0U; bytes != 0U && y < p->window_h; ++y) {
        uintptr_t address = (uintptr_t)p->dest_offset + ADDR_ADJ +
                            (p->window_y + y) * p->dest_pitch +
                            p->window_x * pixel_bytes(p->dest_format);
        minigl_platform_cache_invalidate(NULL, address, bytes);
    }
    if (!minigl_present_copy_rgb565(p, color_buffer,
                                    (uint8_t *)(uintptr_t)FRAMEBUFFER_ADDRESS,
                                    length))
        return 0;
    for (y = 0U; bytes != 0U && y < p->window_h; ++y) {
        uintptr_t address = (uintptr_t)p->dest_offset + ADDR_ADJ +
                            (p->window_y + y) * p->dest_pitch +
                            p->window_x * pixel_bytes(p->dest_format);
        minigl_platform_cache_clean(NULL, address, bytes);
    }
    minigl_platform_barrier(NULL);
    return 1;
}
