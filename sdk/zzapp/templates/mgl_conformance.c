/*
 * MiniGL texture environment conformance probe for the ZZApp in-process path.
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"
#include "mgl_conformance_matrix.h"

#include <stdio.h>
#include <string.h>

typedef struct mgl_probe_group {
    unsigned int pass;
    unsigned int fail;
    int have_first_failure;
    unsigned int texel;
    unsigned int colour;
    int alpha_test;
    int blend;
    mgl_probe_rgba actual;
    mgl_probe_rgba expected;
} mgl_probe_group;

static void log_line(const char *line)
{
    size_t length = strlen(line);
    if (length != 0u) {
        zz_log(line, (uint32_t)length);
        zz_sleep_us(150000u);
    }
}

static void log_case(unsigned int number, enum mgl_probe_env env,
                     enum mgl_probe_format format, unsigned int texel,
                     unsigned int colour, enum mgl_probe_draw draw,
                     int alpha_test, int blend, mgl_probe_rgba actual,
                     mgl_probe_rgba expected, int passed)
{
    char line[256];
    (void)snprintf(line, sizeof(line),
        "MGL case=%u env=%s fmt=%s texel=%s colour=%s draw=%s at=%s bl=%s actual=%02x%02x%02x%02x expected=%02x%02x%02x%02x %s\n",
        number, mgl_probe_env_names[env], mgl_probe_format_names[format],
        mgl_probe_texels[texel].name, mgl_probe_colours[colour].name,
        mgl_probe_draw_names[draw], alpha_test ? "on" : "off",
        blend ? "src-alpha" : "off", actual.r, actual.g, actual.b, actual.a,
        expected.r, expected.g, expected.b, expected.a, passed ? "PASS" : "FAIL");
    log_line(line);
}

static void log_group(enum mgl_probe_env env, enum mgl_probe_format format,
                      enum mgl_probe_draw draw, const mgl_probe_group *group)
{
    char line[256];
    if (!group->have_first_failure) {
        (void)snprintf(line, sizeof(line),
            "MGL env=%s fmt=%s draw=%s pass=%u fail=%u first-fail:none\n",
            mgl_probe_env_names[env], mgl_probe_format_names[format],
            mgl_probe_draw_names[draw], group->pass, group->fail);
    } else {
        (void)snprintf(line, sizeof(line),
            "MGL env=%s fmt=%s draw=%s pass=%u fail=%u first-fail: texel=%s colour=%s at=%s bl=%s actual=%02x%02x%02x%02x expected=%02x%02x%02x%02x\n",
            mgl_probe_env_names[env], mgl_probe_format_names[format],
            mgl_probe_draw_names[draw], group->pass, group->fail,
            mgl_probe_texels[group->texel].name, mgl_probe_colours[group->colour].name,
            group->alpha_test ? "on" : "off", group->blend ? "src-alpha" : "off",
            group->actual.r, group->actual.g, group->actual.b, group->actual.a,
            group->expected.r, group->expected.g, group->expected.b, group->expected.a);
    }
    log_line(line);
}

static int verbosity_from_argv(int argc, char **argv)
{
    int arg;
    int verbosity = 0;
    for (arg = 1; arg < argc; arg++) {
        if (strcmp(argv[arg], "-vv") == 0)
            verbosity = 2;
        else if (strcmp(argv[arg], "-v") == 0 && verbosity < 1)
            verbosity = 1;
    }
    return verbosity;
}

#define GL_COLOR_BUFFER_BIT 0x00004000u
#define GL_TEXTURE_2D 0x0de1u
#define GL_TEXTURE_ENV 0x2300u
#define GL_TEXTURE_ENV_MODE 0x2200u
#define GL_REPLACE 0x1e01u
#define GL_MODULATE 0x2100u
#define GL_DECAL 0x2101u
#define GL_BLEND 0x0be2u
#define GL_ADD 0x0104u
#define GL_RGBA 0x1908u
#define GL_RGB 0x1907u
#define GL_LUMINANCE 0x1909u
#define GL_ALPHA 0x1906u
#define GL_LUMINANCE_ALPHA 0x190au
#define GL_UNSIGNED_BYTE 0x1401u
#define GL_FLOAT 0x1406u
#define GL_UNSIGNED_SHORT 0x1403u
#define GL_NEAREST 0x2600u
#define GL_TEXTURE_MIN_FILTER 0x2801u
#define GL_TEXTURE_MAG_FILTER 0x2800u
#define GL_QUADS 0x0007u
#define GL_VERTEX_ARRAY 0x8074u
#define GL_TEXTURE_COORD_ARRAY 0x8078u
#define GL_COLOR_ARRAY 0x8076u
#define GL_ALPHA_TEST 0x0bc0u
#define GL_GREATER 0x0204u
#define GL_SRC_ALPHA 0x0302u
#define GL_ONE_MINUS_SRC_ALPHA 0x0303u

#define WIDTH 32
#define HEIGHT 32

static const float vertices[4][3] = {
    { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f },
    { 1.0f, 1.0f, 0.0f }, { -1.0f, 1.0f, 0.0f }
};
static const float coords[4][2] = {
    { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f }
};
static const uint16_t indexes[4] = { 0u, 1u, 2u, 3u };

static uint32_t gl_env(enum mgl_probe_env mode)
{
    static const uint32_t values[] = { GL_REPLACE, GL_MODULATE, GL_DECAL, GL_BLEND, GL_ADD };
    return values[mode];
}

static uint32_t gl_format(enum mgl_probe_format format)
{
    static const uint32_t values[] = { GL_RGBA, GL_RGB, GL_LUMINANCE, GL_ALPHA, GL_LUMINANCE_ALPHA };
    return values[format];
}


static void upload_texture(enum mgl_probe_format format, mgl_probe_rgba value)
{
    uint8_t pixels[4];
    uint8_t luma = mgl_probe_luminance(value);
    uint32_t source_format = gl_format(format);
    int32_t internal_format = (int32_t)source_format;

    pixels[0] = value.r; pixels[1] = value.g; pixels[2] = value.b; pixels[3] = value.a;
    if (format == MGL_PROBE_LUMINANCE) pixels[0] = luma;
    if (format == MGL_PROBE_ALPHA) pixels[0] = value.a;
    if (format == MGL_PROBE_LUMINANCE_ALPHA) { pixels[0] = luma; pixels[1] = value.a; }
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, 1, 1, 0,
                 source_format, GL_UNSIGNED_BYTE, pixels);
}

static void draw_quad(enum mgl_probe_draw draw, mgl_probe_rgba colour)
{
    float colours[4][4];
    unsigned int vertex;
    for (vertex = 0u; vertex < 4u; vertex++) {
        colours[vertex][0] = colour.r / 255.0f;
        colours[vertex][1] = colour.g / 255.0f;
        colours[vertex][2] = colour.b / 255.0f;
        colours[vertex][3] = colour.a / 255.0f;
    }
    if (draw == MGL_PROBE_IMMEDIATE) {
        glBegin(GL_QUADS);
        for (vertex = 0u; vertex < 4u; vertex++) {
            glColor4fv(colours[vertex]);
            glTexCoord2fv(coords[vertex]);
            glVertex3fv(vertices[vertex]);
        }
        glEnd();
        return;
    }
    glVertexPointer(3, GL_FLOAT, 0, vertices);
    glTexCoordPointer(2, GL_FLOAT, 0, coords);
    glColorPointer(4, GL_FLOAT, 0, colours);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    if (draw == MGL_PROBE_ARRAYS)
        glDrawArrays(GL_QUADS, 0, 4);
    else
        glDrawElements(GL_QUADS, 4, GL_UNSIGNED_SHORT, indexes);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

int zz_main(int argc, char **argv)
{
    zzapp_surface surface;
    uint32_t texture;
    unsigned int env, format, texel, colour, draw, alpha_test, blend;
    unsigned int cases = 0u, failures = 0u;
    unsigned int total_cases =
        (unsigned int)(sizeof(mgl_probe_envs) / sizeof(mgl_probe_envs[0])) *
        (unsigned int)(sizeof(mgl_probe_formats) / sizeof(mgl_probe_formats[0])) *
        (unsigned int)(sizeof(mgl_probe_texels) / sizeof(mgl_probe_texels[0])) *
        (unsigned int)(sizeof(mgl_probe_colours) / sizeof(mgl_probe_colours[0])) *
        (unsigned int)(sizeof(mgl_probe_draws) / sizeof(mgl_probe_draws[0])) * 4u;
    int verbosity = verbosity_from_argv(argc, argv);
    char line[96];

    if (zz_display_open(WIDTH, HEIGHT, ZZ_FMT_RGB565, ZZ_DISPLAY_VSYNC, &surface) != ZZ_OK ||
        !zz_gl_bind(surface.id))
        return 2;
    glViewport(0, 0, WIDTH, HEIGHT);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEnable(GL_TEXTURE_2D);
    (void)snprintf(line, sizeof(line), "MGL start cases=%u\n", total_cases);
    log_line(line);

    for (env = 0u; env < sizeof(mgl_probe_envs) / sizeof(mgl_probe_envs[0]); env++)
    for (format = 0u; format < sizeof(mgl_probe_formats) / sizeof(mgl_probe_formats[0]); format++)
    for (draw = 0u; draw < sizeof(mgl_probe_draws) / sizeof(mgl_probe_draws[0]); draw++) {
        mgl_probe_group group = { 0u, 0u, 0, 0u, 0u, 0, 0, { 0u, 0u, 0u, 0u }, { 0u, 0u, 0u, 0u } };
        for (texel = 0u; texel < sizeof(mgl_probe_texels) / sizeof(mgl_probe_texels[0]); texel++)
        for (colour = 0u; colour < sizeof(mgl_probe_colours) / sizeof(mgl_probe_colours[0]); colour++)
        for (alpha_test = 0u; alpha_test < 2u; alpha_test++)
        for (blend = 0u; blend < 2u; blend++) {
            mgl_probe_rgba actual;
            mgl_probe_rgba expected;
            int passed;
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            upload_texture(mgl_probe_formats[format], mgl_probe_texels[texel].value);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, (int32_t)gl_env(mgl_probe_envs[env]));
            if (alpha_test) { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.5f); }
            else glDisable(GL_ALPHA_TEST);
            if (blend) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
            else glDisable(GL_BLEND);
            draw_quad(mgl_probe_draws[draw], mgl_probe_colours[colour].value);
            glFinish();
            glReadPixels(WIDTH / 2, HEIGHT / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &actual);
            expected = mgl_probe_expected(mgl_probe_envs[env], mgl_probe_formats[format],
                mgl_probe_texels[texel].value, mgl_probe_colours[colour].value,
                (int)alpha_test, (int)blend);
            passed = mgl_probe_match(actual, expected, 8u);
            cases++;
            if (passed)
                group.pass++;
            else {
                failures++;
                group.fail++;
                if (!group.have_first_failure) {
                    group.have_first_failure = 1;
                    group.texel = texel;
                    group.colour = colour;
                    group.alpha_test = (int)alpha_test;
                    group.blend = (int)blend;
                    group.actual = actual;
                    group.expected = expected;
                }
            }
            if (verbosity == 2 || (!passed && verbosity == 1))
                log_case(cases, mgl_probe_envs[env], mgl_probe_formats[format], texel, colour,
                         mgl_probe_draws[draw], (int)alpha_test, (int)blend, actual, expected, passed);
        }
        log_group(mgl_probe_envs[env], mgl_probe_formats[format], mgl_probe_draws[draw], &group);
    }
    glDeleteTextures(1, &texture);
    zz_gl_swap();
    (void)snprintf(line, sizeof(line), "MGL summary cases=%u failures=%u\n", cases, failures);
    log_line(line);
    zz_gl_unbind();
    zz_display_close(surface.id);
    return failures ? 1 : 0;
}
