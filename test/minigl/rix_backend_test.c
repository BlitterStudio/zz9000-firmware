/* RasterIX backend state gate, texture residency, and double-buffer ownership. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "card_pool.h"
#include "minigl_fence_guard.h"
#include "minigl_rix_backend.h"
#include "rix_texture.h"
extern void (*g_rix_test_log)(const char *fmt, ...);

static unsigned g_disabled_logs;
static unsigned g_diagnostic_snapshots;
static unsigned g_self_test_failures;
static unsigned g_watchdog_logs;
static uint64_t g_now_us;
int smp_cpu_id(void) { return 0; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t state) { (void)state; }
void smp_raw_spin_lock(volatile uint32_t *word) { *word = 1U; }
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word)
        return 0;
    *word = 1U;
    return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *word) { *word = 0U; }


static void count_log(const char *fmt, ...)
{
    if (strstr(fmt, "backend disabled:") != NULL)
        ++g_disabled_logs;
    if (strstr(fmt, "diag csr") != NULL)
        ++g_diagnostic_snapshots;
    if (strstr(fmt, "wait watchdog") != NULL)
        ++g_watchdog_logs;
    if (strstr(fmt, "self-test failed") != NULL)
        ++g_self_test_failures;
}

uint64_t minigl_platform_time_us(void)
{
    return g_now_us;
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
    uint8_t command[MINIGL_RIX_COMMAND_BUFFERS][COMMAND_BYTES];
    uint8_t white[2048];
    uint8_t textures[TEXTURE_SLOTS * MINIGL_RIX_TEXTURE_VIRTUAL_BYTES];
    size_t texture_bytes;
    uintptr_t command_address[MINIGL_RIX_COMMAND_BUFFERS];
    uint32_t color_address;
    uint32_t clear_color;
    unsigned csr_reads;
    unsigned fifo_words;
    unsigned packet_count;
    unsigned cleans;
    unsigned invalidates;
    unsigned complete_packets;
    uint32_t fifo_vacancy;
    uint32_t last_tlf;
    unsigned fifo_reads;
    uintptr_t cleaned_address;
    size_t cleaned_bytes;
    uintptr_t invalidated_address;
    size_t invalidated_bytes;
    unsigned texture_pool_grows;
    unsigned dma_packets;
    int dma_fail;
    MiniGLRixBackend *yield_backend;
    unsigned yield_calls;
    int dma_launch_error;
    int dma_launch_state;
    unsigned dma_pending_grants;
    int dma_complete_state;
    int dma_abort_state;
    unsigned fence_after_yields;
    unsigned idle_after_yields;
    unsigned rendering_yields;
    unsigned fence_after_rendering_yields;
    unsigned fence_signal_yield;
    unsigned idle_after_fence_yields;
    int fence_signaled;
    int fail_feed_on_yield;
    int fail_memfetch_commit;
    unsigned fail_memfetch_after_packet;
    unsigned command_count;
    /* Parser state carried across packets and envelopes: a DDR list chunk
     * may end inside a command. */
    uint32_t parse_skip;
    uint32_t parse_reg;       /* register number + 1 awaiting its value */
    unsigned triangles_parsed;
    uintptr_t published_address;
    uint32_t published_sequence;
    unsigned retired_publications;
    unsigned publish_count;
    unsigned guard_sleeps;
    uint32_t quarantined_mask;
};

static uint16_t rgba_to_rgb565(uint32_t rgba)
{
    return (uint16_t)(((rgba >> 27U) << 11U) |
                      ((rgba >> 18U & 63U) << 5U) |
                      (rgba >> 11U & 31U));
}

static void render_word(struct model *model, uint32_t word)
{
    if (model->parse_skip != 0U) {
        --model->parse_skip;
        return;
    }
    if (model->parse_reg != 0U) {
        if (model->parse_reg - 1U == RIX_REG_COLOR_BUFFER_ADDR)
            model->color_address = word;
        else if (model->parse_reg - 1U == RIX_REG_CLEAR_COLOR)
            model->clear_color = word;
        model->parse_reg = 0U;
        return;
    }
    switch (word >> 28U) {
    case 1U:
        model->parse_reg = (word & 0xffU) + 1U;
        break;
    case 2U:
        if ((word & (RIX_FRAMEBUFFER_MEMSET | RIX_FRAMEBUFFER_COLOR)) ==
            (RIX_FRAMEBUFFER_MEMSET | RIX_FRAMEBUFFER_COLOR) &&
            model->complete_packets != 0U) {
            size_t index;

            for (index = 0U; index < model->command_count; ++index)
                if (model->color_address ==
                    model->command_address[index] + COMMAND_BYTES -
                    MINIGL_RIX_TEXTURE_PAGE_BYTES)
                    *(uint16_t *)(void *)(model->command[index] +
                        COMMAND_BYTES - MINIGL_RIX_TEXTURE_PAGE_BYTES) =
                        rgba_to_rgb565(model->clear_color);
        }
        break;
    case 3U:
        model->parse_skip = (word & 0x3ffU) / sizeof(uint32_t);
        ++model->triangles_parsed;
        break;
    case 4U:
        model->parse_skip = 64U;
        break;
    case 5U:
        model->parse_skip = word & 0x7ffffU;
        break;
    default:
        break;
    }
}

static void render_commands(struct model *model, const uint32_t *commands,
                            size_t command_count)
{
    size_t word;

    for (word = 0U; word < command_count; ++word)
        render_word(model, commands[word]);
}

static uint32_t fifo_read(void *context, uint32_t offset)
{
    struct model *model = context;

    ++model->fifo_reads;
    if (offset == RIX_DRV_FIFO_TDFV)
        return model->fifo_vacancy;
    return model->fifo[offset / sizeof(uint32_t)];
}

static void fifo_write(void *context, uint32_t offset, uint32_t value)
{
    struct model *model = context;

    if (offset != RIX_DRV_FIFO_TDFD)
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
        model->last_tlf = value;
        assert(model->fifo_words >= 2U);
        if ((model->packet[0] & 0xf0000000U) == RIX_DRV_MEMFETCH_ENVELOPE) {
            const uint32_t *commands = NULL;
            uint32_t bytes = model->packet[0] & 0x0fffffffU;
            uintptr_t address = model->packet[1];
            size_t index;

            assert(value == 2U * sizeof(uint32_t));
            assert(model->fifo_words == 2U);
            assert((bytes & (RIX_DRV_MEMFETCH_ALIGN_BYTES - 1U)) == 0U);
            for (index = 0U; index < model->command_count; ++index)
                if (address >= model->command_address[index] &&
                    address + bytes <= model->command_address[index] +
                                       COMMAND_BYTES)
                    commands = (const uint32_t *)(void *)(
                        model->command[index] +
                        (address - model->command_address[index]));
            assert(commands != NULL);
            render_commands(model, commands, bytes / sizeof(uint32_t));
        } else {
            assert(value == model->fifo_words * sizeof(uint32_t));
            assert(model->packet[0] == (0x90000000U |
                                        (model->fifo_words - 2U) *
                                        sizeof(uint32_t)));
            assert(model->packet[1] == 0U);
            render_commands(model, model->packet + 2U,
                            model->fifo_words - 2U);
            /* An inline packet always ends on a command boundary. */
            assert(model->parse_skip == 0U && model->parse_reg == 0U);
        }
        ++model->packet_count;
        if (model->fail_memfetch_commit != 0 ||
            (model->fail_memfetch_after_packet != 0U &&
             model->packet_count >= model->fail_memfetch_after_packet))
            model->fifo[RIX_DRV_FIFO_ISR / sizeof(uint32_t)] = RIX_DRV_FIFO_ISR_ERRORS;
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



static void publish_fence(void *context, uintptr_t address, uint32_t sequence)
{
    struct model *model = context;

    model->published_address = address;
    model->published_sequence = sequence;
    if (address == 0U && sequence == 0U)
        ++model->retired_publications;
    else
        ++model->publish_count;
}
static int guard_snapshot(void *context, uint32_t *address, uint32_t *sequence)
{
    struct model *model = context;

    *address = (uint32_t)model->published_address;
    *sequence = model->published_sequence;
    return *address != 0U;
}

static uint16_t guard_read(void *context, uint32_t address)
{
    (void)context;
    (void)address;
    return 0U;
}

static int guard_idle(void *context)
{
    (void)context;
    return 1;
}

static uint32_t guard_clock(void *context)
{
    (void)context;
    return 0U;
}

static void guard_sleep(void *context, uint32_t milliseconds)
{
    struct model *model = context;

    (void)milliseconds;
    ++model->guard_sleeps;
}

static const minigl_fence_guard_hooks_t guard_hooks = {
    .snapshot = guard_snapshot,
    .read_fence = guard_read,
    .engine_idle = guard_idle,
    .clock_ms = guard_clock,
    .sleep_ms = guard_sleep
};

static uint32_t owner_for_address(void *context, uintptr_t address)
{
    struct model *model = context;

    if (address >= model->command_address[0] &&
        address < model->command_address[MINIGL_RIX_COMMAND_BUFFERS - 1U] +
                  COMMAND_BYTES)
        return 1U;
    if (address == 0x03000000U)
        return 2U;
    return address == 0x03100000U ? 3U : 0U;
}

static void quarantine_owner(void *context, uint32_t owner)
{
    ((struct model *)context)->quarantined_mask |= 1U << (owner - 1U);
}


static MiniGLRixPlatform platform(struct model *model)
{
    MiniGLRixPlatform value;
    if (model->command_count == 0U)
        model->command_count = 2U;

    memset(&value, 0, sizeof(value));
    value.context = model;
    value.csr_read = csr_read;
    value.csr_write = csr_write;
    value.fifo_read = fifo_read;
    value.fifo_write = fifo_write;
    for (unsigned index = 0U; index < MINIGL_RIX_COMMAND_BUFFERS; ++index) {
        value.command_buffer[index] = model->command[index];
        model->command_address[index] = 0x01000000U + index * COMMAND_BYTES;
        value.command_address[index] = model->command_address[index];
    }
    value.command_capacity = COMMAND_BYTES;
    value.white_texture = model->white;
    value.white_texture_address = 0x02000000U;
    value.texture_memory = model->textures;
    value.texture_address = 0x03000000U;
    value.texture_bytes = model->texture_bytes;
    value.cache_clean = cache_clean;
    value.cache_invalidate = cache_invalidate;
    value.white_texture_bytes = sizeof(model->white);
    value.publish_fence = publish_fence;
    value.owner_for_address = owner_for_address;
    value.quarantine_owner = quarantine_owner;
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
    for (index = 0U; index < model->command_count; ++index)
        if (source >= model->command_address[index] &&
            source < model->command_address[index] + COMMAND_BYTES)
            payload = (const uint32_t *)(void *)(model->command[index] +
                (source - model->command_address[index]));
    assert(payload != NULL);
    for (index = 0U; index < words; ++index)
        fifo_write(model, RIX_DRV_FIFO_TDFD, payload[index]);
    return 1;
}

/* The async launcher used for overlapped packets: same delivery, retired
 * before the next packet. */
static int dma_start_async(void *context, uintptr_t source, uint32_t words,
                           uint32_t burst_words)
{
    return dma_stream(context, source, words, burst_words);
}

static int dma_retire(void *context)
{
    (void)context;
    return 1;
}

static int dma_ready(void *context)
{
    (void)context;
    return 1;
}

static int dma_start_nonblocking(void *context, uintptr_t source, uint32_t words,
                                 uint32_t burst_words)
{
    struct model *model = context;

    (void)source;
    assert(words >= 16U && burst_words == 16U);
    return model->dma_launch_error ? RIX_DRV_DMA_ERROR : RIX_DRV_DMA_COMPLETE;
}

static int dma_state(void *context)
{
    struct model *model = context;

    return model->dma_launch_error;
}

static int dma_abort_state(void *context)
{
    struct model *model = context;

    return model->dma_fail;
}

static int dma_start_state(void *context, uintptr_t source, uint32_t words,
                           uint32_t burst_words)
{
    struct model *model = context;

    (void)source;
    assert(words >= 16U && burst_words == 16U);
    return model->dma_launch_error;
}

/* The eight-buffer async fake can hold every hardware completion boundary
 * independently: launch handshake, DMA retirement, fence, and renderer idle. */
static int dma_start_delayed(void *context, uintptr_t source, uint32_t words,
                             uint32_t burst_words)
{
    struct model *model = context;

    if (model->dma_pending_grants != 0U) {
        --model->dma_pending_grants;
        return RIX_DRV_DMA_PENDING;
    }
    if (model->dma_launch_state != RIX_DRV_DMA_COMPLETE)
        return model->dma_launch_state;
    return dma_stream(context, source, words, burst_words) ?
           RIX_DRV_DMA_COMPLETE : RIX_DRV_DMA_ERROR;
}

static int dma_complete_delayed(void *context)
{
    return ((struct model *)context)->dma_complete_state;
}

static int dma_abort_delayed(void *context)
{
    return ((struct model *)context)->dma_abort_state;
}

static void yield_delayed_completion(void *context)
{
    struct model *model = context;
    MiniGLRixBackend *backend = model->yield_backend;
    unsigned index;
    int rendering = 0;

    assert(backend != NULL);
    ++model->yield_calls;
    if (model->fail_feed_on_yield)
        model->dma_launch_state = RIX_DRV_DMA_ERROR;
    for (index = 0U; index < backend->command_buffer_count; ++index)
        if (backend->ring_state[index] == MINIGL_RIX_RING_RENDERING)
            rendering = 1;
    if (rendering)
        ++model->rendering_yields;
    if ((model->fence_after_yields != 0U &&
         model->yield_calls >= model->fence_after_yields) ||
        (model->fence_after_rendering_yields != 0U &&
         model->rendering_yields >= model->fence_after_rendering_yields)) {
        for (index = 0U; index < backend->command_buffer_count; ++index)
            if (backend->ring_state[index] == MINIGL_RIX_RING_RENDERING)
                *(uint16_t *)(void *)(model->command[index] + COMMAND_BYTES -
                                       MINIGL_RIX_TEXTURE_PAGE_BYTES) =
                    (uint16_t)backend->pending_fence[index];
        if (rendering && !model->fence_signaled) {
            model->fence_signaled = 1;
            model->fence_signal_yield = model->rendering_yields;
        }
    }
    if ((model->idle_after_yields != 0U &&
         model->yield_calls >= model->idle_after_yields) ||
        (model->idle_after_fence_yields != 0U && model->fence_signaled &&
         model->rendering_yields >= model->fence_signal_yield +
                                    model->idle_after_fence_yields))
        model->csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
}

static void yield_watchdog(void *context)
{
    (void)context;
    g_now_us += 1000000U;
}

static void yield_deadline_completion(void *context)
{
    struct model *model = context;
    MiniGLRixBackend *backend = model->yield_backend;
    unsigned index;

    g_now_us += 1000000U;
    if (g_now_us != 3000000U)
        return;
    for (index = 0U; index < backend->command_buffer_count; ++index)
        if (backend->ring_state[index] == MINIGL_RIX_RING_RENDERING)
            *(uint16_t *)(void *)(model->command[index] + COMMAND_BYTES -
                                   MINIGL_RIX_TEXTURE_PAGE_BYTES) =
                (uint16_t)backend->pending_fence[index];
    model->csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
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
    model->command_count = 2U;
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

static void yield_feed_and_retire(void *context)
{
    struct model *model = context;
    MiniGLRixBackend *backend = model->yield_backend;
    unsigned index;

    assert(backend != NULL);
    ++model->yield_calls;
    while (minigl_rix_backend_feed_pending(backend))
        assert(minigl_rix_backend_try_feed(backend, RIX_DRV_FEED_SOURCE_PARKED) !=
               RIX_DRV_FEED_ERROR);
    for (index = 0U; index < backend->command_buffer_count; ++index)
        if (backend->ring_state[index] == MINIGL_RIX_RING_RENDERING)
            *(uint16_t *)(void *)(model->command[index] + COMMAND_BYTES -
                                   MINIGL_RIX_TEXTURE_PAGE_BYTES) =
                (uint16_t)backend->pending_fence[index];
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
    static uint32_t source[300U * 100U];

    init_backend(&backend, &model, 64U);
    source[0] = 0xff8040ffU;
    assert(minigl_rix_backend_upload_texture(&backend, 1U, 0U, source, 300U, 100U,
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

static const MiniGLRixTexture *find_texture(const MiniGLRixBackend *backend, uint32_t id)
{
    size_t index;

    for (index = 0U; index < MINIGL_RIX_MAX_TEXTURES; ++index)
        if (backend->textures[index].valid && backend->textures[index].id == id)
            return &backend->textures[index];
    return NULL;
}

static uint16_t stored_texel(const MiniGLRixBackend *backend, const MiniGLRixTexture *texture,
                             uint32_t index)
{
    const uint8_t *texel = backend->platform.texture_memory +
        (size_t)texture->first_page * MINIGL_RIX_TEXTURE_PAGE_BYTES + index * 2U;

    return (uint16_t)(texel[0] | (texel[1] << 8U));
}

static uint16_t rgb565(uint32_t rgba)
{
    return (uint16_t)(((rgba >> 27U) << 11U) | (((rgba >> 18U) & 0x3fU) << 5U) |
                      ((rgba >> 11U) & 0x1fU));
}

/* Images RasterIX cannot store at their own size must stay legible: an
 * odd-sized one keeps every source row, and one shrunk past 256 texels keeps
 * its one-texel strokes as grey instead of dropping them. */
static void test_texture_resampling_keeps_detail(void)
{
    static const uint32_t rows[5] = {
        0xff0000ffU, 0x00ff00ffU, 0x0000ffffU, 0xffff00ffU, 0x00ffffffU
    };
    MiniGLRixBackend backend;
    static struct model model;
    static uint32_t source[512U * 2U];
    const MiniGLRixTexture *texture;
    uint32_t r, x, y;

    init_backend(&backend, &model, MINIGL_RIX_TEXTURE_PAGES);
    for (y = 0U; y < 5U; ++y)
        for (x = 0U; x < 3U; ++x)
            source[y * 3U + x] = rows[y];
    assert(minigl_rix_backend_upload_texture(&backend, 1U, 0U, source, 3U, 5U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    texture = find_texture(&backend, 1U);
    assert(texture != NULL && texture->height >= 5U);
    for (r = 0U; r < 5U; ++r) {
        int found = 0;

        for (y = 0U; y < texture->height; ++y)
            found |= stored_texel(&backend, texture, y * texture->width) == rgb565(rows[r]);
        assert(found);
    }

    for (x = 0U; x < 512U; ++x)
        source[x] = source[512U + x] = (x & 1U) != 0U ? 0xffffffffU : 0x000000ffU;
    assert(minigl_rix_backend_upload_texture(&backend, 2U, 0U, source, 512U, 2U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    texture = find_texture(&backend, 2U);
    assert(texture != NULL && texture->width == 256U);
    for (x = 0U; x < 256U; ++x)
        assert(stored_texel(&backend, texture, x) == rgb565(0x808080ffU));
}

/* A mipmapped image whose size was rounded up becomes complete when the app
 * finishes its own (shorter) chain at 1x1. */
static void test_rounded_up_mip_chain_completes(void)
{
    MiniGLRixBackend backend;
    static struct model model;
    static uint32_t source[24U * 24U];
    uint16_t size;
    uint16_t level = 0U;

    init_backend(&backend, &model, MINIGL_RIX_TEXTURE_PAGES);
    for (size = 24U; ; size = (uint16_t)(size / 2U), ++level) {
        assert(minigl_rix_backend_upload_texture(&backend, 3U, level, source, size, size,
                                                  ZZ9K_MGL_TEX_INTERNAL_RGB));
        if (size == 1U)
            break;
    }
    minigl_rix_backend_tex_params(&backend, 3U, GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR,
                                  0x2901U, 0x2901U);
    minigl_rix_backend_bind_texture(&backend, 0U, 3U);
    assert(submit_texture(&backend, 3U));
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

static void test_subimage_writes_only_rectangle_and_copy_on_writes(void)
{
    MiniGLRixBackend backend;
    static struct model model;
    static uint32_t source[16U * 16U];
    MiniGLRixTexture *updated;
    uint16_t old_page;
    uint32_t y;

    init_backend(&backend, &model, 4U);
    for (y = 0U; y < 16U * 16U; ++y)
        source[y] = 0xff0000ffU;
    assert(minigl_rix_backend_upload_texture(&backend, 9U, 0U, source, 16U, 16U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    minigl_rix_backend_tex_params(&backend, 9U, GL_NEAREST, GL_NEAREST,
                                  0x2901U, 0x2901U);
    assert(submit_texture(&backend, 9U));
    assert(backend.textures[0].pin_count == 1U);
    old_page = backend.textures[0].first_page;
    source[3U * 16U + 2U] = 0x00ff00ffU;
    source[3U * 16U + 3U] = 0x00ff00ffU;
    source[4U * 16U + 2U] = 0x00ff00ffU;
    source[4U * 16U + 3U] = 0x00ff00ffU;
    assert(minigl_rix_backend_subimage_texture(&backend, 9U, 0U, 2U, 3U, 2U, 2U,
                                                source, 16U, 16U));
    assert(backend.textures[0].deleted);
    assert(backend.textures[0].first_page == old_page);
    updated = &backend.textures[1];
    assert(updated->valid && !updated->deleted && updated->first_page != old_page);
    assert(model.textures[(size_t)updated->first_page * MINIGL_RIX_TEXTURE_PAGE_BYTES] ==
           0U);
    assert(model.textures[(size_t)updated->first_page * MINIGL_RIX_TEXTURE_PAGE_BYTES +
                          (3U * 16U + 2U) * 2U] == 0xe0U);
    assert(minigl_rix_backend_drain(&backend));
    assert(!backend.textures[0].valid);
}

static void test_reupload_copy_on_writes(void)
{
    MiniGLRixBackend backend;
    static struct model model;
    static uint32_t source[16U * 16U];
    uint16_t old_page;
    size_t slot;

    memset(source, 0xff, sizeof(source));
    init_backend(&backend, &model, 4U);
    assert(minigl_rix_backend_upload_texture(&backend, 11U, 0U, source, 16U, 16U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    minigl_rix_backend_tex_params(&backend, 11U, GL_NEAREST, GL_NEAREST,
                                  GL_REPEAT, GL_REPEAT);
    assert(submit_texture(&backend, 11U));
    assert(backend.textures[0].pin_count == 1U);
    old_page = backend.textures[0].first_page;
    source[0] = 0x00ff00ffU;
    assert(minigl_rix_backend_upload_texture(&backend, 11U, 0U, source, 16U, 16U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    assert(backend.textures[0].deleted && backend.textures[0].pin_count == 1U);
    for (slot = 0U; slot < MINIGL_RIX_MAX_TEXTURES; ++slot)
        if (backend.textures[slot].valid && !backend.textures[slot].deleted &&
            backend.textures[slot].id == 11U)
            break;
    assert(slot < MINIGL_RIX_MAX_TEXTURES);
    assert(backend.textures[slot].first_page != old_page);
    assert(stored_texel(&backend, &backend.textures[slot], 0U) == 0x07e0U);
    assert(minigl_rix_backend_drain(&backend));
    assert(!backend.textures[0].valid);
}

static void test_abandon_retires_copy_only_after_engine_stop(void)
{
    MiniGLRixBackend backend;
    static struct model model;
    static uint32_t source[16U * 16U];
    uint16_t old_page;
    uint32_t y;

    init_backend(&backend, &model, 4U);
    for (y = 0U; y < 16U * 16U; ++y)
        source[y] = 0xff0000ffU;
    assert(minigl_rix_backend_upload_texture(&backend, 9U, 0U, source, 16U, 16U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    minigl_rix_backend_tex_params(&backend, 9U, GL_NEAREST, GL_NEAREST,
                                  0x2901U, 0x2901U);
    assert(submit_texture(&backend, 9U));
    assert(backend.textures[0].pin_count == 1U);
    old_page = backend.textures[0].first_page;
    source[0] = 0x00ff00ffU;
    assert(minigl_rix_backend_subimage_texture(&backend, 9U, 0U, 0U, 0U, 1U, 1U,
                                                source, 16U, 16U));
    /* The in-flight command buffer, and any present still reading that frame,
     * still owns the original pages. Abandon is only legal once PL330 and
     * RasterIX have been stopped. */
    assert(backend.textures[0].deleted && backend.textures[0].valid);
    assert(backend.textures[0].first_page == old_page);
    assert(backend.pending[0] || backend.textures[0].pin_count != 0U);
    minigl_rix_backend_abandon(&backend);
    assert(!backend.pending[0]);
    assert(!backend.textures[0].valid);
    assert(backend.driver.dma_stream == NULL);
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

/* GLQuake draws its console and HUD one glBegin per character.  Each op
 * carries the full state plus the texture's page list, so a long run of
 * them exhausts the command budget through state words alone; the backend
 * must submit and continue, not refuse the next draw (hardware log:
 * "draw rejected at record ... ops=111 tri=222"). */
static void test_many_small_textured_draws_flush_instead_of_refusing(void)
{
    MiniGLRixBackend backend;
    static struct model model;
    static uint32_t source[256U * 256U];
    unsigned draw;

    init_backend(&backend, &model, MINIGL_RIX_TEXTURE_PAGES);
    assert(minigl_rix_backend_upload_texture(&backend, 1U, 0U, source, 256U,
                                              256U, ZZ9K_MGL_TEX_INTERNAL_RGB));
    minigl_rix_backend_tex_params(&backend, 1U, GL_LINEAR, GL_LINEAR,
                                  0x2901U, 0x2901U);
    for (draw = 0U; draw < 400U; ++draw)
        assert(submit_texture(&backend, 1U));
    assert(minigl_rix_backend_drain(&backend));
    assert(model.packet_count >= 2U);
}

/* Consecutive ops with identical state emit it once; a state change emits
 * again.  Counted by feature-enable writes: one per emitted state plus the
 * fence's. */
static void test_repeated_state_is_emitted_once(void)
{
    MiniGLRixBackend backend;
    static struct model model;
    ZZ9KMiniGLPacketSetState state = state16();
    MiniGLRixVertex a = vertex(10.0f, 10.0f, 0xff0000ffU);
    MiniGLRixVertex b = vertex(60.0f, 10.0f, 0x00ff00ffU);
    MiniGLRixVertex c = vertex(10.0f, 60.0f, 0x0000ffffU);
    uint32_t *command;
    size_t word;
    unsigned draw, enables = 0U;

    init_backend(&backend, &model, TEXTURE_SLOTS);
    for (draw = 0U; draw < 3U; ++draw) {
        if (draw == 2U)
            state.enabled_flags |= ZZ9K_MGL_STATE_DEPTH_TEST;
        minigl_rix_backend_set_state(&backend, &state);
        assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
        assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
        assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    }
    assert(minigl_rix_backend_drain(&backend));
    command = (uint32_t *)model.command[0];
    for (word = 0U; word < COMMAND_WORDS; ++word)
        if (command[word] == (RIX_OP_RENDER_CONFIG | RIX_REG_FEATURE_ENABLE))
            ++enables;
    assert(enables == 3U);
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
    assert(strcmp(minigl_rix_backend_fallback_name(ZZ9K_MGL_FALLBACK_DRAW_REJECTED), "draw rejected") == 0);
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
                             int fail, int async)
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
    if (async) {
        io.dma_start_async = dma_start_async;
        io.dma_retire = dma_retire;
    }
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
    int async;

    g_self_test_failures = 0U;
    g_diagnostic_snapshots = 0U;
    g_rix_test_log = count_log;

    /* A delivered DMA packet lands the self-test fence and keeps the PL330. */
    init_dma_backend(&backend, &model, 0, 0);
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

    /* A failed DMA packet falls back to MMIO after a reset, not to the CPU,
     * whichever DMA entry points the platform offered. */
    for (async = 0; async < 2; ++async) {
        g_self_test_failures = 0U;
        g_diagnostic_snapshots = 0U;
        init_dma_backend(&backend, &model, 1, async);
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
    }
    g_rix_test_log = NULL;
}
/* A passed transport self-test retires its fence before the command buffer
 * can be reused. A queued frame has not reached RasterIX, so reset must not
 * wait on the self-test fence or quarantine its allocations. */
static void test_self_test_retirement_clears_published_fence(void)
{
    static struct model model;
    MiniGLRixPlatform io;
    MiniGLRixBackend backend;
    ZZ9KMiniGLPacketSetState state = state16();
    card_pool_t pool;
    minigl_fence_guard_hooks_t hooks = guard_hooks;
    uint32_t allocation;

    hooks.context = &model;
    memset(&model, 0, sizeof(model));
    model.texture_bytes = TEXTURE_SLOTS * MINIGL_RIX_TEXTURE_PAGE_BYTES;
    model.complete_packets = 1U;
    model.fifo_vacancy = FIFO_CAPACITY;
    model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    io = platform(&model);
    io.dma_stream = dma_stream;
    io.async_feed = 1U;
    io.command_buffer_count = 2U;
    minigl_rix_backend_init(&backend, &io);
    assert(minigl_rix_backend_available(&backend));
    assert(model.publish_count == 1U);
    assert(model.retired_publications == 1U);
    assert(model.published_address == 0U && model.published_sequence == 0U);

    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U,
                                    0x03100000U, GL_COLOR_BUFFER_BIT, 0U, 0U));
    assert(minigl_rix_backend_submit(&backend));
    assert(backend.ring_state[0] == MINIGL_RIX_RING_QUEUED);
    minigl_rix_backend_reset(&backend);
    assert(model.published_address == 0U && model.published_sequence == 0U);
    assert(model.retired_publications != 0U && model.quarantined_mask == 0U);
    card_pool_init(&pool);
    assert(card_pool_add_range(&pool, 0x10000000U, 0x10020000U, 0U) == 0);
    card_pool_open_range(&pool, 0U);
    allocation = card_pool_alloc(&pool, CARD_POOL_PAGE_BYTES,
                                 CARD_POOL_OWNER(CARD_POOL_CLASS_MINIGL, 1U));
    assert(allocation != 0U);
    minigl_fence_guard_pool(&pool, 1, &hooks, 100U);
    assert(model.guard_sleeps == 0U);
    assert(card_pool_free(&pool, allocation,
                          CARD_POOL_OWNER(CARD_POOL_CLASS_MINIGL, 1U)) == 1);
}

/* A DDR envelope accepted during the actual transport self-test remains
 * owned across a second initialization of the same context. */
static void test_failed_memfetch_self_test_retains_list(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    MiniGLRixPlatform io;
    uint16_t *fence;
    unsigned packets;

    memset(&model, 0, sizeof(model));
    model.texture_bytes = TEXTURE_SLOTS * MINIGL_RIX_TEXTURE_PAGE_BYTES;
    model.fifo_vacancy = FIFO_CAPACITY;
    model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model.fail_memfetch_commit = 1;
    io = platform(&model);
    io.submit_mode = RIX_DRV_SUBMIT_MEMFETCH;
    minigl_rix_backend_init(&backend, &io);
    assert(!minigl_rix_backend_available(&backend));
    assert(backend.pending[0] && backend.pending_fence[0] == 0xa55aU);
    packets = model.packet_count;
    assert(packets == 1U);
    fence = (uint16_t *)(void *)(model.command[0] + COMMAND_BYTES -
                                 MINIGL_RIX_TEXTURE_PAGE_BYTES);
    assert(*fence == 0U);

    minigl_rix_backend_reinit(&backend, &io);
    assert(backend.pending[0] && backend.pending_fence[0] == 0xa55aU);
    assert(model.packet_count == packets);
    assert(minigl_rix_backend_abort_feed(&backend) == RIX_DRV_FEED_BLOCKED);
    *fence = 0xa55aU;
    assert(minigl_rix_backend_abort_feed(&backend) == RIX_DRV_FEED_DONE);
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
    unsigned streams = 0U;

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
    for (word = 0U; word < COMMAND_WORDS;) {
        uint32_t op = command[word] & 0xf0000000U;

        if (command[word] == (RIX_OP_TRIANGLE_STREAM | RIX_TRIANGLE_WORDS * 4U))
            ++triangles;
        else if (op == RIX_OP_TEXTURE_STREAM)
            ++streams;
        word += op == RIX_OP_RENDER_CONFIG ? 2U :
                op == RIX_OP_TRIANGLE_STREAM ? 1U + (command[word] & 0x3ffU) / 4U :
                op == RIX_OP_FOG_LUT_STREAM ? 65U :
                op == RIX_OP_TEXTURE_STREAM ? 1U + (command[word] & 0x7ffffU) : 1U;
    }
    assert(triangles == 4U);
    /* One state block and page list per pass for the whole draw, not per
     * triangle: each page list also invalidates the RasterIX texture cache. */
    assert(streams == 2U);

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

static void test_probe_version_compatibility(void)
{
    static const uint32_t accepted[] = { 0x00020000U, 0x00020001U, 0x00020007U };
    static const uint32_t rejected[] = { 0x00010007U, 0x00030000U };
    struct model model;
    MiniGLRixPlatform io;
    unsigned int index;

    for (index = 0U; index < sizeof(accepted) / sizeof(accepted[0]); ++index) {
        memset(&model, 0, sizeof(model));
        io = platform(&model);
        model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
        model.csr[RIX_DRV_CSR_VERSION / 4U] = accepted[index];
        assert(minigl_rix_backend_probe_platform(&io));
    }
    for (index = 0U; index < sizeof(rejected) / sizeof(rejected[0]); ++index) {
        memset(&model, 0, sizeof(model));
        io = platform(&model);
        model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
        model.csr[RIX_DRV_CSR_VERSION / 4U] = rejected[index];
        assert(!minigl_rix_backend_probe_platform(&io));
    }
}

static void test_async_submit_queues_before_feed(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    MiniGLRixPlatform io;
    ZZ9KMiniGLPacketSetState state = state16();

    memset(&model, 0, sizeof(model));
    model.texture_bytes = sizeof(model.textures);
    model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model.fifo_vacancy = FIFO_CAPACITY;
    model.complete_packets = 1U;
    io = platform(&model);
    io.async_feed = 1U;
    io.command_buffer_count = 2U;
    minigl_rix_backend_init(&backend, &io);
    model.publish_count = 0U;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U,
                                    0x03100000U, GL_COLOR_BUFFER_BIT, 0U, 0U));
    assert(minigl_rix_backend_submit(&backend));
    assert(backend.ring_state[0] == MINIGL_RIX_RING_QUEUED);
    assert(model.publish_count == 0U);
    assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) !=
           RIX_DRV_FEED_ERROR);
    assert(model.publish_count == 1U);
}

static void test_async_memfetch_keeps_ring_and_fence_lifecycle(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    MiniGLRixPlatform io;
    ZZ9KMiniGLPacketSetState state = state16();
    unsigned cleans;

    memset(&model, 0, sizeof(model));
    model.texture_bytes = sizeof(model.textures);
    model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model.fifo_vacancy = FIFO_CAPACITY;
    model.complete_packets = 1U;
    io = platform(&model);
    io.async_feed = 1U;
    io.command_buffer_count = 2U;
    io.submit_mode = RIX_DRV_SUBMIT_MEMFETCH;
    minigl_rix_backend_init(&backend, &io);
    assert(minigl_rix_backend_available(&backend));
    assert(model.packet_count == 1U); /* Transport self-test. */

    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U,
                                    0x03100000U, GL_COLOR_BUFFER_BIT, 0U, 0U));
    cleans = model.cleans;
    assert(minigl_rix_backend_submit(&backend));
    assert(model.cleans > cleans);
    assert((backend.submitted_command_words[0] & 15U) == 0U);
    assert(model.cleaned_bytes ==
           backend.submitted_command_words[0] * sizeof(uint32_t));
    assert(backend.ring_state[0] == MINIGL_RIX_RING_QUEUED);

    assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) ==
           RIX_DRV_FEED_DONE);
    assert(model.last_tlf == 2U * sizeof(uint32_t));
    assert((model.packet[0] & 0xf0000000U) == RIX_DRV_MEMFETCH_ENVELOPE);
    assert(model.packet[1] == model.command_address[0]);
    assert(backend.ring_state[0] == MINIGL_RIX_RING_RENDERING);
    /* Retirement is demand-driven: the present poll observes fence and idle. */
    assert(minigl_rix_backend_poll(&backend));
    assert(backend.ring_state[0] == MINIGL_RIX_RING_FREE);
}
static void init_async_memfetch_backend(MiniGLRixBackend *backend,
                                        struct model *model)
{
    MiniGLRixPlatform io;

    memset(model, 0, sizeof(*model));
    model->texture_bytes = sizeof(model->textures);
    model->csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model->csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model->csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model->fifo_vacancy = FIFO_CAPACITY;
    model->complete_packets = 1U;
    io = platform(model);
    io.async_feed = 1U;
    io.command_buffer_count = 2U;
    io.submit_mode = RIX_DRV_SUBMIT_MEMFETCH;
    minigl_rix_backend_init(backend, &io);
    assert(minigl_rix_backend_available(backend));
    model->publish_count = 0U;
    model->retired_publications = 0U;
}

static void test_async_memfetch_error_publishes_accepted_envelope(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    ZZ9KMiniGLPacketSetState state = state16();

    init_async_memfetch_backend(&backend, &model);
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U,
                                    0x03100000U, GL_COLOR_BUFFER_BIT, 0U, 0U));
    assert(minigl_rix_backend_submit(&backend));
    model.fail_memfetch_after_packet = model.packet_count + 1U;
    assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) ==
           RIX_DRV_FEED_ERROR);
    assert(model.publish_count == 1U && model.published_address != 0U);
}

static void test_async_memfetch_abort_publishes_accepted_envelope(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    ZZ9KMiniGLPacketSetState state = state16();

    init_async_memfetch_backend(&backend, &model);
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U,
                                    0x03100000U, GL_COLOR_BUFFER_BIT, 0U, 0U));
    assert(minigl_rix_backend_submit(&backend));
    model.fifo_vacancy = 1U;
    assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) ==
           RIX_DRV_FEED_BLOCKED);
    model.fifo_vacancy = FIFO_CAPACITY;
    model.complete_packets = 0U;
    assert(minigl_rix_backend_abort_feed(&backend) == RIX_DRV_FEED_BLOCKED);
    assert(model.publish_count == 1U && model.published_address != 0U);
}

static void test_queued_work_does_not_keep_a_retired_fence_published(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    ZZ9KMiniGLPacketSetState state = state16();
    card_pool_t pool;
    minigl_fence_guard_hooks_t hooks = guard_hooks;
    uint32_t allocation;
    uint8_t queued;
    uint8_t retired;

    init_async_memfetch_backend(&backend, &model);
    hooks.context = &model;
    minigl_rix_backend_set_state(&backend, &state);
    model.complete_packets = 0U;
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U,
                                    0x03100000U, GL_COLOR_BUFFER_BIT, 0U, 0U));
    assert(minigl_rix_backend_submit(&backend));
    assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) ==
           RIX_DRV_FEED_DONE);
    for (retired = 0U; retired < 2U; ++retired)
        if (backend.ring_state[retired] == MINIGL_RIX_RING_RENDERING)
            break;
    assert(retired < 2U);
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03200000U,
                                    0x03300000U, GL_COLOR_BUFFER_BIT, 0U, 0U));
    assert(minigl_rix_backend_submit(&backend));
    for (queued = 0U; queued < 2U; ++queued)
        if (backend.ring_state[queued] == MINIGL_RIX_RING_QUEUED)
            break;
    assert(queued < 2U && queued != retired);
    *(uint16_t *)(void *)(model.command[retired] + COMMAND_BYTES -
                          MINIGL_RIX_TEXTURE_PAGE_BYTES) =
        (uint16_t)backend.pending_fence[retired];
    model.fifo_vacancy = 0U;
    backend.driver.fifo_vacancy_estimate = 0U;
    assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_PARKED) ==
           RIX_DRV_FEED_BLOCKED);
    assert(backend.ring_state[retired] == MINIGL_RIX_RING_FREE);
    assert(backend.ring_state[queued] == MINIGL_RIX_RING_FEEDING);
    assert(model.published_address == 0U && model.published_sequence == 0U);

    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03400000U,
                                    0x03500000U, GL_COLOR_BUFFER_BIT, 0U, 0U));
    assert(minigl_rix_backend_submit(&backend));
    assert(backend.ring_state[retired] == MINIGL_RIX_RING_QUEUED);
    assert(minigl_rix_backend_abort_feed(&backend) == RIX_DRV_FEED_BLOCKED);
    minigl_rix_backend_abandon(&backend);

    card_pool_init(&pool);
    assert(card_pool_add_range(&pool, 0x10000000U, 0x10020000U, 0U) == 0);
    card_pool_open_range(&pool, 0U);
    allocation = card_pool_alloc(&pool, CARD_POOL_PAGE_BYTES,
                                 CARD_POOL_OWNER(CARD_POOL_CLASS_MINIGL, 2U));
    assert(allocation != 0U);
    minigl_fence_guard_pool(&pool, 1, &hooks, 100U);
    assert(model.quarantined_mask == 0U);
    assert(model.guard_sleeps == 0U);
    assert(card_pool_free(&pool, allocation,
                          CARD_POOL_OWNER(CARD_POOL_CLASS_MINIGL, 2U)) == 1);
}


/* An ordinary untextured batch: an 18-word setup, then 40-word triangle
 * commands, more than one 16 KiB DDR chunk.  Chunk ends fall inside
 * triangles; the parser continues across envelopes, so every triangle and
 * the frame fence must arrive and the backend must stay enabled. */
static void test_memfetch_triangle_batch_spans_envelopes(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    MiniGLRixPlatform io;
    ZZ9KMiniGLPacketSetState state = state16();
    MiniGLRixVertex a = vertex(2.0f, 2.0f, 0xff0000ffU);
    MiniGLRixVertex b = vertex(60.0f, 2.0f, 0x00ff00ffU);
    MiniGLRixVertex c = vertex(2.0f, 60.0f, 0x0000ffffU);
    unsigned index;
    int result;

    memset(&model, 0, sizeof(model));
    model.texture_bytes = sizeof(model.textures);
    model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model.fifo_vacancy = FIFO_CAPACITY;
    model.complete_packets = 1U;
    io = platform(&model);
    io.async_feed = 1U;
    io.command_buffer_count = 2U;
    io.submit_mode = RIX_DRV_SUBMIT_MEMFETCH;
    minigl_rix_backend_init(&backend, &io);
    assert(minigl_rix_backend_available(&backend));
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U,
                                         0x03100000U));
    for (index = 0U; index < 103U; ++index)
        assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_submit(&backend));
    assert(backend.submitted_command_words[0] >
           RIX_DRV_MEMFETCH_CHUNK_WORDS);

    model.packet_count = 0U;
    model.triangles_parsed = 0U;
    model.publish_count = 0U;
    result = minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC);
    assert(result == RIX_DRV_FEED_PROGRESS);
    assert(model.publish_count == 1U);
    do
        result = minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC);
    while (result == RIX_DRV_FEED_PROGRESS);
    assert(result == RIX_DRV_FEED_DONE);
    assert(minigl_rix_backend_available(&backend));
    assert(model.packet_count == 2U);
    assert(model.triangles_parsed == 103U);
    assert(model.parse_skip == 0U && model.parse_reg == 0U);
    assert(minigl_rix_backend_poll(&backend));
    assert(backend.ring_state[0] == MINIGL_RIX_RING_FREE);
}

/* A FIFO error following the first accepted envelope cannot make a synchronous
 * multi-envelope submission relinquish its list before the fence is observed. */
static void test_failed_sync_memfetch_submit_retains_list(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    MiniGLRixPlatform io;
    ZZ9KMiniGLPacketSetState state = state16();
    MiniGLRixVertex a = vertex(2.0f, 2.0f, 0xff0000ffU);
    MiniGLRixVertex b = vertex(60.0f, 2.0f, 0x00ff00ffU);
    MiniGLRixVertex c = vertex(2.0f, 60.0f, 0x0000ffffU);
    uint16_t *fence;
    unsigned index;

    memset(&model, 0, sizeof(model));
    model.texture_bytes = sizeof(model.textures);
    model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model.fifo_vacancy = FIFO_CAPACITY;
    model.complete_packets = 1U;
    io = platform(&model);
    io.submit_mode = RIX_DRV_SUBMIT_MEMFETCH;
    minigl_rix_backend_init(&backend, &io);
    assert(minigl_rix_backend_available(&backend));
    model.complete_packets = 0U;
    model.fail_memfetch_after_packet = model.packet_count + 1U;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U,
                                         0x03100000U));
    for (index = 0U; index < 103U; ++index)
        assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    assert(!minigl_rix_backend_submit(&backend));
    assert(backend.pending[0] && backend.pending_fence[0] != 0U);
    assert(minigl_rix_backend_abort_feed(&backend) == RIX_DRV_FEED_BLOCKED);
    fence = (uint16_t *)(void *)(model.command[0] + COMMAND_BYTES -
                                 MINIGL_RIX_TEXTURE_PAGE_BYTES);
    *fence = (uint16_t)backend.pending_fence[0];
    assert(minigl_rix_backend_abort_feed(&backend) == RIX_DRV_FEED_DONE);
}

/* A forced stop in memfetch mode cannot recall the list: it pushes the rest
 * of the list being fed and reports DONE only once the list's fence has
 * landed with the engine idle. */
static void test_memfetch_abort_waits_for_list_fence(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    MiniGLRixPlatform io;
    ZZ9KMiniGLPacketSetState state = state16();
    MiniGLRixVertex a = vertex(2.0f, 2.0f, 0xff0000ffU);
    MiniGLRixVertex b = vertex(60.0f, 2.0f, 0x00ff00ffU);
    MiniGLRixVertex c = vertex(2.0f, 60.0f, 0x0000ffffU);
    unsigned index;

    memset(&model, 0, sizeof(model));
    model.texture_bytes = sizeof(model.textures);
    model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model.fifo_vacancy = FIFO_CAPACITY;
    model.complete_packets = 1U;
    io = platform(&model);
    io.async_feed = 1U;
    io.command_buffer_count = 2U;
    io.submit_mode = RIX_DRV_SUBMIT_MEMFETCH;
    minigl_rix_backend_init(&backend, &io);
    assert(minigl_rix_backend_available(&backend));
    model.complete_packets = 0U; /* The GPU has not reached the fence yet. */
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U,
                                         0x03100000U));
    for (index = 0U; index < 103U; ++index)
        assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_submit(&backend));
    model.packet_count = 0U;
    assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) ==
           RIX_DRV_FEED_PROGRESS);

    model.fifo[RIX_DRV_FIFO_TDFR / 4U] = 0U;
    assert(minigl_rix_backend_abort_feed(&backend) == RIX_DRV_FEED_BLOCKED);
    assert(model.packet_count == 2U);       /* the list was finished */
    assert(model.fifo[RIX_DRV_FIFO_TDFR / 4U] == 0U);
    assert(backend.ring_state[0] == MINIGL_RIX_RING_RENDERING);
    minigl_rix_backend_quarantine_outstanding(&backend);
    assert(model.quarantined_mask == 7U);
    assert(minigl_rix_backend_abort_feed(&backend) == RIX_DRV_FEED_BLOCKED);
    *(uint16_t *)(void *)(model.command[0] + COMMAND_BYTES -
                          MINIGL_RIX_TEXTURE_PAGE_BYTES) =
        (uint16_t)backend.pending_fence[0];
    assert(minigl_rix_backend_abort_feed(&backend) == RIX_DRV_FEED_DONE);
    assert(backend.ring_state[0] == MINIGL_RIX_RING_FREE);
}

/* GL calls step the feeder on every SVC boundary.  With nothing left to
 * feed and only rendering buffers, such a step must not touch RasterIX,
 * maintain the cache or read a timer; only a caller that needs a buffer
 * back (a PARKED step) polls the fence. */
static void test_idle_feed_steps_touch_no_hardware(void)
{
    unsigned mode;

    for (mode = RIX_DRV_SUBMIT_INLINE; mode <= RIX_DRV_SUBMIT_MEMFETCH; ++mode) {
        static struct model model;
        MiniGLRixBackend backend;
        MiniGLRixPlatform io;
        ZZ9KMiniGLPacketSetState state = state16();
        unsigned step;

        memset(&model, 0, sizeof(model));
        model.texture_bytes = sizeof(model.textures);
        model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
        model.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
        model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
        model.fifo_vacancy = FIFO_CAPACITY;
        model.complete_packets = 1U; /* Only the transport self-test fence lands. */
        io = platform(&model);
        io.async_feed = 1U;
        io.command_buffer_count = 2U;
        io.submit_mode = (uint8_t)mode;
        minigl_rix_backend_init(&backend, &io);
        assert(minigl_rix_backend_available(&backend));
        model.complete_packets = 0U;
        minigl_rix_backend_set_state(&backend, &state);
        assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U,
                                        0x03100000U, GL_COLOR_BUFFER_BIT, 0U, 0U));
        assert(minigl_rix_backend_submit(&backend));
        assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) ==
               RIX_DRV_FEED_DONE);
        assert(backend.ring_state[0] == MINIGL_RIX_RING_RENDERING);

        model.csr_reads = 0U;
        model.fifo_reads = 0U;
        model.invalidates = 0U;
        for (step = 0U; step < 1000U; ++step) {
            assert(minigl_rix_backend_try_feed(&backend, step & 1U ?
                                               RIX_DRV_FEED_SOURCE_SVC :
                                               RIX_DRV_FEED_SOURCE_SCHEDULER) ==
                   RIX_DRV_FEED_DONE);
            assert(!minigl_rix_backend_feed_pending(&backend));
            assert(minigl_rix_backend_work_outstanding(&backend));
        }
        assert(model.csr_reads == 0U && model.fifo_reads == 0U);
        assert(model.invalidates == 0U);
        assert(backend.ring_state[0] == MINIGL_RIX_RING_RENDERING);

        /* The fence lands: an ordinary step still does not look... */
        *(uint16_t *)(void *)(model.command[0] + COMMAND_BYTES -
                              MINIGL_RIX_TEXTURE_PAGE_BYTES) =
            (uint16_t)backend.pending_fence[0];
        assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) ==
               RIX_DRV_FEED_DONE);
        assert(backend.ring_state[0] == MINIGL_RIX_RING_RENDERING);
        assert(model.invalidates == 0U && model.csr_reads == 0U);
        /* ...a waiter observes fence and idle once. */
        assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_PARKED) ==
               RIX_DRV_FEED_DONE);
        assert(backend.ring_state[0] == MINIGL_RIX_RING_FREE);
        assert(model.invalidates == 1U && model.csr_reads == 1U);
        assert(!minigl_rix_backend_work_outstanding(&backend));
    }
}

/* Filling the async ring must park until the feeder retires a buffer, rather
 * than failing an otherwise valid third submission in host builds. */
static void test_async_full_ring_parks(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    MiniGLRixPlatform io;
    ZZ9KMiniGLPacketSetState state = state16();
    unsigned frame;

    memset(&model, 0, sizeof(model));
    model.texture_bytes = sizeof(model.textures);
    model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model.fifo_vacancy = FIFO_CAPACITY;
    model.complete_packets = 1U;
    io = platform(&model);
    io.async_feed = 1U;
    io.command_buffer_count = 2U;
    io.yield_wait = yield_feed_and_retire;
    model.yield_backend = &backend;
    minigl_rix_backend_init(&backend, &io);
    minigl_rix_backend_set_state(&backend, &state);
    for (frame = 0U; frame < 3U; ++frame) {
        assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U,
                                        0x03100000U, GL_COLOR_BUFFER_BIT,
                                        frame, 0U));
        assert(minigl_rix_backend_submit(&backend));
    }
    assert(model.yield_calls != 0U);
}

static void test_async_texture_reuse_parks(void)
{
    static struct model model;
    static uint32_t texture[16U * 16U];
    MiniGLRixBackend backend;
    MiniGLRixPlatform io;
    ZZ9KMiniGLPacketSetState state = state16();

    memset(&model, 0, sizeof(model));
    model.texture_bytes = MINIGL_RIX_TEXTURE_PAGE_BYTES;
    model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model.fifo_vacancy = FIFO_CAPACITY;
    model.complete_packets = 1U;
    io = platform(&model);
    io.async_feed = 1U;
    io.command_buffer_count = 2U;
    io.yield_wait = yield_feed_and_retire;
    model.yield_backend = &backend;
    minigl_rix_backend_init(&backend, &io);
    assert(minigl_rix_backend_upload_texture(&backend, 1U, 0U, texture, 16U, 16U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    minigl_rix_backend_tex_params(&backend, 1U, GL_NEAREST, GL_NEAREST,
                                  GL_REPEAT, GL_REPEAT);
    minigl_rix_backend_bind_texture(&backend, 0U, 1U);
    state.enabled_flags = ZZ9K_MGL_STATE_TEXTURE_2D_0;
    minigl_rix_backend_set_state(&backend, &state);
    assert(submit_texture(&backend, 1U));
    assert(minigl_rix_backend_upload_texture(&backend, 1U, 0U, texture, 16U, 16U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    assert(model.yield_calls != 0U);
}

static void test_async_target_reuse_parks(void)
{
    static struct model model;
    static uint32_t target[512] __attribute__((aligned(MINIGL_RIX_TEXTURE_PAGE_BYTES)));
    MiniGLRixBackend backend;
    MiniGLRixPlatform io;
    ZZ9KMiniGLPacketSetState state = state16();

    memset(&model, 0, sizeof(model));
    model.texture_bytes = sizeof(model.textures);
    model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model.fifo_vacancy = FIFO_CAPACITY;
    model.complete_packets = 1U;
    io = platform(&model);
    io.async_feed = 1U;
    io.command_buffer_count = 2U;
    io.yield_wait = yield_feed_and_retire;
    model.yield_backend = &backend;
    minigl_rix_backend_init(&backend, &io);
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_clear(&backend, 2U, 2U, (uint32_t)(uintptr_t)target,
                                    0x03100000U, GL_COLOR_BUFFER_BIT, 0U, 0U));
    assert(minigl_rix_backend_submit(&backend));
    assert(minigl_rix_backend_prepare_target(&backend, target, 4U));
    assert(model.yield_calls != 0U);
}

static void test_async_feeds_submission_order_after_slot_reuse(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    MiniGLRixPlatform io;
    ZZ9KMiniGLPacketSetState state = state16();

    memset(&model, 0, sizeof(model));
    model.texture_bytes = sizeof(model.textures);
    model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model.fifo_vacancy = FIFO_CAPACITY;
    model.complete_packets = 1U;
    io = platform(&model);
    io.async_feed = 1U;
    io.command_buffer_count = 2U;
    io.owner_generation = 17U;
    minigl_rix_backend_init(&backend, &io);
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U,
                                    0x03100000U, GL_COLOR_BUFFER_BIT, 1U, 0U));
    assert(minigl_rix_backend_submit(&backend));
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U,
                                    0x03100000U, GL_COLOR_BUFFER_BIT, 2U, 0U));
    assert(minigl_rix_backend_submit(&backend));
    assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) ==
           RIX_DRV_FEED_DONE);
    assert(backend.feed_buffer == 0U);
    /* Simulate slot 0's observed fence retirement without giving the feeder
     * another chance to start slot 1 before slot 0 is reused. */
    backend.ring_state[0] = MINIGL_RIX_RING_FREE;
    backend.pending[0] = 0U;
    backend.pending_fence[0] = 0U;
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U,
                                    0x03100000U, GL_COLOR_BUFFER_BIT, 3U, 0U));
    assert(minigl_rix_backend_submit(&backend));
    assert(backend.ring_state[0] == MINIGL_RIX_RING_QUEUED);
    assert(backend.ring_state[1] == MINIGL_RIX_RING_QUEUED);
    assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SCHEDULER) ==
           RIX_DRV_FEED_DONE);
    assert(backend.feed_buffer == 1U);
}

static void test_async_stale_generation_is_not_fed(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    MiniGLRixPlatform io;
    ZZ9KMiniGLPacketSetState state = state16();

    memset(&model, 0, sizeof(model));
    model.texture_bytes = sizeof(model.textures);
    model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model.fifo_vacancy = FIFO_CAPACITY;
    model.complete_packets = 1U;
    io = platform(&model);
    io.async_feed = 1U;
    io.command_buffer_count = 2U;
    io.owner_generation = 21U;
    minigl_rix_backend_init(&backend, &io);
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U,
                                    0x03100000U, GL_COLOR_BUFFER_BIT, 0U, 0U));
    assert(minigl_rix_backend_submit(&backend));
    backend.platform.owner_generation = 22U;
    assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) ==
           RIX_DRV_FEED_ERROR);
    assert(model.packet_count == 0U);
    assert(backend.error);
}

/* A feed error is terminal: callers that were prepared to park must observe
 * backend failure instead of continuing to wait on a ring entry which can no
 * longer advance. */
static void test_async_feed_error_terminates_waits(void)
{
    static struct model model;
    MiniGLRixBackend backend;
    MiniGLRixPlatform io;
    ZZ9KMiniGLPacketSetState state = state16();

    memset(&model, 0, sizeof(model));
    model.texture_bytes = sizeof(model.textures);
    model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model.fifo_vacancy = FIFO_CAPACITY;
    model.complete_packets = 1U;
    model.dma_launch_error = 1;
    io = platform(&model);
    io.async_feed = 1U;
    io.command_buffer_count = 2U;
    io.dma_ready = dma_ready;
    io.dma_start_nonblocking = dma_start_nonblocking;
    io.dma_complete = dma_retire;
    minigl_rix_backend_init(&backend, &io);
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U,
                                    0x03100000U, GL_COLOR_BUFFER_BIT, 0U, 0U));
    assert(minigl_rix_backend_submit(&backend));
    assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) ==
           RIX_DRV_FEED_ERROR);
    assert(!minigl_rix_backend_feed_pending(&backend));
    assert(!minigl_rix_backend_drain(&backend));
}

static void test_async_abort_preserves_staged_and_rendering_buffers(void)
{
    struct model model;
    MiniGLRixBackend backend;
    MiniGLRixPlatform io;
    ZZ9KMiniGLPacketSetState state = state16();

    memset(&model, 0, sizeof(model));
    model.command_count = MINIGL_RIX_COMMAND_BUFFERS;
    model.texture_bytes = sizeof(model.textures);
    model.complete_packets = 0U;
    model.fifo_vacancy = FIFO_CAPACITY;
    model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    io = platform(&model);
    io.async_feed = 1U;
    io.command_buffer_count = MINIGL_RIX_COMMAND_BUFFERS;
    io.dma_ready = dma_ready;
    io.dma_start_nonblocking = dma_start_state;
    io.dma_complete = dma_state;
    io.dma_abort = dma_abort_state;
    minigl_rix_backend_init(&backend, &io);
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U,
                                    0x03100000U, GL_COLOR_BUFFER_BIT, 0U, 0U));
    assert(minigl_rix_backend_submit(&backend));

    model.dma_launch_error = RIX_DRV_DMA_PENDING;
    model.dma_fail = RIX_DRV_DMA_PENDING;
    assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) ==
           RIX_DRV_FEED_BLOCKED);
    assert(backend.ring_state[0] == MINIGL_RIX_RING_FEEDING);
    assert(minigl_rix_backend_abort_feed(&backend) == RIX_DRV_FEED_BLOCKED);
    assert(backend.ring_state[0] == MINIGL_RIX_RING_FEEDING);
    model.dma_fail = RIX_DRV_DMA_COMPLETE;
    assert(minigl_rix_backend_abort_feed(&backend) == RIX_DRV_FEED_DONE);
    assert(backend.ring_state[0] == MINIGL_RIX_RING_FEEDING);

    minigl_rix_backend_abandon(&backend);
    memset(&model, 0, sizeof(model));
    model.command_count = MINIGL_RIX_COMMAND_BUFFERS;
    model.texture_bytes = sizeof(model.textures);
    model.complete_packets = 1U;
    model.fifo_vacancy = FIFO_CAPACITY;
    model.csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model.csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    io = platform(&model);
    io.async_feed = 1U;
    io.command_buffer_count = MINIGL_RIX_COMMAND_BUFFERS;
    minigl_rix_backend_init(&backend, &io);
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U,
                                    0x03100000U, GL_COLOR_BUFFER_BIT, 0U, 0U));
    assert(minigl_rix_backend_submit(&backend));
    assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) ==
           RIX_DRV_FEED_DONE);
    assert((model.fifo[RIX_DRV_FIFO_ISR / 4U] & RIX_DRV_FIFO_ISR_TSE) == 0U);
}

static void init_async8(MiniGLRixBackend *backend, struct model *model, int delayed_dma)
{
    MiniGLRixPlatform io;
    ZZ9KMiniGLPacketSetState state = state16();

    memset(model, 0, sizeof(*model));
    model->command_count = MINIGL_RIX_COMMAND_BUFFERS;
    model->texture_bytes = sizeof(model->textures);
    model->fifo_vacancy = FIFO_CAPACITY;
    model->csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model->csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model->csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model->dma_launch_state = RIX_DRV_DMA_COMPLETE;
    model->dma_complete_state = RIX_DRV_DMA_COMPLETE;
    model->dma_abort_state = RIX_DRV_DMA_COMPLETE;
    io = platform(model);
    io.async_feed = 1U;
    io.command_buffer_count = MINIGL_RIX_COMMAND_BUFFERS;
    if (delayed_dma) {
        io.dma_ready = dma_ready;
        io.dma_start_nonblocking = dma_start_delayed;
        io.dma_complete = dma_complete_delayed;
        io.dma_abort = dma_abort_delayed;
    }
    minigl_rix_backend_init(backend, &io);
    minigl_rix_backend_set_state(backend, &state);
}

static void queue_async_clear(MiniGLRixBackend *backend, uint32_t color)
{
    assert(minigl_rix_backend_clear(backend, 64U, 64U, 0x03000000U,
                                    0x03100000U, GL_COLOR_BUFFER_BIT, color, 0U));
    assert(minigl_rix_backend_submit(backend));
}

static void assert_async_rebind_feeds_cleanly(MiniGLRixBackend *backend,
                                              struct model *model)
{
    init_async8(backend, model, 0);
    queue_async_clear(backend, 0U);
    assert(minigl_rix_backend_try_feed(backend, RIX_DRV_FEED_SOURCE_SVC) ==
           RIX_DRV_FEED_DONE);
    *(uint16_t *)(void *)(model->command[0] + COMMAND_BYTES -
                          MINIGL_RIX_TEXTURE_PAGE_BYTES) =
        (uint16_t)backend->pending_fence[0];
    assert(minigl_rix_backend_try_feed(backend, RIX_DRV_FEED_SOURCE_PARKED) ==
           RIX_DRV_FEED_DONE);
    assert(backend->ring_state[0] == MINIGL_RIX_RING_FREE);
    assert((model->fifo[RIX_DRV_FIFO_ISR / 4U] & RIX_DRV_FIFO_ISR_TSE) == 0U);
}

/* A stop may arrive with only a header staged, with PL330 outstanding, just
 * after its TLF, or while RasterIX executes.  No slot may be released until
 * the applicable hardware proof (DMA abort or fence+idle) has occurred. */
static void test_async_abort_waits_for_every_inflight_stage(void)
{
    enum { HEADER_STAGED, DMA_PENDING, TLF_COMMITTED, RENDERING };
    unsigned stage;

    for (stage = HEADER_STAGED; stage <= RENDERING; ++stage) {
        MiniGLRixBackend backend;
        struct model model;

        init_async8(&backend, &model, 1);
        queue_async_clear(&backend, stage);
        if (stage == HEADER_STAGED) {
            model.dma_launch_state = RIX_DRV_DMA_PENDING;
            assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) ==
                   RIX_DRV_FEED_BLOCKED);
            assert(backend.feed.pending_launch);
        } else {
            model.dma_launch_state = RIX_DRV_DMA_COMPLETE;
            model.dma_complete_state = RIX_DRV_DMA_PENDING;
            assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) ==
                   RIX_DRV_FEED_PROGRESS);
            assert(backend.feed.pending_dma);
            if (stage >= TLF_COMMITTED) {
                model.dma_complete_state = RIX_DRV_DMA_COMPLETE;
                assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) ==
                       RIX_DRV_FEED_DONE);
                assert(model.packet_count == 1U);
                assert(backend.ring_state[0] == MINIGL_RIX_RING_RENDERING);
                if (stage == RENDERING)
                    assert(minigl_rix_backend_try_feed(
                               &backend, RIX_DRV_FEED_SOURCE_SCHEDULER) ==
                           RIX_DRV_FEED_DONE);
            }
        }
        assert(backend.ring_state[0] != MINIGL_RIX_RING_FREE);
        assert(backend.pending_color_address[0] == 0x03000000U);
        if (stage < TLF_COMMITTED) {
            model.dma_abort_state = RIX_DRV_DMA_PENDING;
            assert(minigl_rix_backend_abort_feed(&backend) == RIX_DRV_FEED_BLOCKED);
            assert(backend.ring_state[0] == MINIGL_RIX_RING_FEEDING);
            model.dma_abort_state = RIX_DRV_DMA_COMPLETE;
            assert(minigl_rix_backend_abort_feed(&backend) == RIX_DRV_FEED_DONE);
        } else {
            assert(minigl_rix_backend_abort_feed(&backend) == RIX_DRV_FEED_DONE);
            *(uint16_t *)(void *)(model.command[0] + COMMAND_BYTES -
                                  MINIGL_RIX_TEXTURE_PAGE_BYTES) =
                (uint16_t)backend.pending_fence[0];
            assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_PARKED) ==
                   RIX_DRV_FEED_DONE);
            assert(backend.ring_state[0] == MINIGL_RIX_RING_FREE);
        }
        assert(backend.ring_state[0] != MINIGL_RIX_RING_FREE ||
               stage >= TLF_COMMITTED);
        minigl_rix_backend_abandon(&backend);
        assert_async_rebind_feeds_cleanly(&backend, &model);
    }
}

static void test_async_drain_waits_for_fence_and_idle(void)
{
    unsigned order;

    for (order = 0U; order != 2U; ++order) {
        MiniGLRixBackend backend;
        struct model model;

        init_async8(&backend, &model, 0);
        model.yield_backend = &backend;
        backend.platform.yield_wait = yield_delayed_completion;
        model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_BUSY;
        model.fence_after_rendering_yields = order == 0U ? 1U : 2U;
        model.idle_after_yields = order == 0U ? 0U : 1U;
        model.idle_after_fence_yields = order == 0U ? 1U : 0U;
        queue_async_clear(&backend, order);
        assert(minigl_rix_backend_drain(&backend));
        assert(model.fence_signaled);
        assert(model.csr[RIX_DRV_CSR_STATUS / 4U] == RIX_DRV_STATUS_IDLE);
        assert(backend.ring_state[0] == MINIGL_RIX_RING_FREE);
    }
}
static int test_cannot_park(void *ctx)
{
    (void)ctx;
    return 0;
}

static void test_async_drain_cannot_park_does_bounded_feed_and_returns_incomplete(void)
{
    MiniGLRixBackend backend;
    struct model model;

    init_async8(&backend, &model, 0);
    backend.platform.can_park = test_cannot_park;
    model.yield_backend = &backend;
    backend.platform.yield_wait = yield_delayed_completion;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_BUSY;
    queue_async_clear(&backend, 0);
    assert(minigl_rix_backend_drain(&backend) == 0);
    assert(backend.error == 0);
    assert(minigl_rix_backend_work_outstanding(&backend));
}

static void test_async_feed_mmio_when_dma_start_nonblocking_null(void)
{
    MiniGLRixBackend backend;
    struct model model;

    init_async8(&backend, &model, 0);
    backend.platform.dma_start_nonblocking = NULL;
    backend.platform.dma_complete = NULL;
    queue_async_clear(&backend, 0);
    assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) == RIX_DRV_FEED_DONE);
    assert(backend.error == 0);
    assert(backend.ring_state[0] == MINIGL_RIX_RING_RENDERING);
}

static void arm_parked_feed_error(MiniGLRixBackend *backend, struct model *model)
{
    model->yield_backend = backend;
    backend->platform.yield_wait = yield_delayed_completion;
    model->dma_launch_state = RIX_DRV_DMA_PENDING;
    model->fail_feed_on_yield = 1;
}

static void test_async_feed_error_wakes_parked_waiters(void)
{
    static uint32_t target[512] __attribute__((aligned(MINIGL_RIX_TEXTURE_PAGE_BYTES)));
    static uint32_t texture[16U * 16U];
    MiniGLRixBackend backend;
    struct model model;
    unsigned index;

    init_async8(&backend, &model, 1);
    for (index = 0U; index < MINIGL_RIX_COMMAND_BUFFERS; ++index)
        queue_async_clear(&backend, index);
    arm_parked_feed_error(&backend, &model);
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U,
                                    0x03100000U, GL_COLOR_BUFFER_BIT, 9U, 0U));
    assert(!minigl_rix_backend_submit(&backend));
    assert(backend.error && model.yield_calls != 0U);

    init_async8(&backend, &model, 1);
    queue_async_clear(&backend, 0U);
    arm_parked_feed_error(&backend, &model);
    assert(!minigl_rix_backend_drain(&backend));
    assert(backend.error && model.yield_calls != 0U);

    init_async8(&backend, &model, 1);
    assert(minigl_rix_backend_prepare_target(&backend, target, 4U));
    queue_async_clear(&backend, 0U);
    arm_parked_feed_error(&backend, &model);
    assert(!minigl_rix_backend_make_cpu_visible(&backend, target, 4U));
    assert(backend.error && model.yield_calls != 0U);

    init_async8(&backend, &model, 1);
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, (uint32_t)(uintptr_t)target,
                                    0x03100000U, GL_COLOR_BUFFER_BIT, 0U, 0U));
    assert(minigl_rix_backend_submit(&backend));
    arm_parked_feed_error(&backend, &model);
    assert(!minigl_rix_backend_prepare_target(&backend, target, 4U));
    assert(backend.error && model.yield_calls != 0U);

    init_async8(&backend, &model, 1);
    model.texture_bytes = MINIGL_RIX_TEXTURE_PAGE_BYTES;
    backend.platform.texture_bytes = model.texture_bytes;
    assert(minigl_rix_backend_upload_texture(&backend, 1U, 0U, texture, 16U, 16U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    minigl_rix_backend_tex_params(&backend, 1U, GL_NEAREST, GL_NEAREST,
                                  GL_REPEAT, GL_REPEAT);
    assert(submit_texture(&backend, 1U));
    arm_parked_feed_error(&backend, &model);
    assert(!minigl_rix_backend_upload_texture(&backend, 1U, 0U, texture, 16U, 16U,
                                               ZZ9K_MGL_TEX_INTERNAL_RGB));
    assert(backend.error && model.yield_calls != 0U);
}

/* This is the unbind path's contract: a normal teardown drains every sealed
 * entry instead of throwing the queued frames away. */
static void test_async_unbind_drain_renders_all_queued_work(void)
{
    MiniGLRixBackend backend;
    struct model model;
    unsigned index;

    init_async8(&backend, &model, 0);
    model.yield_backend = &backend;
    backend.platform.yield_wait = yield_delayed_completion;
    model.fence_after_yields = 1U;
    model.idle_after_yields = 1U;
    for (index = 0U; index < MINIGL_RIX_COMMAND_BUFFERS; ++index)
        queue_async_clear(&backend, index);
    assert(minigl_rix_backend_drain(&backend));
    assert(model.packet_count == MINIGL_RIX_COMMAND_BUFFERS);
    for (index = 0U; index < MINIGL_RIX_COMMAND_BUFFERS; ++index)
        assert(backend.ring_state[index] == MINIGL_RIX_RING_FREE);
}

static void test_async_readpixels_drain_waits_for_delayed_completion(void)
{
    static uint32_t target[512] __attribute__((aligned(MINIGL_RIX_TEXTURE_PAGE_BYTES)));
    MiniGLRixBackend backend;
    struct model model;

    init_async8(&backend, &model, 0);
    model.yield_backend = &backend;
    backend.platform.yield_wait = yield_delayed_completion;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_BUSY;
    model.fence_after_yields = 1U;
    model.idle_after_yields = 2U;
    assert(minigl_rix_backend_prepare_target(&backend, target, 4U));
    queue_async_clear(&backend, 0U);
    assert(minigl_rix_backend_make_cpu_visible(&backend, target, 4U));
    assert(model.yield_calls >= 2U);
    assert(backend.ring_state[0] == MINIGL_RIX_RING_FREE);
}

static void test_async_wait_watchdog_disables_backend(void)
{
    MiniGLRixBackend backend;
    struct model model;

    init_async8(&backend, &model, 0);
    backend.command_buffer_count = 2U;
    backend.platform.yield_wait = yield_watchdog;
    g_now_us = 0U;
    g_watchdog_logs = 0U;
    g_rix_test_log = count_log;
    g_disabled_logs = 0U;
    queue_async_clear(&backend, 0U);
    queue_async_clear(&backend, 1U);
    assert(minigl_rix_backend_clear(&backend, 64U, 64U, 0x03000000U,
                                    0x03100000U, GL_COLOR_BUFFER_BIT, 2U, 0U));
    assert(!minigl_rix_backend_submit(&backend));
    assert(g_now_us >= 3000000U);
    assert(g_watchdog_logs == 4U);
    assert(g_disabled_logs == 1U && backend.error);
    g_rix_test_log = NULL;
}

static void test_async_wait_completion_at_deadline_succeeds(void)
{
    MiniGLRixBackend backend;
    struct model model;

    init_async8(&backend, &model, 0);
    model.yield_backend = &backend;
    backend.platform.yield_wait = yield_deadline_completion;
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_BUSY;
    g_now_us = 0U;
    queue_async_clear(&backend, 0U);
    assert(minigl_rix_backend_drain(&backend));
    assert(!backend.error && g_now_us == 3000000U);
}

static void test_async_error_does_not_block_retirement(void)
{
    MiniGLRixBackend backend;
    struct model model;

    init_async8(&backend, &model, 0);
    queue_async_clear(&backend, 0U);
    assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) ==
           RIX_DRV_FEED_DONE);
    *(uint16_t *)(void *)(model.command[0] + COMMAND_BYTES -
                          MINIGL_RIX_TEXTURE_PAGE_BYTES) =
        (uint16_t)backend.pending_fence[0];
    backend.error = 1U;
    assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_PARKED) ==
           RIX_DRV_FEED_ERROR);
    assert(backend.ring_state[0] == MINIGL_RIX_RING_FREE);
}

static void test_async_sequence_mismatch_disables_backend(void)
{
    MiniGLRixBackend backend;
    struct model model;

    init_async8(&backend, &model, 0);
    queue_async_clear(&backend, 0U);
    ++backend.submission_sequence[0];
    assert(minigl_rix_backend_try_feed(&backend, RIX_DRV_FEED_SOURCE_SVC) ==
           RIX_DRV_FEED_ERROR);
    assert(backend.error);
}

/* A feed error in the non-parking drain must not look like completion:
 * texture fence bookkeeping stays, and the caller must not reuse storage. */
static void test_bounded_drain_feed_error_is_not_success(void)
{
    MiniGLRixBackend backend;
    struct model model;

    init_async8(&backend, &model, 1);
    backend.platform.can_park = test_cannot_park;
    model.dma_pending_grants = 1U;
    model.dma_launch_state = RIX_DRV_DMA_ERROR;
    queue_async_clear(&backend, 1U);
    backend.textures[0].last_used_fence = 7U;
    assert(!minigl_rix_backend_drain(&backend));
    assert(backend.error);
    assert(backend.textures[0].last_used_fence == 7U);
}

static void test_async_poll_feed_error_is_not_success(void)
{
    MiniGLRixBackend backend;
    struct model model;

    init_async8(&backend, &model, 1);
    model.dma_launch_state = RIX_DRV_DMA_ERROR;
    queue_async_clear(&backend, 1U);
    backend.textures[0].last_used_fence = 9U;
    assert(!minigl_rix_backend_poll(&backend));
    assert(backend.error);
    assert(backend.textures[0].last_used_fence == 9U);
}

static void test_pinned_reupload_keeps_storage_when_drain_fails(void)
{
    static uint32_t source[16U * 16U];
    MiniGLRixBackend backend;
    struct model model;
    uint16_t page;
    uint16_t old;

    init_async8(&backend, &model, 1);
    backend.platform.can_park = test_cannot_park;
    model.texture_bytes = MINIGL_RIX_TEXTURE_PAGE_BYTES;
    backend.platform.texture_bytes = model.texture_bytes;
    memset(source, 0xff, sizeof(source));
    assert(minigl_rix_backend_upload_texture(&backend, 4U, 0U, source, 16U, 16U,
                                              ZZ9K_MGL_TEX_INTERNAL_RGB));
    minigl_rix_backend_tex_params(&backend, 4U, GL_NEAREST, GL_NEAREST,
                                  GL_REPEAT, GL_REPEAT);
    assert(submit_texture(&backend, 4U));
    assert(minigl_rix_backend_submit(&backend));
    assert(backend.textures[0].pin_count != 0U);
    page = backend.textures[0].first_page;
    old = stored_texel(&backend, &backend.textures[0], 0U);
    model.dma_pending_grants = 1U;
    model.dma_launch_state = RIX_DRV_DMA_ERROR;
    source[0] = 0x00ff00ffU;
    assert(!minigl_rix_backend_upload_texture(&backend, 4U, 0U, source, 16U, 16U,
                                               ZZ9K_MGL_TEX_INTERNAL_RGB));
    assert(backend.textures[0].valid);
    assert(backend.textures[0].first_page == page);
    assert(stored_texel(&backend, &backend.textures[0], 0U) == old);
}

int main(void)
{
    test_probe_version_compatibility();
    test_async_submit_queues_before_feed();
    test_async_memfetch_error_publishes_accepted_envelope();
    test_async_memfetch_abort_publishes_accepted_envelope();
    test_queued_work_does_not_keep_a_retired_fence_published();
    test_async_full_ring_parks();
    test_async_texture_reuse_parks();
    test_async_target_reuse_parks();
    test_async_feeds_submission_order_after_slot_reuse();
    test_async_stale_generation_is_not_fed();
    test_async_memfetch_keeps_ring_and_fence_lifecycle();
    test_idle_feed_steps_touch_no_hardware();
    test_memfetch_triangle_batch_spans_envelopes();
    test_failed_sync_memfetch_submit_retains_list();
    test_memfetch_abort_waits_for_list_fence();
    test_async_abort_preserves_staged_and_rendering_buffers();
    test_async_abort_waits_for_every_inflight_stage();
    test_async_unbind_drain_renders_all_queued_work();
    test_async_drain_waits_for_fence_and_idle();
    test_async_readpixels_drain_waits_for_delayed_completion();
    test_async_feed_error_wakes_parked_waiters();
    test_async_wait_watchdog_disables_backend();
    test_async_wait_completion_at_deadline_succeeds();
    test_async_error_does_not_block_retirement();
    test_async_sequence_mismatch_disables_backend();
    test_bounded_drain_feed_error_is_not_success();
    test_async_poll_feed_error_is_not_success();
    test_pinned_reupload_keeps_storage_when_drain_fails();
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

    test_async_feed_error_terminates_waits();
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
    /* A scissor box larger than the target is clipped, as GL does, not a
     * reason to send the draw to the CPU rasterizer. */
    state = state16();
    state.enabled_flags |= ZZ9K_MGL_STATE_SCISSOR_TEST;
    state.scissor_x = 0;
    state.scissor_y = 0;
    state.scissor_w = 640U;
    state.scissor_h = 480U;
    minigl_rix_backend_set_state(&backend, &state);
    assert(minigl_rix_backend_begin_draw(&backend, 64U, 64U, 0x03000000U, 0x03100000U));
    assert(backend.ops[0].scissor.start_x == 0U);
    assert(backend.ops[0].scissor.start_y == 0U);
    assert(backend.ops[0].scissor.end_x == 64U);
    assert(backend.ops[0].scissor.end_y == 64U);
    assert(!backend.ops[0].scissor_empty);
    assert(minigl_rix_backend_add_triangle(&backend, &a, &b, &c));
    assert(minigl_rix_backend_end_draw(&backend, 0x03000000U, 0x03100000U));
    assert(minigl_rix_backend_drain(&backend));
    state = state16();
    minigl_rix_backend_set_state(&backend, &state);
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
    test_texture_resampling_keeps_detail();
    test_rounded_up_mip_chain_completes();
    test_lru_eviction_and_subimage_reupload();
    test_subimage_writes_only_rectangle_and_copy_on_writes();
    test_reupload_copy_on_writes();
    test_abandon_retires_copy_only_after_engine_stop();
    test_texture_pool_growth();
    test_recorded_frame_replay();
    test_clear_preserves_depth_feature();
    test_overflow_splits_with_reload();
    test_many_small_textured_draws_flush_instead_of_refusing();
    test_repeated_state_is_emitted_once();
    test_depth_mapping_and_ordering();
    test_fallback_reasons_and_logging();
    test_timeout_diagnostics();
    test_abort_draw_rolls_back_recording();
    test_zero_fence_remains_pending();
    test_surface_handoff_and_recorded_texture_state();
    test_disabled_backend_is_one_shot();
    test_two_texture_multipass_recording();
    test_dma_self_test_gates_transport();
    test_failed_memfetch_self_test_retains_list();
    test_self_test_retirement_clears_published_fence();
    test_empty_triangle_stays_on_rasterix();
    test_async_drain_cannot_park_does_bounded_feed_and_returns_incomplete();
    test_async_feed_mmio_when_dma_start_nonblocking_null();
    puts("RasterIX EF backend: frame recording, FIFO replay and texture residency passed");
    return 0;
}
