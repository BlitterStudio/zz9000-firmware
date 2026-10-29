/* RasterIX backend state gate, texture residency, and double-buffer ownership. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "minigl_rix_backend.h"
#include "rix_texture.h"
extern void (*g_rix_test_log)(const char *fmt, ...);

static unsigned g_disabled_logs;
static unsigned g_diagnostic_snapshots;
static unsigned g_self_test_failures;

static void count_log(const char *fmt, ...)
{
    if (strstr(fmt, "backend disabled:") != NULL)
        ++g_disabled_logs;
    if (strstr(fmt, "diag csr") != NULL)
        ++g_diagnostic_snapshots;
    if (strstr(fmt, "self-test failed") != NULL)
        ++g_self_test_failures;
}

#define COMMAND_BYTES MINIGL_RIX_COMMAND_BYTES
#define FIFO_CAPACITY 508U
#define FIFO_PACKET_WORDS (RIX_DRV_FIFO_PACKET_WORDS + 2U)
#define COMMAND_WORDS ((COMMAND_BYTES - MINIGL_RIX_TEXTURE_PAGE_BYTES) / sizeof(uint32_t))
#define TEXTURE_SLOTS 4U
#define GL_NEAREST 0x2600U
#define GL_LINEAR 0x2601U
#define GL_NEAREST_MIPMAP_NEAREST 0x2700U
#define GL_LINEAR_MIPMAP_NEAREST 0x2701U
#define GL_COLOR_BUFFER_BIT 0x00004000U
#define GL_DEPTH_BUFFER_BIT 0x00000100U
#define GL_LINEAR_MIPMAP_LINEAR 0x2703U
#define GL_DECAL 0x2101U
#define GL_REPLACE 0x1e01U
#define GL_MODULATE 0x2100U
#define GL_REPEAT 0x2901U

struct model {
    uint32_t csr[64];
    uint32_t fifo[6];
    uint32_t packet[FIFO_PACKET_WORDS];
    uint8_t command[2][COMMAND_BYTES];
    uint8_t white[2048];
    uint8_t textures[TEXTURE_SLOTS * MINIGL_RIX_TEXTURE_VIRTUAL_BYTES];
    size_t texture_bytes;
    uintptr_t command_address[2];
    uint32_t color_address;
    uint32_t clear_color;
    unsigned csr_reads;
    unsigned fifo_words;
    unsigned packet_count;
    unsigned cleans;
    unsigned invalidates;
    unsigned complete_packets;
    uint32_t fifo_vacancy;
    uintptr_t cleaned_address;
    size_t cleaned_bytes;
    uintptr_t invalidated_address;
    size_t invalidated_bytes;
    unsigned texture_pool_grows;
    unsigned dma_packets;
    int dma_fail;
};

static uint16_t rgba_to_rgb565(uint32_t rgba)
{
    return (uint16_t)(((rgba >> 27U) << 11U) |
                      ((rgba >> 18U & 63U) << 5U) |
                      (rgba >> 11U & 31U));
}

static void render_packet(struct model *model)
{
    size_t word = 2U;

    while (word < model->fifo_words) {
        uint32_t command = model->packet[word++];
        uint32_t command_words = 1U;

        switch (command >> 28U) {
        case 1U:
            assert(word < model->fifo_words);
            if ((command & 0xffU) == RIX_REG_COLOR_BUFFER_ADDR)
                model->color_address = model->packet[word];
            else if ((command & 0xffU) == RIX_REG_CLEAR_COLOR)
                model->clear_color = model->packet[word];
            command_words = 2U;
            break;
        case 2U:
            if ((command & (RIX_FRAMEBUFFER_MEMSET | RIX_FRAMEBUFFER_COLOR)) ==
                (RIX_FRAMEBUFFER_MEMSET | RIX_FRAMEBUFFER_COLOR) &&
                model->complete_packets != 0U) {
                size_t index;

                for (index = 0U; index < 2U; ++index)
                    if (model->color_address ==
                        model->command_address[index] + COMMAND_BYTES -
                        MINIGL_RIX_TEXTURE_PAGE_BYTES)
                        *(uint16_t *)(void *)(model->command[index] +
                            COMMAND_BYTES - MINIGL_RIX_TEXTURE_PAGE_BYTES) =
                            rgba_to_rgb565(model->clear_color);
            }
            break;
        case 3U:
            command_words += (command & 0x3ffU) / sizeof(uint32_t);
            break;
        case 4U:
            command_words = 65U;
            break;
        case 5U:
            command_words += command & 0x7ffffU;
            break;
        default:
            break;
        }
        assert(command_words <= model->fifo_words - word + 1U);
        word += command_words - 1U;
    }
}

static uint32_t fifo_read(void *context, uint32_t offset)
{
    struct model *model = context;

    if (offset == RIX_DRV_FIFO_TDFV)
        return model->fifo_vacancy;
    return model->fifo[offset / sizeof(uint32_t)];
}

static void fifo_write(void *context, uint32_t offset, uint32_t value)
{
    struct model *model = context;

    model->fifo[offset / sizeof(uint32_t)] = value;
    if (offset == RIX_DRV_FIFO_TDFR) {
        assert(value == RIX_DRV_FIFO_RESET_KEY);
        model->fifo_words = 0U;
        model->fifo_vacancy = FIFO_CAPACITY;
    } else if (offset == RIX_DRV_FIFO_TDFD) {
        assert(model->fifo_vacancy != 0U);
        assert(model->fifo_words < FIFO_PACKET_WORDS);
        model->packet[model->fifo_words++] = value;
        --model->fifo_vacancy;
    } else if (offset == RIX_DRV_FIFO_TLF) {
        assert(model->fifo_words >= 2U);
        assert(value == model->fifo_words * sizeof(uint32_t));
        assert(model->packet[0] == (0x90000000U |
                                    (model->fifo_words - 2U) * sizeof(uint32_t)));
        assert(model->packet[1] == 0U);
        render_packet(model);
        ++model->packet_count;
        model->fifo_words = 0U;
        model->fifo_vacancy = FIFO_CAPACITY;
    }
}
static uint32_t csr_read(void *context, uint32_t offset)
{
    struct model *model = context;

    ++model->csr_reads;
    return model->csr[offset / sizeof(uint32_t)];
}

static void csr_write(void *context, uint32_t offset, uint32_t value)
{
    struct model *model = context;

    model->csr[offset / sizeof(uint32_t)] = value;
}


static void cache_clean(void *context, uintptr_t address, size_t bytes)
{
    struct model *model = context;

    ++model->cleans;
    model->cleaned_address = address;
    model->cleaned_bytes = bytes;
}
static void cache_invalidate(void *context, uintptr_t address, size_t bytes)
{
    struct model *model = context;

    ++model->invalidates;
    model->invalidated_address = address;
    model->invalidated_bytes = bytes;
}

static int grow_texture_pool(void *context, size_t minimum_bytes,
                             uint8_t **memory, uintptr_t *address,
                             size_t *bytes)
{
    struct model *model = context;
    size_t capacity = 2U * MINIGL_RIX_TEXTURE_VIRTUAL_BYTES;

    if (minimum_bytes > capacity)
        return 0;
    ++model->texture_pool_grows;
    *memory = model->textures;
    *address = 0x03000000U;
    *bytes = capacity;
    return 1;
}



static MiniGLRixPlatform platform(struct model *model)
{
    MiniGLRixPlatform value;

    memset(&value, 0, sizeof(value));
    value.context = model;
    value.csr_read = csr_read;
    value.csr_write = csr_write;
    value.fifo_read = fifo_read;
    value.fifo_write = fifo_write;
    value.command_buffer[0] = model->command[0];
    value.command_buffer[1] = model->command[1];
    model->command_address[0] = 0x01000000U;
    model->command_address[1] = model->command_address[0] + COMMAND_BYTES;
    value.command_address[0] = model->command_address[0];
    value.command_address[1] = model->command_address[1];
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

/* PL330 stand-in: the payload reaches TDFD exactly as single stores would. */
static int dma_stream(void *context, uintptr_t source, uint32_t words,
                      uint32_t burst_words)
{
    struct model *model = context;
    const uint32_t *payload = NULL;
    size_t index;

    assert(burst_words == 16U && words >= 16U);
    ++model->dma_packets;
    if (model->dma_fail)
        return 0;
    for (index = 0U; index < 2U; ++index)
        if (source >= model->command_address[index] &&
            source < model->command_address[index] + COMMAND_BYTES)
            payload = (const uint32_t *)(void *)(model->command[index] +
                (source - model->command_address[index]));
    assert(payload != NULL);
    for (index = 0U; index < words; ++index)
        fifo_write(model, RIX_DRV_FIFO_TDFD, payload[index]);
    return 1;
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
    MiniGLRixVertex value = { x, y, 0.5f, 1.0f, color,
                              0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f };
    return value;
}

static void init_backend(MiniGLRixBackend *backend, struct model *model,
                         size_t pages)
{
    MiniGLRixPlatform io;

    memset(model, 0, sizeof(*model));
    model->texture_bytes = pages * MINIGL_RIX_TEXTURE_PAGE_BYTES;
    model->complete_packets = 1U;
    model->fifo_vacancy = FIFO_CAPACITY;
    model->csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model->csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model->csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
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
    minigl_rix_backend_bind_texture(backend, 0U, id);
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

    init_backend(&backend, &model, MINIGL_RIX_TEXTURE_PAGES);
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
    for (word = 0U; word < COMMAND_WORDS; ++word) {
        uint32_t *command = (uint32_t *)model.command[backend.current_buffer];

        if (command[word] == (RIX_OP_TEXTURE_STREAM | backend.textures[0].page_count))
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

    init_backend(&backend, &model, 64U);
    source[0] = 0xff8040ffU;
    assert(minigl_rix_backend_upload_texture(&backend, 1U, 0U, source, 300U, 130U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    assert(backend.textures[0].width == 256U && backend.textures[0].height == 128U);
    minigl_rix_backend_tex_params(&backend, 1U, GL_NEAREST, GL_NEAREST,
                                  0x2901U, 0x2901U);
    assert(submit_texture(&backend, 1U));
    assert(backend.textures[0].pin_count == 1U);
    assert(minigl_rix_backend_upload_texture(&backend, 2U, 0U, source, 3U, 5U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    minigl_rix_backend_tex_params(&backend, 2U, GL_NEAREST, GL_NEAREST,
                                  0x2901U, 0x2901U);
    minigl_rix_backend_bind_texture(&backend, 0U, 2U);
    assert(submit_texture(&backend, 2U));
    assert(minigl_rix_backend_drain(&backend));
    minigl_rix_backend_delete_texture(&backend, 1U);
    assert(!has_texture(&backend, 1U) && has_texture(&backend, 2U));
}

static void test_texture_pool_growth(void)
{
    MiniGLRixBackend backend;
    static struct model model;
    static uint32_t source[256U * 256U];

    init_backend(&backend, &model, MINIGL_RIX_TEXTURE_PAGES);
    backend.platform.grow_texture_pool = grow_texture_pool;
    assert(minigl_rix_backend_upload_texture(&backend, 1U, 0U, source, 256U,
                                              256U, ZZ9K_MGL_TEX_INTERNAL_RGB));
    assert(minigl_rix_backend_upload_texture(&backend, 2U, 0U, source, 256U,
                                              256U, ZZ9K_MGL_TEX_INTERNAL_RGB));
    assert(model.texture_pool_grows == 1U);
    assert(backend.platform.texture_bytes ==
           2U * MINIGL_RIX_TEXTURE_VIRTUAL_BYTES);
    assert(has_texture(&backend, 1U) && has_texture(&backend, 2U));
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
    assert(model.textures[(size_t)backend.textures[1].first_page *
                          MINIGL_RIX_TEXTURE_PAGE_BYTES] == 0xe0U);
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
    unsigned memsets = 0U;
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
    for (word = 0U; word < COMMAND_WORDS; ++word) {
        if ((command[word] & 0xf0000000U) != RIX_OP_FRAMEBUFFER)
            continue;
        if (command[word] & RIX_FRAMEBUFFER_READ)
            ++reads;
        if (command[word] & RIX_FRAMEBUFFER_COMMIT)
            ++commits;
        if (command[word] & RIX_FRAMEBUFFER_MEMSET)
            ++memsets;
    }
    assert(reads == 0U && commits == 0U && memsets == 2U);
    memset(surface, 0, sizeof(surface));
    assert(minigl_rix_backend_make_cpu_visible(&backend, surface, 64U * 64U));
    surface[0] = 0xff00ffffU;
    assert(minigl_rix_backend_prepare_target(&backend, surface, 64U * 64U));
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_drain(&backend));
    command = (uint32_t *)model.command[1];
    for (word = 0U; word < COMMAND_WORDS; ++word)
        if ((command[word] & 0xf0000000U) == RIX_OP_FRAMEBUFFER &&
            (command[word] & RIX_FRAMEBUFFER_READ) != 0U)
            ++reads;
    assert(reads == 0U);
}

static void test_clear_preserves_depth_feature(void)
{
    MiniGLRixBackend backend;
    static struct model model;
    ZZ9KMiniGLPacketSetState state = state16();
    uint32_t *command;
    size_t word;
    int found_depth_feature = 0;
    init_backend(&backend, &model, TEXTURE_SLOTS);
    state.enabled_flags = ZZ9K_MGL_STATE_DEPTH_TEST | ZZ9K_MGL_STATE_DEPTH_WRITE;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U, 0x03100000U,
                                    GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT,
                                    0x102030ffU, 0xffffffffU));
    assert(minigl_rix_backend_drain(&backend));
    command = (uint32_t *)model.command[0];
    for (word = 0U; word + 1U < COMMAND_WORDS; ++word)
        if (command[word] == (RIX_OP_RENDER_CONFIG | RIX_REG_FEATURE_ENABLE) &&
            command[word + 1U] == (RIX_FEATURE_TMU0 | RIX_FEATURE_DEPTH))
            found_depth_feature = 1;
    assert(found_depth_feature);
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
    assert(model.packet_count >= 2U);
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
    minigl_rix_backend_bind_texture(&backend, 0U, 99U);
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
static void test_timeout_diagnostics(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    ZZ9KMiniGLPacketSetState state = state16();
    MiniGLRixVertex a = vertex(10.0f, 10.0f, 0xff0000ffU);
    MiniGLRixVertex b = vertex(60.0f, 10.0f, 0x00ff00ffU);
    MiniGLRixVertex c = vertex(10.0f, 60.0f, 0x0000ffffU);

    init_backend(&backend, &model, TEXTURE_SLOTS);
    model.complete_packets = 0U;
    g_diagnostic_snapshots = 0U;
    g_rix_test_log = count_log;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    assert(!minigl_rix_backend_drain(&backend));
    assert(g_diagnostic_snapshots == 1U);
    assert(model.invalidates != 0U);
    g_rix_test_log = NULL;
}

static void init_dma_backend(MiniGLRixBackend *backend, struct model *model,
                             int fail)
{
    MiniGLRixPlatform io;

    memset(model, 0, sizeof(*model));
    model->texture_bytes = TEXTURE_SLOTS * MINIGL_RIX_TEXTURE_PAGE_BYTES;
    model->complete_packets = 1U;
    model->fifo_vacancy = FIFO_CAPACITY;
    model->dma_fail = fail;
    model->csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model->csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model->csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    io = platform(model);
    io.dma_stream = dma_stream;
    minigl_rix_backend_init(backend, &io);
    assert(minigl_rix_backend_available(backend));
}

static void test_dma_self_test_gates_transport(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    ZZ9KMiniGLPacketSetState state = state16();
    MiniGLRixVertex a = vertex(10.0f, 10.0f, 0xff0000ffU);
    MiniGLRixVertex b = vertex(60.0f, 10.0f, 0x00ff00ffU);
    MiniGLRixVertex c = vertex(10.0f, 60.0f, 0x0000ffffU);
    unsigned dma_packets;

    g_self_test_failures = 0U;
    g_diagnostic_snapshots = 0U;
    g_rix_test_log = count_log;

    /* A delivered DMA packet lands the self-test fence and keeps the PL330. */
    init_dma_backend(&backend, &model, 0);
    assert(backend.driver.dma_stream != NULL);
    assert(model.dma_packets == 1U && model.packet_count == 1U);
    assert(*(uint16_t *)(void *)(model.command[0] + COMMAND_BYTES -
                                 MINIGL_RIX_TEXTURE_PAGE_BYTES) == 0xa55aU);
    assert(g_self_test_failures == 0U && g_diagnostic_snapshots == 0U);
    dma_packets = model.dma_packets;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_drain(&backend));
    assert(model.dma_packets > dma_packets);

    /* A failed DMA packet falls back to MMIO after a reset, not to the CPU. */
    init_dma_backend(&backend, &model, 1);
    assert(backend.driver.dma_stream == NULL);
    assert(model.dma_packets == 1U && model.packet_count == 0U);
    assert(g_self_test_failures == 1U && g_diagnostic_snapshots == 1U);
    assert(model.csr[RIX_DRV_CSR_CONTROL / 4U] == RIX_DRV_CONTROL_ENABLE);
    assert(model.fifo_words == 0U && model.fifo_vacancy == FIFO_CAPACITY);
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_drain(&backend));
    assert(model.dma_packets == 1U && model.packet_count >= 1U);
    g_rix_test_log = NULL;
}

static void test_surface_handoff_and_recorded_texture_state(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    ZZ9KMiniGLPacketSetState state = state16();
    MiniGLRixVertex a = vertex(10.0f, 10.0f, 0xff0000ffU);
    MiniGLRixVertex b = vertex(60.0f, 10.0f, 0x00ff00ffU);
    MiniGLRixVertex c = vertex(10.0f, 60.0f, 0x0000ffffU);
    uint32_t image[4] = { 0xffffffffU, 0U, 0U, 0U };
    uint32_t surface[2] = { 0xf80000ffU, 0x0000ffffU };
    uint16_t old_slot;
    uint32_t old_incarnation;
    size_t slot;
    unsigned cleans_before;

    init_backend(&backend, &model, TEXTURE_SLOTS);
    cleans_before = model.cleans;
    assert(minigl_rix_backend_prepare_target(&backend, surface, 2U));
    assert(model.cleans == cleans_before + 1U &&
           model.cleaned_address == (uintptr_t)surface);
    assert(model.cleaned_bytes == 2U * sizeof(uint16_t));
    assert(minigl_rix_backend_make_cpu_visible(&backend, surface, 2U));
    assert(surface[0] == 0xff0000ffU && surface[1] == 0x0000ffffU);

    state.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0;
    state.tex_env_mode[0] = 0x1e01U;
    minigl_rix_backend_set_state(&backend, &state);
    minigl_rix_backend_bind_texture(&backend, 0U, 99U);
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U, 0x03100000U,
                                    GL_COLOR_BUFFER_BIT, 0U, 0U));
    assert(backend.ops[0].texture_slot == UINT16_MAX);
    assert(minigl_rix_backend_drain(&backend));

    assert(minigl_rix_backend_upload_texture(&backend, 7U, 0U, image, 2U, 2U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    minigl_rix_backend_tex_params(&backend, 7U, GL_NEAREST, GL_NEAREST,
                                  0x2901U, 0x2901U);
    state.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0;
    state.tex_env_mode[0] = 0x1e01U;
    minigl_rix_backend_set_state(&backend, &state);
    minigl_rix_backend_bind_texture(&backend, 0U, 7U);
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    old_slot = backend.ops[0].texture_slot;
    old_incarnation = backend.ops[0].texture_incarnation;
    minigl_rix_backend_tex_params(&backend, 7U, GL_LINEAR, GL_LINEAR,
                                  0x812fU, 0x812fU);
    assert(backend.ops[0].min_filter == GL_NEAREST &&
           backend.ops[0].wrap_s == 0x2901U);
    assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    minigl_rix_backend_delete_texture(&backend, 7U);
    assert(minigl_rix_backend_upload_texture(&backend, 7U, 0U, image, 2U, 2U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    assert(backend.textures[old_slot].deleted &&
           backend.textures[old_slot].incarnation == old_incarnation);
    for (slot = 0U; slot < MINIGL_RIX_MAX_TEXTURES; ++slot)
        if (backend.textures[slot].valid && !backend.textures[slot].deleted &&
            backend.textures[slot].id == 7U)
            break;
    assert(slot < MINIGL_RIX_MAX_TEXTURES &&
           backend.textures[slot].incarnation != old_incarnation);
    assert(backend.ops[0].texture_slot == old_slot &&
           backend.ops[0].texture_incarnation == old_incarnation);
    assert(minigl_rix_backend_drain(&backend));
}

static void test_zero_fence_remains_pending(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    ZZ9KMiniGLPacketSetState state = state16();
    MiniGLRixVertex a = vertex(10.0f, 10.0f, 0xff0000ffU);
    MiniGLRixVertex b = vertex(60.0f, 10.0f, 0x00ff00ffU);
    MiniGLRixVertex c = vertex(10.0f, 60.0f, 0x0000ffffU);

    init_backend(&backend, &model, TEXTURE_SLOTS);
    backend.driver.submitted_target = UINT32_MAX;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_drain(&backend));
    assert(!backend.pending[0] && backend.pending_fence[0] == 0U);
}

static void test_abort_draw_rolls_back_recording(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    ZZ9KMiniGLPacketSetState state = state16();
    MiniGLRixVertex a = vertex(10.0f, 10.0f, 0xff0000ffU);
    MiniGLRixVertex b = vertex(60.0f, 10.0f, 0x00ff00ffU);
    MiniGLRixVertex c = vertex(10.0f, 60.0f, 0x0000ffffU);

    init_backend(&backend, &model, TEXTURE_SLOTS);
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    minigl_rix_backend_abort_draw(&backend);
    assert(!backend.collecting && backend.op_count == 0U && backend.triangle_count == 0U);
    assert(backend.command_words_estimate == 12U);
    assert(minigl_rix_backend_drain(&backend));
}

static void test_disabled_backend_is_one_shot(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    ZZ9KMiniGLPacketSetState state = state16();
    MiniGLRixVertex a = vertex(10.0f, 10.0f, 0xff0000ffU);
    MiniGLRixVertex b = vertex(60.0f, 10.0f, 0x00ff00ffU);
    MiniGLRixVertex c = vertex(10.0f, 60.0f, 0x0000ffffU);
    unsigned reads_after_disable;

    init_backend(&backend, &model, TEXTURE_SLOTS);
    model.complete_packets = 0U;
    g_disabled_logs = 0U;
    g_rix_test_log = count_log;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    assert(!minigl_rix_backend_drain(&backend));
    assert(!minigl_rix_backend_available(&backend));
    reads_after_disable = model.csr_reads;
    assert(!minigl_rix_backend_drain(&backend));
    assert(model.csr_reads == reads_after_disable);
    assert(g_disabled_logs == 1U);
    g_rix_test_log = NULL;
}

static void test_two_texture_multipass_recording(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    ZZ9KMiniGLPacketSetState state = state16();
    MiniGLRixVertex a = vertex(10.0f, 10.0f, 0xffffffffU);
    MiniGLRixVertex b = vertex(60.0f, 10.0f, 0xffffffffU);
    MiniGLRixVertex c = vertex(10.0f, 60.0f, 0xffffffffU);
    MiniGLRixVertex d = vertex(60.0f, 60.0f, 0xffffffffU);
    uint32_t diffuse[16] = { 0xff8040ffU };
    uint32_t lightmap[16] = { 0x808080ffU };
    uint32_t *command;
    size_t word;
    unsigned triangles = 0U;

    init_backend(&backend, &model, TEXTURE_SLOTS);
    assert(minigl_rix_backend_upload_texture(&backend, 1U, 0U, diffuse, 4U, 4U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    assert(minigl_rix_backend_upload_texture(&backend, 2U, 0U, lightmap, 4U, 4U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    minigl_rix_backend_tex_params(&backend, 1U, GL_NEAREST, GL_NEAREST,
                                  GL_REPEAT, GL_REPEAT);
    minigl_rix_backend_tex_params(&backend, 2U, GL_NEAREST, GL_NEAREST,
                                  GL_REPEAT, GL_REPEAT);
    minigl_rix_backend_bind_texture(&backend, 0U, 1U);
    minigl_rix_backend_bind_texture(&backend, 1U, 2U);
    state.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0 |
                          ZZ9K_MGL_STATE_TEXTURE_2D_1 |
                          ZZ9K_MGL_STATE_DEPTH_TEST |
                          ZZ9K_MGL_STATE_DEPTH_WRITE;
    state.depth_func = ZZ9K_MGL_WIRE_LEQUAL;
    state.tex_env_mode[0] = GL_REPLACE;
    state.tex_env_mode[1] = GL_MODULATE;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_NONE);
    state.depth_func = ZZ9K_MGL_WIRE_LESS;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_MULTITEXTURE);
    state.depth_func = ZZ9K_MGL_WIRE_LEQUAL;
    minigl_rix_backend_set_state(&backend, &state);
    a.s1 = 0.0f; a.t1 = 0.0f;
    b.s1 = 1.0f; b.t1 = 0.0f;
    c.s1 = 0.0f; c.t1 = 1.0f;
    d.s1 = 1.0f; d.t1 = 1.0f;
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_add_triangle(&backend, &b, &d, &c));
    assert(backend.ops[0].two_pass);
    assert(backend.ops[0].texture_slot != backend.ops[0].texture1_slot);
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_drain(&backend));
    command = (uint32_t *)model.command[0];
    for (word = 0U; word < COMMAND_WORDS; ++word)
        if (command[word] == (RIX_OP_TRIANGLE_STREAM | RIX_TRIANGLE_WORDS * 4U))
            ++triangles;
    assert(triangles == 4U);

    state.enabled_flags |= ZZ9K_MGL_STATE_BLEND;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_MULTITEXTURE);
    state.enabled_flags &= ~ZZ9K_MGL_STATE_BLEND;
    state.tex_env_mode[1] = GL_DECAL;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_eval_fallback(&backend) == ZZ9K_MGL_FALLBACK_MULTITEXTURE);
}

/* A triangle RasterIX setup finds empty (collinear at 1/32 px) draws
 * nothing on the engine: it is dropped and the draw stays on RasterIX
 * instead of failing over to the CPU for the rest of the frame. */
static void test_empty_triangle_stays_on_rasterix(void)
{
    MiniGLRixBackend backend;
    static struct model model;
    ZZ9KMiniGLPacketSetState state = state16();
    MiniGLRixVertex a = vertex(10.0f, 10.0f, 0xff0000ffU);
    MiniGLRixVertex b = vertex(60.0f, 10.0f, 0x00ff00ffU);
    MiniGLRixVertex c = vertex(10.0f, 60.0f, 0x0000ffffU);
    MiniGLRixVertex sliver = vertex(35.0f, 10.001f, 0x0000ffffU);

    init_backend(&backend, &model, TEXTURE_SLOTS);
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &sliver));
    assert(backend.triangle_count == 0U);
    assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(backend.triangle_count == 1U);
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_drain(&backend));
    assert(minigl_rix_backend_available(&backend));
    assert(model.packet_count >= 1U);
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
    model.fifo_vacancy = FIFO_CAPACITY;
    model.complete_packets = 1U;
    minigl_rix_backend_init(&backend, &io);
    assert(minigl_rix_backend_available(&backend));
    assert(model.white[0] == 0xffU && model.white[1] == 0xffU);

    state = state16();
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_drain(&backend));
    assert((((uint32_t *)model.command[0])[0] & 0xf0000000U) ==
           RIX_OP_RENDER_CONFIG);
    assert(model.packet_count >= 1U);
    assert(*(uint16_t *)(void *)(model.command[0] + COMMAND_BYTES -
                                 MINIGL_RIX_TEXTURE_PAGE_BYTES) == 1U);
    {
        uint32_t surface[64U * 64U];
        unsigned int invalidates = model.invalidates;

        memset(surface, 0, sizeof(surface));
        assert(minigl_rix_backend_make_cpu_visible(&backend, surface,
                                                    64U * 64U));
        assert(model.invalidates == invalidates + 1U);
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
    test_texture_pool_growth();
    test_recorded_frame_replay();
    test_clear_preserves_depth_feature();
    test_overflow_splits_with_reload();
    test_depth_mapping_and_ordering();
    test_fallback_reasons_and_logging();
    test_timeout_diagnostics();
    test_abort_draw_rolls_back_recording();
    test_zero_fence_remains_pending();
    test_surface_handoff_and_recorded_texture_state();
    test_disabled_backend_is_one_shot();
    test_two_texture_multipass_recording();
    test_dma_self_test_gates_transport();
    test_empty_triangle_stays_on_rasterix();
    puts("RasterIX EF backend: frame recording, FIFO replay and texture residency passed");
    return 0;
}
