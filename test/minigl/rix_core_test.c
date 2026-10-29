/* RasterIX EF command and descriptor tests. */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "rix_cmd.h"
#include "rix_setup.h"

static uint32_t word_at(const uint8_t *bytes, size_t word)
{
    const uint8_t *p = bytes + word * sizeof(uint32_t);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static RixVertex vertex(float x, float y, float z, float w, uint32_t rgba,
                        float s, float t, float q)
{
    RixVertex value;
    value.x = x;
    value.y = y;
    value.z = z;
    value.w = w;
    value.rgba = rgba;
    value.s = s;
    value.t = t;
    value.q = q;
    return value;
}

static void test_external_frame_stream(void)
{
    uint8_t bytes[512];
    RixCmdWriter writer;
    RixFragmentState fragment;
    RixTriangle triangle;
    RixVertex a = vertex(10.25f, 4.5f, 0.1f, 1.0f, 0xff0000ffU,
                         0.0f, 0.0f, 1.0f);
    RixVertex b = vertex(100.75f, 40.125f, 0.6f, 0.5f, 0x00ff00ffU,
                         1.0f, 0.0f, 1.0f);
    RixVertex c = vertex(20.0f, 88.875f, 0.9f, 0.25f, 0x0000ffffU,
                         0.0f, 1.0f, 1.0f);
    size_t index;

    memset(&fragment, 0, sizeof(fragment));
    fragment.depth_func = RIX_FUNC_LEQUAL;
    fragment.depth_mask = 1U;
    fragment.color_mask_r = fragment.color_mask_g = 1U;
    fragment.color_mask_b = fragment.color_mask_a = 1U;
    assert(rix_setup_triangle(&triangle, &a, &b, &c, NULL));
    assert(triangle.words[1] == 0x0005000aU);
    assert(triangle.words[2] == 0x005a0066U);

    rix_cmd_init(&writer, bytes, sizeof(bytes));
    assert(rix_cmd_resolution(&writer, 640U, 480U));
    assert(rix_cmd_buffer_address(&writer, RIX_REG_COLOR_BUFFER_ADDR, 0x07820000U));
    assert(rix_cmd_buffer_address(&writer, RIX_REG_DEPTH_BUFFER_ADDR, 0x07b20000U));
    assert(rix_cmd_feature_enable(&writer, RIX_FEATURE_DEPTH));
    assert(rix_cmd_fragment_state(&writer, &fragment));
    assert(rix_cmd_clear_color(&writer, 0x11223344U));
    assert(rix_cmd_clear_depth(&writer, 0xffffU));
    assert(rix_cmd_framebuffer(&writer, RIX_FRAMEBUFFER_MEMSET |
                               RIX_FRAMEBUFFER_COLOR | RIX_FRAMEBUFFER_DEPTH,
                               640U * 480U));
    assert(rix_cmd_triangle(&writer, triangle.words, RIX_TRIANGLE_WORDS));
    for (index = 0U; index < rix_cmd_words(&writer); ++index) {
        uint32_t word = word_at(bytes, index);
        assert((word & RIX_OP_FRAMEBUFFER) != RIX_OP_FRAMEBUFFER ||
               (word & (RIX_FRAMEBUFFER_READ | RIX_FRAMEBUFFER_COMMIT)) == 0U);
    }
    assert(word_at(bytes, 0U) == (RIX_OP_RENDER_CONFIG | RIX_REG_RESOLUTION));
    assert(word_at(bytes, 1U) == 0x01e00280U);
}

static void test_scissor_and_capacity(void)
{
    RixVertex a = vertex(-10.0f, 5.0f, 0.1f, 1.0f, 0xffffffffU, 0.0f, 0.0f, 1.0f);
    RixVertex b = vertex(50.0f, 50.0f, 0.5f, 1.0f, 0xffffffffU, 1.0f, 0.0f, 1.0f);
    RixVertex c = vertex(5.0f, 90.0f, 0.9f, 1.0f, 0xffffffffU, 0.0f, 1.0f, 1.0f);
    RixScissor scissor = { 0U, 0U, 64U, 64U, 1U };
    RixTriangle triangle;
    uint8_t bytes[4];
    RixCmdWriter writer;

    assert(rix_setup_triangle(&triangle, &a, &b, &c, &scissor));
    rix_cmd_init(&writer, bytes, sizeof(bytes));
    assert(!rix_cmd_triangle(&writer, triangle.words, RIX_TRIANGLE_WORDS));
    assert(writer.failed);
}

int main(void)
{
    test_external_frame_stream();
    test_scissor_and_capacity();
    return 0;
}
