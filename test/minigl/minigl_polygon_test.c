/* Host regression for primitive ownership in polygon modes. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "../../ZZ9000_proto.sdk/ZZ9000OS/src/minigl_raster.h"

#define W 64U
#define H 48U
#define VSTRIDE MINIGL_RASTER_VERTEX_STRIDE
#define GL_LINES 0x0001U
#define GL_LINE_LOOP 0x0002U
#define GL_LINE_STRIP 0x0003U
#define GL_TRIANGLE_FAN 0x0006U
#define GL_QUADS 0x0007U
#define GL_QUAD_STRIP 0x0008U
#define GL_POLYGON 0x0009U
#define GL_FLAT 0x1d00U
#define GL_POINT 0x1b00U
#define GL_LINE 0x1b01U
#define GL_FILL 0x1b02U
#define GL_LESS 0x0201U
#define GL_SRC_ALPHA 0x0302U
#define GL_ONE_MINUS_SRC_ALPHA 0x0303U

static uint32_t color[W * H];
static uint8_t depth[W * H * 3U];
static uint8_t vertices[80U * VSTRIDE];
static uint8_t staging[1];
static uint8_t textures[1];

int minigl_present_frame(const ZZ9KMiniGLPacketPresent *present, const uint32_t *pixels)
{ (void)present; (void)pixels; return 1; }
int minigl_present_frame_rgb565(const ZZ9KMiniGLPacketPresent *present, const uint16_t *pixels)
{ (void)present; (void)pixels; return 1; }

static void put32(uint8_t *p, uint32_t value)
{ p[0] = (uint8_t)(value >> 24); p[1] = (uint8_t)(value >> 16); p[2] = (uint8_t)(value >> 8); p[3] = (uint8_t)value; }

static void putfloat(uint8_t *p, float value)
{ union { float f; uint32_t u; } bits; bits.f = value; put32(p, bits.u); }

static void vertex(unsigned index, float x, float y, uint32_t rgba)
{
    uint8_t *record = vertices + index * VSTRIDE;
    memset(record, 0, VSTRIDE);
    putfloat(record, x); putfloat(record + 4U, y); put32(record + 12U, rgba);
    putfloat(record + 44U, 1.0f); putfloat(record + 48U, 1.0f); putfloat(record + 52U, 1.0f);
}

static ZZ9KMiniGLPacketSetState state(void)
{
    ZZ9KMiniGLPacketSetState out;
    memset(&out, 0, sizeof(out));
    out.depth_bits = 24U; out.depth_func = GL_LESS; out.depth_far = 0x00ffffffU;
    out.viewport_w = W; out.viewport_h = H; out.color_mask = 0xfU; out.polygon_mode = GL_FILL;
    out.blend_src = ZZ9K_MGL_WIRE_ONE; out.blend_dst = ZZ9K_MGL_WIRE_ZERO;
    out.blend_src_alpha = ZZ9K_MGL_WIRE_ONE; out.blend_dst_alpha = ZZ9K_MGL_WIRE_ZERO;
    out.blend_equation = ZZ9K_MGL_WIRE_FUNC_ADD;
    return out;
}

static void begin(uint16_t polygon_mode, int blended)
{
    ZZ9KMiniGLPacketSetState s = state();
    s.polygon_mode = polygon_mode;
    if (blended) {
        s.enabled_flags = ZZ9K_MGL_STATE_BLEND;
        s.blend_src = GL_SRC_ALPHA;
        s.blend_dst = GL_ONE_MINUS_SRC_ALPHA;
        s.blend_src_alpha = GL_SRC_ALPHA;
        s.blend_dst_alpha = GL_ONE_MINUS_SRC_ALPHA;
    }
    minigl_raster_set_state(&s);
    minigl_raster_clear(0x4000U, 0U, 0x00ffffffU);
}

static uint32_t pixel(unsigned x, unsigned y) { return color[y * W + x]; }
static unsigned red(unsigned x, unsigned y) { return pixel(x, y) >> 24; }

static void square(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    vertex(0U, -0.75f, -0.75f, a);
    vertex(1U, 0.75f, -0.75f, b);
    vertex(2U, 0.75f, 0.75f, c);
    vertex(3U, -0.75f, 0.75f, d);
}

static void assert_quad_and_polygon_boundaries(void)
{
    square(0xff000080U, 0xff000080U, 0xff000080U, 0xff000080U);
    begin(GL_LINE, 1);
    minigl_raster_draw_arrays(GL_QUADS, 0U, VSTRIDE, 0U, 4U, 0U);
    /* GL line rule: horizontal line on integer sy=42 covers row 41 */
    assert(red(32U, 41U) == 128U);
    assert(red(32U, 24U) == 0U);
    begin(GL_POINT, 1);
    minigl_raster_draw_arrays(GL_QUADS, 0U, VSTRIDE, 0U, 4U, 0U);
    /* GL point rule: floor(s+0.5) places points on row 42 and row 6 */
    assert(red(8U, 42U) == 128U && red(56U, 42U) == 128U);
    assert(red(56U, 6U) == 128U && red(8U, 6U) == 128U);

    begin(GL_LINE, 1);
    minigl_raster_draw_arrays(GL_POLYGON, 0U, VSTRIDE, 0U, 4U, 0U);
    /* GL line rule: horizontal line on integer sy=42 covers row 41 */
    assert(red(32U, 41U) == 128U);
    assert(red(32U, 24U) == 0U);
    begin(GL_POINT, 1);
    minigl_raster_draw_arrays(GL_POLYGON, 0U, VSTRIDE, 0U, 4U, 0U);
    /* GL point rule: floor(s+0.5) places points on row 42 and row 6 */
    assert(red(8U, 42U) == 128U && red(56U, 42U) == 128U);
    assert(red(56U, 6U) == 128U && red(8U, 6U) == 128U);
}
static void assert_clipped_polygon_boundary(void)
{
    vertex(0U, -2.0f, -0.75f, 0xff000080U);
    vertex(1U, 0.75f, -0.75f, 0xff000080U);
    vertex(2U, 0.75f, 0.75f, 0xff000080U);
    vertex(3U, -2.0f, 0.75f, 0xff000080U);
    begin(GL_LINE, 1);
    minigl_raster_draw_arrays(GL_POLYGON, 0U, VSTRIDE, 0U, 4U, 0U);
    /* GL line rules: horizontal boundary on sy=42 covers row 41; right edge at sx=56 covers col 55.
     * The clipped left edge at sx=0 covers x=-1 (outside viewport [0, W)). */
    assert(red(55U, 24U) == 128U);
    assert(red(28U, 41U) == 128U);
    assert(red(28U, 17U) == 0U);
}

static void assert_quad_strip_ownership(void)
{
    vertex(0U, -0.75f, -0.75f, 0xff000080U); vertex(1U, -0.75f, 0.75f, 0xff000080U);
    vertex(2U, 0.0f, -0.75f, 0xff000080U); vertex(3U, 0.0f, 0.75f, 0xff000080U);
    vertex(4U, 0.75f, -0.75f, 0xff000080U); vertex(5U, 0.75f, 0.75f, 0xff000080U);
    begin(GL_LINE, 1);
    minigl_raster_draw_arrays(GL_QUAD_STRIP, 0U, VSTRIDE, 0U, 6U, 0U);
    /* GL line rule: horizontal line on integer sy=42 covers row 41 */
    assert(red(20U, 41U) == 128U);
    assert(red(20U, 24U) == 0U && red(44U, 24U) == 0U);
    /* GL line rule: vertical line on integer sx=32 covers column 31 */
    assert(red(31U, 24U) == 192U);
    begin(GL_POINT, 1);
    minigl_raster_draw_arrays(GL_QUAD_STRIP, 0U, VSTRIDE, 0U, 6U, 0U);
    /* GL point rule: floor(s+0.5) places points on row 42 */
    assert(red(8U, 42U) == 128U && red(32U, 42U) == 192U && red(56U, 42U) == 128U);
}

static void assert_triangle_fan_is_not_polygon(void)
{
    square(0xff000080U, 0xff000080U, 0xff000080U, 0xff000080U);
    begin(GL_LINE, 1);
    minigl_raster_draw_arrays(GL_TRIANGLE_FAN, 0U, VSTRIDE, 0U, 4U, 0U);
    /* GL line rule: vertical line on integer sx=32 covers column 31 */
    assert(red(31U, 24U) == 192U);
    begin(GL_POINT, 1);
    minigl_raster_draw_arrays(GL_TRIANGLE_FAN, 0U, VSTRIDE, 0U, 4U, 0U);
    /* GL point rule: floor(s+0.5) places points on row 42 and row 6 */
    assert(red(8U, 42U) == 192U && red(56U, 6U) == 192U);
    assert(red(56U, 42U) == 128U && red(8U, 6U) == 128U);
}

static void assert_line_end_ownership(void)
{
    vertex(0U, -0.75f, 0.0f, 0xff000080U);
    vertex(1U, 0.0f, 0.0f, 0xff000080U);
    vertex(2U, 0.75f, 0.0f, 0xff000080U);
    begin(GL_FILL, 1);
    minigl_raster_draw_arrays(GL_LINE_STRIP, 0U, VSTRIDE, 0U, 3U, 0U);
    /* GL line rule: horizontal line on integer sy=24 covers row 23 */
    assert(red(32U, 23U) == 128U);

    vertex(0U, -0.75f, -0.75f, 0xff000080U);
    vertex(1U, 0.75f, -0.75f, 0xff000080U);
    vertex(2U, 0.0f, 0.75f, 0xff000080U);
    begin(GL_FILL, 1);
    minigl_raster_draw_arrays(GL_LINE_LOOP, 0U, VSTRIDE, 0U, 3U, 0U);
    /* GL line rule: horizontal line on sy=42 covers row 41; corners at x=8 and x=55 blend twice (192) */
    assert(red(8U, 41U) == 192U && red(55U, 41U) == 192U);
    assert(red(32U, 6U) == 128U);
}

static void assert_flat_provoking_vertices(void)
{
    ZZ9KMiniGLPacketSetState s;
    square(0xff0000ffU, 0x00ff00ffU, 0x0000ffffU, 0xffff00ffU);
    s = state(); s.shade_model = GL_FLAT; minigl_raster_set_state(&s);
    minigl_raster_clear(0x4000U, 0U, 0x00ffffffU);
    minigl_raster_draw_arrays(GL_QUADS, 0U, VSTRIDE, 0U, 4U, 0U);
    assert(pixel(40U, 32U) == 0xffff00ffU && pixel(20U, 16U) == 0xffff00ffU);

    minigl_raster_clear(0x4000U, 0U, 0x00ffffffU);
    minigl_raster_draw_arrays(GL_POLYGON, 0U, VSTRIDE, 0U, 4U, 0U);
    assert(pixel(40U, 32U) == 0xff0000ffU && pixel(20U, 16U) == 0xff0000ffU);

    vertex(0U, -0.5f, 0.0f, 0xff0000ffU);
    vertex(1U, 2.0f, 0.0f, 0x0000ffffU);
    minigl_raster_clear(0x4000U, 0U, 0x00ffffffU);
    minigl_raster_draw_arrays(GL_LINES, 0U, VSTRIDE, 0U, 2U, 0U);
    /* GL line rule: horizontal line on integer sy=24 covers row 23 */
    assert(pixel(20U, 23U) == 0x0000ffffU);
}

static void assert_large_polygon(void)
{
    unsigned i;
    for (i = 0U; i < 40U; ++i) {
        float angle = (float)i * 6.28318530718f / 40.0f;
        vertex(i, 1.3f * cosf(angle), 0.8f * sinf(angle), i ? 0x0000ffffU : 0xff0000ffU);
    }
    begin(GL_FILL, 0);
    {
        ZZ9KMiniGLPacketSetState s = state();
        s.shade_model = GL_FLAT; minigl_raster_set_state(&s);
    }
    minigl_raster_draw_arrays(GL_POLYGON, 0U, VSTRIDE, 0U, 40U, 0U);
    assert(minigl_raster_get_error() == 0U);
    assert(pixel(32U, 24U) == 0xff0000ffU);
    {
        ZZ9KMiniGLPacketSetState s = state();
        s.min_triangle_area = 1.0e9f; minigl_raster_set_state(&s);
        minigl_raster_clear(0x4000U, 0U, 0U);
        minigl_raster_draw_arrays(GL_POLYGON, 0U, VSTRIDE, 0U, 40U, 0U);
        assert(pixel(32U, 24U) == 0U);
        s.min_triangle_area = 10.0f; s.shade_model = GL_FLAT; minigl_raster_set_state(&s);
        minigl_raster_draw_arrays(GL_POLYGON, 0U, VSTRIDE, 0U, 40U, 0U);
        assert(pixel(32U, 24U) == 0xff0000ffU);
    }
    begin(GL_LINE, 0);
    minigl_raster_draw_arrays(GL_POLYGON, 0U, VSTRIDE, 0U, 40U, 0U);
    assert(minigl_raster_get_error() == 0U);
    assert(pixel(32U, 24U) == 0U);
    /* GL line rule: clipped right boundary on sx=64 covers col 63 (sx=0 covers x=-1) */
    assert(pixel(63U, 24U) != 0U);
    begin(GL_POINT, 0);
    minigl_raster_draw_arrays(GL_POLYGON, 0U, VSTRIDE, 0U, 40U, 0U);
    assert(minigl_raster_get_error() == 0U);
    assert(pixel(32U, 24U) == 0U);
}

static void assert_clipped_streaming_polygon_offset(void)
{
    ZZ9KMiniGLPacketSetState s;
    unsigned i;

    for (i = 0U; i < 40U; ++i) {
        float angle = (float)i * 6.28318530718f / 40.0f;
        float x = 1.3f * cosf(angle);
        vertex(i, x, 0.8f * sinf(angle), 0xff0000ffU);
        putfloat(vertices + i * VSTRIDE + 8U, x * 0.2f);
    }
    vertex(0U, 1.3f, 0.0f, 0xff0000ffU);
    vertex(1U, 1.3f, 0.15f, 0xff0000ffU);
    vertex(2U, 1.3f, 0.30f, 0xff0000ffU);
    putfloat(vertices + 0U * VSTRIDE + 8U, 0.26f);
    putfloat(vertices + 1U * VSTRIDE + 8U, 0.26f);
    putfloat(vertices + 2U * VSTRIDE + 8U, 0.26f);
    s = state();
    s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE;
    s.polygon_mode = GL_FILL;
    minigl_raster_set_state(&s);
    minigl_raster_clear(0x4100U, 0U, 0x00ffffffU);
    minigl_raster_draw_arrays(GL_POLYGON, 0U, VSTRIDE, 0U, 40U, 0U);
    s.enabled_flags |= ZZ9K_MGL_STATE_POLYGON_OFFSET_FILL;
    s.polygon_offset_factor = -1.0f;
    s.polygon_offset_units = 0.0f;
    minigl_raster_set_state(&s);
    for (i = 0U; i < 40U; ++i)
        put32(vertices + i * VSTRIDE + 12U, 0x00ff00ffU);
    minigl_raster_draw_arrays(GL_POLYGON, 0U, VSTRIDE, 0U, 40U, 0U);
    assert((pixel(32U, 24U) >> 24) == 0U);
    assert(((pixel(32U, 24U) >> 16) & 255U) >= 254U);
}


static void assert_leading_collinear_streaming_polygon_offset(void)
{
    ZZ9KMiniGLPacketSetState s;
    unsigned i;

    for (i = 0U; i < 40U; ++i) {
        float angle = (float)i * 6.28318530718f / 40.0f;
        float x = 0.8f * cosf(angle);
        vertex(i, x, 0.8f * sinf(angle), 0xff0000ffU);
        putfloat(vertices + i * VSTRIDE + 8U, x * 0.2f);
    }
    vertex(0U, 0.8f, 0.0f, 0xff0000ffU);
    vertex(1U, 0.8f, 0.10f, 0xff0000ffU);
    vertex(2U, 0.8f, 0.20f, 0xff0000ffU);
    putfloat(vertices + 0U * VSTRIDE + 8U, 0.16f);
    putfloat(vertices + 1U * VSTRIDE + 8U, 0.16f);
    putfloat(vertices + 2U * VSTRIDE + 8U, 0.16f);
    s = state();
    s.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE;
    s.polygon_mode = GL_FILL;
    minigl_raster_set_state(&s);
    minigl_raster_clear(0x4100U, 0U, 0x00ffffffU);
    minigl_raster_draw_arrays(GL_POLYGON, 0U, VSTRIDE, 0U, 40U, 0U);
    s.enabled_flags |= ZZ9K_MGL_STATE_POLYGON_OFFSET_FILL;
    s.polygon_offset_factor = -1.0f;
    minigl_raster_set_state(&s);
    for (i = 0U; i < 40U; ++i)
        put32(vertices + i * VSTRIDE + 12U, 0x00ff00ffU);
    minigl_raster_draw_arrays(GL_POLYGON, 0U, VSTRIDE, 0U, 40U, 0U);
    assert((pixel(32U, 24U) >> 24) == 0U);
    assert(((pixel(32U, 24U) >> 16) & 255U) >= 254U);
}
int main(void)
{
    MiniGLRasterBuffers buffers = {
        color, sizeof(color), depth, sizeof(depth), vertices, sizeof(vertices),
        staging, sizeof(staging), textures, sizeof(textures), W, H
    };
    assert(minigl_raster_configure_buffers(&buffers));
    minigl_raster_init();
    assert_quad_and_polygon_boundaries();
    assert_quad_strip_ownership();
    assert_triangle_fan_is_not_polygon();
    assert_line_end_ownership();
    assert_clipped_polygon_boundary();
    assert_flat_provoking_vertices();
    assert_large_polygon();
    assert_clipped_streaming_polygon_offset();
    assert_leading_collinear_streaming_polygon_offset();
    puts("MiniGL polygon topology assertions passed");
    return 0;
}
