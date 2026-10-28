/* RasterIX backend state gate, texture residency, and double-buffer ownership. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "minigl_rix_backend.h"
#include "rix_texture.h"

#define COMMAND_BYTES (256U * 1024U)
#define TEXTURE_SLOTS 4U
#define GL_NEAREST 0x2600U
#define GL_LINEAR 0x2601U
#define GL_NEAREST_MIPMAP_NEAREST 0x2700U
#define GL_LINEAR_MIPMAP_NEAREST 0x2701U
#define GL_COLOR_BUFFER_BIT 0x00004000U
#define GL_DEPTH_BUFFER_BIT 0x00000100U
#define GL_LINEAR_MIPMAP_LINEAR 0x2703U

struct model {
    uint32_t csr[64];
    uint8_t command[2][COMMAND_BYTES];
    uint8_t white[2048];
    uint8_t textures[TEXTURE_SLOTS * MINIGL_RIX_TEXTURE_VIRTUAL_BYTES];
    size_t texture_bytes;
    unsigned csr_reads;
    unsigned invalidates;
    uintptr_t invalidated_address;
    size_t invalidated_bytes;
};

static uint32_t csr_read(void *context, uint32_t offset)
{
    struct model *model = context;

    ++model->csr_reads;
    return model->csr[offset / 4U];
}

static void csr_write(void *context, uint32_t offset, uint32_t value)
{
    struct model *model = context;

    model->csr[offset / 4U] = value;
    if (offset == RIX_DRV_CSR_CMD_START && value != 0U) {
        ++model->csr[RIX_DRV_CSR_SUBMITTED / 4U];
        model->csr[RIX_DRV_CSR_COMPLETED / 4U] =
            model->csr[RIX_DRV_CSR_SUBMITTED / 4U];
    }
}

static void cache_clean(void *context, uintptr_t address, size_t bytes)
{
    (void)context;
    (void)address;
    (void)bytes;
}
static void cache_invalidate(void *context, uintptr_t address, size_t bytes)
{
    struct model *model = context;

    ++model->invalidates;
    model->invalidated_address = address;
    model->invalidated_bytes = bytes;
}


static MiniGLRixPlatform platform(struct model *model)
{
    MiniGLRixPlatform value;

    memset(&value, 0, sizeof(value));
    value.context = model;
    value.csr_read = csr_read;
    value.csr_write = csr_write;
    value.command_buffer[0] = model->command[0];
    value.command_buffer[1] = model->command[1];
    value.command_address[0] = 0x01000000U;
    value.command_address[1] = 0x01040000U;
    value.command_capacity = COMMAND_BYTES;
    value.white_texture = model->white;
    value.white_texture_address = 0x02000000U;
    value.texture_memory = model->textures;
    value.texture_address = 0x03000000U;
    value.texture_bytes = model->texture_bytes;
    value.cache_clean = cache_clean;
    value.cache_invalidate = cache_invalidate;
    value.white_texture_bytes = sizeof(model->white);
    return value;
}

static ZZ9KMiniGLPacketSetState state16(void)
{
    ZZ9KMiniGLPacketSetState state;

    memset(&state, 0, sizeof(state));
    state.depth_bits = 16U;
    state.depth_func = ZZ9K_MGL_WIRE_LESS;
    state.alpha_func = ZZ9K_MGL_WIRE_ALWAYS;
    state.blend_src = ZZ9K_MGL_WIRE_ONE;
    state.blend_dst = ZZ9K_MGL_WIRE_ZERO;
    state.blend_equation = ZZ9K_MGL_WIRE_FUNC_ADD;
    state.color_mask = 0xfU;
    state.polygon_mode = 0x1b02U;
    state.tex_env_mode[0] = 0x2100U;
    return state;
}

static MiniGLRixVertex vertex(float x, float y, uint32_t color)
{
    MiniGLRixVertex value = { x, y, 0.5f, 1.0f, color, 0.0f, 0.0f, 1.0f };
    return value;
}

static void init_backend(MiniGLRixBackend *backend, struct model *model,
                         size_t slots)
{
    MiniGLRixPlatform io;

    memset(model, 0, sizeof(*model));
    model->texture_bytes = slots * MINIGL_RIX_TEXTURE_VIRTUAL_BYTES;
    model->csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model->csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model->csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model->csr[RIX_DRV_CSR_CMD_START / 4U] = RIX_DRV_CMD_START_QUEUE_READY;
    io = platform(model);
    minigl_rix_backend_init(backend, &io);
    assert(minigl_rix_backend_available(backend));
}

static int has_texture(const MiniGLRixBackend *backend, uint32_t id)
{
    size_t index;

    for (index = 0U; index < MINIGL_RIX_MAX_TEXTURES; ++index)
        if (backend->textures[index].valid && !backend->textures[index].deleted &&
            backend->textures[index].id == id)
            return 1;
    return 0;
}

static int submit_texture(MiniGLRixBackend *backend, uint32_t id)
{
    ZZ9KMiniGLPacketSetState state = state16();
    MiniGLRixVertex a = vertex(10.0f, 10.0f, 0xff0000ffU);
    MiniGLRixVertex b = vertex(60.0f, 10.0f, 0x00ff00ffU);
    MiniGLRixVertex c = vertex(10.0f, 60.0f, 0x0000ffffU);

    state.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0;
    state.tex_env_mode[0] = 0x1e01U;
    minigl_rix_backend_bind_texture(backend, id);
    minigl_rix_backend_set_state(backend, &state);
    return minigl_rix_backend_begin_draw(backend, 64U, 64U, 0x03000000U, 0x03100000U) &&
           minigl_rix_backend_add_triangle(backend, &a, &b, &c) &&
           minigl_rix_backend_end_draw(backend, 0x03000000U, 0x03100000U);
}

static void test_mip_layout_and_filtering(void)
{
    static const uint32_t expected[] = {
        0x00000U, 0x10000U, 0x14000U, 0x15000U, 0x15400U,
        0x15500U, 0x15540U, 0x15550U, 0x15554U
    };
    MiniGLRixBackend backend;
    static struct model model;
    static uint32_t source[256U * 256U];
    size_t level;
    size_t word;
    int stream = 0;

    init_backend(&backend, &model, TEXTURE_SLOTS);
    for (level = 0U; level < sizeof(expected) / sizeof(expected[0]); ++level)
        assert(rix_texture_mip_offset(8U, 8U, (uint8_t)level) == expected[level]);
    source[0] = 0xff0000ffU;
    assert(minigl_rix_backend_upload_texture(&backend, 7U, 0U, source, 256U, 256U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    minigl_rix_backend_tex_params(&backend, 7U, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR,
                                  0x2901U, 0x2901U);
    assert(!submit_texture(&backend, 7U));
    for (level = 1U; level < sizeof(expected) / sizeof(expected[0]); ++level) {
        uint16_t size = (uint16_t)(256U >> level);
        source[0] = (uint32_t)(0x10U + level) << 24U | 0x000000ffU;
        assert(minigl_rix_backend_upload_texture(&backend, 7U, (uint16_t)level,
                                                  source, size, size,
                                                  ZZ9K_MGL_TEX_INTERNAL_RGB));
        assert(model.textures[expected[level] * 2U] == 0U);
    }
    assert(model.textures[0] == 0U && model.textures[1] == 0xf8U);
    assert(!submit_texture(&backend, 7U));
    minigl_rix_backend_tex_params(&backend, 7U, GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR,
                                  0x2901U, 0x2901U);
    assert(submit_texture(&backend, 7U));
    assert(minigl_rix_backend_drain(&backend));
    for (word = 2U; word < COMMAND_BYTES / sizeof(uint32_t); ++word) {
        uint32_t *command = (uint32_t *)model.command[backend.current_buffer];

        if (command[word] == 0x50000080U)
            stream = 1;
        if (command[word] == (RIX_OP_RENDER_CONFIG | RIX_REG_TMU0_TEXTURE))
            assert(command[word + 1U] == 0x00002c88U);
    }
    assert(stream);
}

static void test_downscale_and_residency_lifetime(void)
{
    MiniGLRixBackend backend;
    static struct model model;
    static uint32_t source[300U * 130U];

    init_backend(&backend, &model, 1U);
    source[0] = 0xff8040ffU;
    assert(minigl_rix_backend_upload_texture(&backend, 1U, 0U, source, 300U, 130U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    assert(backend.textures[0].width == 256U && backend.textures[0].height == 128U);
    minigl_rix_backend_tex_params(&backend, 1U, GL_NEAREST, GL_NEAREST,
                                  0x2901U, 0x2901U);
    assert(submit_texture(&backend, 1U));
    assert(backend.textures[0].pin_count == 1U);
    assert(!minigl_rix_backend_upload_texture(&backend, 2U, 0U, source, 3U, 5U,
                                               ZZ9K_MGL_TEX_INTERNAL_RGB));
    minigl_rix_backend_bind_texture(&backend, 2U);
    assert(!submit_texture(&backend, 2U));
    minigl_rix_backend_delete_texture(&backend, 1U);
    assert(!minigl_rix_backend_upload_texture(&backend, 2U, 0U, source, 3U, 5U,
                                               ZZ9K_MGL_TEX_INTERNAL_RGB));
    assert(minigl_rix_backend_drain(&backend));
    assert(minigl_rix_backend_upload_texture(&backend, 2U, 0U, source, 3U, 5U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    assert(!has_texture(&backend, 1U) && has_texture(&backend, 2U));
}

static void test_lru_eviction_and_subimage_reupload(void)
{
    MiniGLRixBackend backend;
    static struct model model;
    static uint32_t source[16U * 16U];

    init_backend(&backend, &model, 2U);
    source[0] = 0xff0000ffU;
    assert(minigl_rix_backend_upload_texture(&backend, 1U, 0U, source, 16U, 16U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    minigl_rix_backend_tex_params(&backend, 1U, GL_NEAREST, GL_NEAREST,
                                  0x2901U, 0x2901U);
    assert(submit_texture(&backend, 1U));
    assert(minigl_rix_backend_drain(&backend));
    assert(minigl_rix_backend_upload_texture(&backend, 2U, 0U, source, 16U, 16U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    minigl_rix_backend_tex_params(&backend, 2U, GL_NEAREST, GL_NEAREST,
                                  0x2901U, 0x2901U);
    assert(submit_texture(&backend, 2U));
    source[0] = 0x00ff00ffU;
    assert(minigl_rix_backend_upload_texture(&backend, 2U, 0U, source, 16U, 16U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    assert(backend.textures[1].pin_count == 0U);
    assert(model.textures[backend.textures[1].offset] == 0xe0U);
    assert(minigl_rix_backend_upload_texture(&backend, 3U, 0U, source, 16U, 16U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    assert(!has_texture(&backend, 1U) && has_texture(&backend, 2U) &&
           has_texture(&backend, 3U));
}

static void test_recorded_frame_replay(void)
{
    MiniGLRixBackend backend;
    static struct model model;
    ZZ9KMiniGLPacketSetState state = state16();
    MiniGLRixVertex a = vertex(10.0f, 10.0f, 0xff0000ffU);
    MiniGLRixVertex b = vertex(60.0f, 10.0f, 0x00ff00ffU);
    MiniGLRixVertex c = vertex(10.0f, 60.0f, 0x0000ffffU);
    uint32_t *command;
    size_t word;
    unsigned reads = 0U;
    unsigned commits = 0U;
    uint32_t surface[64U * 64U];

    init_backend(&backend, &model, TEXTURE_SLOTS);
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U, 0x03100000U,
                                    GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT,
                                    0x102030ffU, 0xffffffffU));
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_drain(&backend));
    command = (uint32_t *)model.command[0];
    for (word = 2U; word < 2U + (command[0] - 0x90000000U) / sizeof(uint32_t); ++word) {
        if ((command[word] & 0xf0000000U) != RIX_OP_FRAMEBUFFER)
            continue;
        if (command[word] & RIX_FRAMEBUFFER_READ)
            ++reads;
        if (command[word] & RIX_FRAMEBUFFER_COMMIT) {
            assert(((command[word] >> 8U) & 0xfffffU) == 64U * 64U);
            ++commits;
        }
    }
    assert(reads == 0U && commits == 1U);
    memset(surface, 0, sizeof(surface));
    assert(minigl_rix_backend_make_cpu_visible(&backend, surface, 64U * 64U));
    surface[0] = 0xff00ffffU;
    assert(minigl_rix_backend_prepare_target(&backend, surface, 64U * 64U));
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_drain(&backend));
    command = (uint32_t *)model.command[1];
    for (word = 2U; word < 2U + (command[0] - 0x90000000U) / sizeof(uint32_t); ++word)
        if ((command[word] & 0xf0000000U) == RIX_OP_FRAMEBUFFER &&
            (command[word] & RIX_FRAMEBUFFER_READ) != 0U)
            ++reads;
    assert(reads == 1U);
}

static void test_clear_preserves_depth_feature(void)
{
    MiniGLRixBackend backend;
    static struct model model;
    ZZ9KMiniGLPacketSetState state = state16();
    uint32_t *command;
    size_t word;

    init_backend(&backend, &model, TEXTURE_SLOTS);
    state.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U, 0x03100000U,
                                    GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT,
                                    0x102030ffU, 0xffffffffU));
    assert(minigl_rix_backend_drain(&backend));
    command = (uint32_t *)model.command[0];
    for (word = 2U; word < 2U + (command[0] - 0x90000000U) / sizeof(uint32_t); ++word)
        if (command[word] == (RIX_OP_RENDER_CONFIG | RIX_REG_FEATURE_ENABLE))
            assert(command[word + 1U] == (RIX_FEATURE_TMU0 | RIX_FEATURE_DEPTH));
}

static void test_overflow_splits_with_reload(void)
{
    MiniGLRixBackend backend;
    static struct model model;
    ZZ9KMiniGLPacketSetState state = state16();
    MiniGLRixVertex a = vertex(10.0f, 10.0f, 0xff0000ffU);
    MiniGLRixVertex b = vertex(60.0f, 10.0f, 0x00ff00ffU);
    MiniGLRixVertex c = vertex(10.0f, 60.0f, 0x0000ffffU);
    size_t triangle;

    init_backend(&backend, &model, TEXTURE_SLOTS);
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    for (triangle = 0U; triangle < 200U; ++triangle)
        assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_drain(&backend));
    assert(model.csr[RIX_DRV_CSR_SUBMITTED / 4U] >= 2U);
}
static void test_depth_mapping_and_ordering(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    ZZ9KMiniGLPacketSetState state;
    MiniGLRixVertex a = vertex(10.0f, 10.0f, 0xff0000ffU);
    MiniGLRixVertex b = vertex(60.0f, 10.0f, 0x00ff00ffU);
    MiniGLRixVertex c = vertex(10.0f, 60.0f, 0x0000ffffU);

    init_backend(&backend, &model, TEXTURE_SLOTS);
    state = state16();
    state.depth_bits = 24U;
    state.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE;
    state.depth_func = ZZ9K_MGL_WIRE_LESS;
    state.depth_far = 0x00ffffffU;
    minigl_rix_backend_set_state(&backend, &state);

    /* 24-bit depth context takes RasterIX path */
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));

    /* 24-bit clear depth normalisation: 0x00ffffff maps to 0xffff */
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U, 0x03100000U,
                                    GL_DEPTH_BUFFER_BIT, 0U, 0x00ffffffU));
    assert(minigl_rix_backend_drain(&backend));

    /* Test LESS/LEQUAL ordering preserved after quantization */
    uint32_t z24_1 = 0x00800000U;
    uint32_t z24_2 = 0x00800100U;
    uint16_t z16_1 = (uint16_t)(z24_1 >> 8U);
    uint16_t z16_2 = (uint16_t)(z24_2 >> 8U);
    assert(z16_1 < z16_2);
    assert(z16_1 <= z16_2);

    uint32_t z24_same1 = 0x00800010U;
    uint32_t z24_same2 = 0x00800080U;
    uint16_t z16_same1 = (uint16_t)(z24_same1 >> 8U);
    uint16_t z16_same2 = (uint16_t)(z24_same2 >> 8U);
    assert(z16_same1 == z16_same2);
    assert(!(z16_same2 < z16_same1));
    assert(z16_same2 <= z16_same1);

    /* Custom depth range */
    state.depth_near = 0x00333333U;
    state.depth_far  = 0x00ccccccU;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_drain(&backend));
}

static void test_fallback_reasons_and_logging(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    ZZ9KMiniGLPacketSetState state;

    init_backend(&backend, &model, TEXTURE_SLOTS);
    state = state16();
    state.depth_bits = 24U;
    minigl_rix_backend_set_state(&backend, &state);

    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_NONE);

    state.reserved = ZZ9K_MGL_STATE_FLAG_FORCE_CPU;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_FORCED_CPU);
    state.reserved = 0U;

    state.depth_bits = 32U;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_NONE);
    state.depth_bits = 24U;

    state.enabled_flags |= ZZ9K_MGL_STATE_FOG;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_FOG);
    state.enabled_flags &= ~ZZ9K_MGL_STATE_FOG;

    state.enabled_flags |= ZZ9K_MGL_STATE_TEXTURE_2D_1;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_MULTITEXTURE);
    state.enabled_flags &= ~ZZ9K_MGL_STATE_TEXTURE_2D_1;

    state.enabled_flags |= ZZ9K_MGL_STATE_POLYGON_OFFSET_FILL;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_POLYGON_OFFSET);
    state.enabled_flags &= ~ZZ9K_MGL_STATE_POLYGON_OFFSET_FILL;

    state.polygon_mode = 0x1b01U;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_POLYGON_MODE);
    state.polygon_mode = 0U;

    state.blend_equation = 0x8007U;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_BLEND_EQUATION);
    state.blend_equation = ZZ9K_MGL_WIRE_FUNC_ADD;

    state.enabled_flags |= ZZ9K_MGL_STATE_TEXTURE_2D_0;
    minigl_rix_backend_bind_texture(&backend, 99U);
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_RESIDENCY_EXHAUSTED);

    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_NONE), "none") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_DEPTH_BITS), "depth bits") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_FOG), "fog") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_MULTITEXTURE), "multitexture") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_POLYGON_OFFSET), "polygon offset") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_POLYGON_MODE), "polygon mode") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_BLEND_EQUATION), "blend equation") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_TEXTURE_FILTER), "texture filter") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_RESIDENCY_EXHAUSTED), "residency exhausted") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_BACKEND_DISABLED_OR_ABSENT), "backend disabled/absent") == 0);
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_FORCED_CPU), "forced CPU") == 0);
}

int main(void)
{
    MiniGLRixBackend backend;
    static struct model model;
    MiniGLRixPlatform io;
    ZZ9KMiniGLPacketSetState state;
    MiniGLRixVertex a = vertex(10.0f, 10.0f, 0xff0000ffU);
    MiniGLRixVertex b = vertex(60.0f, 10.0f, 0x00ff00ffU);
    MiniGLRixVertex c = vertex(10.0f, 60.0f, 0x0000ffffU);

    memset(&model, 0, sizeof(model));
    model.texture_bytes = sizeof(model.textures);
    io = platform(&model);
    minigl_rix_backend_init(&backend, &io);
    assert(!minigl_rix_backend_available(&backend));
    assert(model.csr_reads == 1U);

    model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model.csr[RIX_DRV_CSR_CMD_START / 4U] = RIX_DRV_CMD_START_QUEUE_READY;
    minigl_rix_backend_init(&backend, &io);
    assert(minigl_rix_backend_available(&backend));
    assert(model.white[0] == 0xffU && model.white[1] == 0xffU);

    state = state16();
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_drain(&backend));
    assert(((uint32_t *)model.command[0])[0] > 0x90000000U);
    assert(model.csr[RIX_DRV_CSR_SUBMITTED / 4U] == 1U);
    {
        uint32_t surface[64U * 64U];

        memset(surface, 0, sizeof(surface));
        assert(minigl_rix_backend_make_cpu_visible(&backend, surface,
                                                    64U * 64U));
        assert(model.invalidates == 1U);
        assert(model.invalidated_address == (uintptr_t)surface);
        assert(model.invalidated_bytes == 64U * 64U * 2U);
    }

    state.reserved = ZZ9K_MGL_STATE_FLAG_FORCE_CPU;
    minigl_rix_backend_set_state(&backend, &state);
    assert(!minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    state.reserved = 0U;
    state.depth_bits = 24U;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    /* MiniGL's default 32-bit request runs on RasterIX's documented Z16
     * surface instead of forcing the whole context back to CPU. */
    state.depth_bits = 32U;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));

    test_mip_layout_and_filtering();
    test_downscale_and_residency_lifetime();
    test_lru_eviction_and_subimage_reupload();
    test_recorded_frame_replay();
    test_clear_preserves_depth_feature();
    test_overflow_splits_with_reload();
    test_depth_mapping_and_ordering();
    test_fallback_reasons_and_logging();
    puts("RasterIX backend: frame recording, band replay and texture residency passed");
    return 0;
}
