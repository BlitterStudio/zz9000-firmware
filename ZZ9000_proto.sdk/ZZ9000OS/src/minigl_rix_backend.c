/*
 * MiniGL RasterIX backend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "minigl_rix_backend.h"
#include "rix_texture.h"

#if !defined(MINIGL_HOST_TEST) && !defined(MINIGL_SERVICE_HOST_TEST)
#include "xil_io.h"
#include "xil_printf.h"
#define RIX_LOG(...) xil_printf(__VA_ARGS__)
#define RIX_AFI_DIAGNOSTICS 1
#else
void (*g_rix_test_log)(const char *fmt, ...) = NULL;
#define RIX_LOG(...) do { if (g_rix_test_log) g_rix_test_log(__VA_ARGS__); } while (0)
#define RIX_AFI_DIAGNOSTICS 0
#endif

#include <string.h>

#define RIX_TIMEOUT 1000000U
#define RIX_AFI0_BASE 0xf8008000U
#define RIX_AFI1_BASE 0xf8009000U
#define RIX_AFI_RD_CTRL 0x00U
#define RIX_AFI_RD_ISSUING_CAP 0x04U
#define RIX_AFI_RD_QOS 0x08U
#define RIX_AFI_RD_DATAFIFO_LEVEL 0x0cU
#define RIX_AFI_RD_DEBUG 0x10U
#define RIX_AFI_WR_CTRL 0x14U
#define RIX_AFI_WR_ISSUING_CAP 0x18U
#define RIX_AFI_WR_QOS 0x1cU
#define RIX_AFI_WR_DATAFIFO_LEVEL 0x20U
#define RIX_AFI_WR_DEBUG 0x24U
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

    /* RasterIX always stores depth as Z16.  MiniGL's 24/32-bit requests
     * are accepted with that documented precision on this backend. */
    if (state == NULL || fragment == NULL || environment == NULL || features == NULL ||
        (state->depth_bits != 16U && state->depth_bits != 24U &&
         state->depth_bits != 32U) ||
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
    enabled = state->enabled_flags;
    if (!map_compare(state->depth_func, &fragment->depth_func) ||
        ((enabled & ZZ9K_MGL_STATE_ALPHA_TEST) != 0U &&
         !map_compare(state->alpha_func, &fragment->alpha_func)) ||
        !map_blend(state->blend_src, &fragment->src_factor) ||
        !map_blend(state->blend_dst, &fragment->dst_factor)) {
        return 0;
    }
    if ((enabled & ZZ9K_MGL_STATE_ALPHA_TEST) == 0U)
        fragment->alpha_func = RIX_FUNC_ALWAYS;
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

static MiniGLRixTexture *backend_recorded_texture(MiniGLRixBackend *backend,
                                                   const MiniGLRixRecordedOp *op)
{
    MiniGLRixTexture *texture;

    if (op->texture_slot >= MINIGL_RIX_MAX_TEXTURES)
        return NULL;
    texture = &backend->textures[op->texture_slot];
    return texture->valid && texture->incarnation == op->texture_incarnation ?
        texture : NULL;
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

static uint16_t texture_page_count(uint8_t width_log2, uint8_t height_log2)
{
    uint8_t level;
    uint8_t last = width_log2 > height_log2 ? width_log2 : height_log2;
    uint32_t bytes = 0U;

    for (level = 0U; level <= last; ++level) {
        uint8_t active_width = 0U;
        uint8_t active_height = 0U;
        uint32_t offset = rix_texture_mip_offset(width_log2, height_log2, level);
        uint32_t width;
        uint32_t height;
        uint32_t end;

        if (level < width_log2)
            active_width = (uint8_t)(width_log2 - level);
        if (level < height_log2)
            active_height = (uint8_t)(height_log2 - level);
        width = 1U << active_width;
        height = 1U << active_height;
        end = offset + width * height;
        if (offset == UINT32_MAX || end < offset || end > UINT32_MAX / 2U)
            return 0U;
        end *= 2U;
        if (end > bytes)
            bytes = end;
    }
    return (uint16_t)((bytes + MINIGL_RIX_TEXTURE_PAGE_BYTES - 1U) /
                      MINIGL_RIX_TEXTURE_PAGE_BYTES);
}

static void backend_touch_texture(MiniGLRixBackend *backend, MiniGLRixTexture *texture)
{
    ++backend->lru_clock;
    if (backend->lru_clock == 0U)
        ++backend->lru_clock;
    texture->lru_stamp = backend->lru_clock;
}

static void backend_release_texture(MiniGLRixBackend *backend, MiniGLRixTexture *texture)
{
    (void)backend;
    memset(texture, 0, sizeof(*texture));
}

static MiniGLRixTexture *backend_evict_texture(MiniGLRixBackend *backend,
                                                const MiniGLRixTexture *excluded)
{
    MiniGLRixTexture *candidate = NULL;
    size_t index;

    for (index = 0U; index < MINIGL_RIX_MAX_TEXTURES; ++index) {
        MiniGLRixTexture *texture = &backend->textures[index];

        if (texture == excluded || !texture->valid || texture->pin_count != 0U)
            continue;
        if (candidate == NULL || (texture->deleted && !candidate->deleted) ||
            (texture->deleted == candidate->deleted &&
             texture->lru_stamp < candidate->lru_stamp))
            candidate = texture;
    }
    return candidate;
}

static int backend_range_available(const MiniGLRixBackend *backend,
                                   const MiniGLRixTexture *excluded,
                                   uint16_t first_page, uint16_t page_count)
{
    uint32_t end_page = (uint32_t)first_page + page_count;
    size_t index;

    for (index = 0U; index < MINIGL_RIX_MAX_TEXTURES; ++index) {
        const MiniGLRixTexture *texture = &backend->textures[index];
        uint32_t texture_end;

        if (texture == excluded || !texture->valid)
            continue;
        texture_end = (uint32_t)texture->first_page + texture->page_count;
        if (first_page < texture_end && texture->first_page < end_page)
            return 0;
    }
    return 1;
}

static int backend_grow_texture_pool(MiniGLRixBackend *backend,
                                     size_t minimum_bytes)
{
    uint8_t *memory;
    uintptr_t address;
    size_t bytes;

    if (backend->platform.grow_texture_pool == NULL ||
        !minigl_rix_backend_drain(backend))
        return 0;
    memory = backend->platform.texture_memory;
    address = backend->platform.texture_address;
    bytes = backend->platform.texture_bytes;
    if (!backend->platform.grow_texture_pool(backend->platform.context,
                                             minimum_bytes, &memory, &address,
                                             &bytes))
        return 0;
    backend->platform.texture_memory = memory;
    backend->platform.texture_address = address;
    backend->platform.texture_bytes = bytes;
    return 1;
}

static int backend_allocate_pages(MiniGLRixBackend *backend, MiniGLRixTexture *texture,
                                  uint16_t page_count)
{
    uint32_t total_pages;
    uint32_t first_page;
    int grew = 0;

    total_pages = (uint32_t)(backend->platform.texture_bytes /
                             MINIGL_RIX_TEXTURE_PAGE_BYTES);
    if (page_count == 0U || page_count > MINIGL_RIX_TEXTURE_PAGES)
        return 0;
    if (page_count > total_pages) {
        if (!backend_grow_texture_pool(backend,
                (size_t)page_count * MINIGL_RIX_TEXTURE_PAGE_BYTES))
            return 0;
        total_pages = (uint32_t)(backend->platform.texture_bytes /
                                 MINIGL_RIX_TEXTURE_PAGE_BYTES);
        if (page_count > total_pages)
            return 0;
        grew = 1;
    }
    for (;;) {
        for (first_page = 0U; first_page <= total_pages - page_count; ++first_page)
            if (backend_range_available(backend, texture, (uint16_t)first_page,
                                        page_count)) {
                texture->first_page = (uint16_t)first_page;
                texture->page_count = page_count;
                return 1;
            }
        if (!grew && backend_grow_texture_pool(backend,
                backend->platform.texture_bytes +
                (size_t)page_count * MINIGL_RIX_TEXTURE_PAGE_BYTES)) {
            total_pages = (uint32_t)(backend->platform.texture_bytes /
                                     MINIGL_RIX_TEXTURE_PAGE_BYTES);
            grew = 1;
            continue;
        }
        {
            MiniGLRixTexture *victim = backend_evict_texture(backend, texture);

            if (victim == NULL)
                return 0;
            backend_release_texture(backend, victim);
        }
    }
}

static MiniGLRixTexture *backend_alloc_texture(MiniGLRixBackend *backend, uint32_t id,
                                                uint16_t page_count)
{
    MiniGLRixTexture *candidate = NULL;
    size_t index;

    for (index = 0U; index < MINIGL_RIX_MAX_TEXTURES; ++index)
        if (!backend->textures[index].valid) {
            candidate = &backend->textures[index];
            break;
        }
    if (candidate == NULL) {
        candidate = backend_evict_texture(backend, NULL);
        if (candidate == NULL)
            return NULL;
        backend_release_texture(backend, candidate);
    }
    memset(candidate, 0, sizeof(*candidate));
    candidate->id = id;
    candidate->valid = 1U;
    ++backend->texture_incarnation_clock;
    if (backend->texture_incarnation_clock == 0U)
        ++backend->texture_incarnation_clock;
    candidate->incarnation = backend->texture_incarnation_clock;
    candidate->min_filter = GL_NEAREST_MIPMAP_LINEAR;
    candidate->mag_filter = GL_LINEAR;
    candidate->wrap_s = GL_REPEAT;
    candidate->wrap_t = GL_REPEAT;
    if (!backend_allocate_pages(backend, candidate, page_count)) {
        memset(candidate, 0, sizeof(*candidate));
        return NULL;
    }
    backend_touch_texture(backend, candidate);
    return candidate;
}

static int backend_write_texture(MiniGLRixBackend *backend, MiniGLRixTexture *texture,
                                 uint32_t offset, const uint32_t *rgba, uint32_t texels)
{
    uint32_t byte_offset = offset * 2U;
    uint32_t written = 0U;

    while (written < texels) {
        uint32_t page = byte_offset / MINIGL_RIX_TEXTURE_PAGE_BYTES;
        uint32_t page_offset = byte_offset % MINIGL_RIX_TEXTURE_PAGE_BYTES;
        uint32_t chunk = (MINIGL_RIX_TEXTURE_PAGE_BYTES - page_offset) / 2U;
        uint8_t *destination;

        if (page >= texture->page_count)
            return 0;
        if (chunk > texels - written)
            chunk = texels - written;
        destination = backend->platform.texture_memory +
            (size_t)(texture->first_page + page) * MINIGL_RIX_TEXTURE_PAGE_BYTES + page_offset;
        if (!rix_texture_convert_rgba8_le(destination, (size_t)chunk * 2U,
                                           rgba + written, chunk, texture->format))
            return 0;
        if (backend->platform.cache_clean != NULL)
            backend->platform.cache_clean(backend->platform.context,
                                          (uintptr_t)destination, (size_t)chunk * 2U);
        written += chunk;
        byte_offset += chunk * 2U;
    }
    return 1;
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

int minigl_rix_backend_texture_resident(const MiniGLRixBackend *backend, uint32_t id)
{
    return backend != NULL &&
           backend_texture((MiniGLRixBackend *)backend, id) != NULL;
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
        if (texture->pin_count == 0U)
            backend_release_texture(backend, texture);
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
        backend->error)
        return 0;
    if (backend->platform.texture_memory == NULL &&
        !backend_grow_texture_pool(backend, MINIGL_RIX_TEXTURE_PAGE_BYTES))
        return 0;
    texture = backend_texture(backend, id);
    if (texture != NULL && texture->pin_count != 0U &&
        !minigl_rix_backend_drain(backend))
        return 0;
    if (level == 0U) {
        uint8_t width_log2;
        uint8_t height_log2;
        uint16_t page_count;
        uint16_t min_filter = GL_NEAREST_MIPMAP_LINEAR;
        uint16_t mag_filter = GL_LINEAR;
        uint16_t wrap_s = GL_REPEAT;
        uint16_t wrap_t = GL_REPEAT;
        if (texture != NULL) {
            min_filter = texture->min_filter;
            mag_filter = texture->mag_filter;
            wrap_s = texture->wrap_s;
            wrap_t = texture->wrap_t;
        }

        out_width = texture_dimension(width);
        out_height = texture_dimension(height);
        width_log2 = texture_log2(out_width);
        height_log2 = texture_log2(out_height);
        page_count = texture_page_count(width_log2, height_log2);
        if (texture != NULL)
            backend_release_texture(backend, texture);
        texture = backend_alloc_texture(backend, id, page_count);
        if (texture == NULL)
            return 0;
        texture->min_filter = min_filter;
        texture->mag_filter = mag_filter;
        texture->wrap_s = wrap_s;
        texture->wrap_t = wrap_t;
        texture->width = out_width;
        texture->height = out_height;
        texture->width_log2 = width_log2;
        texture->height_log2 = height_log2;
        texture->required_levels = texture_required_levels(width_log2, height_log2);
        texture->format = (format & ZZ9K_MGL_TEX_INTERNAL_RGB) != 0U ?
                          RIX_TEXTURE_RGB565 : RIX_TEXTURE_RGBA4444;
        if (texture->format == RIX_TEXTURE_RGBA4444) {
            size_t texel;

            for (texel = 0U; texel < (size_t)width * height; ++texel)
                if ((rgba[texel] & 0xffU) != 0xffU)
                    break;
            if (texel == (size_t)width * height)
                texture->format = RIX_TEXTURE_RGB565;
        }
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
    if (offset == UINT32_MAX || bytes / 2U > UINT32_MAX - offset ||
        !backend_write_texture(backend, texture, offset, scaled, bytes / 2U))
        return 0;
    texture->levels |= (uint16_t)(1U << level);
    backend_touch_texture(backend, texture);
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
            if (texture->pin_count == 0U) {
                texture->last_used_fence = 0U;
                if (texture->deleted)
                    backend_release_texture(backend, texture);
            }
        }
        backend->pending_pins[index][texture_index] = 0U;
    }
}

static uint8_t g_timeout_diagnostics_logged;

#if RIX_AFI_DIAGNOSTICS
static void backend_log_afi_timeout_diagnostics(uint32_t base, const char *name)
{
    RIX_LOG("[RasterIX] timeout %s-r=%08lx/%08lx/%08lx/%08lx/%08lx %s-w=%08lx/%08lx/%08lx/%08lx/%08lx\r\n",
            name, (unsigned long)Xil_In32(base + RIX_AFI_RD_CTRL),
            (unsigned long)Xil_In32(base + RIX_AFI_RD_ISSUING_CAP),
            (unsigned long)Xil_In32(base + RIX_AFI_RD_QOS),
            (unsigned long)Xil_In32(base + RIX_AFI_RD_DATAFIFO_LEVEL),
            (unsigned long)Xil_In32(base + RIX_AFI_RD_DEBUG),
            name, (unsigned long)Xil_In32(base + RIX_AFI_WR_CTRL),
            (unsigned long)Xil_In32(base + RIX_AFI_WR_ISSUING_CAP),
            (unsigned long)Xil_In32(base + RIX_AFI_WR_QOS),
            (unsigned long)Xil_In32(base + RIX_AFI_WR_DATAFIFO_LEVEL),
            (unsigned long)Xil_In32(base + RIX_AFI_WR_DEBUG));
}
#endif

static void backend_log_timeout_diagnostics(MiniGLRixBackend *backend, uint8_t index)
{
    uint32_t csr[23];
    uint32_t words[16] = { 0U };
    uint32_t command_words = backend->submitted_command_words[index];
    size_t word_count = command_words < 16U ? command_words : 16U;
    size_t word_index;
    const uint8_t *buffer = backend->platform.command_buffer[index];

    if (g_timeout_diagnostics_logged)
        return;
    g_timeout_diagnostics_logged = 1U;
    for (word_index = 0U; word_index < 23U; ++word_index)
        csr[word_index] = backend->platform.csr_read(backend->platform.context,
                                                      (uint32_t)word_index * 4U);
    if (buffer != NULL && word_count != 0U) {
        if (backend->platform.cache_invalidate != NULL)
            backend->platform.cache_invalidate(backend->platform.context,
                                               (uintptr_t)buffer,
                                               word_count * sizeof(words[0]));
        memcpy(words, buffer, word_count * sizeof(words[0]));
    }
    RIX_LOG("[RasterIX] timeout snapshot csr=%08lx/%08lx/%08lx/%08lx %08lx/%08lx/%08lx\r\n",
            (unsigned long)csr[0], (unsigned long)csr[1], (unsigned long)csr[2],
            (unsigned long)csr[3], (unsigned long)csr[4], (unsigned long)csr[5],
            (unsigned long)csr[6]);
    RIX_LOG("[RasterIX] timeout csr=%08lx/%08lx/%08lx/%08lx %08lx/%08lx/%08lx\r\n",
            (unsigned long)csr[7], (unsigned long)csr[8], (unsigned long)csr[9],
            (unsigned long)csr[10], (unsigned long)csr[11], (unsigned long)csr[12],
            (unsigned long)csr[13]);
    RIX_LOG("[RasterIX] timeout cmd=%08lx/%08lx color=%08lx depth=%08lx tex=%08lx white=%08lx\r\n",
            (unsigned long)backend->platform.command_address[index],
            (unsigned long)command_words, (unsigned long)backend->color_address,
            (unsigned long)backend->depth_address,
            (unsigned long)backend->platform.texture_address,
            (unsigned long)backend->platform.white_texture_address);
    RIX_LOG("[RasterIX] timeout words=%08lx/%08lx/%08lx/%08lx %08lx/%08lx/%08lx/%08lx\r\n",
            (unsigned long)words[0], (unsigned long)words[1], (unsigned long)words[2],
            (unsigned long)words[3], (unsigned long)words[4], (unsigned long)words[5],
            (unsigned long)words[6], (unsigned long)words[7]);
    RIX_LOG("[RasterIX] timeout words=%08lx/%08lx/%08lx/%08lx %08lx/%08lx/%08lx/%08lx\r\n",
            (unsigned long)words[8], (unsigned long)words[9], (unsigned long)words[10],
            (unsigned long)words[11], (unsigned long)words[12], (unsigned long)words[13],
            (unsigned long)words[14], (unsigned long)words[15]);
    RIX_LOG("[RasterIX] timeout dbg live=%08lx/%08lx ar=%08lx rb=%08lx\r\n",
            (unsigned long)csr[14], (unsigned long)csr[15],
            (unsigned long)csr[16], (unsigned long)csr[17]);
    RIX_LOG("[RasterIX] timeout dbg rl=%08lx car=%08lx crl=%08lx addr=%08lx id=%08lx\r\n",
            (unsigned long)csr[18], (unsigned long)csr[19],
            (unsigned long)csr[20], (unsigned long)csr[21],
            (unsigned long)csr[22]);
#if RIX_AFI_DIAGNOSTICS
    backend_log_afi_timeout_diagnostics(RIX_AFI0_BASE, "afi0");
    backend_log_afi_timeout_diagnostics(RIX_AFI1_BASE, "afi1");
#endif
}

static void backend_disable(MiniGLRixBackend *backend, const char *operation, int result)
{
    size_t index;

    if (backend->error)
        return;
    backend->error = 1U;
    RIX_LOG("[RasterIX] backend disabled: %s error %d (status=0x%08lx err_status=0x%08lx err_addr=0x%08lx)\r\n",
            operation, result, (unsigned long)backend->driver.last_status,
            (unsigned long)backend->driver.last_error_status,
            (unsigned long)backend->driver.last_error_address);
    for (index = 0U; index < MINIGL_RIX_COMMAND_BUFFERS; ++index) {
        backend->pending[index] = 0U;
        backend->pending_fence[index] = 0U;
        backend_release_buffer(backend, index);
    }
    for (index = 0U; index < MINIGL_RIX_MAX_TEXTURES; ++index)
        if (backend->recorded_pins[index] != 0U && backend->textures[index].pin_count != 0U)
            --backend->textures[index].pin_count;
    memset(backend->recorded_pins, 0, sizeof(backend->recorded_pins));
    backend->collecting = 0U;
    backend->frame_active = 0U;
    backend->triangle_count = 0U;
    backend->op_count = 0U;
    backend->surface_rgb565 = 0U;
}

static int backend_wait_buffer(MiniGLRixBackend *backend, uint8_t index)
{
    int result;

    if (backend->error)
        return 0;
    if (!backend->pending[index])
        return 1;
    result = rix_drv_poll(&backend->driver, backend->pending_fence[index], RIX_TIMEOUT);
    if (result != RIX_DRV_OK) {
        if (result == RIX_DRV_TIMEOUT)
            backend_log_timeout_diagnostics(backend, index);
        backend_disable(backend, "poll", result);
        return 0;
    }
    backend->pending[index] = 0U;
    backend->pending_fence[index] = 0U;
    backend->submitted_command_words[index] = 0U;
    backend_release_buffer(backend, index);
    return 1;
}

static int backend_emit_state(MiniGLRixBackend *backend, RixCmdWriter *writer,
                              const MiniGLRixRecordedOp *op,
                              uint32_t disabled_features)
{
    ZZ9KMiniGLPacketSetState state = op->state;
    RixFragmentState fragment;
    RixTexEnvState environment;
    RixTextureState texture;
    MiniGLRixTexture sampled;
    MiniGLRixTexture *image = NULL;
    uint32_t features;
    uint32_t pages[MINIGL_RIX_TEXTURE_PAGES] = { 0U };
    size_t page_count;
    size_t page_index;
    int textured;

    textured = (state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_0) != 0U &&
               op->texture_slot != UINT16_MAX;
    if (!textured)
        state.enabled_flags &= ~ZZ9K_MGL_STATE_TEXTURE_2D_0;
    if (!backend_maps_state(&state, &fragment, &environment, &features))
        return 0;
    features &= ~disabled_features;
    memset(&texture, 0, sizeof(texture));
    if (textured) {
        image = backend_recorded_texture(backend, op);
        if (image == NULL)
            return 0;
        sampled = *image;
        sampled.min_filter = op->min_filter;
        sampled.mag_filter = op->mag_filter;
        sampled.wrap_s = op->wrap_s;
        sampled.wrap_t = op->wrap_t;
        if (!texture_filter_supported(&sampled) ||
            (mip_filter(sampled.min_filter) && sampled.levels != sampled.required_levels))
            return 0;
        texture.width_log2 = sampled.width_log2;
        texture.height_log2 = sampled.height_log2;
        texture.pixel_format = sampled.format;
        texture.clamp_s = sampled.wrap_s != GL_REPEAT;
        texture.clamp_t = sampled.wrap_t != GL_REPEAT;
        texture.mag_linear = sampled.mag_filter == GL_LINEAR;
        texture.min_linear = mip_filter(sampled.min_filter);
        page_count = sampled.page_count;
        for (page_index = 0U; page_index < page_count; ++page_index)
            pages[page_index] = (uint32_t)(backend->platform.texture_address +
                (size_t)(sampled.first_page + page_index) * MINIGL_RIX_TEXTURE_PAGE_BYTES);
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
        !rix_cmd_tex_env_color(writer, 0U, state.tex_env_color[0]) ||
        !rix_cmd_texture_pages(writer, 0U, pages, page_count))
        return 0;
    return 1;
}

static int backend_submit(MiniGLRixBackend *backend, RixCmdWriter *writer)
{
    uint8_t index = backend->current_buffer;
    uint8_t *buffer = backend->platform.command_buffer[index];
    uint32_t raw_bytes;
    uint32_t command_words;
    int result;

    if (backend->error || writer->failed || writer->length == 0U ||
        writer->length > 0x00ffffffU ||
        buffer == NULL || backend->platform.command_address[index] == 0U) {
        return 0;
    }
    raw_bytes = (uint32_t)writer->length;
    command_words = (raw_bytes + 8U) / 4U;
    ((uint32_t *)buffer)[0] = 0x90000000U | raw_bytes;
    ((uint32_t *)buffer)[1] = 0U;
    result = rix_drv_submit(&backend->driver, backend->platform.command_address[index],
                            command_words, RIX_TIMEOUT);
    if (result != RIX_DRV_OK) {
        backend_disable(backend, "submit", result);
        return 0;
    }
    backend->pending_fence[index] = backend->driver.submitted_target;
    backend->pending[index] = 1U;
    backend->submitted_command_words[index] = command_words;
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
    uint32_t csr_id;
    uint32_t csr_ver;

    if (platform == NULL || platform->csr_read == NULL ||
        platform->csr_write == NULL) {
        if (!logged) {
            RIX_LOG("[RasterIX] discovery: CPU fallback (platform unavailable)\r\n");
            logged = 1;
        }
        return 0;
    }
    csr_id = platform->csr_read(platform->context, RIX_DRV_CSR_IDENTITY);
    if (csr_id != RIX_DRV_IDENTITY_VALUE) {
        if (!logged) {
            RIX_LOG("[RasterIX] discovery: CPU fallback (CSR id=0x%08lx)\r\n",
                    (unsigned long)csr_id);
            logged = 1;
        }
        return 0;
    }
    csr_ver = platform->csr_read(platform->context, RIX_DRV_CSR_VERSION);
    if (csr_ver != RIX_DRV_VERSION_VALUE) {
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
    backend->command_words_estimate = 2U + (size_t)backend->bands.count * 10U;
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

static int backend_emit_frame(MiniGLRixBackend *backend, RixCmdWriter *writer)
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

                if (!backend_emit_state(backend, writer, op, 0U))
                    return 0;
                if (op->mask & GL_COLOR_BUFFER_BIT)
                    clear_flags |= RIX_FRAMEBUFFER_COLOR;
                if (op->mask & GL_DEPTH_BUFFER_BIT)
                    clear_flags |= RIX_FRAMEBUFFER_DEPTH;
                if (!rix_cmd_clear_color(writer, op->color) ||
                    !rix_cmd_clear_depth(writer, (uint16_t)(op->depth >> 8U)) ||
                    !rix_cmd_framebuffer(writer, RIX_FRAMEBUFFER_MEMSET | clear_flags,
                                         setup.pixels))
                    return 0;
            } else if (!backend_emit_state(backend, writer, op, 0U) ||
                       !backend_emit_triangles(writer, backend, op, band)) {
                return 0;
            }
        }
        if (!rix_cmd_framebuffer(writer, RIX_FRAMEBUFFER_COMMIT | RIX_FRAMEBUFFER_COLOR |
                                 RIX_FRAMEBUFFER_DEPTH, setup.pixels))
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
    if (!backend_emit_frame(backend, &writer) ||
        writer.length / sizeof(uint32_t) > MINIGL_RIX_COMMAND_WORD_BUDGET ||
        !backend_submit(backend, &writer))
        return 0;
    backend->triangle_count = 0U;
    if (continue_frame &&
        !rix_band_list_init(&backend->bands, backend->width, backend->height,
                            backend->references, MINIGL_RIX_MAX_TRIANGLES))
        return 0;
    backend->op_count = 0U;
    backend->needs_color_read = 1U;
    backend->needs_depth_read = 1U;
    backend->command_words_estimate = continue_frame ?
        2U + (size_t)backend->bands.count * 10U : 0U;
    if (!continue_frame)
        backend->frame_active = 0U;
    return 1;
}

static int backend_record_draw(MiniGLRixBackend *backend)
{
    MiniGLRixRecordedOp *op;
    MiniGLRixTexture *texture;
    size_t state_words;

    if (backend->op_count == MINIGL_RIX_MAX_OPS)
        return 0;
    op = &backend->ops[backend->op_count];
    memset(op, 0, sizeof(*op));
    op->type = MINIGL_RIX_OP_DRAW;
    op->state = backend->state;
    op->texture_slot = UINT16_MAX;
    op->first_triangle = backend->triangle_count;
    state_words = (size_t)backend->bands.count * 33U;
    if ((op->state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_0) != 0U) {
        texture = backend_texture(backend, backend->bound_texture);
        if (texture == NULL || !texture_filter_supported(texture) ||
            (mip_filter(texture->min_filter) && texture->levels != texture->required_levels))
            return 0;
        state_words += (size_t)backend->bands.count * texture->page_count;
        if (backend->command_words_estimate + state_words >
            MINIGL_RIX_COMMAND_WORD_BUDGET)
            return 0;
        op->texture_slot = (uint16_t)(texture - backend->textures);
        op->texture_incarnation = texture->incarnation;
        op->min_filter = texture->min_filter;
        op->mag_filter = texture->mag_filter;
        op->wrap_s = texture->wrap_s;
        op->wrap_t = texture->wrap_t;
        if (backend->recorded_pins[op->texture_slot] == 0U) {
            backend->recorded_pins[op->texture_slot] = 1U;
            ++texture->pin_count;
        }
        backend_touch_texture(backend, texture);
    } else if (backend->command_words_estimate + state_words >
               MINIGL_RIX_COMMAND_WORD_BUDGET) {
        return 0;
    }
    backend->command_words_estimate += state_words;
    ++backend->op_count;
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
        if (backend->triangle_count == 0U &&
            backend->ops[backend->op_count - 1U].triangle_count == 0U)
            return 0;
        if (!backend_flush(backend, 1) || !backend_record_draw(backend) ||
            backend->command_words_estimate +
                touched_bands * (RIX_TRIANGLE_WORDS + 1U) >
                MINIGL_RIX_COMMAND_WORD_BUDGET)
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
static void backend_recalculate_command_words(MiniGLRixBackend *backend)
{
    size_t op_index;

    backend->command_words_estimate = 2U + (size_t)backend->bands.count * 10U;
    for (op_index = 0U; op_index < backend->op_count; ++op_index) {
        const MiniGLRixRecordedOp *op = &backend->ops[op_index];

        if (op->type == MINIGL_RIX_OP_CLEAR) {
            backend->command_words_estimate += (size_t)backend->bands.count * 37U;
        } else {
            size_t triangle_index;

            backend->command_words_estimate += (size_t)backend->bands.count * 33U;
            if (op->texture_slot < MINIGL_RIX_MAX_TEXTURES)
                backend->command_words_estimate += (size_t)backend->bands.count *
                    backend->textures[op->texture_slot].page_count;
            for (triangle_index = 0U; triangle_index < op->triangle_count;
                 ++triangle_index) {
                size_t band_index;
                const RixTriangle *triangle =
                    &backend->triangles[op->first_triangle + triangle_index];

                for (band_index = 0U; band_index < backend->bands.count; ++band_index)
                    if (rix_triangle_in_band(triangle,
                                             backend->bands.band[band_index].y_start,
                                             backend->bands.band[band_index].y_end))
                        backend->command_words_estimate += RIX_TRIANGLE_WORDS + 1U;
            }
        }
    }
}

void minigl_rix_backend_abort_draw(MiniGLRixBackend *backend)
{
    MiniGLRixRecordedOp *op;
    size_t index;
    size_t retained_triangles;
    uint16_t slot;
    int still_pinned = 0;

    if (backend == NULL || !backend->collecting)
        return;
    backend->collecting = 0U;
    if (backend->op_count == 0U)
        return;
    op = &backend->ops[backend->op_count - 1U];
    if (op->type != MINIGL_RIX_OP_DRAW)
        return;
    retained_triangles = op->first_triangle;
    slot = op->texture_slot;
    --backend->op_count;
    for (index = 0U; index < backend->op_count; ++index)
        if (backend->ops[index].type == MINIGL_RIX_OP_DRAW &&
            backend->ops[index].texture_slot == slot) {
            still_pinned = 1;
            break;
        }
    if (!still_pinned && slot < MINIGL_RIX_MAX_TEXTURES &&
        backend->recorded_pins[slot] != 0U) {
        backend->recorded_pins[slot] = 0U;
        if (backend->textures[slot].pin_count != 0U)
            --backend->textures[slot].pin_count;
    }
    backend->triangle_count = retained_triangles;
    backend_recalculate_command_words(backend);
    if (!rix_band_list_init(&backend->bands, backend->width, backend->height,
                            backend->references, MINIGL_RIX_MAX_TRIANGLES))
        backend_disable(backend, "abort", RIX_DRV_ERROR);
    for (index = 0U; index < retained_triangles && !backend->error; ++index)
        if (!rix_band_list_add(&backend->bands, &backend->triangles[index]))
            backend_disable(backend, "abort", RIX_DRV_ERROR);
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
    if (backend->op_count == MINIGL_RIX_MAX_OPS ||
        backend->command_words_estimate + (size_t)backend->bands.count * 37U >
            MINIGL_RIX_COMMAND_WORD_BUDGET) {
        if (!backend_flush(backend, 1))
            return 0;
    }
    if (backend->op_count == 0U) {
        if (mask & GL_COLOR_BUFFER_BIT) backend->needs_color_read = 0U;
        if (mask & GL_DEPTH_BUFFER_BIT) backend->needs_depth_read = 0U;
    }
    op = &backend->ops[backend->op_count++];
    memset(op, 0, sizeof(*op));
    op->type = MINIGL_RIX_OP_CLEAR;
    op->state = backend->state;
    op->texture_slot = UINT16_MAX;
    op->mask = mask;
    op->color = rgba;
    op->depth = depth;
    backend->command_words_estimate += (size_t)backend->bands.count * 37U;
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
    if (backend->platform.cache_clean != NULL)
        backend->platform.cache_clean(backend->platform.context,
                                      (uintptr_t)rgba, pixels * 2U);
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

        red = (red << 3U) | (red >> 2U);
        green = (green << 2U) | (green >> 4U);
        blue = (blue << 3U) | (blue >> 2U);
        rgba[index - 1U] = (red << 24U) | (green << 16U) |
                            (blue << 8U) | 0xffU;
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
    size_t index;

    if (backend == NULL || !backend->available)
        return 1;
    if (backend->error)
        return 0;
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
    if (backend->state.depth_bits != 16U && backend->state.depth_bits != 24U &&
        backend->state.depth_bits != 32U)
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
