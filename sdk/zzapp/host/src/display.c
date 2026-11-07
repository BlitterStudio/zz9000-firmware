/*
 * One display surface. Layouts match the card defaults: RGB565 is
 * big-endian in memory, BGRA8888 is byte order B,G,R,A, CLUT8 is indices.
 * Pitch is width times bytes per pixel; the card may pad, so apps must use
 * surface.pitch.
 *
 * A synchronous flip presents the back buffer and swaps. flip_async in
 * strict mode stays pending until yield or vsync, so flip_idle can return 0.
 * Mailbox mode keeps three buffers: a second flip before vblank replaces the
 * pending page, increments the replaced counter, and returns a buffer to
 * drawing immediately, so flip_idle stays 1.
 *
 * --frames N sets stop after N presented frames. --scale N sizes the window
 * only; the surface stays at the app's resolution.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "host_internal.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct host_display {
    int open;
    uint32_t id;
    uint32_t w, h, pitch, format, layout, flags;
    uint8_t *buf[3];
    int nbuf;
    int showing;
    int drawing;
    int pending;
    uint32_t mailbox_replaced;
    int mailbox;
    uint32_t palette[256];
    uint8_t *convert;
    uint8_t *shown;
    uint32_t shown_bytes;
    uint32_t flips;
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
};

static struct host_display disp;

static uint32_t bpp_of(uint32_t format)
{
    if (format == ZZ_FMT_RGB565)
        return 2u;
    if (format == ZZ_FMT_BGRA8888)
        return 4u;
    if (format == ZZ_FMT_CLUT8)
        return 1u;
    return 0u;
}

static uint32_t layout_of(uint32_t format)
{
    if (format == ZZ_FMT_RGB565)
        return ZZ_LAYOUT_RGB565_BE;
    if (format == ZZ_FMT_BGRA8888)
        return ZZ_LAYOUT_BGRA8888;
    if (format == ZZ_FMT_CLUT8)
        return ZZ_LAYOUT_CLUT8;
    return 0u;
}

static void expand565(uint16_t px, int bgr, uint8_t bgra[4])
{
    uint8_t r5 = (uint8_t)((px >> 11) & 31u);
    uint8_t g6 = (uint8_t)((px >> 5) & 63u);
    uint8_t b5 = (uint8_t)(px & 31u);
    uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
    uint8_t g = (uint8_t)((g6 << 2) | (g6 >> 4));
    uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));

    if (bgr) {
        uint8_t t = r;
        r = b;
        b = t;
    }
    bgra[0] = b;
    bgra[1] = g;
    bgra[2] = r;
    bgra[3] = 255u;
}

static uint16_t load565(const uint8_t *p, int le)
{
    if (le)
        return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* dst is bytes B,G,R,A, the SDL ARGB8888 memory order on a little-endian host
 * and the order we upload on every host. */
static void convert_row(const uint8_t *src, uint32_t format, uint32_t layout,
                        uint32_t width, const uint32_t *palette, uint8_t *dst)
{
    uint32_t x;

    for (x = 0; x < width; x++) {
        uint8_t *px = dst + x * 4u;
        if (format == ZZ_FMT_RGB565) {
            int le = layout == ZZ_LAYOUT_RGB565_LE ||
                     layout == ZZ_LAYOUT_BGR565_LE;
            int bgr = layout == ZZ_LAYOUT_BGR565_BE ||
                      layout == ZZ_LAYOUT_BGR565_LE;
            expand565(load565(src + x * 2u, le), bgr, px);
        } else if (format == ZZ_FMT_BGRA8888 && layout == ZZ_LAYOUT_RGBA8888) {
            px[0] = src[x * 4u + 2u];
            px[1] = src[x * 4u + 1u];
            px[2] = src[x * 4u];
            px[3] = src[x * 4u + 3u];
        } else if (format == ZZ_FMT_BGRA8888) {
            px[0] = src[x * 4u];
            px[1] = src[x * 4u + 1u];
            px[2] = src[x * 4u + 2u];
            px[3] = src[x * 4u + 3u];
        } else {
            uint32_t pal = palette ? palette[src[x]] : 0u;
            px[0] = (uint8_t)pal;
            px[1] = (uint8_t)(pal >> 8);
            px[2] = (uint8_t)(pal >> 16);
            px[3] = 255u;
        }
    }
}

static int expect_bgra(const uint8_t *px, uint8_t b, uint8_t g, uint8_t r)
{
    return px[0] == b && px[1] == g && px[2] == r && px[3] == 255u;
}

int host_display_selftest(void)
{
    uint8_t dst[4];
    uint8_t be[2] = {0xf8u, 0x00u};
    uint8_t le[2] = {0x00u, 0xf8u};
    uint8_t bgr[2] = {0x00u, 0x1fu};
    uint8_t bgra[4] = {0x00u, 0x00u, 0xffu, 0xffu};
    uint8_t index = 7u;
    uint32_t pal[256];

    convert_row(be, ZZ_FMT_RGB565, ZZ_LAYOUT_RGB565_BE, 1u, 0, dst);
    if (!expect_bgra(dst, 0, 0, 255))
        return 1;
    convert_row(le, ZZ_FMT_RGB565, ZZ_LAYOUT_RGB565_LE, 1u, 0, dst);
    if (!expect_bgra(dst, 0, 0, 255))
        return 2;
    convert_row(bgr, ZZ_FMT_RGB565, ZZ_LAYOUT_BGR565_BE, 1u, 0, dst);
    if (!expect_bgra(dst, 0, 0, 255))
        return 3;
    convert_row(bgra, ZZ_FMT_BGRA8888, ZZ_LAYOUT_BGRA8888, 1u, 0, dst);
    if (!expect_bgra(dst, 0, 0, 255))
        return 4;
    memset(pal, 0, sizeof pal);
    pal[7] = 0x0000ff00u;
    convert_row(&index, ZZ_FMT_CLUT8, ZZ_LAYOUT_CLUT8, 1u, pal, dst);
    if (!expect_bgra(dst, 0, 255, 0))
        return 5;
    return 0;
}

static void count_flip(void)
{
    const struct host_options *opt = host_options();

    disp.flips++;
    if (opt->frames_limit && disp.flips >= opt->frames_limit)
        host_request_stop();
}

static void present_buffer(int index)
{
    const struct host_options *opt = host_options();
    uint32_t y;
    uint8_t *src = disp.buf[index];

    if (!src)
        return;
    if (disp.shown && disp.shown_bytes)
        memcpy(disp.shown, src, disp.shown_bytes);
    if (!disp.convert || !disp.tex)
        return;
    for (y = 0; y < disp.h; y++) {
        convert_row(src + y * disp.pitch, disp.format, disp.layout, disp.w,
                    disp.palette, disp.convert + y * disp.w * 4u);
    }
    SDL_UpdateTexture(disp.tex, NULL, disp.convert, (int)(disp.w * 4u));
    SDL_RenderClear(disp.ren);
    {
        SDL_Rect dst;
        dst.x = 0;
        dst.y = 0;
        dst.w = (int)disp.w * opt->scale;
        dst.h = (int)disp.h * opt->scale;
        SDL_RenderCopy(disp.ren, disp.tex, NULL, &dst);
    }
    SDL_RenderPresent(disp.ren);
}

static int spare_buffer(void)
{
    int i;

    for (i = 0; i < disp.nbuf; i++) {
        if (i != disp.showing && i != disp.drawing && i != disp.pending)
            return i;
    }
    return disp.drawing;
}

static void complete_pending(void)
{
    int was;

    if (!disp.open || disp.pending < 0)
        return;
    was = disp.pending;
    present_buffer(was);
    disp.showing = was;
    disp.pending = -1;
    if (!disp.mailbox)
        disp.drawing = disp.drawing == was ? (was ^ 1) : disp.drawing;
    count_flip();
}

static void fill_surface(zzapp_surface *out)
{
    memset(out, 0, sizeof *out);
    out->id = disp.id;
    out->width = disp.w;
    out->height = disp.h;
    out->pitch = disp.pitch;
    out->format = disp.format;
    out->layout = disp.layout;
    out->front = disp.buf[disp.showing];
    out->back = disp.buf[disp.drawing];
    out->card_front = 0x00200000u + (uint32_t)disp.showing * disp.shown_bytes;
    out->card_back = 0x00200000u + (uint32_t)disp.drawing * disp.shown_bytes;
}

static int api_display_open(uint32_t w, uint32_t h, uint32_t format,
                            uint32_t flags, zzapp_surface *out)
{
    const struct host_options *opt = host_options();
    uint32_t bpp;
    uint32_t pitch;
    uint64_t bytes;
    int i;

    if (!out)
        return ZZ_EINVAL;
    if (disp.open)
        return ZZ_EBUSY;
    bpp = bpp_of(format);
    if (!w || !h || !bpp || w > 4096u || h > 4096u)
        return ZZ_EINVAL;
    if ((flags & ~(ZZ_DISPLAY_VSYNC | ZZ_DISPLAY_MAILBOX)) != 0u)
        return ZZ_EINVAL;
    pitch = w * bpp;
    bytes = (uint64_t)pitch * h;
    if (bytes == 0u || bytes > (16u << 20))
        return ZZ_ENOMEM;
    if (!host_sdl_video_ok())
        return ZZ_EIO;
    free(disp.shown);
    free(disp.convert);
    memset(&disp, 0, sizeof disp);
    disp.pending = -1;
    disp.w = w;
    disp.h = h;
    disp.pitch = pitch;
    disp.format = format;
    disp.layout = layout_of(format);
    disp.flags = flags;
    disp.mailbox = (flags & ZZ_DISPLAY_MAILBOX) != 0u;
    disp.nbuf = disp.mailbox ? 3 : 2;
    disp.showing = 0;
    disp.drawing = 1;
    disp.id = 1u;
    disp.shown_bytes = (uint32_t)bytes;
    for (i = 0; i < 256; i++)
        disp.palette[i] = (uint32_t)i << 16 | (uint32_t)i << 8 | (uint32_t)i;
    for (i = 0; i < disp.nbuf; i++) {
        disp.buf[i] = (uint8_t *)calloc(1, (size_t)bytes);
        if (!disp.buf[i])
            goto fail;
    }
    disp.shown = (uint8_t *)calloc(1, (size_t)bytes);
    disp.convert = (uint8_t *)malloc((size_t)w * h * 4u);
    if (!disp.shown || !disp.convert)
        goto fail;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    disp.win = SDL_CreateWindow("zzapp-host", SDL_WINDOWPOS_UNDEFINED,
                                SDL_WINDOWPOS_UNDEFINED, (int)w * opt->scale,
                                (int)h * opt->scale,
                                opt->headless ? SDL_WINDOW_HIDDEN : 0);
    if (!disp.win)
        goto fail;
    disp.ren = SDL_CreateRenderer(
        disp.win, -1,
        opt->headless ? SDL_RENDERER_SOFTWARE
                      : SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!disp.ren)
        disp.ren = SDL_CreateRenderer(disp.win, -1, SDL_RENDERER_SOFTWARE);
    if (!disp.ren)
        goto fail;
    disp.tex = SDL_CreateTexture(disp.ren, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h);
    if (!disp.tex)
        goto fail;
    disp.open = 1;
    fill_surface(out);
    return ZZ_OK;
fail:
    for (i = 0; i < 3; i++)
        free(disp.buf[i]);
    free(disp.shown);
    free(disp.convert);
    if (disp.tex)
        SDL_DestroyTexture(disp.tex);
    if (disp.ren)
        SDL_DestroyRenderer(disp.ren);
    if (disp.win)
        SDL_DestroyWindow(disp.win);
    memset(&disp, 0, sizeof disp);
    disp.pending = -1;
    return ZZ_EIO;
}

static int valid_id(uint32_t id)
{
    return disp.open && id == disp.id;
}

static int api_display_flip(uint32_t surface_id)
{
    if (!valid_id(surface_id))
        return ZZ_EINVAL;
    if (disp.pending >= 0)
        complete_pending();
    present_buffer(disp.drawing);
    disp.showing = disp.drawing;
    /*
     * Pick the next drawing buffer from the live rotation (showing /
     * pending / free), like the firmware mailbox rules. The old
     * `drawing ^= 1` assumed two buffers and produced index 3 after a
     * mailbox async flip left drawing at 2.
     */
    disp.drawing = spare_buffer();
    if (disp.drawing == disp.showing)
        disp.drawing = (disp.showing + 1) % disp.nbuf;
    count_flip();
    return ZZ_OK;
}

static int api_display_flip_async(uint32_t surface_id)
{
    int old;

    if (!valid_id(surface_id))
        return ZZ_EINVAL;
    if (!disp.mailbox) {
        if (disp.pending >= 0)
            return ZZ_EBUSY;
        disp.pending = disp.drawing;
        return ZZ_OK;
    }
    if (disp.pending >= 0)
        disp.mailbox_replaced++;
    old = disp.pending;
    disp.pending = disp.drawing;
    disp.drawing = spare_buffer();
    if (disp.drawing == disp.pending)
        disp.drawing = old >= 0 ? old : (disp.showing ^ 1);
    return ZZ_OK;
}

static int api_display_flip_idle(void)
{
    if (!disp.open)
        return 0;
    if (disp.mailbox)
        return 1;
    return disp.pending < 0;
}

static int api_display_refresh(zzapp_surface *surface)
{
    if (!surface || !disp.open || surface->id != disp.id)
        return ZZ_EINVAL;
    fill_surface(surface);
    return ZZ_OK;
}

static int api_display_set_palette(uint32_t surface_id, uint32_t first,
                                   uint32_t count, const uint32_t *rgb)
{
    uint32_t i;

    if (!valid_id(surface_id))
        return ZZ_EINVAL;
    if (disp.format != ZZ_FMT_CLUT8)
        return ZZ_EINVAL;
    if (!rgb || count == 0u || first > 255u || count > 256u - first)
        return ZZ_EINVAL;
    for (i = 0; i < count; i++)
        disp.palette[first + i] = rgb[i];
    return ZZ_OK;
}

static uint32_t api_display_mailbox_replaced(void)
{
    if (!disp.open || !disp.mailbox)
        return 0u;
    return disp.mailbox_replaced;
}

static int api_display_vsync(void)
{
    if (!disp.open)
        return ZZ_EINVAL;
    host_display_pump();
    return ZZ_OK;
}

static int api_display_close(uint32_t surface_id)
{
    int i;

    if (!valid_id(surface_id))
        return ZZ_EINVAL;
    if (disp.tex)
        SDL_DestroyTexture(disp.tex);
    if (disp.ren)
        SDL_DestroyRenderer(disp.ren);
    if (disp.win)
        SDL_DestroyWindow(disp.win);
    for (i = 0; i < 3; i++) {
        free(disp.buf[i]);
        disp.buf[i] = 0;
    }
    free(disp.convert);
    disp.convert = 0;
    disp.tex = 0;
    disp.ren = 0;
    disp.win = 0;
    disp.open = 0;
    disp.pending = -1;
    return ZZ_OK;
}

void host_display_pump(void)
{
    if (!disp.open)
        return;
    if (disp.mailbox) {
        if (disp.pending >= 0)
            complete_pending();
        return;
    }
    if (disp.pending >= 0)
        complete_pending();
}

void host_display_dump(const char *path)
{
    FILE *fp;
    const uint8_t *src;
    uint32_t n;

    if (!path)
        return;
    src = disp.shown;
    n = disp.shown_bytes;
    if ((!src || n == 0u) && disp.buf[disp.showing]) {
        src = disp.buf[disp.showing];
        n = disp.pitch * disp.h;
    }
    if (!src || n == 0u)
        return;
    fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "zzapp-host: cannot write %s\n", path);
        return;
    }
    if (fwrite(src, 1, n, fp) != n)
        fprintf(stderr, "zzapp-host: short write %s\n", path);
    fclose(fp);
}

uint32_t host_display_flips(void)
{
    return disp.flips;
}

int host_display_init(void)
{
    disp.pending = -1;
    return 0;
}

void host_display_shutdown(void)
{
    if (disp.open)
        api_display_close(disp.id);
    free(disp.shown);
    disp.shown = 0;
    free(disp.convert);
    disp.convert = 0;
}

void host_display_bind(zzapp_api *api)
{
    api->display_open = api_display_open;
    api->display_flip = api_display_flip;
    api->display_flip_async = api_display_flip_async;
    api->display_flip_idle = api_display_flip_idle;
    api->display_refresh = api_display_refresh;
    api->display_set_palette = api_display_set_palette;
    api->display_mailbox_replaced = api_display_mailbox_replaced;
    api->display_vsync = api_display_vsync;
    api->display_close = api_display_close;
}
