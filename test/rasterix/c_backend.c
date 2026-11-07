#include "rix_bin.h"

#include <stdint.h>
#include <string.h>

size_t rix_harness_build_fill(uint8_t *bytes, size_t capacity)
{
    RixCmdWriter writer;
    RixTriangle triangles[2];
    RixTriangle *references[2] = {&triangles[0], &triangles[1]};
    RixBandList bands;
    RixBandConfig config;
    RixFragmentState fragment;
    RixTextureState texture;
    RixVertex vertices[4] = {
        {0.0f, 0.0f, 0.5f, 1.0f, 0xffffffffU, 0.0f, 0.0f, 1.0f},
        {640.0f, 0.0f, 0.5f, 1.0f, 0xffffffffU, 1.0f, 0.0f, 1.0f},
        {640.0f, 480.0f, 0.5f, 1.0f, 0xffffffffU, 1.0f, 1.0f, 1.0f},
        {0.0f, 480.0f, 0.5f, 1.0f, 0xffffffffU, 0.0f, 1.0f, 1.0f}
    };
    uint32_t texels[128];
    uint32_t index;

    memset(&fragment, 0, sizeof(fragment));
    memset(&texture, 0, sizeof(texture));
    memset(&config, 0, sizeof(config));
    for (index = 0; index < 128U; ++index) {
        texels[index] = (index & 1U) ? 0xffffffffU : 0xff00ffffU;
    }
    rix_cmd_init(&writer, bytes, capacity);
    fragment.depth_func = RIX_FUNC_LESS;
    fragment.depth_mask = 1U;
    fragment.color_mask_r = fragment.color_mask_g = fragment.color_mask_b = fragment.color_mask_a = 1U;
    texture.width_log2 = texture.height_log2 = 4U;
    texture.mag_linear = texture.min_linear = 1U;
    texture.pixel_format = RIX_TEXTURE_RGB565;
#if !defined(C_UNTEXTURED)
    if (!rix_cmd_texture_pages(&writer, 0U, texels, 128U) ||
        !rix_cmd_feature_enable(&writer, RIX_FEATURE_DEPTH | RIX_FEATURE_TMU0) ||
        !rix_cmd_fragment_state(&writer, &fragment) ||
        !rix_cmd_texture_state(&writer, 0U, &texture) ||
#else
    if (!rix_cmd_feature_enable(&writer, RIX_FEATURE_DEPTH) ||
        !rix_cmd_fragment_state(&writer, &fragment) ||
#endif
        !rix_setup_triangle(&triangles[0], &vertices[0], &vertices[1], &vertices[2], 0) ||
        !rix_setup_triangle(&triangles[1], &vertices[0], &vertices[2], &vertices[3], 0) ||
        !rix_band_list_init(&bands, 640U, 480U, RIX_BAND_LAYOUT_COMPLETE, references, 2U) ||
        !rix_band_list_add(&bands, &triangles[0]) || !rix_band_list_add(&bands, &triangles[1])) {
        return 0U;
    }
    config.width = 640U; config.height = 480U;
    config.color_addr = 0x01e00000U; config.depth_addr = 0x01a00000U;
    config.color_pitch = 1280U; config.depth_pitch = 1280U;
    config.clear_color = 0x102030ffU; config.clear_depth = 0xffffU;
    config.clear_mask = RIX_CLEAR_COLOR | RIX_CLEAR_DEPTH;
    if (!rix_band_emit_frame(&writer, &bands, &config) || writer.failed) {
        return 0U;
    }
    return writer.length;
}
