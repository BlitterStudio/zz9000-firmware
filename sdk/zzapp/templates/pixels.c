/*
 * Opens 320x240 RGB565, fills the back buffer, flips until stop or a key.
 * Draws the CPU mapping from zz_display_open. Does not call GL.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"
#include "zzapp_note.h"

ZZAPP_NOTE(ZZAPP_NOTE_FLAG_DISPLAY, 0)

static void put_rgb565(uint8_t *dst, uint16_t pixel, uint32_t layout)
{
    if (layout == ZZ_LAYOUT_BGR565_BE || layout == ZZ_LAYOUT_BGR565_LE)
        pixel = (uint16_t)((pixel & 0x07e0u) |
                           ((pixel & 0xf800u) >> 11) |
                           ((pixel & 0x001fu) << 11));
    if (layout == ZZ_LAYOUT_RGB565_BE || layout == ZZ_LAYOUT_BGR565_BE) {
        dst[0] = (uint8_t)(pixel >> 8);
        dst[1] = (uint8_t)pixel;
    } else {
        dst[0] = (uint8_t)pixel;
        dst[1] = (uint8_t)(pixel >> 8);
    }
}

int zz_main(int argc, char **argv)
{
    zzapp_surface surface = {0};
    zzapp_event ev;
    uint32_t n;
    uint32_t y;
    uint32_t x;
    int rc;

    (void)argc;
    (void)argv;
    rc = zz_display_open(320, 240, ZZ_FMT_RGB565, ZZ_DISPLAY_VSYNC, &surface);
    if (rc != ZZ_OK)
        zz_exit(rc);
    if (surface.layout == 0u)
        surface.layout = ZZ_LAYOUT_RGB565_BE;
    if (!surface.back || surface.pitch < surface.width * 2u || surface.height == 0 ||
        (surface.layout != ZZ_LAYOUT_RGB565_BE &&
         surface.layout != ZZ_LAYOUT_RGB565_LE &&
         surface.layout != ZZ_LAYOUT_BGR565_BE &&
         surface.layout != ZZ_LAYOUT_BGR565_LE))
        zz_exit(ZZ_EIO);

    for (y = 0; y < surface.height; y++) {
        uint8_t *row = (uint8_t *)surface.back + y * surface.pitch;
        for (x = 0; x < surface.width; x++) {
            uint16_t px = (uint16_t)(((x & 32u) ? 0xf800u : 0u) |
                                     ((y & 32u) ? 0x07e0u : 0u) |
                                     0x001fu);
            put_rgb565(row + x * 2u, px, surface.layout);
        }
    }
    if (surface.front != surface.back) {
        for (y = 0; y < surface.height; y++)
            __builtin_memcpy((uint8_t *)surface.front + y * surface.pitch,
                             (uint8_t *)surface.back + y * surface.pitch,
                             surface.width * 2u);
    }

    for (;;) {
        if (zz_should_stop())
            break;
        n = 0;
        rc = zz_input_poll(&ev, 1, &n);
        if (rc == ZZ_OK && n == 1 &&
            (ev.type == ZZ_EV_STOP || ev.type == ZZ_EV_KEY_DOWN))
            break;
        rc = zz_display_flip(surface.id);
        if (rc != ZZ_OK)
            zz_exit(rc);
        rc = zz_display_refresh(&surface);
        if (rc != ZZ_OK && rc != ZZ_ENOSYS)
            zz_exit(rc);
        zz_yield();
    }
    zz_display_close(surface.id);
    return 0;
}
