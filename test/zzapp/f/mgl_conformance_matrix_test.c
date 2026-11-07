/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>

#include "../../../sdk/zzapp/templates/mgl_conformance_matrix.h"

static void expect(mgl_probe_rgba actual, unsigned r, unsigned g, unsigned b,
                   unsigned a)
{
    assert(actual.r == r);
    assert(actual.g == g);
    assert(actual.b == b);
    assert(actual.a == a);
}

int main(void)
{
    mgl_probe_rgba brown = { 0xa0u, 0x70u, 0x40u, 0xffu };
    mgl_probe_rgba white = { 0xffu, 0xffu, 0xffu, 0xffu };
    mgl_probe_rgba half = { 0xffu, 0xffu, 0xffu, 0x80u };

    expect(mgl_probe_expected(MGL_PROBE_REPLACE, MGL_PROBE_RGBA, brown,
                              white, 0, 0), 0xa0u, 0x70u, 0x40u, 0xffu);
    expect(mgl_probe_expected(MGL_PROBE_MODULATE, MGL_PROBE_RGBA, brown,
                              white, 0, 0), 0xa0u, 0x70u, 0x40u, 0xffu);
    expect(mgl_probe_expected(MGL_PROBE_MODULATE, MGL_PROBE_RGBA, brown,
                              half, 0, 0), 0xa0u, 0x70u, 0x40u, 0x80u);
    expect(mgl_probe_expected(MGL_PROBE_DECAL, MGL_PROBE_RGBA,
                              (mgl_probe_rgba){ 0xffu, 0u, 0u, 0x80u },
                              (mgl_probe_rgba){ 0u, 0u, 0xffu, 0xffu }, 0, 0),
           128u, 0u, 127u, 255u);
    expect(mgl_probe_expected(MGL_PROBE_ADD, MGL_PROBE_RGB, brown, white,
                              0, 0), 255u, 255u, 255u, 255u);
    expect(mgl_probe_expected(MGL_PROBE_REPLACE, MGL_PROBE_ALPHA, brown,
                              white, 0, 0), 255u, 255u, 255u, 255u);
    expect(mgl_probe_expected(MGL_PROBE_REPLACE, MGL_PROBE_LUMINANCE_ALPHA,
                              (mgl_probe_rgba){ 0xffu, 0u, 0u, 0x80u }, white,
                              1, 0), 0u, 0u, 0u, 0u);
    assert(mgl_probe_match(brown, brown, 0u));
    assert(!mgl_probe_match(brown, white, 8u));
    return 0;
}
