/*
 * One MiniGL texture-environment conformance matrix shared by the 68k and
 * ZZApp probes.  Values are RGBA8 and expected results use GL 1.1 rules.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MGL_CONFORMANCE_MATRIX_H
#define MGL_CONFORMANCE_MATRIX_H

#include <stdint.h>

typedef struct mgl_probe_rgba {
    uint8_t r, g, b, a;
} mgl_probe_rgba;

enum mgl_probe_env {
    MGL_PROBE_REPLACE,
    MGL_PROBE_MODULATE,
    MGL_PROBE_DECAL,
    MGL_PROBE_BLEND,
    MGL_PROBE_ADD
};
enum mgl_probe_format {
    MGL_PROBE_RGBA,
    MGL_PROBE_RGB,
    MGL_PROBE_LUMINANCE,
    MGL_PROBE_ALPHA,
    MGL_PROBE_LUMINANCE_ALPHA
};
enum mgl_probe_draw {
    MGL_PROBE_IMMEDIATE,
    MGL_PROBE_ARRAYS,
    MGL_PROBE_ELEMENTS
};

typedef struct mgl_probe_named_rgba {
    const char *name;
    mgl_probe_rgba value;
} mgl_probe_named_rgba;

static const enum mgl_probe_env mgl_probe_envs[] = {
    MGL_PROBE_REPLACE, MGL_PROBE_MODULATE, MGL_PROBE_DECAL,
    MGL_PROBE_BLEND, MGL_PROBE_ADD
};
static const char *const mgl_probe_env_names[] = {
    "replace", "modulate", "decal", "blend", "add"
};
static const enum mgl_probe_format mgl_probe_formats[] = {
    MGL_PROBE_RGBA, MGL_PROBE_RGB, MGL_PROBE_LUMINANCE,
    MGL_PROBE_ALPHA, MGL_PROBE_LUMINANCE_ALPHA
};
static const char *const mgl_probe_format_names[] = {
    "rgba", "rgb", "luminance", "alpha", "luminance-alpha"
};
static const enum mgl_probe_draw mgl_probe_draws[] = {
    MGL_PROBE_IMMEDIATE, MGL_PROBE_ARRAYS, MGL_PROBE_ELEMENTS
};
static const char *const mgl_probe_draw_names[] = {
    "immediate", "arrays", "elements"
};

/* The source texels deliberately include the colours seen in the field log. */
static const mgl_probe_named_rgba mgl_probe_texels[] = {
    { "brown", { 0xa0u, 0x70u, 0x40u, 0xffu } },
    { "red", { 0xffu, 0x00u, 0x00u, 0xffu } },
    { "green", { 0x00u, 0xffu, 0x00u, 0xffu } },
    { "blue", { 0x00u, 0x00u, 0xffu, 0xffu } },
    { "white", { 0xffu, 0xffu, 0xffu, 0xffu } },
    { "black", { 0x00u, 0x00u, 0x00u, 0xffu } },
    { "half-alpha", { 0xa0u, 0x70u, 0x40u, 0x80u } }
};
static const mgl_probe_named_rgba mgl_probe_colours[] = {
    { "white", { 0xffu, 0xffu, 0xffu, 0xffu } },
    { "grey", { 0x80u, 0x80u, 0x80u, 0xffu } },
    { "red", { 0xffu, 0x00u, 0x00u, 0xffu } },
    { "half-alpha", { 0xffu, 0xffu, 0xffu, 0x80u } }
};

static uint8_t mgl_probe_mul(uint8_t a, uint8_t b)
{
    return (uint8_t)(((uint16_t)a * (uint16_t)b + 127u) / 255u);
}
static uint8_t mgl_probe_mix(uint8_t a, uint8_t b, uint8_t f)
{
    return (uint8_t)(((uint32_t)a * (255u - f) + (uint32_t)b * f + 127u) / 255u);
}
static uint8_t mgl_probe_add(uint8_t a, uint8_t b)
{
    return (uint8_t)(((uint16_t)a + b) > 255u ? 255u : (uint16_t)a + b);
}
static uint8_t mgl_probe_luminance(mgl_probe_rgba c)
{
    return (uint8_t)(((uint16_t)c.r * 77u + (uint16_t)c.g * 150u +
                      (uint16_t)c.b * 29u) >> 8);
}

static mgl_probe_rgba mgl_probe_sample(enum mgl_probe_format format,
                                       mgl_probe_rgba source)
{
    mgl_probe_rgba out = source;
    uint8_t luma = mgl_probe_luminance(source);
    if (format == MGL_PROBE_RGB)
        out.a = 255u;
    else if (format == MGL_PROBE_LUMINANCE)
        out.r = out.g = out.b = luma, out.a = 255u;
    else if (format == MGL_PROBE_ALPHA)
        out.r = out.g = out.b = 255u;
    else if (format == MGL_PROBE_LUMINANCE_ALPHA)
        out.r = out.g = out.b = luma;
    return out;
}

static mgl_probe_rgba mgl_probe_texenv(enum mgl_probe_env mode,
                                       mgl_probe_rgba primary,
                                       mgl_probe_rgba sample)
{
    const mgl_probe_rgba constant = { 0u, 0u, 0u, 255u };
    mgl_probe_rgba out;
    if (mode == MGL_PROBE_REPLACE)
        return sample;
    if (mode == MGL_PROBE_MODULATE) {
        out.r = mgl_probe_mul(primary.r, sample.r);
        out.g = mgl_probe_mul(primary.g, sample.g);
        out.b = mgl_probe_mul(primary.b, sample.b);
    } else if (mode == MGL_PROBE_DECAL) {
        out.r = mgl_probe_mix(primary.r, sample.r, sample.a);
        out.g = mgl_probe_mix(primary.g, sample.g, sample.a);
        out.b = mgl_probe_mix(primary.b, sample.b, sample.a);
    } else if (mode == MGL_PROBE_BLEND) {
        out.r = mgl_probe_mix(primary.r, constant.r, sample.r);
        out.g = mgl_probe_mix(primary.g, constant.g, sample.g);
        out.b = mgl_probe_mix(primary.b, constant.b, sample.b);
    } else {
        out.r = mgl_probe_add(primary.r, sample.r);
        out.g = mgl_probe_add(primary.g, sample.g);
        out.b = mgl_probe_add(primary.b, sample.b);
    }
    out.a = mode == MGL_PROBE_DECAL ? primary.a : mgl_probe_mul(primary.a, sample.a);
    return out;
}

static mgl_probe_rgba mgl_probe_expected(enum mgl_probe_env mode,
                                         enum mgl_probe_format format,
                                         mgl_probe_rgba source,
                                         mgl_probe_rgba primary,
                                         int alpha_test, int blend)
{
    mgl_probe_rgba out = mgl_probe_texenv(mode, primary,
                                          mgl_probe_sample(format, source));
    if (alpha_test && out.a <= 128u)
        return (mgl_probe_rgba){ 0u, 0u, 0u, 0u };
    if (blend) {
        out.r = mgl_probe_mul(out.r, out.a);
        out.g = mgl_probe_mul(out.g, out.a);
        out.b = mgl_probe_mul(out.b, out.a);
        out.a = mgl_probe_mul(out.a, out.a);
    }
    return out;
}

static int mgl_probe_match(mgl_probe_rgba actual, mgl_probe_rgba expected,
                           uint8_t tolerance)
{
    int dr = (int)actual.r - expected.r;
    int dg = (int)actual.g - expected.g;
    int db = (int)actual.b - expected.b;
    int da = (int)actual.a - expected.a;
    return dr >= -(int)tolerance && dr <= (int)tolerance &&
           dg >= -(int)tolerance && dg <= (int)tolerance &&
           db >= -(int)tolerance && db <= (int)tolerance &&
           da >= -(int)tolerance && da <= (int)tolerance;
}

#endif
