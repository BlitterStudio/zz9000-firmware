/*
 * MiniGL RasterIX backend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "minigl_rix_backend.h"
#include "rix_texture.h"
#include "minigl_profile.h"

#if !defined(MINIGL_HOST_TEST) && !defined(MINIGL_SERVICE_HOST_TEST)
#include "xil_io.h"
#include "xil_printf.h"
#define RIX_LOG(...) xil_printf(__VA_ARGS__)
#define RIX_PS_DIAGNOSTICS 1
#else
void (*g_rix_test_log)(const char *fmt, ...) = NULL;
#define RIX_LOG(...) do { if (g_rix_test_log) g_rix_test_log(__VA_ARGS__); } while (0)
#define RIX_PS_DIAGNOSTICS 0
#endif

#include <string.h>

#define RIX_TIMEOUT 1000000U
/* Start-up PL330 self-test: leading parser NOPs put the packet past one
 * 16-word burst plus a tail before the fence MEMSET that proves delivery. */
#define RIX_DMA_SELF_TEST_NOPS 24U
#define RIX_DMA_SELF_TEST_FENCE 0xa55aU
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
                                                   const MiniGLRixRecordedOp *op,
                                                   unsigned int unit)
{
    MiniGLRixTexture *texture;
    uint16_t slot = unit == 0U ? op->texture_slot : op->texture1_slot;
    uint32_t incarnation = unit == 0U ? op->texture_incarnation :
                                         op->texture1_incarnation;

    if (slot >= MINIGL_RIX_MAX_TEXTURES)
        return NULL;
    texture = &backend->textures[slot];
    return texture->valid && texture->incarnation == incarnation ? texture : NULL;
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

static int backend_multitexture_supported(const MiniGLRixBackend *backend)
{
    const ZZ9KMiniGLPacketSetState *state;
    const MiniGLRixTexture *texture0;
    const MiniGLRixTexture *texture1;
    uint32_t required;

    if (backend == NULL)
        return 0;
    state = &backend->state;
    required = ZZ9K_MGL_STATE_TEXTURE_2D_0 | ZZ9K_MGL_STATE_TEXTURE_2D_1 |
               ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE;
    if ((state->enabled_flags & required) != required ||
        (state->enabled_flags & (ZZ9K_MGL_STATE_BLEND | ZZ9K_MGL_STATE_ALPHA_TEST)) != 0U ||
        (state->color_mask & 7U) != 7U ||
        state->tex_env_mode[1] != GL_MODULATE ||
        (state->depth_func != ZZ9K_MGL_WIRE_LEQUAL &&
         state->depth_func != ZZ9K_MGL_WIRE_EQUAL &&
         state->depth_func != ZZ9K_MGL_WIRE_GEQUAL &&
         state->depth_func != ZZ9K_MGL_WIRE_ALWAYS))
        return 0;
    texture0 = backend_texture((MiniGLRixBackend *)backend, backend->bound_texture);
    texture1 = backend_texture((MiniGLRixBackend *)backend, backend->bound_texture1);
    return texture0 != NULL && texture1 != NULL &&
           texture_filter_supported(texture0) && texture_filter_supported(texture1) &&
           (!mip_filter(texture0->min_filter) ||
            texture0->levels == texture0->required_levels) &&
           (!mip_filter(texture1->min_filter) ||
            texture1->levels == texture1->required_levels);
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
            minigl_profile_count(MGLP_N_EVICTIONS, 1U);
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

void minigl_rix_backend_bind_texture(MiniGLRixBackend *backend, uint16_t unit, uint32_t id)
{
    if (backend == NULL || unit > 1U)
        return;
    if (unit == 0U)
        backend->bound_texture = id;
    else
        backend->bound_texture1 = id;
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
        if (backend->bound_texture1 == id)
            backend->bound_texture1 = 0U;
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
    if (texture != NULL && texture->pin_count != 0U) {
        minigl_profile_count(MGLP_N_PIN_DRAINS, 1U);
        if (!minigl_rix_backend_drain(backend))
            return 0;
    }
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
    minigl_profile_count(MGLP_N_RIX_UPLOADS, 1U);
    minigl_profile_count(MGLP_N_RIX_UPLOAD_BYTES, bytes);
    backend_touch_texture(backend, texture);
    return 1;
}

static int backend_allocate_pages_without_drain(MiniGLRixBackend *backend,
                                                MiniGLRixTexture *texture,
                                                uint16_t page_count)
{
    MiniGLRixTexture *victim;
    uint32_t total_pages = (uint32_t)(backend->platform.texture_bytes /
                                      MINIGL_RIX_TEXTURE_PAGE_BYTES);
    uint32_t first_page;

    if (page_count == 0U || page_count > total_pages)
        return 0;
    for (;;) {
        for (first_page = 0U; first_page <= total_pages - page_count; ++first_page) {
            if (backend_range_available(backend, texture, (uint16_t)first_page,
                                        page_count)) {
                texture->first_page = (uint16_t)first_page;
                texture->page_count = page_count;
                return 1;
            }
        }
        victim = backend_evict_texture(backend, texture);
        if (victim == NULL)
            return 0;
        minigl_profile_count(MGLP_N_EVICTIONS, 1U);
        backend_release_texture(backend, victim);
    }
}

static MiniGLRixTexture *backend_clone_texture(MiniGLRixBackend *backend,
                                                MiniGLRixTexture *source)
{
    MiniGLRixTexture *copy = NULL;
    uint32_t incarnation;
    size_t index;
    size_t bytes;

    for (index = 0U; index < MINIGL_RIX_MAX_TEXTURES; ++index)
        if (!backend->textures[index].valid) {
            copy = &backend->textures[index];
            break;
        }
    if (copy == NULL) {
        copy = backend_evict_texture(backend, source);
        if (copy == NULL)
            return NULL;
        minigl_profile_count(MGLP_N_EVICTIONS, 1U);
        backend_release_texture(backend, copy);
    }
    memcpy(copy, source, sizeof(*copy));
    ++backend->texture_incarnation_clock;
    if (backend->texture_incarnation_clock == 0U)
        ++backend->texture_incarnation_clock;
    incarnation = backend->texture_incarnation_clock;
    copy->incarnation = incarnation;
    copy->pin_count = 0U;
    copy->deleted = 0U;
    if (!backend_allocate_pages_without_drain(backend, copy, source->page_count)) {
        memset(copy, 0, sizeof(*copy));
        return NULL;
    }
    bytes = (size_t)source->page_count * MINIGL_RIX_TEXTURE_PAGE_BYTES;
    memcpy(backend->platform.texture_memory +
               (size_t)copy->first_page * MINIGL_RIX_TEXTURE_PAGE_BYTES,
           backend->platform.texture_memory +
               (size_t)source->first_page * MINIGL_RIX_TEXTURE_PAGE_BYTES,
           bytes);
    if (backend->platform.cache_clean != NULL)
        backend->platform.cache_clean(backend->platform.context,
            (uintptr_t)(backend->platform.texture_memory +
                (size_t)copy->first_page * MINIGL_RIX_TEXTURE_PAGE_BYTES), bytes);
    source->deleted = 1U;
    backend_touch_texture(backend, copy);
    return copy;
}

int minigl_rix_backend_subimage_texture(MiniGLRixBackend *backend, uint32_t id,
                                        uint16_t level, uint16_t xoffset,
                                        uint16_t yoffset, uint16_t width,
                                        uint16_t height, const uint32_t *rgba,
                                        uint16_t source_width,
                                        uint16_t source_height)
{
    MiniGLRixTexture *texture;
    uint16_t level_width;
    uint16_t level_height;
    uint32_t offset;
    uint16_t row;

    if (backend == NULL || rgba == NULL || width == 0U || height == 0U ||
        backend->error)
        return 0;
    texture = backend_texture(backend, id);
    if (texture == NULL || level >= 16U ||
        (texture->levels & (uint16_t)(1U << level)) == 0U)
        return 0;
    level_width = texture->width >> level;
    level_height = texture->height >> level;
    if (level_width == 0U) level_width = 1U;
    if (level_height == 0U) level_height = 1U;
    if (source_width != level_width || source_height != level_height ||
        xoffset > level_width - width || yoffset > level_height - height)
        return 0;
    if (texture->pin_count != 0U) {
        MiniGLRixTexture *copy = backend_clone_texture(backend, texture);

        if (copy != NULL)
            texture = copy;
        else {
            minigl_profile_count(MGLP_N_PIN_DRAINS, 1U);
            if (!minigl_rix_backend_drain(backend))
                return 0;
        }
    }
    offset = rix_texture_mip_offset(texture->width_log2, texture->height_log2,
                                    (uint8_t)level);
    if (offset == UINT32_MAX)
        return 0;
    for (row = 0U; row < height; ++row)
        if (!backend_write_texture(backend, texture,
            offset + (uint32_t)(yoffset + row) * level_width + xoffset,
            rgba + (size_t)(yoffset + row) * source_width + xoffset, width))
            return 0;
    minigl_profile_count(MGLP_N_RIX_UPLOADS, 1U);
    minigl_profile_count(MGLP_N_RIX_UPLOAD_BYTES, (uint32_t)width * height * 2U);
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

#if RIX_PS_DIAGNOSTICS
static void backend_log_ps_diagnostics(void)
{
    const uint32_t afi = MINIGL_RIX_AFI_BASE;
    const uint32_t dmac = MINIGL_RIX_DMAC_BASE;
    const uint32_t channel = MINIGL_RIX_DMAC_CHANNEL;

    RIX_LOG("[RasterIX] diag afi3-r=%08lx/%08lx/%08lx/%08lx/%08lx afi3-w=%08lx/%08lx/%08lx/%08lx/%08lx\r\n",
            (unsigned long)Xil_In32(afi + RIX_AFI_RD_CTRL),
            (unsigned long)Xil_In32(afi + RIX_AFI_RD_ISSUING_CAP),
            (unsigned long)Xil_In32(afi + RIX_AFI_RD_QOS),
            (unsigned long)Xil_In32(afi + RIX_AFI_RD_DATAFIFO_LEVEL),
            (unsigned long)Xil_In32(afi + RIX_AFI_RD_DEBUG),
            (unsigned long)Xil_In32(afi + RIX_AFI_WR_CTRL),
            (unsigned long)Xil_In32(afi + RIX_AFI_WR_ISSUING_CAP),
            (unsigned long)Xil_In32(afi + RIX_AFI_WR_QOS),
            (unsigned long)Xil_In32(afi + RIX_AFI_WR_DATAFIFO_LEVEL),
            (unsigned long)Xil_In32(afi + RIX_AFI_WR_DEBUG));
    RIX_LOG("[RasterIX] diag pl330 ds=%08lx fsrc=%08lx dbg=%08lx ch%lu csr=%08lx cpc=%08lx ftr=%08lx sar=%08lx dar=%08lx ccr=%08lx\r\n",
            (unsigned long)Xil_In32(dmac + MINIGL_RIX_DMAC_DS),
            (unsigned long)Xil_In32(dmac + MINIGL_RIX_DMAC_FSRC),
            (unsigned long)Xil_In32(dmac + MINIGL_RIX_DMAC_DBGSTATUS),
            (unsigned long)channel,
            (unsigned long)Xil_In32(dmac + MINIGL_RIX_DMAC_CSR(channel)),
            (unsigned long)Xil_In32(dmac + MINIGL_RIX_DMAC_CPC(channel)),
            (unsigned long)Xil_In32(dmac + MINIGL_RIX_DMAC_FTR(channel)),
            (unsigned long)Xil_In32(dmac + MINIGL_RIX_DMAC_SAR(channel)),
            (unsigned long)Xil_In32(dmac + MINIGL_RIX_DMAC_DAR(channel)),
            (unsigned long)Xil_In32(dmac + MINIGL_RIX_DMAC_CCR(channel)));
}
#endif

/* One snapshot per backend failure: the EF CSR map, the command FIFO's
 * readable registers (TDFD/TLF/TDFR are write-only), the transport of the
 * last packet, the failed buffer and, on hardware, AFI3 and the PL330. */
static void backend_log_diagnostics(MiniGLRixBackend *backend, uint8_t index)
{
    const MiniGLRixPlatform *platform = &backend->platform;
    const struct rix_drv *driver = &backend->driver;
    uint32_t words[16] = { 0U };
    uint32_t command_words = backend->submitted_command_words[index];
    size_t word_count = command_words < 16U ? command_words : 16U;
    const uint8_t *buffer = platform->command_buffer[index];

    RIX_LOG("[RasterIX] diag csr id=%08lx ver=%08lx ctl=%08lx st=%08lx stream=%08lx resp=%08lx\r\n",
            (unsigned long)platform->csr_read(platform->context, RIX_DRV_CSR_IDENTITY),
            (unsigned long)platform->csr_read(platform->context, RIX_DRV_CSR_VERSION),
            (unsigned long)platform->csr_read(platform->context, RIX_DRV_CSR_CONTROL),
            (unsigned long)platform->csr_read(platform->context, RIX_DRV_CSR_STATUS),
            (unsigned long)platform->csr_read(platform->context, RIX_DRV_CSR_DBG_STREAM),
            (unsigned long)platform->csr_read(platform->context, RIX_DRV_CSR_DBG_RESPONSE));
    RIX_LOG("[RasterIX] diag mem aw=%lu w=%lu b=%lu ar=%lu r=%lu last_ar=%08lx arid=%02lx\r\n",
            (unsigned long)platform->csr_read(platform->context, RIX_DRV_CSR_DBG_AW),
            (unsigned long)platform->csr_read(platform->context, RIX_DRV_CSR_DBG_W),
            (unsigned long)platform->csr_read(platform->context, RIX_DRV_CSR_DBG_B),
            (unsigned long)platform->csr_read(platform->context, RIX_DRV_CSR_DBG_AR),
            (unsigned long)platform->csr_read(platform->context, RIX_DRV_CSR_DBG_R),
            (unsigned long)platform->csr_read(platform->context, RIX_DRV_CSR_DBG_LAST_AR),
            (unsigned long)platform->csr_read(platform->context, RIX_DRV_CSR_DBG_LAST_ARID));
    RIX_LOG("[RasterIX] diag fifo isr=%08lx ier=%08lx tdfv=%lu last=%s/%lu bytes packets=%lu dma=%lu transport=%s\r\n",
            (unsigned long)platform->fifo_read(platform->context, RIX_DRV_FIFO_ISR),
            (unsigned long)platform->fifo_read(platform->context, RIX_DRV_FIFO_IER),
            (unsigned long)platform->fifo_read(platform->context, RIX_DRV_FIFO_TDFV),
            driver->last_packet_dma ? "dma" : "mmio",
            (unsigned long)driver->last_packet_bytes, (unsigned long)driver->packets,
            (unsigned long)driver->dma_packets,
            driver->dma_stream != NULL ? "dma" : "mmio");
    if (buffer != NULL && word_count != 0U) {
        if (platform->cache_invalidate != NULL)
            platform->cache_invalidate(platform->context, (uintptr_t)buffer,
                                       word_count * sizeof(words[0]));
        memcpy(words, buffer, word_count * sizeof(words[0]));
    }
    RIX_LOG("[RasterIX] diag cmd=%08lx/%lu color=%08lx depth=%08lx tex=%08lx white=%08lx\r\n",
            (unsigned long)platform->command_address[index],
            (unsigned long)command_words, (unsigned long)backend->color_address,
            (unsigned long)backend->depth_address,
            (unsigned long)platform->texture_address,
            (unsigned long)platform->white_texture_address);
    RIX_LOG("[RasterIX] diag words=%08lx/%08lx/%08lx/%08lx %08lx/%08lx/%08lx/%08lx\r\n",
            (unsigned long)words[0], (unsigned long)words[1], (unsigned long)words[2],
            (unsigned long)words[3], (unsigned long)words[4], (unsigned long)words[5],
            (unsigned long)words[6], (unsigned long)words[7]);
    RIX_LOG("[RasterIX] diag words=%08lx/%08lx/%08lx/%08lx %08lx/%08lx/%08lx/%08lx\r\n",
            (unsigned long)words[8], (unsigned long)words[9], (unsigned long)words[10],
            (unsigned long)words[11], (unsigned long)words[12], (unsigned long)words[13],
            (unsigned long)words[14], (unsigned long)words[15]);
#if RIX_PS_DIAGNOSTICS
    backend_log_ps_diagnostics();
#endif
}

static void backend_disable(MiniGLRixBackend *backend, const char *operation,
                            int result, uint8_t buffer_index)
{
    size_t index;

    if (backend->error)
        return;
    backend->error = 1U;
    RIX_LOG("[RasterIX] backend disabled: %s error %d (status=0x%08lx err_status=0x%08lx err_addr=0x%08lx)\r\n",
            operation, result, (unsigned long)backend->driver.last_status,
            (unsigned long)backend->driver.last_error_status,
            (unsigned long)backend->driver.last_error_address);
    backend_log_diagnostics(backend, buffer_index);
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

static volatile uint16_t *backend_fence_pixel(MiniGLRixBackend *backend, uint8_t index)
{
    return (volatile uint16_t *)(backend->platform.command_buffer[index] +
                                 backend->platform.command_capacity -
                                 MINIGL_RIX_TEXTURE_PAGE_BYTES);
}

static int backend_wait_fence(MiniGLRixBackend *backend, uint8_t index,
                              uint16_t value)
{
    volatile uint16_t *pixel = backend_fence_pixel(backend, index);
    uint32_t attempt = 0U;

    for (;;) {
        if (backend->platform.cache_invalidate != NULL)
            backend->platform.cache_invalidate(backend->platform.context,
                                               (uintptr_t)pixel, sizeof(*pixel));
        if (*pixel == value)
            return 1;
        if (attempt++ >= RIX_TIMEOUT)
            return 0;
    }
}

static int backend_wait_buffer_now(MiniGLRixBackend *backend, uint8_t index)
{
    int result;

    if (backend->error)
        return 0;
    if (!backend->pending[index])
        return 1;
    if (!backend_wait_fence(backend, index, (uint16_t)backend->pending_fence[index])) {
        backend_disable(backend, "fence", RIX_DRV_TIMEOUT, index);
        return 0;
    }
    result = rix_drv_poll(&backend->driver, RIX_TIMEOUT);
    if (result != RIX_DRV_OK) {
        backend_disable(backend, "poll", result, index);
        return 0;
    }
    backend->pending[index] = 0U;
    backend->pending_fence[index] = 0U;
    backend->submitted_command_words[index] = 0U;
    backend_release_buffer(backend, index);
    return 1;
}

static int backend_wait_buffer(MiniGLRixBackend *backend, uint8_t index)
{
    int ok;

    if (!backend->pending[index])
        return backend->error ? 0 : 1;
    minigl_profile_push(MGLP_FENCE);
    ok = backend_wait_buffer_now(backend, index);
    minigl_profile_pop();
    return ok;
}

static int backend_emit_state(MiniGLRixBackend *backend, RixCmdWriter *writer,
                              const MiniGLRixRecordedOp *op, unsigned int pass)
{
    ZZ9KMiniGLPacketSetState state = op->state;
    RixFragmentState fragment;
    RixTexEnvState environment;
    RixTextureState texture;
    MiniGLRixTexture sampled;
    MiniGLRixTexture *image;
    uint32_t features;
    uint32_t pages[MINIGL_RIX_TEXTURE_PAGES] = { 0U };
    size_t page_count;
    size_t page_index;
    uint16_t slot;

    state.enabled_flags &= ~ZZ9K_MGL_STATE_TEXTURE_2D_1;
    if (pass != 0U) {
        state.enabled_flags |= ZZ9K_MGL_STATE_TEXTURE_2D_0 |
                               ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_BLEND;
        state.enabled_flags &= ~(ZZ9K_MGL_STATE_DEPTH_WRITE |
                                 ZZ9K_MGL_STATE_ALPHA_TEST);
        state.depth_func = ZZ9K_MGL_WIRE_EQUAL;
        state.blend_src = ZZ9K_MGL_WIRE_DST_COLOR;
        state.blend_dst = ZZ9K_MGL_WIRE_ZERO;
        state.tex_env_mode[0] = GL_REPLACE;
        state.tex_env_color[0] = state.tex_env_color[1];
    }
    slot = pass == 0U ? op->texture_slot : op->texture1_slot;
    if ((state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_0) == 0U ||
        slot == UINT16_MAX)
        state.enabled_flags &= ~ZZ9K_MGL_STATE_TEXTURE_2D_0;
    if (!backend_maps_state(&state, &fragment, &environment, &features))
        return 0;
    memset(&texture, 0, sizeof(texture));
    if ((state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_0) != 0U) {
        image = backend_recorded_texture(backend, op, pass);
        if (image == NULL)
            return 0;
        sampled = *image;
        if (pass == 0U) {
            sampled.min_filter = op->min_filter;
            sampled.mag_filter = op->mag_filter;
            sampled.wrap_s = op->wrap_s;
            sampled.wrap_t = op->wrap_t;
        } else {
            sampled.min_filter = op->texture1_min_filter;
            sampled.mag_filter = op->texture1_mag_filter;
            sampled.wrap_s = op->texture1_wrap_s;
            sampled.wrap_t = op->texture1_wrap_t;
        }
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
    return pages[0] != 0U && rix_cmd_feature_enable(writer, features) &&
           rix_cmd_fragment_state(writer, &fragment) &&
           rix_cmd_texture_state(writer, 0U, &texture) &&
           rix_cmd_tex_env(writer, 0U, &environment) &&
           rix_cmd_tex_env_color(writer, 0U, state.tex_env_color[0]) &&
           rix_cmd_texture_pages(writer, 0U, pages, page_count);
}

static uint32_t backend_fence_rgba(uint16_t value)
{
    uint32_t red = (value >> 11U) & 31U;
    uint32_t green = (value >> 5U) & 63U;
    uint32_t blue = value & 31U;

    return (((red << 3U) | (red >> 2U)) << 24U) |
           (((green << 2U) | (green >> 4U)) << 16U) |
           (((blue << 3U) | (blue >> 2U)) << 8U) | 0xffU;
}

static int backend_emit_fence(MiniGLRixBackend *backend, RixCmdWriter *writer,
                              uint8_t index, uint16_t value)
{
    RixFragmentState fragment;
    uint32_t address = (uint32_t)(backend->platform.command_address[index] +
                                  backend->platform.command_capacity -
                                  MINIGL_RIX_TEXTURE_PAGE_BYTES);

    memset(&fragment, 0, sizeof(fragment));
    fragment.color_mask_r = fragment.color_mask_g = 1U;
    fragment.color_mask_b = fragment.color_mask_a = 1U;
    return (address & (MINIGL_RIX_TEXTURE_PAGE_BYTES - 1U)) == 0U &&
           rix_cmd_feature_enable(writer, 0U) &&
           rix_cmd_fragment_state(writer, &fragment) &&
           rix_cmd_resolution(writer, 64U, 1U) &&
           rix_cmd_buffer_address(writer, RIX_REG_COLOR_BUFFER_ADDR, address) &&
           rix_cmd_clear_color(writer, backend_fence_rgba(value)) &&
           rix_cmd_framebuffer(writer, RIX_FRAMEBUFFER_MEMSET |
                               RIX_FRAMEBUFFER_COLOR, 1U);
}

static int backend_submit(MiniGLRixBackend *backend, RixCmdWriter *writer,
                          uint16_t fence)
{
    uint8_t index = backend->current_buffer;
    uint8_t *buffer = backend->platform.command_buffer[index];
    uint32_t command_words;
    int result;

    if (backend->error || writer->failed || writer->length == 0U ||
        buffer == NULL || backend->platform.command_address[index] == 0U)
        return 0;
    command_words = (uint32_t)(writer->length / sizeof(uint32_t));
    backend->submitted_command_words[index] = command_words;
    result = rix_drv_submit(&backend->driver, (const uint32_t *)buffer,
                            backend->platform.command_address[index],
                            command_words, RIX_TIMEOUT);
    if (result != RIX_DRV_OK) {
        backend_disable(backend, "submit", result, index);
        return 0;
    }
    backend->pending_fence[index] = fence;
    backend->pending[index] = 1U;
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
        platform->csr_write == NULL || platform->fifo_read == NULL ||
        platform->fifo_write == NULL) {
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
        RIX_LOG("[RasterIX] discovery: RasterIX EF FIFO ABI 0x%08lx (TMU=1)\r\n",
                (unsigned long)csr_ver);
        logged = 1;
    }
    return 1;
}

/* README contract: PL330 transport may carry packets only after a start-up
 * test proves the count and content of a DMA packet end to end.  One packet
 * of parser NOPs plus a fence MEMSET goes out through the DMA path; the
 * commit must be accepted, the fence pixel must land and the renderer must
 * drain. */
static int backend_dma_self_test(MiniGLRixBackend *backend)
{
    RixCmdWriter writer;
    volatile uint16_t *pixel = backend_fence_pixel(backend, 0U);
    uint8_t *buffer = backend->platform.command_buffer[0];
    uint32_t packets = backend->driver.dma_packets;
    uint32_t index;

    if (buffer == NULL || backend->platform.command_address[0] == 0U ||
        backend->platform.command_capacity <= MINIGL_RIX_TEXTURE_PAGE_BYTES)
        return 0;
    *pixel = 0U;
    if (backend->platform.cache_clean != NULL)
        backend->platform.cache_clean(backend->platform.context, (uintptr_t)pixel,
                                      sizeof(*pixel));
    rix_cmd_init(&writer, buffer, backend->platform.command_capacity -
                                 MINIGL_RIX_TEXTURE_PAGE_BYTES);
    for (index = 0U; index < RIX_DMA_SELF_TEST_NOPS; ++index)
        (void)rix_cmd_word(&writer, 0U);
    if (!backend_emit_fence(backend, &writer, 0U, RIX_DMA_SELF_TEST_FENCE) ||
        writer.failed)
        return 0;
    backend->submitted_command_words[0] = (uint32_t)(writer.length / sizeof(uint32_t));
    if (rix_drv_submit(&backend->driver, (const uint32_t *)buffer,
                       backend->platform.command_address[0],
                       backend->submitted_command_words[0], RIX_TIMEOUT) != RIX_DRV_OK ||
        backend->driver.dma_packets == packets ||
        !backend_wait_fence(backend, 0U, RIX_DMA_SELF_TEST_FENCE) ||
        rix_drv_poll(&backend->driver, RIX_TIMEOUT) != RIX_DRV_OK)
        return 0;
    backend->submitted_command_words[0] = 0U;
    return 1;
}

static int backend_select_transport(MiniGLRixBackend *backend)
{
    static int logged = 0;

    if (backend->driver.dma_stream == NULL) {
        if (!logged)
            RIX_LOG("[RasterIX] transport: MMIO\r\n");
        logged = 1;
        return 1;
    }
    if (backend_dma_self_test(backend)) {
        if (!logged)
            RIX_LOG("[RasterIX] transport: PL330 DMA (self-test passed)\r\n");
        logged = 1;
        return 1;
    }
    RIX_LOG("[RasterIX] transport: PL330 self-test failed (err_status=0x%08lx); using MMIO\r\n",
            (unsigned long)backend->driver.last_error_status);
    backend_log_diagnostics(backend, 0U);
    backend->submitted_command_words[0] = 0U;
    backend->driver.dma_stream = NULL;
    /* Discard the renderer and FIFO state of the failed packet, then clear
     * the FIFO's sticky errors before MMIO packets are committed. */
    return rix_drv_recover(&backend->driver, RIX_TIMEOUT, 1) == RIX_DRV_OK &&
           rix_drv_enable(&backend->driver) == RIX_DRV_OK;
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
    config.fifo_read = platform->fifo_read;
    config.fifo_write = platform->fifo_write;
    config.fifo_write_words = platform->fifo_write_words;
    config.fifo_drain = platform->fifo_drain;
    config.clean_dcache = platform->cache_clean;
    config.dma_stream = platform->dma_stream;
    config.hardware_present = 1;
    if (rix_drv_init(&backend->driver, &config) != RIX_DRV_OK ||
        rix_drv_enable(&backend->driver) != RIX_DRV_OK) {
        RIX_LOG("[RasterIX] backend enable failed (status=0x%08lx)\r\n",
                (unsigned long)backend->driver.last_status);
        return;
    }
    backend_fill_white_texture(backend);
    if (!backend_select_transport(backend)) {
        RIX_LOG("[RasterIX] backend enable failed (status=0x%08lx)\r\n",
                (unsigned long)backend->driver.last_status);
        return;
    }
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
    if ((color_address & (MINIGL_RIX_TEXTURE_PAGE_BYTES - 1U)) != 0U ||
        (depth_address & (MINIGL_RIX_TEXTURE_PAGE_BYTES - 1U)) != 0U)
        return 0;
    backend->width = width;
    backend->height = height;
    backend->color_address = color_address;
    backend->depth_address = depth_address;
    backend->triangle_count = 0U;
    backend->op_count = 0U;
    backend->frame_active = 1U;
    backend->command_words_estimate = 12U;
    return 1;
}

static int backend_emit_triangle(RixCmdWriter *writer, const MiniGLRixBackend *backend,
                                 const MiniGLRixRecordedOp *op, size_t index,
                                 unsigned int pass)
{
    const RixTriangle *triangle = &(pass == 0U ? backend->triangles :
                                   backend->texture1_triangles)[op->first_triangle + index];

    return rix_cmd_triangle(writer, triangle->words, RIX_TRIANGLE_WORDS);
}

static int backend_emit_triangles(RixCmdWriter *writer, const MiniGLRixBackend *backend,
                                  const MiniGLRixRecordedOp *op, unsigned int pass)
{
    size_t index;

    for (index = 0U; index < op->triangle_count; ++index)
        if (!backend_emit_triangle(writer, backend, op, index, pass))
            return 0;
    return 1;
}

static size_t backend_draw_state_words(const MiniGLRixBackend *backend,
                                       const MiniGLRixRecordedOp *op, unsigned int pass)
{
    uint16_t slot = pass == 0U ? op->texture_slot : op->texture1_slot;
    size_t words = 33U;

    if (slot < MINIGL_RIX_MAX_TEXTURES)
        words += backend->textures[slot].page_count;
    return words;
}

static int backend_emit_frame(MiniGLRixBackend *backend, RixCmdWriter *writer)
{
    size_t op_index;
    uint32_t pixels = (uint32_t)backend->width * (uint32_t)backend->height;

    if (pixels == 0U || pixels > 0x000fffffU ||
        !rix_cmd_resolution(writer, backend->width, backend->height) ||
        !rix_cmd_buffer_address(writer, RIX_REG_COLOR_BUFFER_ADDR, backend->color_address) ||
        !rix_cmd_buffer_address(writer, RIX_REG_DEPTH_BUFFER_ADDR, backend->depth_address))
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
                !rix_cmd_framebuffer(writer, RIX_FRAMEBUFFER_MEMSET | clear_flags, pixels))
                return 0;
        } else if (!op->two_pass) {
            if (!backend_emit_state(backend, writer, op, 0U) ||
                !backend_emit_triangles(writer, backend, op, 0U))
                return 0;
        } else {
            size_t triangle_index;

            for (triangle_index = 0U; triangle_index < op->triangle_count;
                 ++triangle_index)
                if (!backend_emit_state(backend, writer, op, 0U) ||
                    !backend_emit_triangle(writer, backend, op, triangle_index, 0U) ||
                    !backend_emit_state(backend, writer, op, 1U) ||
                    !backend_emit_triangle(writer, backend, op, triangle_index, 1U))
                    return 0;
        }
    }
    return !writer->failed;
}

static int backend_flush_now(MiniGLRixBackend *backend, int continue_frame)
{
    RixCmdWriter writer;
    uint8_t *buffer;
    volatile uint16_t *pixel;
    uint16_t fence;

    if (!backend->frame_active || backend->op_count == 0U)
        return 1;
    if (!backend_wait_buffer(backend, backend->next_buffer))
        return 0;
    backend->current_buffer = backend->next_buffer;
    buffer = backend->platform.command_buffer[backend->current_buffer];
    if (buffer == NULL || backend->platform.command_capacity <=
                       MINIGL_RIX_TEXTURE_PAGE_BYTES)
        return 0;
    fence = (uint16_t)(backend->driver.submitted_target + 1U);
    if (fence == 0U)
        fence = 1U;
    pixel = backend_fence_pixel(backend, backend->current_buffer);
    *pixel = 0U;
    if (backend->platform.cache_clean != NULL)
        backend->platform.cache_clean(backend->platform.context, (uintptr_t)pixel,
                                      sizeof(*pixel));
    rix_cmd_init(&writer, buffer, backend->platform.command_capacity -
                                 MINIGL_RIX_TEXTURE_PAGE_BYTES);
    if (!backend_emit_frame(backend, &writer) ||
        !backend_emit_fence(backend, &writer, backend->current_buffer, fence) ||
        writer.length / sizeof(uint32_t) > MINIGL_RIX_COMMAND_WORD_BUDGET ||
        !backend_submit(backend, &writer, fence))
        return 0;
    backend->triangle_count = 0U;
    backend->op_count = 0U;
    backend->command_words_estimate = continue_frame ? 12U : 0U;
    if (!continue_frame)
        backend->frame_active = 0U;
    return 1;
}

/* EMIT is the command build; the fence wait for the reused buffer and the
 * FIFO push inside are charged to FENCE and FIFO/STALL. */
static int backend_flush(MiniGLRixBackend *backend, int continue_frame)
{
    int ok;

    if (!backend->frame_active || backend->op_count == 0U)
        return 1;
    minigl_profile_push(MGLP_EMIT);
    ok = backend_flush_now(backend, continue_frame);
    minigl_profile_pop();
    return ok;
}

static int backend_record_draw(MiniGLRixBackend *backend)
{
    MiniGLRixRecordedOp *op;
    MiniGLRixTexture *texture0 = NULL;
    MiniGLRixTexture *texture1 = NULL;
    size_t state_words;

    if (backend->op_count == MINIGL_RIX_MAX_OPS)
        return 0;
    op = &backend->ops[backend->op_count];
    memset(op, 0, sizeof(*op));
    op->type = MINIGL_RIX_OP_DRAW;
    op->state = backend->state;
    op->texture_slot = UINT16_MAX;
    op->texture1_slot = UINT16_MAX;
    op->first_triangle = backend->triangle_count;
    op->two_pass = (op->state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_1) != 0U;
    if ((op->state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_0) != 0U) {
        texture0 = backend_texture(backend, backend->bound_texture);
        if (texture0 == NULL || !texture_filter_supported(texture0) ||
            (mip_filter(texture0->min_filter) && texture0->levels != texture0->required_levels))
            return 0;
    }
    if (op->two_pass) {
        texture1 = backend_texture(backend, backend->bound_texture1);
        if (texture1 == NULL || !texture_filter_supported(texture1) ||
            (mip_filter(texture1->min_filter) && texture1->levels != texture1->required_levels))
            return 0;
    }
    state_words = op->two_pass ? 0U : 33U +
        (texture0 != NULL ? texture0->page_count : 0U);
    if (backend->command_words_estimate + state_words > MINIGL_RIX_COMMAND_WORD_BUDGET)
        return 0;
    if (texture0 != NULL) {
        op->texture_slot = (uint16_t)(texture0 - backend->textures);
        op->texture_incarnation = texture0->incarnation;
        op->min_filter = texture0->min_filter;
        op->mag_filter = texture0->mag_filter;
        op->wrap_s = texture0->wrap_s;
        op->wrap_t = texture0->wrap_t;
        if (backend->recorded_pins[op->texture_slot] == 0U) {
            backend->recorded_pins[op->texture_slot] = 1U;
            ++texture0->pin_count;
        }
        backend_touch_texture(backend, texture0);
    }
    if (texture1 != NULL) {
        op->texture1_slot = (uint16_t)(texture1 - backend->textures);
        op->texture1_incarnation = texture1->incarnation;
        op->texture1_min_filter = texture1->min_filter;
        op->texture1_mag_filter = texture1->mag_filter;
        op->texture1_wrap_s = texture1->wrap_s;
        op->texture1_wrap_t = texture1->wrap_t;
        if (backend->recorded_pins[op->texture1_slot] == 0U) {
            backend->recorded_pins[op->texture1_slot] = 1U;
            ++texture1->pin_count;
        }
        backend_touch_texture(backend, texture1);
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
    ZZ9KMiniGLPacketSetState mapped_state;
    uint32_t features;

    if (!minigl_rix_backend_available(backend) || width == 0U || height == 0U)
        return 0;
    mapped_state = backend->state;
    if ((mapped_state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_1) != 0U) {
        if (!backend_multitexture_supported(backend))
            return 0;
        mapped_state.enabled_flags &= ~ZZ9K_MGL_STATE_TEXTURE_2D_1;
    }
    if (!backend_maps_state(&mapped_state, &fragment, &environment, &features))
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
    RixVertex texture1_vertices[3];
    RixScissor scissor;
    RixTriangle prepared;
    RixTriangle prepared_texture1;
    RixTriangle *triangle;
    MiniGLRixRecordedOp *op;
    size_t triangle_words;
    unsigned int passes;
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
        texture1_vertices[0] = (RixVertex){a->x, (float)backend->height - a->y, za,
                                           a->reciprocal_w, a->rgba, a->s1, a->t1, a->q1};
        texture1_vertices[1] = (RixVertex){b->x, (float)backend->height - b->y, zb,
                                           b->reciprocal_w, b->rgba, b->s1, b->t1, b->q1};
        texture1_vertices[2] = (RixVertex){c->x, (float)backend->height - c->y, zc,
                                           c->reciprocal_w, c->rgba, c->s1, c->t1, c->q1};
    }
    passes = (backend->state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_1) != 0U ? 2U : 1U;
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
    if (!rix_setup_triangle(&prepared, &vertices[0], &vertices[1], &vertices[2], &scissor)) {
        /* Zero area at RasterIX's 1/32-pixel precision, or no pixel inside
         * the scissor: the engine would draw nothing. Failing here aborted
         * the whole draw and, through MINIGL_RIX_CPU_FALLBACKS_PER_FRAME,
         * put the rest of the frame on the CPU rasterizer with no logged
         * reason. Slivers from polygon fans and distant models produce
         * such triangles. */
        minigl_profile_count(MGLP_N_CULLED, 1U);
        return 1;
    }
    if (passes == 2U &&
        !rix_setup_triangle(&prepared_texture1, &texture1_vertices[0],
                            &texture1_vertices[1], &texture1_vertices[2], &scissor))
        return 0;
    op = &backend->ops[backend->op_count - 1U];
    triangle_words = passes * (RIX_TRIANGLE_WORDS + 1U);
    if (op->two_pass)
        triangle_words += backend_draw_state_words(backend, op, 0U) +
                          backend_draw_state_words(backend, op, 1U);
    if (backend->triangle_count == MINIGL_RIX_MAX_TRIANGLES ||
        backend->command_words_estimate + triangle_words >
            MINIGL_RIX_COMMAND_WORD_BUDGET) {
        if (backend->triangle_count == 0U && op->triangle_count == 0U)
            return 0;
        if (!backend_flush(backend, 1) || !backend_record_draw(backend))
            return 0;
        op = &backend->ops[backend->op_count - 1U];
        triangle_words = passes * (RIX_TRIANGLE_WORDS + 1U);
        if (op->two_pass)
            triangle_words += backend_draw_state_words(backend, op, 0U) +
                              backend_draw_state_words(backend, op, 1U);
        if (backend->command_words_estimate + triangle_words >
            MINIGL_RIX_COMMAND_WORD_BUDGET)
            return 0;
    }
    triangle = &backend->triangles[backend->triangle_count];
    *triangle = prepared;
    if (passes == 2U)
        backend->texture1_triangles[backend->triangle_count] = prepared_texture1;
    ++backend->triangle_count;
    ++op->triangle_count;
    backend->command_words_estimate += triangle_words;
    minigl_profile_count(MGLP_N_TRIANGLES, 1U);
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

    backend->command_words_estimate = 12U;
    for (op_index = 0U; op_index < backend->op_count; ++op_index) {
        const MiniGLRixRecordedOp *op = &backend->ops[op_index];

        if (op->type == MINIGL_RIX_OP_CLEAR) {
            backend->command_words_estimate += 37U;
        } else {
            if (!op->two_pass) {
                backend->command_words_estimate +=
                    backend_draw_state_words(backend, op, 0U);
                backend->command_words_estimate += op->triangle_count *
                    (RIX_TRIANGLE_WORDS + 1U);
            } else {
                backend->command_words_estimate += op->triangle_count *
                    (2U * (RIX_TRIANGLE_WORDS + 1U) +
                     backend_draw_state_words(backend, op, 0U) +
                     backend_draw_state_words(backend, op, 1U));
            }
        }
    }
}

void minigl_rix_backend_abort_draw(MiniGLRixBackend *backend)
{
    MiniGLRixRecordedOp *op;
    size_t index;
    size_t retained_triangles;
    uint16_t slots[2];
    unsigned int unit;

    if (backend == NULL || !backend->collecting)
        return;
    backend->collecting = 0U;
    if (backend->op_count == 0U)
        return;
    op = &backend->ops[backend->op_count - 1U];
    if (op->type != MINIGL_RIX_OP_DRAW)
        return;
    retained_triangles = op->first_triangle;
    slots[0] = op->texture_slot;
    slots[1] = op->texture1_slot;
    --backend->op_count;
    for (unit = 0U; unit < 2U; ++unit) {
        int still_pinned = 0;

        for (index = 0U; index < backend->op_count; ++index) {
            const MiniGLRixRecordedOp *recorded = &backend->ops[index];

            if (recorded->type == MINIGL_RIX_OP_DRAW &&
                (recorded->texture_slot == slots[unit] ||
                 recorded->texture1_slot == slots[unit])) {
                still_pinned = 1;
                break;
            }
        }
        if (!still_pinned && slots[unit] < MINIGL_RIX_MAX_TEXTURES &&
            backend->recorded_pins[slots[unit]] != 0U) {
            backend->recorded_pins[slots[unit]] = 0U;
            if (backend->textures[slots[unit]].pin_count != 0U)
                --backend->textures[slots[unit]].pin_count;
        }
    }
    backend->triangle_count = retained_triangles;
    backend_recalculate_command_words(backend);
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
        backend->command_words_estimate + 37U >
            MINIGL_RIX_COMMAND_WORD_BUDGET) {
        if (!backend_flush(backend, 1))
            return 0;
    }
    op = &backend->ops[backend->op_count++];
    memset(op, 0, sizeof(*op));
    op->type = MINIGL_RIX_OP_CLEAR;
    op->state = backend->state;
    op->texture_slot = UINT16_MAX;
    op->mask = mask;
    op->color = rgba;
    op->depth = depth;
    backend->command_words_estimate += 37U;
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
    if ((backend->state.enabled_flags & ZZ9K_MGL_STATE_TEXTURE_2D_1) != 0U &&
        !backend_multitexture_supported(backend))
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
