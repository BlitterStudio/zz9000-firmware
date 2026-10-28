/*
 * MiniGL RasterIX backend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "minigl_rix_backend.h"
#include "rix_texture.h"

#if !defined(MINIGL_HOST_TEST) && !defined(MINIGL_SERVICE_HOST_TEST)
#include "xil_printf.h"
#define RIX_LOG(...) xil_printf(__VA_ARGS__)
#else
void (*g_rix_test_log)(const char *fmt, ...) = NULL;
#define RIX_LOG(...) do { if (g_rix_test_log) g_rix_test_log(__VA_ARGS__); } while (0)
#endif

#include <string.h>

#define RIX_TIMEOUT 1000000U
#define GL_COLOR_BUFFER_BIT 0x00004000U
#define GL_DEPTH_BUFFER_BIT 0x00000100U
#define GL_FILL 0x1b02U
#define GL_FUNC_ADD 0x8006U
#define GL_REPEAT 0x2901U
#define GL_CLAMP 0x2900U
#define GL_CLAMP_TO_EDGE 0x812fU
#define GL_NEAREST 0x2600U
#define GL_LINEAR 0x2601U
#define GL_NEAREST_MIPMAP_NEAREST 0x2700U
#define GL_LINEAR_MIPMAP_NEAREST 0x2701U
#define GL_NEAREST_MIPMAP_LINEAR 0x2702U
#define GL_LINEAR_MIPMAP_LINEAR 0x2703U
#define GL_REPLACE 0x1e01U
#define GL_MODULATE 0x2100U
#ifndef GL_DECAL
#define GL_DECAL 0x2101U
#endif
#ifndef GL_BLEND
#define GL_BLEND 0x0be2U
#endif
#ifndef GL_ADD
#define GL_ADD 0x0104U
#endif

static int map_compare(uint16_t value, uint8_t *out)
{
    switch (value) {
    case ZZ9K_MGL_WIRE_ALWAYS: *out = RIX_FUNC_ALWAYS; break;
    case ZZ9K_MGL_WIRE_NEVER: *out = RIX_FUNC_NEVER; break;
    case ZZ9K_MGL_WIRE_LESS: *out = RIX_FUNC_LESS; break;
    case ZZ9K_MGL_WIRE_EQUAL: *out = RIX_FUNC_EQUAL; break;
    case ZZ9K_MGL_WIRE_LEQUAL: *out = RIX_FUNC_LEQUAL; break;
    case ZZ9K_MGL_WIRE_GREATER: *out = RIX_FUNC_GREATER; break;
    case ZZ9K_MGL_WIRE_NOTEQUAL: *out = RIX_FUNC_NOTEQUAL; break;
    case ZZ9K_MGL_WIRE_GEQUAL: *out = RIX_FUNC_GEQUAL; break;
    default: return 0;
    }
    return 1;
}

static int map_blend(uint16_t value, uint8_t *out)
{
    switch (value) {
    case ZZ9K_MGL_WIRE_ZERO: *out = RIX_BLEND_ZERO; break;
    case ZZ9K_MGL_WIRE_ONE: *out = RIX_BLEND_ONE; break;
    case ZZ9K_MGL_WIRE_DST_COLOR: *out = RIX_BLEND_DST_COLOR; break;
    case ZZ9K_MGL_WIRE_SRC_COLOR: *out = RIX_BLEND_SRC_COLOR; break;
    case ZZ9K_MGL_WIRE_ONE_MINUS_DST_COLOR: *out = RIX_BLEND_ONE_MINUS_DST_COLOR; break;
    case ZZ9K_MGL_WIRE_ONE_MINUS_SRC_COLOR: *out = RIX_BLEND_ONE_MINUS_SRC_COLOR; break;
    case ZZ9K_MGL_WIRE_SRC_ALPHA: *out = RIX_BLEND_SRC_ALPHA; break;
    case ZZ9K_MGL_WIRE_ONE_MINUS_SRC_ALPHA: *out = RIX_BLEND_ONE_MINUS_SRC_ALPHA; break;
    case ZZ9K_MGL_WIRE_DST_ALPHA: *out = RIX_BLEND_DST_ALPHA; break;
    case ZZ9K_MGL_WIRE_ONE_MINUS_DST_ALPHA: *out = RIX_BLEND_ONE_MINUS_DST_ALPHA; break;
    case ZZ9K_MGL_WIRE_SRC_ALPHA_SATURATE: *out = RIX_BLEND_SRC_ALPHA_SATURATE; break;
    default: return 0;
    }
    return 1;
}

static int backend_maps_state(const ZZ9KMiniGLPacketSetState *state,
                              RixFragmentState *fragment,
                              RixTexEnvState *environment,
                              uint32_t *features)
{
    uint32_t enabled;
    uint16_t environment_mode;

    if (state == NULL || fragment == NULL || environment == NULL || features == NULL ||
        (state->depth_bits != 16U && state->depth_bits != 24U) ||
        (state->enabled_flags & (ZZ9K_MGL_STATE_TEXTURE_2D_1 |
                                 ZZ9K_MGL_STATE_FOG |
                                 ZZ9K_MGL_STATE_POLYGON_OFFSET_FILL |
                                 ZZ9K_MGL_STATE_POLYGON_OFFSET_LINE |
                                 ZZ9K_MGL_STATE_POLYGON_OFFSET_POINT)) != 0U ||
        (state->polygon_mode != 0U && state->polygon_mode != GL_FILL) ||
        state->polygon_offset_factor != 0.0f || state->polygon_offset_units != 0.0f ||
        state->blend_equation != GL_FUNC_ADD) {
        return 0;
    }

    memset(fragment, 0, sizeof(*fragment));
    if (!map_compare(state->depth_func, &fragment->depth_func) ||
        !map_compare(state->alpha_func, &fragment->alpha_func) ||
        !map_blend(state->blend_src, &fragment->src_factor) ||
        !map_blend(state->blend_dst, &fragment->dst_factor)) {
        return 0;
    }
    enabled = state->enabled_flags;
    fragment->alpha_ref = (uint8_t)state->alpha_ref;
    fragment->depth_mask = (enabled & ZZ9K_MGL_STATE_DEPTH_WRITE) != 0U;
    fragment->color_mask_r = (state->color_mask & 1U) != 0U;
    fragment->color_mask_g = (state->color_mask & 2U) != 0U;
    fragment->color_mask_b = (state->color_mask & 4U) != 0U;
    fragment->color_mask_a = (state->color_mask & 8U) != 0U;
    *features = RIX_FEATURE_TMU0;
    if (enabled & ZZ9K_MGL_STATE_DEPTH_TEST) *features |= RIX_FEATURE_DEPTH;
    if (enabled & ZZ9K_MGL_STATE_ALPHA_TEST) *features |= RIX_FEATURE_ALPHA;
    if (enabled & ZZ9K_MGL_STATE_BLEND) *features |= RIX_FEATURE_BLEND;
    /* Scissoring is applied in software by rix_setup_triangle's bounding box
     * clip; the RTL scissor compares band-local descriptor rows and stays
     * disabled. */

    environment_mode = state->tex_env_mode[0];
    memset(environment, 0, sizeof(*environment));
    if ((enabled & ZZ9K_MGL_STATE_TEXTURE_2D_0) == 0U) {
        environment->combine_rgb = RIX_TEX_REPLACE;
        environment->combine_alpha = RIX_TEX_REPLACE;
        environment->src_rgb[0] = RIX_TEXSRC_PRIMARY_COLOR;
        environment->src_alpha[0] = RIX_TEXSRC_PRIMARY_COLOR;
        environment->operand_rgb[0] = RIX_TEXOP_SRC_COLOR;
        environment->operand_alpha[0] = RIX_TEXAOP_SRC_ALPHA;
    } else if (environment_mode == GL_REPLACE) {
        environment->combine_rgb = RIX_TEX_REPLACE;
        environment->combine_alpha = RIX_TEX_REPLACE;
        environment->src_rgb[0] = RIX_TEXSRC_TEXTURE;
        environment->src_alpha[0] = RIX_TEXSRC_TEXTURE;
        environment->operand_rgb[0] = RIX_TEXOP_SRC_COLOR;
        environment->operand_alpha[0] = RIX_TEXAOP_SRC_ALPHA;
    } else if (environment_mode == GL_MODULATE) {
        environment->combine_rgb = RIX_TEX_MODULATE;
        environment->combine_alpha = RIX_TEX_MODULATE;
        environment->src_rgb[0] = RIX_TEXSRC_TEXTURE;
        environment->src_alpha[0] = RIX_TEXSRC_TEXTURE;
        environment->src_rgb[1] = RIX_TEXSRC_PRIMARY_COLOR;
        environment->src_alpha[1] = RIX_TEXSRC_PRIMARY_COLOR;
        environment->operand_rgb[0] = RIX_TEXOP_SRC_COLOR;
        environment->operand_alpha[0] = RIX_TEXAOP_SRC_ALPHA;
        environment->operand_rgb[1] = RIX_TEXOP_SRC_COLOR;
        environment->operand_alpha[1] = RIX_TEXAOP_SRC_ALPHA;
    } else if (environment_mode == GL_ADD) {
        environment->combine_rgb = RIX_TEX_ADD;
        environment->combine_alpha = RIX_TEX_MODULATE;
        environment->src_rgb[0] = RIX_TEXSRC_TEXTURE;
        environment->src_alpha[0] = RIX_TEXSRC_TEXTURE;
        environment->src_rgb[1] = RIX_TEXSRC_PRIMARY_COLOR;
        environment->src_alpha[1] = RIX_TEXSRC_PRIMARY_COLOR;
        environment->operand_rgb[0] = RIX_TEXOP_SRC_COLOR;
        environment->operand_alpha[0] = RIX_TEXAOP_SRC_ALPHA;
        environment->operand_rgb[1] = RIX_TEXOP_SRC_COLOR;
        environment->operand_alpha[1] = RIX_TEXAOP_SRC_ALPHA;
    } else if (environment_mode == GL_DECAL) {
        environment->combine_rgb = RIX_TEX_INTERPOLATE;
        environment->combine_alpha = RIX_TEX_REPLACE;
        environment->src_rgb[0] = RIX_TEXSRC_TEXTURE;
        environment->operand_rgb[0] = RIX_TEXOP_SRC_COLOR;
        environment->src_rgb[1] = RIX_TEXSRC_PRIMARY_COLOR;
        environment->operand_rgb[1] = RIX_TEXOP_SRC_COLOR;
        environment->src_rgb[2] = RIX_TEXSRC_TEXTURE;
        environment->operand_rgb[2] = RIX_TEXOP_SRC_ALPHA;
        environment->src_alpha[0] = RIX_TEXSRC_PRIMARY_COLOR;
        environment->operand_alpha[0] = RIX_TEXAOP_SRC_ALPHA;
    } else if (environment_mode == GL_BLEND) {
        environment->combine_rgb = RIX_TEX_INTERPOLATE;
        environment->combine_alpha = RIX_TEX_MODULATE;
        environment->src_rgb[0] = RIX_TEXSRC_CONSTANT;
        environment->operand_rgb[0] = RIX_TEXOP_SRC_COLOR;
        environment->src_rgb[1] = RIX_TEXSRC_PRIMARY_COLOR;
        environment->operand_rgb[1] = RIX_TEXOP_SRC_COLOR;
        environment->src_rgb[2] = RIX_TEXSRC_TEXTURE;
        environment->operand_rgb[2] = RIX_TEXOP_SRC_COLOR;
        environment->src_alpha[0] = RIX_TEXSRC_TEXTURE;
        environment->operand_alpha[0] = RIX_TEXAOP_SRC_ALPHA;
        environment->src_alpha[1] = RIX_TEXSRC_PRIMARY_COLOR;
        environment->operand_alpha[1] = RIX_TEXAOP_SRC_ALPHA;
    } else {
        return 0;
    }
    return 1;
}

static MiniGLRixTexture *backend_texture(MiniGLRixBackend *backend, uint32_t id)
{
    size_t index;

    for (index = 0U; index < MINIGL_RIX_MAX_TEXTURES; ++index)
        if (backend->textures[index].valid && !backend->textures[index].deleted &&
            backend->textures[index].id == id)
            return &backend->textures[index];
    return NULL;
}

static MiniGLRixTexture *backend_recorded_texture(MiniGLRixBackend *backend, uint32_t id)
{
    size_t index;

    for (index = 0U; index < MINIGL_RIX_MAX_TEXTURES; ++index)
        if (backend->textures[index].valid && backend->textures[index].id == id)
            return &backend->textures[index];
    return NULL;
}

static int mip_filter(uint16_t filter)
{
    return filter >= GL_NEAREST_MIPMAP_NEAREST &&
           filter <= GL_LINEAR_MIPMAP_LINEAR;
}

static int texture_filter_supported(const MiniGLRixTexture *texture)
{
    if (!mip_filter(texture->min_filter))
        return texture->min_filter == texture->mag_filter;
    return (texture->min_filter == GL_NEAREST_MIPMAP_NEAREST &&
            texture->mag_filter == GL_NEAREST) ||
           (texture->min_filter == GL_LINEAR_MIPMAP_NEAREST &&
            texture->mag_filter == GL_LINEAR);
}

static uint16_t texture_dimension(uint16_t value)
{
    uint16_t result = 1U;

    while (result <= value / 2U && result < 256U)
        result = (uint16_t)(result << 1U);
    return result;
}

static uint8_t texture_log2(uint16_t value)
{
    uint8_t result = 0U;

    while (value > 1U) {
        value = (uint16_t)(value >> 1U);
        ++result;
    }
    return result;
}

static uint16_t texture_required_levels(uint8_t width_log2, uint8_t height_log2)
{
    uint8_t last = width_log2 > height_log2 ? width_log2 : height_log2;

    return (uint16_t)((1U << (last + 1U)) - 1U);
}

static size_t backend_texture_slots(const MiniGLRixBackend *backend)
{
    size_t slots = backend->platform.texture_bytes / MINIGL_RIX_TEXTURE_VIRTUAL_BYTES;

    return slots < MINIGL_RIX_MAX_TEXTURES ? slots : MINIGL_RIX_MAX_TEXTURES;
}

static MiniGLRixTexture *backend_alloc_texture(MiniGLRixBackend *backend, uint32_t id)
{
    MiniGLRixTexture *candidate = NULL;
    size_t index;
    size_t slots = backend_texture_slots(backend);

    for (index = 0U; index < slots; ++index)
        if (!backend->textures[index].valid) {
            candidate = &backend->textures[index];
            break;
        }
    if (candidate == NULL) {
        for (index = 0U; index < slots; ++index) {
            MiniGLRixTexture *texture = &backend->textures[index];

            if (texture->pin_count != 0U)
                continue;
            if (candidate == NULL || (texture->deleted && !candidate->deleted) ||
                (texture->deleted == candidate->deleted &&
                 texture->lru_stamp < candidate->lru_stamp))
                candidate = texture;
        }
    }
    if (candidate == NULL)
        return NULL;
    memset(candidate, 0, sizeof(*candidate));
    candidate->id = id;
    candidate->offset = (uint32_t)(candidate - backend->textures) *
                        MINIGL_RIX_TEXTURE_VIRTUAL_BYTES;
    candidate->valid = 1U;
    candidate->min_filter = GL_NEAREST_MIPMAP_LINEAR;
    candidate->mag_filter = GL_LINEAR;
    candidate->wrap_s = GL_REPEAT;
    candidate->wrap_t = GL_REPEAT;
    return candidate;
}

static void backend_fill_white_texture(MiniGLRixBackend *backend)
{
    size_t index;

    if (backend->platform.white_texture == NULL || backend->platform.white_texture_bytes < 2048U)
        return;
    for (index = 0U; index < 2048U; index += 2U) {
        backend->platform.white_texture[index] = 0xffU;
        backend->platform.white_texture[index + 1U] = 0xffU;
    }
    if (backend->platform.cache_clean != NULL) {
        backend->platform.cache_clean(backend->platform.context,
                                      (uintptr_t)backend->platform.white_texture,
                                      2048U);
    }
}

void minigl_rix_backend_bind_texture(MiniGLRixBackend *backend, uint32_t id)
{
    if (backend != NULL) backend->bound_texture = id;
}

void minigl_rix_backend_tex_params(MiniGLRixBackend *backend, uint32_t id,
                                   uint16_t min_filter, uint16_t mag_filter,
                                   uint16_t wrap_s, uint16_t wrap_t)
{
    MiniGLRixTexture *texture = backend ? backend_texture(backend, id) : NULL;

    if (texture != NULL) {
        texture->min_filter = min_filter;
        texture->mag_filter = mag_filter;
        texture->wrap_s = wrap_s;
        texture->wrap_t = wrap_t;
    }
}

void minigl_rix_backend_delete_texture(MiniGLRixBackend *backend, uint32_t id)
{
    MiniGLRixTexture *texture = backend ? backend_texture(backend, id) : NULL;

    if (texture != NULL) {
        texture->deleted = 1U;
        if (backend->bound_texture == id)
            backend->bound_texture = 0U;
    }
}

int minigl_rix_backend_upload_texture(MiniGLRixBackend *backend, uint32_t id,
                                      uint16_t level, const uint32_t *rgba,
                                      uint16_t width, uint16_t height,
                                      uint16_t format)
{
    static uint32_t scaled[256U * 256U];
    MiniGLRixTexture *texture;
    uint16_t out_width;
    uint16_t out_height;
    uint32_t source_x;
    uint32_t source_y;
    uint32_t offset;
    uint32_t bytes;

    if (backend == NULL || rgba == NULL || width == 0U || height == 0U ||
        backend->platform.texture_memory == NULL ||
        backend_texture_slots(backend) == 0U)
        return 0;
    texture = backend_texture(backend, id);
    if (texture != NULL && texture->pin_count != 0U &&
        !minigl_rix_backend_drain(backend))
        return 0;
    if (level == 0U) {
        out_width = texture_dimension(width);
        out_height = texture_dimension(height);
        if (texture == NULL)
            texture = backend_alloc_texture(backend, id);
        if (texture == NULL)
            return 0;
        texture->width = out_width;
        texture->height = out_height;
        texture->width_log2 = texture_log2(out_width);
        texture->height_log2 = texture_log2(out_height);
        texture->required_levels =
            texture_required_levels(texture->width_log2, texture->height_log2);
        texture->levels = 0U;
        texture->format = (format & ZZ9K_MGL_TEX_INTERNAL_RGB) != 0U ?
                          RIX_TEXTURE_RGB565 : RIX_TEXTURE_RGBA4444;
        /* RGBA4444 drops four bits per channel; RGB565 drops three or two
         * and matches the target surface exactly.  Fully opaque RGBA data
         * loses nothing by storing RGB565: REPLACE/MODULATE alpha stays 1.0
         * either way. */
        if (texture->format == RIX_TEXTURE_RGBA4444) {
            size_t texel;

            for (texel = 0U; texel < (size_t)width * height; ++texel) {
                if ((rgba[texel] & 0xffU) != 0xffU)
                    break;
            }
            if (texel == (size_t)width * height)
                texture->format = RIX_TEXTURE_RGB565;
        }
        memset(backend->platform.texture_memory + texture->offset, 0,
               MINIGL_RIX_TEXTURE_VIRTUAL_BYTES);
    } else {
        if (texture == NULL || level >= 16U ||
            (texture->required_levels & (uint16_t)(1U << level)) == 0U)
            return 0;
        out_width = texture->width >> level;
        out_height = texture->height >> level;
        if (out_width == 0U) out_width = 1U;
        if (out_height == 0U) out_height = 1U;
    }
    for (source_y = 0U; source_y < out_height; ++source_y)
        for (source_x = 0U; source_x < out_width; ++source_x)
            scaled[source_y * out_width + source_x] =
                rgba[((uint32_t)source_y * height / out_height) * width +
                     ((uint32_t)source_x * width / out_width)];
    offset = rix_texture_mip_offset(texture->width_log2, texture->height_log2,
                                    (uint8_t)level);
    bytes = (uint32_t)out_width * out_height * 2U;
    if (offset == UINT32_MAX || offset > MINIGL_RIX_TEXTURE_VIRTUAL_BYTES / 2U ||
        bytes > MINIGL_RIX_TEXTURE_VIRTUAL_BYTES - offset * 2U ||
        !rix_texture_convert_rgba8_le(backend->platform.texture_memory +
                                       texture->offset + offset * 2U, bytes, scaled,
                                       (size_t)out_width * out_height, texture->format))
        return 0;
    if (backend->platform.cache_clean != NULL)
        backend->platform.cache_clean(backend->platform.context,
                                      (uintptr_t)(backend->platform.texture_memory +
                                                  texture->offset + offset * 2U),
                                      bytes);
    texture->levels |= (uint16_t)(1U << level);
    ++texture->incarnation;
    if (texture->incarnation == 0U)
        ++texture->incarnation;
    return 1;
}

static void backend_release_buffer(MiniGLRixBackend *backend, uint8_t index)
{
    size_t texture_index;

    for (texture_index = 0U; texture_index < MINIGL_RIX_MAX_TEXTURES; ++texture_index) {
        MiniGLRixTexture *texture = &backend->textures[texture_index];

        if (backend->pending_pins[index][texture_index] != 0U &&
            texture->valid && texture->pin_count != 0U) {
            --texture->pin_count;
            if (texture->pin_count == 0U)
                texture->last_used_fence = 0U;
        }
        backend->pending_pins[index][texture_index] = 0U;
    }
}


static int backend_wait_buffer(MiniGLRixBackend *backend, uint8_t index)
{
    int result;

    if (backend->pending_fence[index] == 0U)
        return 1;
    result = rix_drv_poll(&backend->driver, backend->pending_fence[index], RIX_TIMEOUT);
    if (result != RIX_DRV_OK) {
        backend->error = 1U;
        RIX_LOG("[RasterIX] backend disabled: poll error %d (status=0x%08lx err_status=0x%08lx err_addr=0x%08lx)\r\n",
                result,
                (unsigned long)backend->driver.last_status,
                (unsigned long)backend->driver.last_error_status,
                (unsigned long)backend->driver.last_error_address);
        return 0;
    }
    backend->pending_fence[index] = 0U;
    backend_release_buffer(backend, index);
    return 1;
}

static int backend_emit_state(MiniGLRixBackend *backend, RixCmdWriter *writer,
                              const ZZ9KMiniGLPacketSetState *state,
                              uint32_t bound_texture, uint32_t disabled_features)
{
    RixFragmentState fragment;
    RixTexEnvState environment;
    RixTextureState texture;
    uint32_t features;
    uint32_t pages[MINIGL_RIX_TEXTURE_PAGES];
    size_t page_count;
    size_t page_index;
    MiniGLRixTexture *image = NULL;

    if (!backend_maps_state(state, &fragment, &environment, &features))
        return 0;
    features &= ~disabled_features;
    memset(&texture, 0, sizeof(texture));
    if ((state->enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_0) != 0U) {
        image = backend_recorded_texture(backend, bound_texture);
        if (image == NULL || !texture_filter_supported(image) ||
            (mip_filter(image->min_filter) && image->levels != image->required_levels))
            return 0;
        texture.width_log2 = image->width_log2;
        texture.height_log2 = image->height_log2;
        texture.pixel_format = image->format;
        texture.clamp_s = image->wrap_s != GL_REPEAT;
        texture.clamp_t = image->wrap_t != GL_REPEAT;
        texture.mag_linear = image->mag_filter == GL_LINEAR;
        /* RasterIX exposes one minification bit: it enables its discrete LOD
         * calculator for every GL mipmap mode. */
        texture.min_linear = mip_filter(image->min_filter);
        page_count = MINIGL_RIX_TEXTURE_PAGES;
        for (page_index = 0U; page_index < page_count; ++page_index)
            pages[page_index] = (uint32_t)(backend->platform.texture_address +
                image->offset + page_index * MINIGL_RIX_TEXTURE_PAGE_BYTES);
    } else {
        texture.width_log2 = 3U;
        texture.height_log2 = 3U;
        texture.clamp_s = 1U;
        texture.clamp_t = 1U;
        texture.pixel_format = RIX_TEXTURE_RGB565;
        pages[0] = (uint32_t)backend->platform.white_texture_address;
        page_count = 1U;
    }
    if (pages[0] == 0U || !rix_cmd_feature_enable(writer, features) ||
        !rix_cmd_fragment_state(writer, &fragment) ||
        !rix_cmd_texture_state(writer, 0U, &texture) ||
        !rix_cmd_tex_env(writer, 0U, &environment) ||
        !rix_cmd_tex_env_color(writer, 0U, state->tex_env_color[0]) ||
        !rix_cmd_texture_pages(writer, 0U, pages, page_count)) {
        return 0;
    }
    return 1;
}

static int backend_submit(MiniGLRixBackend *backend, RixCmdWriter *writer)
{
    uint8_t index = backend->current_buffer;
    uint8_t *buffer = backend->platform.command_buffer[index];
    uint32_t raw_bytes;
    int result;

    if (writer->failed || writer->length == 0U || writer->length > 0x00ffffffU ||
        buffer == NULL || backend->platform.command_address[index] == 0U) {
        return 0;
    }
    raw_bytes = (uint32_t)writer->length;
    ((uint32_t *)buffer)[0] = 0x90000000U | raw_bytes;
    ((uint32_t *)buffer)[1] = 0U;
    result = rix_drv_submit(&backend->driver, backend->platform.command_address[index],
                            (uint32_t)((raw_bytes + 8U) / 4U), RIX_TIMEOUT);
    if (result != RIX_DRV_OK) {
        backend->error = 1U;
        RIX_LOG("[RasterIX] backend disabled: submit error %d (status=0x%08lx err_status=0x%08lx err_addr=0x%08lx)\r\n",
                result,
                (unsigned long)backend->driver.last_status,
                (unsigned long)backend->driver.last_error_status,
                (unsigned long)backend->driver.last_error_address);
        return 0;
    }
    backend->pending_fence[index] = backend->driver.submitted_target;
    memcpy(backend->pending_pins[index], backend->recorded_pins,
           sizeof(backend->pending_pins[index]));
    memset(backend->recorded_pins, 0, sizeof(backend->recorded_pins));
    backend->next_buffer = (uint8_t)((index + 1U) % MINIGL_RIX_COMMAND_BUFFERS);
    backend->surface_rgb565 = 1U;
    return 1;
}

int minigl_rix_backend_probe_platform(const MiniGLRixPlatform *platform)
{
    static int logged = 0;
    uint32_t mntz_id;
    uint32_t csr_id;
    uint32_t csr_ver;

    if (platform == NULL || platform->mntzorro_read == NULL ||
        platform->csr_read == NULL || platform->csr_write == NULL) {
        if (!logged) {
            RIX_LOG("[RasterIX] discovery: CPU fallback (platform unavailable)\r\n");
            logged = 1;
        }
        return 0;
    }
    mntz_id = platform->mntzorro_read(platform->context,
                                      MINIGL_RIX_MNTZORRO_IDENTITY_OFFSET);
    if (mntz_id != RIX_DRV_IDENTITY_VALUE) {
        if (!logged) {
            RIX_LOG("[RasterIX] discovery: CPU fallback (MNTZorro=0x%08lx)\r\n",
                    (unsigned long)mntz_id);
            logged = 1;
        }
        return 0;
    }
    csr_id = platform->csr_read(platform->context, RIX_DRV_CSR_IDENTITY);
    csr_ver = platform->csr_read(platform->context, RIX_DRV_CSR_VERSION);
    if (csr_id != RIX_DRV_IDENTITY_VALUE || csr_ver != RIX_DRV_VERSION_VALUE) {
        if (!logged) {
            RIX_LOG("[RasterIX] discovery: CPU fallback (CSR id=0x%08lx ver=0x%08lx)\r\n",
                    (unsigned long)csr_id, (unsigned long)csr_ver);
            logged = 1;
        }
        return 0;
    }
    if (!logged) {
        RIX_LOG("[RasterIX] discovery: RasterIX ABI 0x%08lx (TMU=1, band=16k)\r\n",
                (unsigned long)csr_ver);
        logged = 1;
    }
    return 1;
}

void minigl_rix_backend_init(MiniGLRixBackend *backend,
                             const MiniGLRixPlatform *platform)
{
    struct rix_drv_config config;

    if (backend == NULL)
        return;
    memset(backend, 0, sizeof(*backend));
    if (!minigl_rix_backend_probe_platform(platform))
        return;
    backend->platform = *platform;
    memset(&config, 0, sizeof(config));
    config.context = platform->context;
    config.read = platform->csr_read;
    config.write = platform->csr_write;
    config.clean_dcache = platform->cache_clean;
    config.hardware_present = 1;
    if (rix_drv_init(&backend->driver, &config) != RIX_DRV_OK ||
        rix_drv_enable(&backend->driver) != RIX_DRV_OK) {
        RIX_LOG("[RasterIX] backend enable failed (status=0x%08lx)\r\n",
                (unsigned long)backend->driver.last_status);
        return;
    }
    backend_fill_white_texture(backend);
    backend->available = 1U;
}

void minigl_rix_backend_reset(MiniGLRixBackend *backend)
{
    if (backend == NULL)
        return;
    (void)minigl_rix_backend_drain(backend);
    backend->collecting = 0U;
    backend->frame_active = 0U;
    backend->surface_rgb565 = 0U;
    backend->triangle_count = 0U;
    backend->op_count = 0U;
}

int minigl_rix_backend_available(const MiniGLRixBackend *backend)
{
    return backend != NULL && backend->available && !backend->error && !backend->force_cpu;
}

void minigl_rix_backend_set_state(MiniGLRixBackend *backend,
                                  const ZZ9KMiniGLPacketSetState *state)
{
    if (backend == NULL || state == NULL)
        return;
    backend->state = *state;
    backend->force_cpu = (state->reserved & ZZ9K_MGL_STATE_FLAG_FORCE_CPU) != 0U;
}

static int backend_start_frame(MiniGLRixBackend *backend, uint16_t width,
                               uint16_t height, uint32_t color_address,
                               uint32_t depth_address)
{
    backend->width = width;
    backend->height = height;
    backend->color_address = color_address;
    backend->depth_address = depth_address;
    backend->triangle_count = 0U;
    backend->op_count = 0U;
    backend->frame_active = 1U;
    backend->needs_color_read = 1U;
    backend->needs_depth_read = 1U;
    if (!rix_band_list_init(&backend->bands, width, height,
                            backend->references, MINIGL_RIX_MAX_TRIANGLES))
        return 0;
    backend->command_words_estimate = 2U + (size_t)backend->bands.count * 8U;
    return 1;
}

static int backend_emit_triangles(RixCmdWriter *writer, const MiniGLRixBackend *backend,
                                  const MiniGLRixRecordedOp *op, const RixBand *band)
{
    size_t index;

    for (index = 0U; index < op->triangle_count; ++index) {
        RixTriangle triangle = backend->triangles[op->first_triangle + index];

        if (!rix_triangle_in_band(&triangle, band->y_start, band->y_end))
            continue;
        if (!rix_triangle_increment(&triangle, band->y_start, band->y_end) ||
            !rix_cmd_triangle(writer, triangle.words, RIX_TRIANGLE_WORDS))
            return 0;
    }
    return 1;
}

static int backend_emit_frame(MiniGLRixBackend *backend, RixCmdWriter *writer,
                              int continue_frame)
{
    uint16_t band_index;

    /* One RasterIX_IF band per iteration: band-local resolution, Y offset,
     * buffer addresses, and transfer sizes come from rix_band_setup(). */
    for (band_index = 0U; band_index < backend->bands.count; ++band_index) {
        const RixBand *band = &backend->bands.band[band_index];
        RixBandConfig config;
        RixBandSetup setup;
        size_t op_index;
        uint32_t read_flags = 0U;

        memset(&config, 0, sizeof(config));
        config.width = backend->width;
        config.height = backend->height;
        config.color_addr = backend->color_address;
        config.depth_addr = backend->depth_address;
        config.color_pitch = (uint32_t)backend->width * 2U;
        config.depth_pitch = (uint32_t)backend->width * 2U;
        rix_band_setup(&setup, &backend->bands, &config, band_index);
        if (!rix_band_emit_header(writer, &setup))
            return 0;
        if (backend->needs_color_read)
            read_flags |= RIX_FRAMEBUFFER_COLOR;
        if (backend->needs_depth_read)
            read_flags |= RIX_FRAMEBUFFER_DEPTH;
        if (read_flags != 0U &&
            !rix_cmd_framebuffer(writer, RIX_FRAMEBUFFER_READ | read_flags, setup.pixels))
            return 0;
        for (op_index = 0U; op_index < backend->op_count; ++op_index) {
            const MiniGLRixRecordedOp *op = &backend->ops[op_index];

            if (op->type == MINIGL_RIX_OP_CLEAR) {
                uint32_t clear_flags = 0U;

                if (!backend_emit_state(backend, writer, &op->state, op->texture_id, 0U))
                    return 0;
                if (op->mask & GL_COLOR_BUFFER_BIT)
                    clear_flags |= RIX_FRAMEBUFFER_COLOR;
                if (op->mask & GL_DEPTH_BUFFER_BIT)
                    clear_flags |= RIX_FRAMEBUFFER_DEPTH;
                if (!rix_cmd_clear_color(writer, op->color) ||
                    !rix_cmd_clear_depth(writer, op->depth > 0xffffU ? (uint16_t)(op->depth >> 8U) : (uint16_t)op->depth) ||
                    !rix_cmd_framebuffer(writer, RIX_FRAMEBUFFER_MEMSET | clear_flags,
                                         setup.pixels))
                    return 0;
            } else if (!backend_emit_state(backend, writer, &op->state, op->texture_id, 0U) ||
                       !backend_emit_triangles(writer, backend, op, band)) {
                return 0;
            }
        }
        if (!rix_cmd_framebuffer(writer, RIX_FRAMEBUFFER_COMMIT | RIX_FRAMEBUFFER_COLOR |
                                 (continue_frame ? RIX_FRAMEBUFFER_DEPTH : 0U), setup.pixels))
            return 0;
    }
    return !writer->failed;
}

static int backend_flush(MiniGLRixBackend *backend, int continue_frame)
{
    RixCmdWriter writer;
    uint8_t *buffer;

    if (!backend->frame_active || backend->op_count == 0U)
        return 1;
    if (!backend_wait_buffer(backend, backend->next_buffer))
        return 0;
    backend->current_buffer = backend->next_buffer;
    buffer = backend->platform.command_buffer[backend->current_buffer];
    if (buffer == NULL || backend->platform.command_capacity <= 8U)
        return 0;
    rix_cmd_init(&writer, buffer + 8U, backend->platform.command_capacity - 8U);
    if (!backend_emit_frame(backend, &writer, continue_frame) ||
        !backend_submit(backend, &writer))
        return 0;
    backend->triangle_count = 0U;
    backend->op_count = 0U;
    backend->needs_color_read = 1U;
    backend->needs_depth_read = 1U;
    backend->command_words_estimate = continue_frame ?
        2U + (size_t)backend->bands.count * 8U : 0U;
    if (!continue_frame)
        backend->frame_active = 0U;
    return 1;
}

static int backend_record_draw(MiniGLRixBackend *backend)
{
    MiniGLRixRecordedOp *op;
    MiniGLRixTexture *texture;

    if (backend->op_count == MINIGL_RIX_MAX_OPS)
        return 0;
    op = &backend->ops[backend->op_count];
    memset(op, 0, sizeof(*op));
    op->type = MINIGL_RIX_OP_DRAW;
    op->state = backend->state;
    op->texture_id = backend->bound_texture;
    op->first_triangle = backend->triangle_count;
    if ((op->state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_0) != 0U) {
        texture = backend_texture(backend, op->texture_id);
        if (texture == NULL || !texture_filter_supported(texture) ||
            (mip_filter(texture->min_filter) && texture->levels != texture->required_levels))
            return 0;
        if (backend->recorded_pins[texture - backend->textures] == 0U) {
            backend->recorded_pins[texture - backend->textures] = 1U;
            ++texture->pin_count;
        }
    }
    ++backend->op_count;
    backend->command_words_estimate += (size_t)backend->bands.count * 16U;
    return 1;
}

int minigl_rix_backend_begin_draw(MiniGLRixBackend *backend, uint16_t width,
                                  uint16_t height, uint32_t color_address,
                                  uint32_t depth_address)
{
    RixFragmentState fragment;
    RixTexEnvState environment;
    uint32_t features;

    if (!minigl_rix_backend_available(backend) || width == 0U || height == 0U ||
        !backend_maps_state(&backend->state, &fragment, &environment, &features))
        return 0;
    if (backend->frame_active &&
        (backend->width != width || backend->height != height ||
         backend->color_address != color_address || backend->depth_address != depth_address) &&
        !backend_flush(backend, 0))
        return 0;
    if (!backend->frame_active &&
        !backend_start_frame(backend, width, height, color_address, depth_address))
        return 0;
    if (backend->op_count == MINIGL_RIX_MAX_OPS &&
        !backend_flush(backend, 1))
        return 0;
    if (!backend_record_draw(backend))
        return 0;
    backend->collecting = 1U;
    return 1;
}

int minigl_rix_backend_add_triangle(MiniGLRixBackend *backend,
                                    const MiniGLRixVertex *a,
                                    const MiniGLRixVertex *b,
                                    const MiniGLRixVertex *c)
{
    RixVertex vertices[3];
    RixScissor scissor;
    RixTriangle prepared;
    RixTriangle *triangle;
    MiniGLRixRecordedOp *op;
    size_t band_index;
    size_t touched_bands = 0U;

    if (backend == NULL || !backend->collecting || a == NULL || b == NULL || c == NULL)
        return 0;
    /* RasterIX descriptors use the OpenGL bottom-up y axis; the raster
     * service delivers top-down screen rows, so reflect y once here. */
    {
        float za = a->z;
        float zb = b->z;
        float zc = c->z;
        if (backend->state.depth_near != 0U || backend->state.depth_far != 0x00ffffffU) {
            float near_z = (float)backend->state.depth_near / 16777215.0f;
            float scale_z = ((float)backend->state.depth_far - (float)backend->state.depth_near) / 16777215.0f;
            za = near_z + za * scale_z;
            zb = near_z + zb * scale_z;
            zc = near_z + zc * scale_z;
        }
        vertices[0] = (RixVertex){a->x, (float)backend->height - a->y, za, a->reciprocal_w,
                                  a->rgba, a->s, a->t, a->q};
        vertices[1] = (RixVertex){b->x, (float)backend->height - b->y, zb, b->reciprocal_w,
                                  b->rgba, b->s, b->t, b->q};
        vertices[2] = (RixVertex){c->x, (float)backend->height - c->y, zc, c->reciprocal_w,
                                  c->rgba, c->s, c->t, c->q};
    }
    scissor.start_x = 0U;
    scissor.start_y = 0U;
    scissor.end_x = backend->width;
    scissor.end_y = backend->height;
    if ((backend->state.enabled_flags & ZZ9K_MGL_STATE_SCISSOR_TEST) != 0U) {
        int32_t bottom = (int32_t)backend->height - backend->state.scissor_y -
                         backend->state.scissor_h;

        if (backend->state.scissor_x < 0 || bottom < 0 ||
            (uint32_t)backend->state.scissor_x + backend->state.scissor_w > backend->width ||
            (uint32_t)bottom + backend->state.scissor_h > backend->height) {
            return 0;
        }
        scissor.start_x = (uint16_t)backend->state.scissor_x;
        scissor.start_y = (uint16_t)bottom;
        scissor.end_x = (uint16_t)((uint32_t)backend->state.scissor_x +
                                   backend->state.scissor_w);
        scissor.end_y = (uint16_t)((uint32_t)bottom + backend->state.scissor_h);
    }
    scissor.enabled = 1U;
    if (!rix_setup_triangle(&prepared, &vertices[0], &vertices[1], &vertices[2], &scissor))
        return 0;
    for (band_index = 0U; band_index < backend->bands.count; ++band_index)
        if (rix_triangle_in_band(&prepared, backend->bands.band[band_index].y_start,
                                 backend->bands.band[band_index].y_end))
            ++touched_bands;
    if (backend->triangle_count == MINIGL_RIX_MAX_TRIANGLES ||
        backend->command_words_estimate +
            touched_bands * (RIX_TRIANGLE_WORDS + 1U) > MINIGL_RIX_COMMAND_WORD_BUDGET) {
        if (!backend_flush(backend, 1) || !backend_record_draw(backend))
            return 0;
    }
    op = &backend->ops[backend->op_count - 1U];
    triangle = &backend->triangles[backend->triangle_count];
    *triangle = prepared;
    if (!rix_band_list_add(&backend->bands, triangle))
        return 0;
    ++backend->triangle_count;
    ++op->triangle_count;
    backend->command_words_estimate += touched_bands * (RIX_TRIANGLE_WORDS + 1U);
    return 1;
}

int minigl_rix_backend_end_draw(MiniGLRixBackend *backend, uint32_t color_address,
                                uint32_t depth_address)
{
    if (backend == NULL || !backend->collecting ||
        color_address != backend->color_address || depth_address != backend->depth_address)
        return 0;
    backend->collecting = 0U;
    return 1;
}

int minigl_rix_backend_clear(MiniGLRixBackend *backend, uint16_t width,
                             uint16_t height, uint32_t color_address,
                             uint32_t depth_address, uint32_t mask,
                             uint32_t rgba, uint32_t depth)
{
    MiniGLRixRecordedOp *op;
    RixFragmentState fragment;
    RixTexEnvState environment;
    uint32_t features;

    if (!minigl_rix_backend_available(backend) || width == 0U || height == 0U ||
        !backend_maps_state(&backend->state, &fragment, &environment, &features) ||
        (mask & ~(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)) != 0U ||
        mask == 0U || (backend->state.enabled_flags & ZZ9K_MGL_STATE_SCISSOR_TEST) != 0U ||
        ((mask & GL_COLOR_BUFFER_BIT) != 0U && (backend->state.color_mask & 0xfU) != 0xfU))
        return 0;
    if (backend->frame_active &&
        (backend->width != width || backend->height != height ||
         backend->color_address != color_address || backend->depth_address != depth_address) &&
        !backend_flush(backend, 0))
        return 0;
    if (!backend->frame_active &&
        !backend_start_frame(backend, width, height, color_address, depth_address))
        return 0;
    if (backend->op_count == MINIGL_RIX_MAX_OPS && !backend_flush(backend, 1))
        return 0;
    if (backend->op_count == 0U) {
        if (mask & GL_COLOR_BUFFER_BIT) backend->needs_color_read = 0U;
        if (mask & GL_DEPTH_BUFFER_BIT) backend->needs_depth_read = 0U;
    }
    op = &backend->ops[backend->op_count++];
    memset(op, 0, sizeof(*op));
    op->type = MINIGL_RIX_OP_CLEAR;
    op->state = backend->state;
    op->texture_id = backend->bound_texture;
    op->mask = mask;
    op->color = rgba;
    op->depth = depth;
    return 1;
}
int minigl_rix_backend_prepare_target(MiniGLRixBackend *backend,
                                      uint32_t *rgba, size_t pixels)
{
    size_t index;
    uint16_t *destination = (uint16_t *)rgba;

    if (backend == NULL || rgba == NULL)
        return 0;
    if (backend->surface_rgb565)
        return 1;
    for (index = 0U; index < pixels; ++index) {
        uint32_t value = rgba[index];
        destination[index] = (uint16_t)(((value >> 16U) & 0xf800U) |
                                        ((value >> 13U) & 0x07e0U) |
                                        ((value >> 11U) & 0x001fU));
    }
    backend->surface_rgb565 = 1U;
    return 1;
}

int minigl_rix_backend_make_cpu_visible(MiniGLRixBackend *backend,
                                        uint32_t *rgba, size_t pixels)
{
    size_t index;
    const uint16_t *source = (const uint16_t *)rgba;

    if (backend == NULL || rgba == NULL)
        return 0;
    if (!backend->surface_rgb565)
        return 1;
    if (!minigl_rix_backend_drain(backend))
        return 0;
    if (backend->platform.cache_invalidate != NULL)
        backend->platform.cache_invalidate(backend->platform.context,
                                           (uintptr_t)rgba, pixels * 2U);
    for (index = pixels; index != 0U; --index) {
        uint16_t value = source[index - 1U];
        uint32_t red = (uint32_t)((value >> 11U) & 0x1fU);
        uint32_t green = (uint32_t)((value >> 5U) & 0x3fU);
        uint32_t blue = (uint32_t)(value & 0x1fU);
        rgba[index - 1U] = (red << 27U) | (red << 22U) |
                            (green << 18U) | (green << 12U) |
                            (blue << 8U) | (blue << 3U) | 0xffU;
    }
    backend->surface_rgb565 = 0U;
    return 1;
}

int minigl_rix_backend_surface_is_rgb565(const MiniGLRixBackend *backend)
{
    return backend != NULL && backend->surface_rgb565;
}


int minigl_rix_backend_drain(MiniGLRixBackend *backend)
{
    uint8_t index;

    if (backend == NULL || !backend->available)
        return 1;
    if (backend->collecting || !backend_flush(backend, 0))
        return 0;
    for (index = 0U; index < MINIGL_RIX_COMMAND_BUFFERS; ++index)
        if (!backend_wait_buffer(backend, index)) return 0;
    for (index = 0U; index < MINIGL_RIX_MAX_TEXTURES; ++index)
        backend->textures[index].last_used_fence = 0U;
    return 1;
}
const char *minigl_rix_backend_fallback_name(uint32_t reason)
{
    switch (reason) {
    case ZZ9K_MGL_FALLBACK_NONE: return "none";
    case ZZ9K_MGL_FALLBACK_DEPTH_BITS: return "depth bits";
    case ZZ9K_MGL_FALLBACK_FOG: return "fog";
    case ZZ9K_MGL_FALLBACK_MULTITEXTURE: return "multitexture";
    case ZZ9K_MGL_FALLBACK_POLYGON_OFFSET: return "polygon offset";
    case ZZ9K_MGL_FALLBACK_POLYGON_MODE: return "polygon mode";
    case ZZ9K_MGL_FALLBACK_BLEND_EQUATION: return "blend equation";
    case ZZ9K_MGL_FALLBACK_TEXTURE_FILTER: return "texture filter";
    case ZZ9K_MGL_FALLBACK_RESIDENCY_EXHAUSTED: return "residency exhausted";
    case ZZ9K_MGL_FALLBACK_BACKEND_DISABLED_OR_ABSENT: return "backend disabled/absent";
    case ZZ9K_MGL_FALLBACK_FORCED_CPU: return "forced CPU";
    default: return "unknown";
    }
}

uint32_t minigl_rix_backend_eval_fallback(const MiniGLRixBackend *backend)
{
    if (backend != NULL && backend->force_cpu)
        return ZZ9K_MGL_FALLBACK_FORCED_CPU;
    if (backend == NULL || !backend->available || backend->error)
        return ZZ9K_MGL_FALLBACK_BACKEND_DISABLED_OR_ABSENT;
    if (backend->state.depth_bits != 16U && backend->state.depth_bits != 24U)
        return ZZ9K_MGL_FALLBACK_DEPTH_BITS;
    if ((backend->state.enabled_flags & ZZ9K_MGL_STATE_FOG) != 0U)
        return ZZ9K_MGL_FALLBACK_FOG;
    if ((backend->state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_1) != 0U)
        return ZZ9K_MGL_FALLBACK_MULTITEXTURE;
    if ((backend->state.enabled_flags & (ZZ9K_MGL_STATE_POLYGON_OFFSET_FILL |
                                         ZZ9K_MGL_STATE_POLYGON_OFFSET_LINE |
                                         ZZ9K_MGL_STATE_POLYGON_OFFSET_POINT)) != 0U ||
        backend->state.polygon_offset_factor != 0.0f || backend->state.polygon_offset_units != 0.0f)
        return ZZ9K_MGL_FALLBACK_POLYGON_OFFSET;
    if (backend->state.polygon_mode != 0U && backend->state.polygon_mode != GL_FILL)
        return ZZ9K_MGL_FALLBACK_POLYGON_MODE;
    if (backend->state.blend_equation != GL_FUNC_ADD)
        return ZZ9K_MGL_FALLBACK_BLEND_EQUATION;
    if ((backend->state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_0) != 0U) {
        const MiniGLRixTexture *tex = backend_texture((MiniGLRixBackend *)backend, backend->bound_texture);
        if (tex == NULL)
            return ZZ9K_MGL_FALLBACK_RESIDENCY_EXHAUSTED;
        if (!texture_filter_supported(tex) ||
            (mip_filter(tex->min_filter) && tex->levels != tex->required_levels))
            return ZZ9K_MGL_FALLBACK_TEXTURE_FILTER;
    }
    return ZZ9K_MGL_FALLBACK_NONE;
}

void minigl_rix_backend_log_fallback(uint32_t reason)
{
    RIX_LOG("[RasterIX] fallback: %s\r\n", minigl_rix_backend_fallback_name(reason));
}

void minigl_rix_backend_log_context_backend(const MiniGLRixBackend *backend)
{
    if (minigl_rix_backend_available(backend)) {
        RIX_LOG("[RasterIX] context bind: backend=RasterIX\r\n");
    } else if (backend != NULL && backend->force_cpu) {
        RIX_LOG("[RasterIX] context bind: backend=CPU (forced)\r\n");
    } else {
        RIX_LOG("[RasterIX] context bind: backend=CPU\r\n");
    }
}
