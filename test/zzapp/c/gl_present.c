/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Section 9 test 6. Real front end, real mw_service, in-process transport.
 * A 64x64 RGB565 surface, 16 triangles, swap, then both the context
 * readback and the present-copy into the mocked P96 buffer must contain
 * pixels that are not the cleared background.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mw_ctx.h"
#include "zzapp.h"



#define W 64u
#define H 64u
#define PITCH (W * 2u)

static uint8_t g_p96[H * PITCH];
static int g_flips;

int zzapp_display_bound(uint32_t *w, uint32_t *h, uint32_t *pitch,
                         uint32_t *format, uint32_t *card_front, uint32_t *card_back)
{
    if (w) *w = W;
    if (h) *h = H;
    if (pitch) *pitch = PITCH;
    if (format) *format = 1u; /* ZZ_FMT_RGB565 */
    if (card_front) *card_front = 0x00010000u;
    if (card_back) *card_back = 0;
    return 0;
}

int zzapp_display_flip_bound(void)
{
    g_flips++;
    return 0;
}

extern int zzapp_gl_bind(uint32_t surface_id, const zzapp_gl **out);
extern void zzapp_gl_swap(void);
extern void zzapp_gl_unbind(void);
extern struct mw_ctx *amiberry_transport_inproc_ctx(void);
extern void zzapp_gl_test_set_p96(uint8_t *pixels, uint32_t bytes);
extern uint32_t mw_host_readback_rgba8(struct mw_ctx *c, uint8_t *out, uint32_t capacity);

static int nonzero_bytes(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; ++i)
        if (p[i])
            return 1;
    return 0;
}

static int non_background_rgba(const uint8_t *p, uint32_t n)
{
    uint32_t i;

    for (i = 0; i + 3u < n; i += 4u)
        if (p[i] || p[i + 1u] || p[i + 2u])
            return 1;
    return 0;
}
int zzapp_c_gl_present_test(void)
{
    const zzapp_gl *gl = 0;
    uint8_t rgba[W * H * 4u];
    int i;
    int rc;
    memset(g_p96, 0, sizeof(g_p96));
    zzapp_gl_test_set_p96(g_p96, sizeof(g_p96));
    rc = zzapp_gl_bind(1u, &gl);
    if (rc != 0 || !gl || !gl->Begin || !gl->Ortho) {
        fprintf(stderr, "bind failed %d\n", rc);
        return 1;
    }
    gl->MatrixMode(0x1701u);
    gl->LoadIdentity();
    gl->Ortho(0.0, (double)W, 0.0, (double)H, -1.0, 1.0);
    gl->MatrixMode(0x1700u);
    gl->LoadIdentity();
    gl->Viewport(0, 0, (int32_t)W, (int32_t)H);
    gl->ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl->Clear(0x00004000u);
    gl->Color3f(1.0f, 0.0f, 0.0f);
    for (i = 0; i < 16; ++i) {
        float x = (float)((i % 4) * 16);
        float y = (float)((i / 4) * 16);
        gl->Begin(0x0004u);
        gl->Vertex2f(x + 1.0f, y + 1.0f);
        gl->Vertex2f(x + 14.0f, y + 1.0f);
        gl->Vertex2f(x + 1.0f, y + 14.0f);
        gl->End();
    }
    zzapp_gl_swap();
    if (g_flips != 1) {
        fprintf(stderr, "flip count %d\n", g_flips);
        zzapp_gl_unbind();
        return 1;
    }
    if (mw_host_readback_rgba8(amiberry_transport_inproc_ctx(), rgba, sizeof(rgba)) != 0 ||
        !non_background_rgba(rgba, sizeof(rgba))) {
        fprintf(stderr, "context readback stayed background\n");
        zzapp_gl_unbind();
        return 1;
    }
    if (!nonzero_bytes(g_p96, sizeof(g_p96))) {
        fprintf(stderr, "P96 buffer stayed background\n");
        zzapp_gl_unbind();
        return 1;
    }
    zzapp_gl_unbind();
    return 0;
}

#ifndef ZZAPP_HARNESS
int main(void)
{
    return zzapp_c_gl_present_test();
}
#endif
