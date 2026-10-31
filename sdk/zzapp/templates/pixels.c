/*
 * Opens 320x240 RGB565, fills the back buffer, flips until stop or a key.
 * Draws the CPU mapping from zz_display_open. Does not call GL.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"

int zz_main(int argc, char **argv)
{
    zzapp_surface surface;
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
    if (!surface.back || surface.pitch < surface.width * 2u || surface.height == 0)
        zz_exit(ZZ_EIO);

    for (y = 0; y < surface.height; y++) {
        uint8_t *row = (uint8_t *)surface.back + y * surface.pitch;
        for (x = 0; x < surface.width; x++) {
            uint16_t px = (uint16_t)(((x & 32u) ? 0xf800u : 0u) |
                                     ((y & 32u) ? 0x07e0u : 0u) |
                                     0x001fu);
            row[x * 2u] = (uint8_t)px;
            row[x * 2u + 1u] = (uint8_t)(px >> 8);
        }
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
        zz_yield();
    }
    zz_display_close(surface.id);
    return 0;
}
