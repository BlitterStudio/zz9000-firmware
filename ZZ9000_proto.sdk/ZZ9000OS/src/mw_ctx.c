/*
 * ZZ9000 MiniGL Wire Protocol v9 Context Implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mw_ctx.h"
#include "mw_parse.h"
#include "mw_draw.h"
#include "minigl_raster.h"
#include "minigl_lighting.h"
#include "card_pool.h"
#include "sdk_palette.h"
#include <stdlib.h>
#include <math.h>

#if defined(MINIGL_RASTERIX) && defined(__arm__) && !defined(MINIGL_HOST_TEST)
#include "xil_cache.h"
#include "xil_io.h"
#endif

#define MW_RIX_DMAC_BASE 0xf8004000U
#define MW_RIX_DMAC_CSR(channel) (0x100U + (channel) * 8U)
#define MW_RIX_DMAC_DBGSTATUS 0xd00U
#define MW_RIX_DMAC_DBGCMD 0xd04U
#define MW_RIX_DMAC_DBGINST0 0xd08U
#define MW_RIX_DMAC_DBGINST1 0xd0cU
#define MW_RIX_DMAC_CHANNEL 6U
#define MW_RIX_DMAC_STOPPED 0U
#define MW_RIX_PL330_DMAMOV 0xbcu
#define MW_RIX_PL330_DMALP 0x20u
#define MW_RIX_PL330_DMALPEND 0x28u
#define MW_RIX_PL330_DMALD 0x04u
#define MW_RIX_PL330_DMAST 0x08u
#define MW_RIX_PL330_DMAWMB 0x13u
#define MW_RIX_PL330_DMAEND 0x00u
#define MW_RIX_PL330_DMAGO 0xa0u
#define MW_RIX_PL330_DMAKILL 0x01u
#define MW_RIX_PL330_RD_SAR 0u
#define MW_RIX_PL330_RD_CCR 1u
#define MW_RIX_PL330_RD_DAR 2u

#define GL_TEXTURE_2D 0x0de1U
#define MW_RIX_TARGET_ALIGNMENT 2048U


static uint32_t g_next_handle = 1U;

#define MW_GL_INVALID_OPERATION 0x0502U
#define MW_GL_OUT_OF_MEMORY     0x0505U

static void mw_ctx_set_error(mw_ctx *c, uint32_t error)
{
    if (c->error == 0U)
        c->error = error;
}

#if defined(__arm__) && !defined(MINIGL_HOST_TEST)
#define MW_CONTEXT_CARD_POOL 1
#else
#define MW_CONTEXT_CARD_POOL 0
#endif

static void mw_context_free(mw_ctx *c, void *memory, uint32_t *address);

static void *mw_context_alloc(mw_ctx *c, uint32_t bytes, uint32_t *address)
{
#if MW_CONTEXT_CARD_POOL
    uint32_t card_address = card_pool_alloc(&card_pool, bytes, c->card_pool_owner);
    if (!card_address)
        return NULL;
    /* card_pool_alloc() returns 64 KiB pages, stronger than EF's 2 KiB
     * colour/depth alignment requirement. */
    *address = card_address;
    memset((void *)(uintptr_t)card_address, 0, bytes);
    return (void *)(uintptr_t)card_address;
#else
    uintptr_t aligned;
    void *raw;

    (void)c;
    raw = calloc(1, (size_t)bytes + MW_RIX_TARGET_ALIGNMENT - 1U + sizeof(raw));
    if (raw == NULL)
        return NULL;
    aligned = ((uintptr_t)raw + sizeof(raw) + MW_RIX_TARGET_ALIGNMENT - 1U) &
              ~(uintptr_t)(MW_RIX_TARGET_ALIGNMENT - 1U);
    ((void **)aligned)[-1] = raw;
    *address = (uint32_t)aligned;
    return (void *)aligned;
#endif
}

static void *mw_context_alloc_uninitialized(mw_ctx *c, uint32_t bytes,
                                            uint32_t *address)
{
#if MW_CONTEXT_CARD_POOL
    uint32_t card_address = card_pool_alloc(&card_pool, bytes, c->card_pool_owner);
    if (!card_address)
        return NULL;
    *address = card_address;
    return (void *)(uintptr_t)card_address;
#else
    uintptr_t aligned;
    void *raw;

    (void)c;
    raw = malloc((size_t)bytes + MW_RIX_TARGET_ALIGNMENT - 1U + sizeof(raw));
    if (raw == NULL)
        return NULL;
    aligned = ((uintptr_t)raw + sizeof(raw) + MW_RIX_TARGET_ALIGNMENT - 1U) &
              ~(uintptr_t)(MW_RIX_TARGET_ALIGNMENT - 1U);
    ((void **)aligned)[-1] = raw;
    *address = (uint32_t)aligned;
    return (void *)aligned;
#endif
}

static void mw_context_free(mw_ctx *c, void *memory, uint32_t *address)
{
#if MW_CONTEXT_CARD_POOL
    (void)memory;
    if (*address)
        (void)card_pool_free(&card_pool, *address, c->card_pool_owner);
    *address = 0;
#else
    (void)c;
    (void)address;
    if (memory != NULL)
        free(((void **)memory)[-1]);
#endif
}

static uint32_t mw_color_from_floats(float r, float g, float b, float a)
{
    uint8_t ur = (uint8_t)(r < 0.0f ? 0.0f : r > 1.0f ? 255.0f : r * 255.0f);
    uint8_t ug = (uint8_t)(g < 0.0f ? 0.0f : g > 1.0f ? 255.0f : g * 255.0f);
    uint8_t ub = (uint8_t)(b < 0.0f ? 0.0f : b > 1.0f ? 255.0f : b * 255.0f);
    uint8_t ua = (uint8_t)(a < 0.0f ? 0.0f : a > 1.0f ? 255.0f : a * 255.0f);
    return ((uint32_t)ur << 24) | ((uint32_t)ug << 16) | ((uint32_t)ub << 8) | (uint32_t)ua;
}

static uint8_t g_dummy_vertices[256];
static uint8_t g_dummy_staging[256];
static uint8_t g_dummy_texture[1];
/* Core 1 builds one CLUT table at a time; staging prevents a rejected
 * palette generation from replacing a context's last known-good table. */
static uint8_t g_presented_clut_staging[32U * 32U * 32U];


static int mw_ctx_grow_texture_pool(void *context, uint32_t minimum_bytes,
                                    uint32_t used_bytes, uint8_t **pool,
                                    uint32_t *pool_bytes)
{
    mw_ctx *c = (mw_ctx *)context;
    uint32_t new_bytes = c->texture_pool == g_dummy_texture ?
                         0U : c->texture_pool_bytes;
    uint32_t new_address = 0U;
    uint8_t *new_pool;

    if (minimum_bytes > MINIGL_TEXTURE_CPU_ARENA_MAX_BYTES)
        return 0;
    if (new_bytes == 0U)
        new_bytes = MINIGL_TEXTURE_CPU_ARENA_INITIAL_BYTES;
    while (new_bytes < minimum_bytes) {
        if (new_bytes > MINIGL_TEXTURE_CPU_ARENA_MAX_BYTES -
                        MINIGL_TEXTURE_ARENA_GROW_BYTES) {
            new_bytes = MINIGL_TEXTURE_CPU_ARENA_MAX_BYTES;
            break;
        }
        new_bytes += MINIGL_TEXTURE_ARENA_GROW_BYTES;
    }
    new_pool = (uint8_t *)mw_context_alloc_uninitialized(c, new_bytes,
                                                          &new_address);
    if (!new_pool)
        return 0;
    if (c->texture_pool != g_dummy_texture) {
        memcpy(new_pool, c->texture_pool, used_bytes);
        mw_context_free(c, c->texture_pool, &c->texture_pool_address);
    }
    c->texture_pool = new_pool;
    c->texture_pool_address = new_address;
    c->texture_pool_bytes = new_bytes;
    *pool = new_pool;
    *pool_bytes = new_bytes;
    return 1;
}

#if defined(MINIGL_RASTERIX) && defined(__arm__) && !defined(MINIGL_HOST_TEST)

static uint32_t mw_rix_csr_read(void *context, uint32_t offset)
{
    (void)context;
    return Xil_In32(MINIGL_RIX_CSR_BASE + offset);
}

static void mw_rix_csr_write(void *context, uint32_t offset, uint32_t value)
{
    (void)context;
    Xil_Out32(MINIGL_RIX_CSR_BASE + offset, value);
}

static uint32_t mw_rix_fifo_read(void *context, uint32_t offset)
{
    (void)context;
    return Xil_In32(MINIGL_RIX_FIFO_BASE + offset);
}

static void mw_rix_fifo_write(void *context, uint32_t offset, uint32_t value)
{
    (void)context;
    Xil_Out32(MINIGL_RIX_FIFO_BASE + offset, value);
}

/* Core1 owns this program.  Commands are cache-cleaned by rix_drv; the
 * conservative single-store path remains in force until a FIFO integrity test
 * qualifies 16-beat fixed-destination bursts. */
static uint8_t mw_rix_dma_program[128] __attribute__((aligned(64)));

static void mw_rix_dma_barrier(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static unsigned mw_rix_dma_word(uint8_t *program, unsigned offset, uint32_t value)
{
    program[offset++] = (uint8_t)value;
    program[offset++] = (uint8_t)(value >> 8U);
    program[offset++] = (uint8_t)(value >> 16U);
    program[offset++] = (uint8_t)(value >> 24U);
    return offset;
}

static uint32_t mw_rix_dma_ccr(uint32_t words)
{
    return 1U | (2U << 1U) | ((words - 1U) << 4U) | (2U << 15U);
}

static int mw_rix_dma_stream(void *context, uintptr_t source, uint32_t words,
                             uint32_t burst_words)
{
    uint8_t *program = mw_rix_dma_program;
    uint32_t blocks = words / 16U, tail = words % 16U, attempt;
    unsigned offset = 0U, body, store, loop;

    (void)context;
    if (source == 0U || words == 0U || words > RIX_DRV_FIFO_PACKET_WORDS ||
        burst_words != 16U)
        return 0;
    program[offset++] = MW_RIX_PL330_DMAMOV; program[offset++] = MW_RIX_PL330_RD_SAR;
    offset = mw_rix_dma_word(program, offset, (uint32_t)source);
    program[offset++] = MW_RIX_PL330_DMAMOV; program[offset++] = MW_RIX_PL330_RD_DAR;
    offset = mw_rix_dma_word(program, offset, MINIGL_RIX_FIFO_BASE + RIX_DRV_FIFO_TDFD);
    if (blocks != 0U) {
        program[offset++] = MW_RIX_PL330_DMAMOV; program[offset++] = MW_RIX_PL330_RD_CCR;
        offset = mw_rix_dma_word(program, offset, mw_rix_dma_ccr(16U));
        program[offset++] = MW_RIX_PL330_DMALP | 2U; program[offset++] = (uint8_t)(blocks - 1U);
        body = offset; program[offset++] = MW_RIX_PL330_DMALD;
        program[offset++] = MW_RIX_PL330_DMALP; program[offset++] = 15U;
        store = offset; program[offset++] = MW_RIX_PL330_DMAST;
        loop = offset; program[offset++] = MW_RIX_PL330_DMALPEND | 0x10U;
        program[offset++] = (uint8_t)(loop - store);
        loop = offset; program[offset++] = MW_RIX_PL330_DMALPEND | 0x14U;
        program[offset++] = (uint8_t)(loop - body);
    }
    if (tail != 0U) {
        program[offset++] = MW_RIX_PL330_DMAMOV; program[offset++] = MW_RIX_PL330_RD_CCR;
        offset = mw_rix_dma_word(program, offset, mw_rix_dma_ccr(tail));
        program[offset++] = MW_RIX_PL330_DMALD;
        program[offset++] = MW_RIX_PL330_DMALP; program[offset++] = (uint8_t)(tail - 1U);
        store = offset; program[offset++] = MW_RIX_PL330_DMAST;
        loop = offset; program[offset++] = MW_RIX_PL330_DMALPEND | 0x10U;
        program[offset++] = (uint8_t)(loop - store);
    }
    program[offset++] = MW_RIX_PL330_DMAWMB; program[offset++] = MW_RIX_PL330_DMAEND;
    if (offset > sizeof(mw_rix_dma_program))
        return 0;
    Xil_DCacheFlushRange((INTPTR)program, (u32)sizeof(mw_rix_dma_program));
    mw_rix_dma_barrier();
    for (attempt = 0U; attempt < 100000U; ++attempt)
        if ((Xil_In32(MW_RIX_DMAC_BASE + MW_RIX_DMAC_DBGSTATUS) & 1U) == 0U)
            break;
    if (attempt == 100000U)
        return 0;
    Xil_Out32(MW_RIX_DMAC_BASE + MW_RIX_DMAC_DBGINST0,
              (MW_RIX_PL330_DMAGO << 16U) | (MW_RIX_DMAC_CHANNEL << 24U));
    Xil_Out32(MW_RIX_DMAC_BASE + MW_RIX_DMAC_DBGINST1, (uint32_t)(uintptr_t)program);
    mw_rix_dma_barrier();
    Xil_Out32(MW_RIX_DMAC_BASE + MW_RIX_DMAC_DBGCMD, 0U);
    mw_rix_dma_barrier();
    for (attempt = 0U; attempt < 1000000U; ++attempt) {
        uint32_t state = Xil_In32(MW_RIX_DMAC_BASE +
                                  MW_RIX_DMAC_CSR(MW_RIX_DMAC_CHANNEL)) & 15U;
        if (state == MW_RIX_DMAC_STOPPED)
            return 1;
        if (state == 14U || state == 15U)
            break;
    }
    Xil_Out32(MW_RIX_DMAC_BASE + MW_RIX_DMAC_DBGINST0,
              (MW_RIX_PL330_DMAKILL << 16U) | (MW_RIX_DMAC_CHANNEL << 8U) | 1U);
    Xil_Out32(MW_RIX_DMAC_BASE + MW_RIX_DMAC_DBGINST1, 0U);
    Xil_Out32(MW_RIX_DMAC_BASE + MW_RIX_DMAC_DBGCMD, 0U);
    return 0;
}

static void mw_rix_cache_clean(void *context, uintptr_t address, size_t bytes)
{
    (void)context;
    Xil_DCacheFlushRange((INTPTR)address, (u32)bytes);
}

static void mw_rix_cache_invalidate(void *context, uintptr_t address, size_t bytes)
{
    (void)context;
    Xil_DCacheInvalidateRange((INTPTR)address, (u32)bytes);
}

static int mw_ctx_grow_rix_texture_pool(void *context, size_t minimum_bytes,
                                        uint8_t **memory, uintptr_t *address,
                                        size_t *bytes)
{
    mw_ctx *c = (mw_ctx *)context;
    uint32_t new_bytes = c->rix_texture_pool_bytes;
    uint32_t new_address = 0U;
    uint8_t *new_pool;

    if (minimum_bytes > MINIGL_RIX_TEXTURE_POOL_MAX_BYTES)
        return 0;
    if (new_bytes == 0U)
        new_bytes = MINIGL_RIX_TEXTURE_POOL_INITIAL_BYTES;
    while (new_bytes < minimum_bytes) {
        if (new_bytes > MINIGL_RIX_TEXTURE_POOL_MAX_BYTES -
                        MINIGL_TEXTURE_ARENA_GROW_BYTES) {
            new_bytes = MINIGL_RIX_TEXTURE_POOL_MAX_BYTES;
            break;
        }
        new_bytes += MINIGL_TEXTURE_ARENA_GROW_BYTES;
    }
    new_pool = (uint8_t *)mw_context_alloc_uninitialized(c, new_bytes,
                                                          &new_address);
    if (!new_pool)
        return 0;
    if (c->rix_texture_pool) {
        memcpy(new_pool, c->rix_texture_pool, c->rix_texture_pool_bytes);
        mw_rix_cache_clean(NULL, (uintptr_t)new_pool,
                           c->rix_texture_pool_bytes);
        mw_context_free(c, c->rix_texture_pool, &c->rix_texture_pool_address);
    }
    c->rix_texture_pool = new_pool;
    c->rix_texture_pool_address = new_address;
    c->rix_texture_pool_bytes = new_bytes;
    *memory = new_pool;
    *address = new_address;
    *bytes = new_bytes;
    return 1;
}

static void mw_ctx_release_rix(mw_ctx *c)
{
    unsigned index;

    for (index = 0U; index < MINIGL_RIX_COMMAND_BUFFERS; ++index) {
        if (c->rix_command_buffers[index]) {
            mw_context_free(c, c->rix_command_buffers[index],
                            &c->rix_command_buffer_addresses[index]);
            c->rix_command_buffers[index] = NULL;
        }
    }
    if (c->rix_white_texture) {
        mw_context_free(c, c->rix_white_texture, &c->rix_white_texture_address);
        c->rix_white_texture = NULL;
    }
    if (c->rix_texture_pool) {
        mw_context_free(c, c->rix_texture_pool, &c->rix_texture_pool_address);
        c->rix_texture_pool = NULL;
        c->rix_texture_pool_bytes = 0U;
    }
    c->rix_enabled = 0U;
}

static void mw_ctx_configure_rix(mw_ctx *c)
{
    MiniGLRixPlatform platform;
    unsigned index;

    memset(&platform, 0, sizeof(platform));
    platform.context = c;
    platform.csr_read = mw_rix_csr_read;
    platform.csr_write = mw_rix_csr_write;
    platform.fifo_read = mw_rix_fifo_read;
    platform.fifo_write = mw_rix_fifo_write;
    platform.dma_stream = mw_rix_dma_stream;
    platform.cache_clean = mw_rix_cache_clean;
    platform.cache_invalidate = mw_rix_cache_invalidate;
    /* This probe reads only the versioned CSR identity pair. No RIX address is
     * touched by standard firmware because this whole path is build-time opt-in. */
    if (!minigl_rix_backend_probe_platform(&platform)) {
        minigl_raster_set_rix_platform(NULL);
        return;
    }
    for (index = 0U; index < MINIGL_RIX_COMMAND_BUFFERS; ++index) {
        c->rix_command_buffers[index] = (uint8_t *)mw_context_alloc(
            c, MINIGL_RIX_COMMAND_BYTES, &c->rix_command_buffer_addresses[index]);
        if (!c->rix_command_buffers[index])
            goto unavailable;
        platform.command_buffer[index] = c->rix_command_buffers[index];
        platform.command_address[index] = c->rix_command_buffer_addresses[index];
    }
    c->rix_white_texture = (uint8_t *)mw_context_alloc(
        c, MINIGL_RIX_WHITE_TEXTURE_BYTES, &c->rix_white_texture_address);
    if (!c->rix_white_texture)
        goto unavailable;
    platform.command_capacity = MINIGL_RIX_COMMAND_BYTES;
    platform.white_texture = c->rix_white_texture;
    platform.white_texture_address = c->rix_white_texture_address;
    platform.white_texture_bytes = MINIGL_RIX_WHITE_TEXTURE_BYTES;
    platform.texture_memory = NULL;
    platform.texture_address = 0U;
    platform.texture_bytes = 0U;
    platform.grow_texture_pool = mw_ctx_grow_rix_texture_pool;
    minigl_raster_set_rix_platform(&platform);
    c->rix_enabled = 1U;
    return;

unavailable:
    mw_ctx_release_rix(c);
    minigl_raster_set_rix_platform(NULL);
}
#else
static void mw_ctx_release_rix(mw_ctx *c)
{
    (void)c;
}

static void mw_ctx_configure_rix(mw_ctx *c)
{
    (void)c;
}
#endif

static inline void mw_ctx_update_latest(mw_ctx *c)
{
    if (c->state.draw_buffer == 0) return; /* GL_NONE */
    int front = (c->state.draw_buffer == 0x404 || c->state.draw_buffer == 0x400);
    c->state.target.latest = front ? c->state.target.front : c->state.target.back;
}

static int mw_ctx_bind_buffers(mw_ctx *c)
{
    MiniGLRasterBuffers buffers;
    int draw_front;
    memset(&buffers, 0, sizeof(buffers));
    minigl_raster_set_texture_pool(c->texture_pool, c->texture_pool_bytes,
                                   mw_ctx_grow_texture_pool, c);
    draw_front = (c->state.draw_buffer == 0x404 || c->state.draw_buffer == 0x400) && (c->state.target.buffer_count > 1);
    buffers.color = c->color_buffers[draw_front ? c->state.target.front : c->state.target.back];
    buffers.color_bytes = c->width * c->height * sizeof(uint32_t);
    buffers.depth = c->depth_buffer;
    buffers.depth_bytes = c->width * c->height * 4;
    buffers.vertices = g_dummy_vertices;
    buffers.vertices_bytes = sizeof(g_dummy_vertices);
    buffers.texture_staging = g_dummy_staging;
    buffers.texture_staging_bytes = sizeof(g_dummy_staging);
    buffers.texture_pool = c->texture_pool;
    buffers.texture_pool_bytes = c->texture_pool_bytes;
    buffers.width = (uint16_t)c->width;
    buffers.height = (uint16_t)c->height;
    return minigl_raster_configure_buffers(&buffers);
}

static int mw_ctx_replace_rix_target(void *context,
                                     MiniGLRasterBuffers *replacement)
{
    mw_ctx *c = (mw_ctx *)context;
    uint32_t *new_colors[3] = { NULL, NULL, NULL };
    uint8_t *new_depth = NULL;
    uint32_t new_color_addresses[3] = { 0U, 0U, 0U };
    uint32_t new_depth_address = 0U;
    size_t color_bytes;
    size_t depth_bytes;
    int index;

    if (!c || !replacement || c->rix_target_quarantined)
        return 0;
    color_bytes = (size_t)c->width * c->height * sizeof(*new_colors[0]);
    depth_bytes = (size_t)c->width * c->height * 4U;
    for (index = 0; index < 3; ++index) {
        new_colors[index] = (uint32_t *)mw_context_alloc(
            c, (uint32_t)color_bytes, &new_color_addresses[index]);
        if (!new_colors[index])
            goto allocation_failed;
        memset(new_colors[index], 0, color_bytes);
    }
    new_depth = (uint8_t *)mw_context_alloc(c, (uint32_t)depth_bytes,
                                             &new_depth_address);
    if (!new_depth)
        goto allocation_failed;

    for (index = 0; index < 3; ++index) {
        c->rix_quarantined_color_buffers[index] = c->color_buffers[index];
        c->rix_quarantined_color_addresses[index] =
            c->color_buffer_addresses[index];
        c->color_buffers[index] = new_colors[index];
        c->color_buffer_addresses[index] = new_color_addresses[index];
    }
    c->rix_quarantined_depth_buffer = c->depth_buffer;
    c->rix_quarantined_depth_address = c->depth_buffer_address;
    c->depth_buffer = new_depth;
    c->depth_buffer_address = new_depth_address;
    c->rix_target_quarantined = 1U;

    memset(replacement, 0, sizeof(*replacement));
    replacement->color = c->color_buffers[(c->state.draw_buffer == 0x404 ||
                                           c->state.draw_buffer == 0x400) &&
                                          c->state.target.buffer_count > 1 ?
                                          c->state.target.front :
                                          c->state.target.back];
    replacement->color_bytes = (uint32_t)color_bytes;
    replacement->depth = c->depth_buffer;
    replacement->depth_bytes = (uint32_t)depth_bytes;
    replacement->vertices = g_dummy_vertices;
    replacement->vertices_bytes = sizeof(g_dummy_vertices);
    replacement->texture_staging = g_dummy_staging;
    replacement->texture_staging_bytes = sizeof(g_dummy_staging);
    replacement->texture_pool = c->texture_pool;
    replacement->texture_pool_bytes = c->texture_pool_bytes;
    replacement->width = (uint16_t)c->width;
    replacement->height = (uint16_t)c->height;
    return 1;

allocation_failed:
    for (index = 0; index < 3; ++index)
        mw_context_free(c, new_colors[index], &new_color_addresses[index]);
    mw_context_free(c, new_depth, &new_depth_address);
    return 0;
}

static void mw_ctx_sync_to_raster(mw_ctx *c)
{
    ZZ9KMiniGLPacketSetState rstate;
    uint32_t flags = 0;
    mw_state *s = &c->state;

    memset(&rstate, 0, sizeof(rstate));
    rstate.depth_bits = (uint16_t)s->target.depth_bits;
    if (s->force_cpu) rstate.reserved |= ZZ9K_MGL_STATE_FLAG_FORCE_CPU;
    if (s->depth_test) flags |= ZZ9K_MGL_STATE_DEPTH_TEST;
    if (s->depth_mask) flags |= ZZ9K_MGL_STATE_DEPTH_WRITE;
    if (s->blend_enabled) flags |= ZZ9K_MGL_STATE_BLEND;
    if (s->alpha_test) flags |= ZZ9K_MGL_STATE_ALPHA_TEST;
    if (s->cull_enabled) flags |= ZZ9K_MGL_STATE_CULL_FACE;
    if (s->scissor_enabled) flags |= ZZ9K_MGL_STATE_SCISSOR_TEST;
    if (s->offset_fill) flags |= ZZ9K_MGL_STATE_POLYGON_OFFSET_FILL;
    if (s->offset_line) flags |= ZZ9K_MGL_STATE_POLYGON_OFFSET_LINE;
    if (s->offset_point) flags |= ZZ9K_MGL_STATE_POLYGON_OFFSET_POINT;
    if (s->stages[0].enabled) flags |= ZZ9K_MGL_STATE_TEXTURE_2D_0;
    if (s->stages[1].enabled) flags |= ZZ9K_MGL_STATE_TEXTURE_2D_1;
    if (s->fog_enabled) flags |= ZZ9K_MGL_STATE_FOG;
    rstate.enabled_flags = flags;
    rstate.viewport_x = (int16_t)s->viewport_x;
    rstate.viewport_y = (int16_t)s->viewport_y;
    rstate.viewport_w = (uint16_t)s->viewport_w;
    rstate.viewport_h = (uint16_t)s->viewport_h;
    rstate.scissor_x = (int16_t)s->scissor_x;
    rstate.scissor_y = (int16_t)s->scissor_y;
    rstate.scissor_w = (uint16_t)s->scissor_w;
    rstate.scissor_h = (uint16_t)s->scissor_h;
    rstate.depth_func = (uint16_t)s->depth_func;
    rstate.depth_near = (uint32_t)(s->depth_range_near * 16777215.0f);
    rstate.depth_far = (uint32_t)(s->depth_range_far * 16777215.0f);
    rstate.blend_src = (uint16_t)s->blend_src_rgb;
    rstate.blend_dst = (uint16_t)s->blend_dst_rgb;
    rstate.blend_src_alpha = (uint16_t)s->blend_src_alpha;
    rstate.blend_dst_alpha = (uint16_t)s->blend_dst_alpha;
    rstate.blend_equation = (uint16_t)s->blend_equation;
    rstate.alpha_func = (uint16_t)s->alpha_func;
    rstate.alpha_ref = (uint16_t)(s->alpha_ref * 255.0f);
    rstate.cull_face_mode = (uint16_t)s->cull_face;
    rstate.front_face = (uint16_t)s->front_face;
    rstate.color_mask = (s->color_mask[0] ? 1U : 0U) |
                        (s->color_mask[1] ? 2U : 0U) |
                        (s->color_mask[2] ? 4U : 0U) |
                        (s->color_mask[3] ? 8U : 0U);
    rstate.shade_model = (uint16_t)s->shade_model;
    rstate.polygon_offset_factor = s->polygon_offset_factor;
    rstate.polygon_offset_units = s->polygon_offset_units;
    rstate.point_size = (uint16_t)s->point_size;
    rstate.line_width = (uint16_t)s->line_width;
    rstate.tex_env_mode[0] = (uint16_t)s->stages[0].environment;
    rstate.tex_env_mode[1] = (uint16_t)s->stages[1].environment;
    rstate.tex_env_color[0] = mw_color_from_floats(s->stages[0].env_color[0], s->stages[0].env_color[1],
                                                  s->stages[0].env_color[2], s->stages[0].env_color[3]);
    rstate.tex_env_color[1] = mw_color_from_floats(s->stages[1].env_color[0], s->stages[1].env_color[1],
                                                  s->stages[1].env_color[2], s->stages[1].env_color[3]);
    rstate.fog_mode = (uint16_t)s->fog_mode;
    rstate.fog_density = s->fog_density;
    rstate.fog_start = s->fog_start;
    rstate.fog_end = s->fog_end;
    rstate.fog_color = mw_color_from_floats(s->fog_color[0], s->fog_color[1],
                                            s->fog_color[2], s->fog_color[3]);
    minigl_raster_set_wire_v9_state(&c->state);
    minigl_lighting_configure_wire_v9(&c->state);

    minigl_raster_set_state(&rstate);
    minigl_raster_load_matrix(0, s->modelview);
    minigl_raster_load_matrix(1, s->projection);
    minigl_raster_load_matrix(2, s->texture_matrix[0]);
    minigl_raster_load_matrix(3, s->texture_matrix[1]);

    if (s->stages[0].enabled)
        minigl_raster_bind_texture(0, GL_TEXTURE_2D, s->stages[0].name);
    if (s->stages[1].enabled)
        minigl_raster_bind_texture(1, GL_TEXTURE_2D, s->stages[1].name);
}

mw_ctx *mw_host_create(uint32_t width, uint32_t height)
{
    return mw_host_create_owned(width, height,
                                CARD_POOL_OWNER(CARD_POOL_CLASS_MINIGL,
                                                g_next_handle));
}

mw_ctx *mw_host_create_owned(uint32_t width, uint32_t height, uint32_t owner)
{
    mw_ctx *c;
    int i;

    if (!width || !height || width > MW_MAX_DIMENSION || height > MW_MAX_DIMENSION)
        return NULL;

    c = (mw_ctx *)calloc(1, sizeof(mw_ctx));
    if (!c)
        return NULL;
    c->card_pool_owner = owner;
    c->card_pool_backed = MW_CONTEXT_CARD_POOL;
    c->width = width;
    c->height = height;
    mw_state_init(&c->state, width, height);
    mw_texmgr_init_owned(&c->texmgr, owner);
    if (!c->texmgr.textures) {
        mw_host_destroy(c);
        return NULL;
    }

    for (i = 0; i < 3; ++i) {
        c->color_buffers[i] = (uint32_t *)mw_context_alloc(
            c, width * height * sizeof(*c->color_buffers[i]),
            &c->color_buffer_addresses[i]);
        if (!c->color_buffers[i]) {
            mw_host_destroy(c);
            return NULL;
        }
        for (size_t px = 0; px < (size_t)width * height; ++px)
            c->color_buffers[i][px] = 0x000000ffU;
    }

    c->depth_buffer = (uint8_t *)mw_context_alloc(
        c, width * height * 4U, &c->depth_buffer_address);
    if (!c->depth_buffer) {
        mw_host_destroy(c);
        return NULL;
    }

    /* Resident CPU texels grow in 16 MiB steps only when uploaded.  The
     * one-byte sentinel keeps empty contexts out of the card pool. */
    c->texture_pool = g_dummy_texture;
    c->texture_pool_bytes = sizeof(g_dummy_texture);

    c->vertex_scratch_capacity = MW_MAX_VERTICES * MW_OBJECT_WORDS;
    c->vertex_scratch = (float *)mw_context_alloc(
        c, c->vertex_scratch_capacity * sizeof(*c->vertex_scratch),
        &c->vertex_scratch_address);
    if (!c->vertex_scratch) {
        mw_host_destroy(c);
        return NULL;
    }

    c->handle = g_next_handle++;
    mw_ctx_configure_rix(c);
    minigl_raster_set_fail_stop_target_handler(mw_ctx_replace_rix_target, c);
    minigl_raster_set_texture_pool(c->texture_pool, c->texture_pool_bytes,
                                   mw_ctx_grow_texture_pool, c);
    if (!mw_ctx_bind_buffers(c)) {
        mw_host_destroy(c);
        return NULL;
    }
    minigl_raster_init();
    mw_ctx_sync_to_raster(c);

    return c;
}

void mw_host_destroy(mw_ctx *c)
{
    int i;

    if (!c)
        return;
    if (!minigl_raster_quiesce() && c->rix_enabled)
        return;
    minigl_raster_set_fail_stop_target_handler(NULL, NULL);
    if (c->rix_target_quarantined) {
#if MW_CONTEXT_CARD_POOL
        /* The disabled engine may still write any allocation owned by this
         * context.  The one permitted fail-stop leaves that whole owner
         * intact rather than risking a card-pool record being recycled. */
        free(c);
        return;
#else
        for (i = 0; i < 3; ++i)
            mw_context_free(c, c->rix_quarantined_color_buffers[i],
                            &c->rix_quarantined_color_addresses[i]);
        mw_context_free(c, c->rix_quarantined_depth_buffer,
                        &c->rix_quarantined_depth_address);
#endif
    }
    mw_ctx_release_rix(c);

    for (i = 0; i < 3; ++i) {
        if (c->color_buffers[i]) {
            mw_context_free(c, c->color_buffers[i], &c->color_buffer_addresses[i]);
            c->color_buffers[i] = NULL;
        }
    }
    if (c->depth_buffer) {
        mw_context_free(c, c->depth_buffer, &c->depth_buffer_address);
        c->depth_buffer = NULL;
    }
    if (c->texture_pool && c->texture_pool != g_dummy_texture) {
        mw_context_free(c, c->texture_pool, &c->texture_pool_address);
        c->texture_pool = NULL;
    }
    if (c->vertex_scratch) {
        mw_context_free(c, c->vertex_scratch, &c->vertex_scratch_address);
        c->vertex_scratch = NULL;
    }
    mw_texmgr_destroy(&c->texmgr);
#if MW_CONTEXT_CARD_POOL
    /* Catch every card-pool block belonging to this context, including one
     * from an interrupted resize, before its handle can be reused. */
    (void)card_pool_release_owner(&card_pool, c->card_pool_owner);
#endif
    free(c);
}

static int mw_ensure_scratch(mw_ctx *c, uint32_t needed_words)
{
    if (c->vertex_scratch_capacity < needed_words) {
        uint32_t new_address = 0;
        float *new_scratch = (float *)mw_context_alloc_uninitialized(
            c, needed_words * sizeof(*new_scratch), &new_address);

        if (!new_scratch)
            return 0;
        memcpy(new_scratch, c->vertex_scratch,
               c->vertex_scratch_capacity * sizeof(*new_scratch));
        mw_context_free(c, c->vertex_scratch, &c->vertex_scratch_address);
        c->vertex_scratch = new_scratch;
        c->vertex_scratch_address = new_address;
        c->vertex_scratch_capacity = needed_words;
    }
    return 1;
}

/* GL_GENERATE_MIPMAP fills the texture manager's own level buffers; the
 * rasterizer keeps an independent texture copy, so every generated level
 * must also be uploaded there -- the reference host regenerates the chain
 * on the GPU the same way. Level 0 is uploaded by the image commands. */
static int mw_upload_generated_mips(mw_ctx *c, const mw_texture *t)
{
    uint32_t lvl;

    (void)c;
    if (!t)
        return 0;
    for (lvl = 1; lvl < MW_MAX_MIP_LEVELS; ++lvl) {
        if (t->pixels[lvl] && t->widths[lvl] && t->heights[lvl] &&
            !minigl_raster_upload_rgba8(t->id, (uint16_t)lvl,
                                        (uint16_t)t->widths[lvl],
                                        (uint16_t)t->heights[lvl],
                                        t->pixels[lvl], t->internal_rgb[lvl]))
            return 0;
    }
    return 1;
}

static int mw_execute_command(void *user, const mw_command *cmd)
{
    mw_ctx *c = (mw_ctx *)user;
    const uint8_t *p = cmd->payload;

    switch (cmd->opcode) {
    case MW_TARGET: {
        uint32_t w = mw_be32(p);
        uint32_t h = mw_be32(p + 4);
        uint32_t buf_count = mw_be32(p + 8);
        uint32_t depth_bits = mw_be32(p + 12);
        uint32_t pres_w = mw_be32(p + 16);
        uint32_t pres_h = mw_be32(p + 20);
        uint32_t *new_colors[3] = { NULL, NULL, NULL };
        uint32_t *old_colors[3];
        uint8_t *new_depth = NULL;
        uint8_t *old_depth;
        uint32_t new_color_addresses[3] = { 0U, 0U, 0U };
        uint32_t old_color_addresses[3];
        uint32_t new_depth_address = 0U;
        uint32_t old_depth_address;
        uint32_t old_width = c->width;
        uint32_t old_height = c->height;
        uint32_t old_target_width = c->state.target.width;
        uint32_t old_target_height = c->state.target.height;
        uint32_t old_buffer_count = c->state.target.buffer_count;
        int32_t old_viewport_x = c->state.viewport_x;
        int32_t old_viewport_y = c->state.viewport_y;
        uint32_t old_viewport_w = c->state.viewport_w;
        uint32_t old_viewport_h = c->state.viewport_h;
        int resize = w != c->width || h != c->height;
        int rebind = resize || buf_count != c->state.target.buffer_count;
        int i;

        if (rebind && !minigl_raster_finish()) {
            mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
            goto command_complete;
        }
        if (resize) {
            for (i = 0; i < 3; ++i) {
                old_colors[i] = c->color_buffers[i];
                old_color_addresses[i] = c->color_buffer_addresses[i];
                new_colors[i] = (uint32_t *)mw_context_alloc(
                    c, w * h * sizeof(*new_colors[i]), &new_color_addresses[i]);
                if (!new_colors[i])
                    goto target_allocation_failed;
                for (size_t px = 0; px < (size_t)w * h; ++px)
                    new_colors[i][px] = 0x000000ffU;
            }
            old_depth = c->depth_buffer;
            old_depth_address = c->depth_buffer_address;
            new_depth = (uint8_t *)mw_context_alloc(
                c, w * h * 4U, &new_depth_address);
            if (!new_depth)
                goto target_allocation_failed;
            for (i = 0; i < 3; ++i) {
                c->color_buffers[i] = new_colors[i];
                c->color_buffer_addresses[i] = new_color_addresses[i];
            }
            c->depth_buffer = new_depth;
            c->depth_buffer_address = new_depth_address;
            c->width = w;
            c->height = h;
            c->state.target.width = w;
            c->state.target.height = h;
            c->state.viewport_x = 0;
            c->state.viewport_y = 0;
            c->state.viewport_w = w;
            c->state.viewport_h = h;
        }
        c->state.target.buffer_count = buf_count;
        if (rebind && !mw_ctx_bind_buffers(c)) {
            if (resize) {
                for (i = 0; i < 3; ++i) {
                    c->color_buffers[i] = old_colors[i];
                    c->color_buffer_addresses[i] = old_color_addresses[i];
                }
                c->depth_buffer = old_depth;
                c->depth_buffer_address = old_depth_address;
                c->width = old_width;
                c->height = old_height;
                c->state.target.width = old_target_width;
                c->state.target.height = old_target_height;
                c->state.viewport_x = old_viewport_x;
                c->state.viewport_y = old_viewport_y;
                c->state.viewport_w = old_viewport_w;
                c->state.viewport_h = old_viewport_h;
            }
            c->state.target.buffer_count = old_buffer_count;
            mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
            goto target_rebind_failed;
        }
        if (resize) {
            for (i = 0; i < 3; ++i)
                mw_context_free(c, old_colors[i], &old_color_addresses[i]);
            mw_context_free(c, old_depth, &old_depth_address);
        }
        c->state.target.depth_bits = depth_bits;
        c->state.target.present_width = pres_w;
        c->state.target.present_height = pres_h;
        break;

target_rebind_failed:
        for (i = 0; i < 3; ++i)
            mw_context_free(c, new_colors[i], &new_color_addresses[i]);
        mw_context_free(c, new_depth, &new_depth_address);
        goto command_complete;

target_allocation_failed:
        for (i = 0; i < 3; ++i)
            mw_context_free(c, new_colors[i], &new_color_addresses[i]);
        mw_context_free(c, new_depth, &new_depth_address);
        mw_ctx_set_error(c, MW_GL_OUT_OF_MEMORY);
        goto command_complete;
    }

    case MW_CLEAR: {
        uint32_t mask = mw_be32(p);
        uint32_t raster_mask = (mask & 1U ? 0x4000U : 0U) | (mask & 2U ? 0x100U : 0U);
        float r = mw_bef(p + 4);
        float g = mw_bef(p + 8);
        float b = mw_bef(p + 12);
        float a = mw_bef(p + 16);
        float depth_f = mw_bef(p + 20);
        uint32_t color_rgba = mw_color_from_floats(r, g, b, a);
        uint32_t depth_val = (uint32_t)(depth_f < 0.0f ? 0.0f : depth_f > 1.0f ? 16777215.0f : depth_f * 16777215.0f);

        c->state.clear_color[0] = r;
        c->state.clear_color[1] = g;
        c->state.clear_color[2] = b;
        c->state.clear_color[3] = a;
        c->state.clear_depth = depth_f;

        mw_ctx_bind_buffers(c);
        mw_ctx_sync_to_raster(c);
        minigl_raster_clear(raster_mask, color_rgba, depth_val);
        mw_ctx_update_latest(c);
        break;
    }

    case MW_STATE: {
        uint32_t key = mw_be32(p);
        const uint8_t *v = p + 8;
        mw_state *s = &c->state;

        switch (key) {
        case MW_DEPTH_TEST: s->depth_test = (uint8_t)(mw_be32(v) != 0); break;
        case MW_DEPTH_FUNC: s->depth_func = mw_be32(v); break;
        case MW_DEPTH_MASK: s->depth_mask = (uint8_t)(mw_be32(v) != 0); break;
        case MW_DEPTH_RANGE: s->depth_range_near = mw_bef(v); s->depth_range_far = mw_bef(v + 4); break;
        case MW_BLEND: s->blend_enabled = (uint8_t)(mw_be32(v) != 0); break;
        case MW_BLEND_FACTORS:
            s->blend_src_rgb = mw_be32(v); s->blend_dst_rgb = mw_be32(v + 4);
            s->blend_src_alpha = mw_be32(v + 8); s->blend_dst_alpha = mw_be32(v + 12);
            break;
        case MW_BLEND_EQUATION: s->blend_equation = mw_be32(v); break;
        case MW_ALPHA_TEST: s->alpha_test = (uint8_t)(mw_be32(v) != 0); break;
        case MW_ALPHA_FUNC: s->alpha_func = mw_be32(v); s->alpha_ref = mw_bef(v + 4); break;
        case MW_CULL: s->cull_enabled = (uint8_t)(mw_be32(v) != 0); break;
        case MW_CULL_FACE: s->cull_face = mw_be32(v); break;
        case MW_FRONT_FACE: s->front_face = mw_be32(v); break;
        case MW_SCISSOR: s->scissor_enabled = (uint8_t)(mw_be32(v) != 0); break;
        case MW_SCISSOR_BOX:
            s->scissor_x = (int32_t)mw_be32(v); s->scissor_y = (int32_t)mw_be32(v + 4);
            s->scissor_w = mw_be32(v + 8); s->scissor_h = mw_be32(v + 12);
            break;
        case MW_COLOR_MASK:
            s->color_mask[0] = (uint8_t)(mw_be32(v) != 0);
            s->color_mask[1] = (uint8_t)(mw_be32(v + 4) != 0);
            s->color_mask[2] = (uint8_t)(mw_be32(v + 8) != 0);
            s->color_mask[3] = (uint8_t)(mw_be32(v + 12) != 0);
            break;
        case MW_POLYGON_OFFSET:
            s->polygon_offset_factor = mw_bef(v); s->polygon_offset_units = mw_bef(v + 4);
            break;
        case MW_OFFSET_FILL: s->offset_fill = (uint8_t)(mw_be32(v) != 0); break;
        case MW_OFFSET_LINE: s->offset_line = (uint8_t)(mw_be32(v) != 0); break;
        case MW_OFFSET_POINT: s->offset_point = (uint8_t)(mw_be32(v) != 0); break;
        case MW_LINE_WIDTH: s->line_width = mw_bef(v); break;
        case MW_POINT_SIZE: s->point_size = mw_bef(v); break;
        case MW_SHADE_MODEL: s->shade_model = mw_be32(v); break;
        case MW_FOG: s->fog_enabled = (uint8_t)(mw_be32(v) != 0); break;
        case MW_FOG_MODE: s->fog_mode = mw_be32(v); break;
        case MW_FOG_PARAMS:
            s->fog_density = mw_bef(v); s->fog_start = mw_bef(v + 4); s->fog_end = mw_bef(v + 8);
            break;
        case MW_FOG_COLOR: {
            int k; for (k = 0; k < 4; ++k) s->fog_color[k] = mw_bef(v + k * 4);
            break;
        }
        case MW_TEXTURE_STAGE: {
            uint32_t unit = mw_be32(v);
            if (unit < 2) {
                s->stages[unit].name = mw_be32(v + 4);
                s->stages[unit].enabled = (uint8_t)(mw_be32(v + 8) != 0);
                s->stages[unit].environment = mw_be32(v + 12);
            }
            break;
        }
        case MW_DRAW_BUFFER: {
            uint32_t draw_buffer = mw_be32(v);
            uint32_t old_draw_buffer = s->draw_buffer;
            uint32_t old_latest = s->target.latest;

            if (draw_buffer == old_draw_buffer)
                break;
            if (!minigl_raster_finish()) {
                mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
                goto command_complete;
            }
            s->draw_buffer = draw_buffer;
            mw_ctx_update_latest(c);
            if (!mw_ctx_bind_buffers(c)) {
                s->draw_buffer = old_draw_buffer;
                s->target.latest = old_latest;
                mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
                goto command_complete;
            }
            break;
        }
        case MW_READ_BUFFER: s->read_buffer = mw_be32(v); break;
        case MW_DITHER: s->dither = (uint8_t)(mw_be32(v) != 0); break;
        case MW_POINT_SMOOTH: s->point_smooth = (uint8_t)(mw_be32(v) != 0); break;
        case MW_VIEWPORT:
            s->viewport_x = (int32_t)mw_be32(v); s->viewport_y = (int32_t)mw_be32(v + 4);
            s->viewport_w = mw_be32(v + 8); s->viewport_h = mw_be32(v + 12);
            break;
        case MW_Z_OFFSET: s->z_offset = mw_bef(v); break;
        case MW_PERSPECTIVE: s->perspective = (uint8_t)(mw_be32(v) != 0); break;
        case MW_MODELVIEW: {
            int k; for (k = 0; k < 16; ++k) s->modelview[k] = mw_bef(v + k * 4);
            mw_state_update_normal_matrix(s);
            mw_state_update_mvp(s);
            break;
        }
        case MW_TEXGEN0:
        case MW_TEXGEN1: {
            uint32_t u = (key == MW_TEXGEN0 ? 0 : 1);
            s->texgen[u].s_enabled = (uint8_t)(mw_be32(v) != 0);
            s->texgen[u].t_enabled = (uint8_t)(mw_be32(v + 4) != 0);
            s->texgen[u].s_mode = mw_be32(v + 8);
            s->texgen[u].t_mode = mw_be32(v + 12);
            int k;
            for (k = 0; k < 4; ++k) {
                s->texgen[u].obj_plane_s[k] = mw_bef(v + (4 + k) * 4);
                s->texgen[u].obj_plane_t[k] = mw_bef(v + (8 + k) * 4);
                s->texgen[u].eye_plane_s[k] = mw_bef(v + (12 + k) * 4);
                s->texgen[u].eye_plane_t[k] = mw_bef(v + (16 + k) * 4);
            }
            break;
        }
        case MW_PROJECTION: {
            int k; for (k = 0; k < 16; ++k) s->projection[k] = mw_bef(v + k * 4);
            mw_state_update_mvp(s);
            break;
        }
        case MW_TEXTURE_MATRIX0: {
            int k; for (k = 0; k < 16; ++k) s->texture_matrix[0][k] = mw_bef(v + k * 4);
            break;
        }
        case MW_TEXTURE_MATRIX1: {
            int k; for (k = 0; k < 16; ++k) s->texture_matrix[1][k] = mw_bef(v + k * 4);
            break;
        }
        case MW_LIGHTING: s->lighting_enabled = (uint8_t)(mw_be32(v) != 0); break;
        case MW_LIGHT: {
            uint32_t idx = mw_be32(v);
            if (idx < 8) {
                int k;
                s->lights[idx].enabled = (uint8_t)(mw_be32(v + 4) != 0);
                for (k = 0; k < 4; ++k) s->lights[idx].ambient[k] = mw_bef(v + (2 + k) * 4);
                for (k = 0; k < 4; ++k) s->lights[idx].diffuse[k] = mw_bef(v + (6 + k) * 4);
                for (k = 0; k < 4; ++k) s->lights[idx].specular[k] = mw_bef(v + (10 + k) * 4);
                for (k = 0; k < 4; ++k) s->lights[idx].position[k] = mw_bef(v + (14 + k) * 4);
                for (k = 0; k < 3; ++k) s->lights[idx].spot_direction[k] = mw_bef(v + (18 + k) * 4);
                s->lights[idx].spot_exponent = mw_bef(v + 21 * 4);
                s->lights[idx].spot_cutoff = mw_bef(v + 22 * 4);
                s->lights[idx].constant = mw_bef(v + 23 * 4);
                s->lights[idx].linear = mw_bef(v + 24 * 4);
                s->lights[idx].quadratic = mw_bef(v + 25 * 4);
            }
            break;
        }
        case MW_LIGHT_MODEL: {
            int k;
            for (k = 0; k < 4; ++k) s->light_model_ambient[k] = mw_bef(v + k * 4);
            s->light_model_local_viewer = (uint8_t)(mw_be32(v + 16) != 0);
            s->light_model_two_side = (uint8_t)(mw_be32(v + 20) != 0);
            break;
        }
        case MW_MATERIAL: {
            uint32_t face = mw_be32(v);
            if (face < 2) {
                int k;
                for (k = 0; k < 4; ++k) s->materials[face].ambient[k] = mw_bef(v + (1 + k) * 4);
                for (k = 0; k < 4; ++k) s->materials[face].diffuse[k] = mw_bef(v + (5 + k) * 4);
                for (k = 0; k < 4; ++k) s->materials[face].specular[k] = mw_bef(v + (9 + k) * 4);
                for (k = 0; k < 4; ++k) s->materials[face].emission[k] = mw_bef(v + (13 + k) * 4);
                s->materials[face].shininess = mw_bef(v + 17 * 4);
            }
            break;
        }
        case MW_COLOR_MATERIAL:
            s->color_material_enabled = (uint8_t)(mw_be32(v) != 0);
            s->color_material_face = mw_be32(v + 4);
            s->color_material_mode = mw_be32(v + 8);
            break;
        case MW_NORMALIZE: s->normalize_enabled = (uint8_t)(mw_be32(v) != 0); break;
        case MW_TEXTURE_ENV_COLOR: {
            uint32_t unit = mw_be32(v);
            if (unit < 2) {
                int k; for (k = 0; k < 4; ++k) s->stages[unit].env_color[k] = mw_bef(v + (1 + k) * 4);
            }
            break;
        }
        default:
            break;
        }
        mw_ctx_sync_to_raster(c);
        break;
    }

    case MW_TEXTURE_IMAGE_RAW: {
        uint32_t name = mw_be32(p);
        uint32_t level = mw_be32(p + 4);
        uint32_t width = mw_be32(p + 12);
        uint32_t height = mw_be32(p + 16);
        mw_texture *t;

        if (!mw_texmgr_image_raw(&c->texmgr, p, cmd->bytes,
                                 (const uint8_t (*)[4])c->palette,
                                 c->palette_width)) {
            mw_ctx_set_error(c, MW_GL_OUT_OF_MEMORY);
            goto command_complete;
        }

        t = mw_texmgr_find(&c->texmgr, name);
        if (!t || !t->pixels[level] ||
            !minigl_raster_upload_rgba8(name, (uint16_t)level,
                                        (uint16_t)width, (uint16_t)height,
                                        t->pixels[level], t->internal_rgb[level])) {
            mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
            goto command_complete;
        }
        break;
    }

    case MW_TEXTURE_SUB_RAW: {
        uint32_t name = mw_be32(p);
        uint32_t level = mw_be32(p + 4);
        mw_texture *t;

        if (!mw_texmgr_sub_raw(&c->texmgr, p, cmd->bytes,
                               (const uint8_t (*)[4])c->palette,
                               c->palette_width)) {
            mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
            goto command_complete;
        }

        t = mw_texmgr_find(&c->texmgr, name);
        if (!t || !t->pixels[level] ||
            !minigl_raster_upload_rgba8(name, (uint16_t)level,
                                        (uint16_t)t->widths[level],
                                        (uint16_t)t->heights[level],
                                        t->pixels[level], t->internal_rgb[level]) ||
            !mw_upload_generated_mips(c, t)) {
            mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
            goto command_complete;
        }
        break;
    }
    case MW_TEXTURE_PARAM: {
        uint32_t name = mw_be32(p);
        uint32_t pname = mw_be32(p + 4);
        uint32_t val = mw_be32(p + 8);
        float fval = mw_bef(p + 8);
        mw_texture *t;

        if (!mw_texmgr_param(&c->texmgr, name, pname, val, fval)) {
            mw_ctx_set_error(c, MW_GL_OUT_OF_MEMORY);
            goto command_complete;
        }
        t = mw_texmgr_find(&c->texmgr, name);
        if (t) {
            minigl_raster_tex_params(name, t->min_filter, t->mag_filter,
                                     t->wrap_s, t->wrap_t);
            if (!mw_upload_generated_mips(c, t)) {
                mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
                goto command_complete;
            }
        }
        break;
    }

    case MW_TEXTURE_DELETE: {
        uint32_t name = mw_be32(p);
        mw_texmgr_delete(&c->texmgr, name);
        minigl_raster_delete_texture(name);
        break;
    }
    case MW_TEXTURE_COPY: {
        uint32_t name = mw_be32(p);
        uint32_t level = mw_be32(p + 4);
        uint32_t internal_format = mw_be32(p + 8);
        uint32_t xoffset = mw_be32(p + 12);
        uint32_t yoffset = mw_be32(p + 16);
        int32_t  x = (int32_t)mw_be32(p + 20);
        int32_t  y = (int32_t)mw_be32(p + 24);
        uint32_t w = mw_be32(p + 28);
        uint32_t h = mw_be32(p + 32);
        uint32_t define = mw_be32(p + 36);
        mw_texture *t;

        if (level >= MW_MAX_MIP_LEVELS || !w || !h ||
            w > MW_MAX_DIMENSION || h > MW_MAX_DIMENSION) {
            mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
            goto command_complete;
        }

        if (!minigl_raster_finish()) {
            mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
            goto command_complete;
        }
        t = mw_texmgr_get_or_create(&c->texmgr, name);
        if (!t) {
            mw_ctx_set_error(c, MW_GL_OUT_OF_MEMORY);
            goto command_complete;
        }

        uint32_t read_idx = (c->state.read_buffer == 0x404 || c->state.read_buffer == 0x400) ?
                            c->state.target.front : c->state.target.back;
        const uint32_t *src_buf = c->color_buffers[read_idx];
        if (!src_buf) {
            mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
            goto command_complete;
        }

        if (define) {
            if (!mw_texmgr_allocate_level(&c->texmgr, t, level, w, h)) {
                mw_ctx_set_error(c, MW_GL_OUT_OF_MEMORY);
                goto command_complete;
            }
            t->base_format[level] = internal_format;
            t->internal_rgb[level] = (internal_format == 3 || internal_format == 0x1907);
            uint32_t r, col;
            for (r = 0; r < h; ++r) {
                int32_t src_y = (int32_t)c->height - 1 - (y + (int32_t)r);
                for (col = 0; col < w; ++col) {
                    int32_t src_x = x + (int32_t)col;
                    uint32_t px = 0;
                    if (src_x >= 0 && src_x < (int32_t)c->width &&
                        src_y >= 0 && src_y < (int32_t)c->height)
                        px = src_buf[(uint32_t)src_y * c->width + (uint32_t)src_x];
                    if (t->internal_rgb[level])
                        px |= 0xffU;
                    t->pixels[level][r * w + col] = px;
                }
            }
        } else {
            uint32_t r, col;
            if (!t->pixels[level] || xoffset + w > t->widths[level] ||
                yoffset + h > t->heights[level]) {
                mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
                goto command_complete;
            }
            for (r = 0; r < h; ++r) {
                int32_t src_y = (int32_t)c->height - 1 - (y + (int32_t)r);
                for (col = 0; col < w; ++col) {
                    int32_t src_x = x + (int32_t)col;
                    uint32_t px = 0;
                    if (src_x >= 0 && src_x < (int32_t)c->width &&
                        src_y >= 0 && src_y < (int32_t)c->height)
                        px = src_buf[(uint32_t)src_y * c->width + (uint32_t)src_x];
                    if (t->internal_rgb[level])
                        px |= 0xffU;
                    t->pixels[level][(yoffset + r) * t->widths[level] +
                                      (xoffset + col)] = px;
                }
            }
        }
        if (!minigl_raster_upload_rgba8(name, (uint16_t)level,
                                        (uint16_t)t->widths[level],
                                        (uint16_t)t->heights[level],
                                        t->pixels[level],
                                        t->internal_rgb[level])) {
            mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
            goto command_complete;
        }
        if (level == 0 && t->generate_mipmap &&
            (!mw_texmgr_generate_mips(&c->texmgr, t) ||
             !mw_upload_generated_mips(c, t))) {
            mw_ctx_set_error(c, MW_GL_OUT_OF_MEMORY);
            goto command_complete;
        }
        break;
    }
    case MW_COLOR_TABLE: {
        uint32_t width = mw_be32(p + 12);
        uint32_t format = mw_be32(p + 16);
        uint32_t i;
        if (width > 256) {
            mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
            goto command_complete;
        }
        for (i = 0; i < width; ++i) {
            const uint8_t *src = p + 28 + i * (format == 0x1908 ? 4 : 3);
            c->palette[i][0] = src[0];
            c->palette[i][1] = src[1];
            c->palette[i][2] = src[2];
            c->palette[i][3] = (format == 0x1908 ? src[3] : 255U);
        }
        c->palette_width = width;
        for (i = 0; i < c->texmgr.capacity; ++i) {
            mw_texture *t = &c->texmgr.textures[i];
            if (!t->valid || !t->indexed) continue;
            uint32_t lvl;
            for (lvl = 0; lvl < MW_MAX_MIP_LEVELS; ++lvl) {
                if (t->indices[lvl] && t->pixels[lvl]) {
                    uint32_t total = t->widths[lvl] * t->heights[lvl];
                    uint32_t k;
                    for (k = 0; k < total; ++k) {
                        uint8_t idx = t->indices[lvl][k];
                        if (idx < c->palette_width) {
                            t->pixels[lvl][k] = ((uint32_t)c->palette[idx][0] << 24) |
                                                ((uint32_t)c->palette[idx][1] << 16) |
                                                ((uint32_t)c->palette[idx][2] << 8)  |
                                                (uint32_t)c->palette[idx][3];
                        } else {
                            t->pixels[lvl][k] = 0x000000ffU;
                        }
                    }
                    if (!minigl_raster_upload_rgba8(t->id, (uint16_t)lvl,
                                                    (uint16_t)t->widths[lvl],
                                                    (uint16_t)t->heights[lvl],
                                                    t->pixels[lvl],
                                                    t->internal_rgb[lvl])) {
                        mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
                        goto command_complete;
                    }
                }
            }
        }
        break;
    }

    case MW_DRAW_PACKED:
    case MW_DRAW_OBJECT:
    case MW_DRAW_LOWERED:
    case MW_DRAW_ARRAY_RAW: {
        uint32_t mode = 0, count = 0, flags = 0;
        const uint8_t *edge_mask = NULL;
        int ok = 0;

        if (cmd->opcode == MW_DRAW_PACKED) {
            uint32_t constants, words;
            if (cmd->bytes < 12 ||
                !mw_packed_layout(mw_be32(p + 8), &constants, &words)) {
                mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
                goto command_complete;
            }
            count = mw_be32(p + 4);
            if (!mw_ensure_scratch(c, count * MW_OBJECT_WORDS)) {
                mw_ctx_set_error(c, MW_GL_OUT_OF_MEMORY);
                goto command_complete;
            }
            ok = mw_decode_draw_packed(p, cmd->bytes, c->vertex_scratch, &mode, &count);
        } else if (cmd->opcode == MW_DRAW_OBJECT) {
            count = mw_be32(p + 4);
            if (!mw_ensure_scratch(c, count * MW_OBJECT_WORDS)) {
                mw_ctx_set_error(c, MW_GL_OUT_OF_MEMORY);
                goto command_complete;
            }
            ok = mw_decode_draw_object(p, cmd->bytes, c->vertex_scratch, &mode, &count);
        } else if (cmd->opcode == MW_DRAW_LOWERED) {
            count = mw_be32(p + 4);
            if (!mw_ensure_scratch(c, count * MW_OBJECT_WORDS)) {
                mw_ctx_set_error(c, MW_GL_OUT_OF_MEMORY);
                goto command_complete;
            }
            ok = mw_decode_draw_lowered(p, cmd->bytes, c->vertex_scratch, &mode, &count, &flags, &edge_mask);
        } else if (cmd->opcode == MW_DRAW_ARRAY_RAW) {
            count = mw_be32(p + 4);
            if (!mw_ensure_scratch(c, count * MW_OBJECT_WORDS)) {
                mw_ctx_set_error(c, MW_GL_OUT_OF_MEMORY);
                goto command_complete;
            }
            ok = mw_decode_draw_array_raw(p, cmd->bytes, c->vertex_scratch, &mode, &count);
        }

        if (!ok) {
            mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
            goto command_complete;
        }

        mw_ctx_sync_to_raster(c);
        minigl_raster_draw_wire_v9((uint16_t)mode, c->vertex_scratch, count, flags, (const uint32_t *)edge_mask);
        mw_ctx_update_latest(c);
        break;
    }

    case MW_PRESENT: {
        uint32_t old_front = c->state.target.front;
        uint32_t old_latest = c->state.target.latest;
        uint32_t old_back = c->state.target.back;

        if (!minigl_raster_finish()) {
            mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
            goto command_complete;
        }
        c->state.target.front = c->state.target.back;
        c->state.target.latest = c->state.target.front;
        c->state.target.back = (c->state.target.back + 1U) % c->state.target.buffer_count;
        if (!mw_ctx_bind_buffers(c)) {
            c->state.target.front = old_front;
            c->state.target.latest = old_latest;
            c->state.target.back = old_back;
            mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
            goto command_complete;
        }
        minigl_raster_frame_boundary();
        break;
    }

    case MW_SYNC:
        if (!minigl_raster_finish())
            mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
        break;

    case MW_UPLOAD: {
        uint32_t x = mw_be32(p);
        uint32_t y = mw_be32(p + 4);
        uint32_t w = mw_be32(p + 8);
        uint32_t h = mw_be32(p + 12);
        const uint8_t *src_pixels = p + 16;
        int draw_front;
        uint32_t *dst_buffer;
        uint32_t r, col;

        if (x > c->width || w > c->width - x ||
            y > c->height || h > c->height - y) {
            mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
            goto command_complete;
        }

        if (!minigl_raster_finish()) {
            mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
            goto command_complete;
        }
        draw_front = (c->state.draw_buffer == 0x404 || c->state.draw_buffer == 0x400) &&
                     c->state.target.buffer_count > 1;
        dst_buffer = c->color_buffers[draw_front ? c->state.target.front :
                                      c->state.target.back];

        for (r = 0; r < h; ++r) {
            const uint8_t *src_row = src_pixels + (size_t)r * w * 4;
            uint32_t *dst_row = dst_buffer + (size_t)(y + r) * c->width + x;
            for (col = 0; col < w; ++col) {
                uint8_t pr = src_row[col * 4];
                uint8_t pg = src_row[col * 4 + 1];
                uint8_t pb = src_row[col * 4 + 2];
                uint8_t pa = src_row[col * 4 + 3];
                dst_row[col] = ((uint32_t)pr << 24) | ((uint32_t)pg << 16) |
                               ((uint32_t)pb << 8) | (uint32_t)pa;
            }
        }
        mw_ctx_update_latest(c);
        break;
    }

    default:
        break;
    }

command_complete:

    return 1;
}

uint32_t mw_host_submit_report(mw_ctx *c, const uint8_t *packet, uint32_t bytes,
                               uint32_t *failed_opcode)
{
    uint32_t status;

    if (failed_opcode)
        *failed_opcode = 0U;
    if (!c || !packet || bytes < 16)
        return MW_STATUS_INVALID;

    status = mw_validate_packet_report(packet, bytes, failed_opcode);
    if (status != MW_STATUS_OK)
        return status;

    if (!mw_ctx_bind_buffers(c)) {
        mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
        return MW_STATUS_FAILED;
    }
    mw_ctx_sync_to_raster(c);

    status = mw_parse_packet_report(packet, bytes, mw_execute_command, c,
                                    failed_opcode);
    if (!minigl_raster_finish())
        mw_ctx_set_error(c, MW_GL_INVALID_OPERATION);
    return status;
}

uint32_t mw_host_submit(mw_ctx *c, const uint8_t *packet, uint32_t bytes)
{
    return mw_host_submit_report(c, packet, bytes, NULL);
}

static inline uint8_t mw_widen5(uint32_t v) {
    return (uint8_t)((v << 3) | (v >> 2));
}

static inline uint8_t mw_widen6(uint32_t v) {
    return (uint8_t)((v << 2) | (v >> 4));
}

static void mw_letterbox_rect(uint32_t width, uint32_t height,
                             uint32_t present_width, uint32_t present_height,
                             int32_t *dx0, int32_t *dy0, int32_t *dx1, int32_t *dy1)
{
    double scale = ((double)present_width / (double)width < (double)present_height / (double)height)
                 ? ((double)present_width / (double)width)
                 : ((double)present_height / (double)height);
    int32_t dw = (int32_t)lround(width * scale);
    int32_t dh = (int32_t)lround(height * scale);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > (int32_t)present_width) dw = (int32_t)present_width;
    if (dh > (int32_t)present_height) dh = (int32_t)present_height;
    *dx0 = (int32_t)((present_width - (uint32_t)dw) / 2U);
    *dy0 = (int32_t)((present_height - (uint32_t)dh) / 2U);
    *dx1 = *dx0 + dw;
    *dy1 = *dy0 + dh;
}

static void mw_refresh_presented_clut(mw_ctx *c)
{
    uint32_t palette[SDK_PALETTE_ENTRIES];
    uint32_t generation = sdk_palette_generation();
    uint32_t r5, g5, b5;

    if (!(generation & 1U) && c->presented_clut_generation == generation)
        return;
    if (!sdk_palette_snapshot(palette, &generation))
        return;

    for (r5 = 0; r5 < 32U; ++r5) {
        const int r = (int)mw_widen5(r5);
        for (g5 = 0; g5 < 32U; ++g5) {
            const int g = (int)mw_widen5(g5);
            for (b5 = 0; b5 < 32U; ++b5) {
                const int b = (int)mw_widen5(b5);
                uint32_t best = 0U;
                uint32_t best_distance = UINT32_MAX;
                uint32_t entry;

                for (entry = 0; entry < SDK_PALETTE_ENTRIES; ++entry) {
                    uint32_t xrgb = palette[entry];
                    int dr = r - (int)(xrgb >> 16);
                    int dg = g - (int)((xrgb >> 8) & 0xffU);
                    int db = b - (int)(xrgb & 0xffU);
                    uint32_t distance = (uint32_t)(dr * dr + dg * dg + db * db);
                    if (distance < best_distance) {
                        best_distance = distance;
                        best = entry;
                        if (distance == 0U)
                            break;
                    }
                }
                g_presented_clut_staging[(r5 << 10) | (g5 << 5) | b5] =
                    (uint8_t)best;
            }
        }
    }
    if (sdk_palette_generation() != generation)
        return;
    memcpy(c->presented_clut, g_presented_clut_staging,
           sizeof(c->presented_clut));
    c->presented_clut_generation = generation;
}

static inline uint8_t mw_clut_index(const mw_ctx *c, const uint8_t *p)
{
    return c->presented_clut[((uint32_t)(p[0] >> 3) << 10) |
                             ((uint32_t)(p[1] >> 3) << 5) |
                             (uint32_t)(p[2] >> 3)];
}

static inline uint32_t mw_present_source_pixel(const uint32_t *rgba,
                                               int source_rgb565, size_t index)
{
    if (source_rgb565) {
        uint16_t value = ((const uint16_t *)rgba)[index];
        return ((uint32_t)mw_widen5(value >> 11) << 24) |
               ((uint32_t)mw_widen6((value >> 5) & 63U) << 16) |
               ((uint32_t)mw_widen5(value & 31U) << 8) | 0xffU;
    }
    return rgba[index];
}

static inline void mw_encode_pixel(uint32_t format, const uint8_t *p, uint8_t *d)
{
    switch (format) {
    case 2: /* RGBFB_R8G8B8 */
        d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
        break;
    case 3: /* RGBFB_B8G8R8 */
        d[0] = p[2]; d[1] = p[1]; d[2] = p[0];
        break;
    case 4: { /* RGBFB_R5G6B5PC (LE) */
        const unsigned r = p[0] >> 3, g = p[1] >> 2, b = p[2] >> 3;
        d[0] = (uint8_t)(((g & 7) << 5) | b);
        d[1] = (uint8_t)((r << 3) | (g >> 3));
        break;
    }
    case 5: { /* RGBFB_R5G5B5PC (LE) */
        const unsigned r = p[0] >> 3, g = p[1] >> 3, b = p[2] >> 3;
        d[0] = (uint8_t)(((g & 7) << 5) | b);
        d[1] = (uint8_t)((r << 2) | (g >> 3));
        break;
    }
    case 6: /* RGBFB_A8R8G8B8 */
        d[0] = 0xff; d[1] = p[0]; d[2] = p[1]; d[3] = p[2];
        break;
    case 7: /* RGBFB_A8B8G8R8 */
        d[0] = 0xff; d[1] = p[2]; d[2] = p[1]; d[3] = p[0];
        break;
    case 8: /* RGBFB_R8G8B8A8 */
        d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = 0xff;
        break;
    case 9: /* RGBFB_B8G8R8A8 */
        d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; d[3] = 0xff;
        break;
    case 10: { /* RGBFB_R5G6B5 (BE) */
        uint16_t r = p[0] >> 3, g = p[1] >> 2, b = p[2] >> 3;
        uint16_t v = (uint16_t)((r << 11) | (g << 5) | b);
        d[0] = (uint8_t)(v >> 8);
        d[1] = (uint8_t)v;
        break;
    }
    case 11: { /* RGBFB_R5G5B5 (BE) */
        uint16_t r = p[0] >> 3, g = p[1] >> 3, b = p[2] >> 3;
        uint16_t v = (uint16_t)((r << 10) | (g << 5) | b);
        d[0] = (uint8_t)(v >> 8);
        d[1] = (uint8_t)v;
        break;
    }
    case 12: { /* RGBFB_B5G6R5PC (LE) */
        const unsigned r = p[0] >> 3, g = p[1] >> 2, b = p[2] >> 3;
        d[0] = (uint8_t)(((g & 7) << 5) | r);
        d[1] = (uint8_t)((b << 3) | (g >> 3));
        break;
    }
    case 13: { /* RGBFB_B5G5R5PC (LE) */
        const unsigned r = p[0] >> 3, g = p[1] >> 3, b = p[2] >> 3;
        d[0] = (uint8_t)(((g & 7) << 5) | r);
        d[1] = (uint8_t)((b << 2) | (g >> 3));
        break;
    }
    default:
        break;
    }
}

static inline void mw_decode_pixel(uint32_t format, const uint8_t *d, uint8_t *o)
{
    switch (format) {
    case 1: { /* RGBFB_CLUT */
        const uint32_t xrgb = sdk_palette_get(d[0]);
        o[0] = (uint8_t)(xrgb >> 16);
        o[1] = (uint8_t)(xrgb >> 8);
        o[2] = (uint8_t)xrgb;
        break;
    }
    case 2: /* RGBFB_R8G8B8 */
        o[0] = d[0]; o[1] = d[1]; o[2] = d[2];
        break;
    case 3: /* RGBFB_B8G8R8 */
        o[0] = d[2]; o[1] = d[1]; o[2] = d[0];
        break;
    case 4: /* RGBFB_R5G6B5PC */
        o[0] = mw_widen5(d[1] >> 3);
        o[1] = mw_widen6(((d[1] & 7) << 3) | (d[0] >> 5));
        o[2] = mw_widen5(d[0] & 31);
        break;
    case 5: /* RGBFB_R5G5B5PC */
        o[0] = mw_widen5((d[1] >> 2) & 31);
        o[1] = mw_widen5(((d[1] & 3) << 3) | (d[0] >> 5));
        o[2] = mw_widen5(d[0] & 31);
        break;
    case 6: /* RGBFB_A8R8G8B8 */
        o[0] = d[1]; o[1] = d[2]; o[2] = d[3];
        break;
    case 7: /* RGBFB_A8B8G8R8 */
        o[0] = d[3]; o[1] = d[2]; o[2] = d[1];
        break;
    case 8: /* RGBFB_R8G8B8A8 */
        o[0] = d[0]; o[1] = d[1]; o[2] = d[2];
        break;
    case 9: /* RGBFB_B8G8R8A8 */
        o[0] = d[2]; o[1] = d[1]; o[2] = d[0];
        break;
    case 10: { /* RGBFB_R5G6B5 */
        const uint32_t v = ((uint32_t)d[0] << 8) | d[1];
        o[0] = mw_widen5(v >> 11);
        o[1] = mw_widen6((v >> 5) & 63);
        o[2] = mw_widen5(v & 31);
        break;
    }
    case 11: { /* RGBFB_R5G5B5 */
        const uint32_t v = ((uint32_t)d[0] << 8) | d[1];
        o[0] = mw_widen5((v >> 10) & 31);
        o[1] = mw_widen5((v >> 5) & 31);
        o[2] = mw_widen5(v & 31);
        break;
    }
    case 12: { /* RGBFB_B5G6R5PC */
        o[0] = mw_widen5(d[1] >> 3);
        o[1] = mw_widen6(((d[1] & 7) << 3) | (d[0] >> 5));
        o[2] = mw_widen5(d[0] & 31);
        break;
    }
    case 13: { /* RGBFB_B5G5R5PC */
        o[0] = mw_widen5((d[1] >> 2) & 31);
        o[1] = mw_widen5(((d[1] & 3) << 3) | (d[0] >> 5));
        o[2] = mw_widen5(d[0] & 31);
        break;
    }
    default:
        o[0] = 0; o[1] = 0; o[2] = 0;
        break;
    }
    o[3] = 0xff;
}

typedef struct {
    uint32_t format;
    uint32_t bytes_per_pixel;
    uint32_t pitch;
    int32_t  frame_x;
    int32_t  frame_y;
    uint32_t count;
    uint32_t rects[MW_BLIT_MAX_RECTS * 4];
} mw_span_request;

static uint32_t mw_parse_span(const uint8_t *p, uint32_t bytes,
                             const uint8_t *span, uint32_t span_bytes,
                             mw_span_request *r)
{
    uint32_t kept = 0, i;
    if (bytes < 24)
        return MW_STATUS_INVALID;

    r->format = mw_be32(p + 4);
    if (r->format < 1 || r->format > 13)
        return MW_STATUS_INVALID;

    r->bytes_per_pixel = r->format == 1 ? 1U :
                         (r->format <= 3) ? 3U :
                         (r->format >= 6 && r->format <= 9) ? 4U : 2U;
    r->pitch = mw_be32(p + 8);
    if (!r->pitch || r->pitch < r->bytes_per_pixel)
        return MW_STATUS_INVALID;

    r->frame_x = (int32_t)mw_be32(p + 12);
    r->frame_y = (int32_t)mw_be32(p + 16);
    r->count = mw_be32(p + 20);
    if (!r->count || r->count > MW_BLIT_MAX_RECTS || bytes != 24 + 16 * r->count)
        return MW_STATUS_INVALID;

    for (i = 0; i < r->count; ++i) {
        uint32_t rx0 = mw_be32(p + 24 + i * 16 + 0);
        uint32_t ry0 = mw_be32(p + 24 + i * 16 + 4);
        uint32_t rx1 = mw_be32(p + 24 + i * 16 + 8);
        uint32_t ry1 = mw_be32(p + 24 + i * 16 + 12);

        if (rx0 > rx1 || ry0 > ry1)
            return MW_STATUS_INVALID;
        if (rx0 == rx1 || ry0 == ry1)
            continue;
        if ((uint64_t)(ry1 - 1) * r->pitch + (uint64_t)rx1 * r->bytes_per_pixel > span_bytes)
            return MW_STATUS_INVALID;

        r->rects[kept * 4 + 0] = rx0;
        r->rects[kept * 4 + 1] = ry0;
        r->rects[kept * 4 + 2] = rx1;
        r->rects[kept * 4 + 3] = ry1;
        kept++;
    }

    r->count = kept;
    if (span_bytes && !span)
        return MW_STATUS_INVALID;

    return MW_STATUS_OK;
}

static uint32_t mw_blit(mw_ctx *c, const uint8_t *p, uint32_t bytes,
                       uint8_t *span, uint32_t span_bytes)
{
    mw_span_request r;
    uint32_t status = mw_parse_span(p, bytes, span, span_bytes, &r);
    if (status != MW_STATUS_OK)
        return status;

    const uint32_t *src_buf;
    int source_rgb565;
    int source_bottom_up;

    if (!minigl_raster_prepare_present())
        return MW_STATUS_FAILED;
    source_rgb565 = minigl_raster_present_is_rgb565();
    source_bottom_up = minigl_raster_present_is_bottom_up();
    src_buf = c->color_buffers[c->state.target.latest];
    if (!src_buf)
        return MW_STATUS_FAILED;
    if (r.format == 1U)
        mw_refresh_presented_clut(c);

    int scaled = (c->state.target.present_width != c->width || c->state.target.present_height != c->height);
    uint32_t frame_w = scaled ? c->state.target.present_width : c->width;
    uint32_t frame_h = scaled ? c->state.target.present_height : c->height;

    int32_t bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    if (scaled) {
        mw_letterbox_rect(c->width, c->height, frame_w, frame_h, &bx0, &by0, &bx1, &by1);
    }

    if (r.count > 0) {
        uint32_t top = 0x7fffffffU, bottom = 0;
        uint32_t i;
        for (i = 0; i < r.count; ++i) {
            if (r.rects[i * 4 + 1] < top) top = r.rects[i * 4 + 1];
            if (r.rects[i * 4 + 3] > bottom) bottom = r.rects[i * 4 + 3];
        }

        int64_t first = (int64_t)top > (int64_t)r.frame_y ? (int64_t)top : (int64_t)r.frame_y;
        int64_t last = (int64_t)bottom < (int64_t)r.frame_y + frame_h ? (int64_t)bottom : (int64_t)r.frame_y + frame_h;

        for (int64_t at = first; at < last; ++at) {
            uint32_t row = (uint32_t)at;
            int64_t fy = at - r.frame_y;
            uint8_t *dst_row = span + (size_t)row * r.pitch;

            for (i = 0; i < r.count; ++i) {
                if (row < r.rects[i * 4 + 1] || row >= r.rects[i * 4 + 3])
                    continue;

                int64_t lo = (int64_t)r.rects[i * 4] > (int64_t)(r.frame_x < 0 ? 0 : r.frame_x)
                           ? (int64_t)r.rects[i * 4] : (int64_t)(r.frame_x < 0 ? 0 : r.frame_x);
                int64_t hi = (int64_t)r.rects[i * 4 + 2] < (int64_t)frame_w + r.frame_x
                           ? (int64_t)r.rects[i * 4 + 2] : (int64_t)frame_w + r.frame_x;

                for (int64_t col = lo; col < hi; ++col) {
                    int64_t fx = col - r.frame_x;
                    uint32_t pix;

                    if (scaled) {
                        if (fx >= bx0 && fx < bx1 && fy >= by0 && fy < by1) {
                            int64_t sx = (fx - bx0) * (int64_t)c->width / (bx1 - bx0);
                            int64_t sy = (fy - by0) * (int64_t)c->height / (by1 - by0);
                            if (sx >= 0 && sx < (int64_t)c->width && sy >= 0 && sy < (int64_t)c->height) {
                                if (source_bottom_up)
                                    sy = (int64_t)c->height - 1 - sy;
                                pix = mw_present_source_pixel(src_buf, source_rgb565,
                                                              (size_t)sy * c->width + sx);
                            } else {
                                pix = 0x000000ffU;
                            }
                        } else {
                            pix = 0x000000ffU;
                        }
                    } else {
                        int64_t sy = source_bottom_up ? (int64_t)c->height - 1 - fy : fy;
                        pix = mw_present_source_pixel(src_buf, source_rgb565,
                                                      (size_t)sy * c->width + fx);
                    }

                    uint8_t p_col[4];
                    p_col[0] = (uint8_t)(pix >> 24);
                    p_col[1] = (uint8_t)(pix >> 16);
                    p_col[2] = (uint8_t)(pix >> 8);
                    p_col[3] = (uint8_t)pix;

                    uint8_t *dst = dst_row + (size_t)col * r.bytes_per_pixel;
                    if (r.format == 1U)
                        *dst = mw_clut_index(c, p_col);
                    else
                        mw_encode_pixel(r.format, p_col, dst);
                }
            }
        }
    }


    return MW_STATUS_OK;
}

static uint32_t mw_capture(mw_ctx *c, const uint8_t *p, uint32_t bytes,
                          const uint8_t *span, uint32_t span_bytes)
{
    mw_span_request r;
    uint32_t status = mw_parse_span(p, bytes, span, span_bytes, &r);
    if (status != MW_STATUS_OK)
        return status;

    if (!minigl_raster_finish())
        return MW_STATUS_FAILED;

    int scaled = (c->state.target.present_width != c->width || c->state.target.present_height != c->height);
    uint32_t frame_w = scaled ? c->state.target.present_width : c->width;
    uint32_t frame_h = scaled ? c->state.target.present_height : c->height;


    /* Slow path: decode span into back buffer */
    memset(c->color_buffers[c->state.target.back], 0, (size_t)c->width * c->height * sizeof(uint32_t));

    int32_t bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    if (scaled) {
        mw_letterbox_rect(c->width, c->height, frame_w, frame_h, &bx0, &by0, &bx1, &by1);
    }

    for (uint32_t i = 0; i < r.count; ++i) {
        const uint32_t *rc = &r.rects[i * 4];
        int64_t lo = (int64_t)rc[0] > (int64_t)(r.frame_x < 0 ? 0 : r.frame_x)
                   ? (int64_t)rc[0] : (int64_t)(r.frame_x < 0 ? 0 : r.frame_x);
        int64_t hi = (int64_t)rc[2] < (int64_t)frame_w + r.frame_x
                   ? (int64_t)rc[2] : (int64_t)frame_w + r.frame_x;
        int64_t first = (int64_t)rc[1] > (int64_t)r.frame_y
                      ? (int64_t)rc[1] : (int64_t)r.frame_y;
        int64_t last = (int64_t)rc[3] < (int64_t)frame_h + r.frame_y
                     ? (int64_t)rc[3] : (int64_t)frame_h + r.frame_y;

        for (int64_t row = first; row < last; ++row) {
            int64_t fy = row - r.frame_y;
            int64_t dy = fy;
            if (scaled) {
                if (bx1 <= bx0 || by1 <= by0)
                    continue;
                dy = (fy - by0) * (int64_t)c->height / (by1 - by0);
                if (dy < 0 || dy >= (int64_t)c->height)
                    continue;
            }

            const uint8_t *src_row = span + (size_t)row * r.pitch;
            uint32_t *dst_row = c->color_buffers[c->state.target.back] + (size_t)dy * c->width;

            for (int64_t c_col = lo; c_col < hi; ++c_col) {
                int64_t px = c_col - r.frame_x;
                int64_t dx = px;
                if (scaled) {
                    dx = (px - bx0) * (int64_t)c->width / (bx1 - bx0);
                    if (dx < 0 || dx >= (int64_t)c->width)
                        continue;
                }

                const uint8_t *d = src_row + (size_t)c_col * r.bytes_per_pixel;
                uint8_t o[4];
                mw_decode_pixel(r.format, d, o);
                dst_row[dx] = ((uint32_t)o[0] << 24) | ((uint32_t)o[1] << 16) | ((uint32_t)o[2] << 8) | 0xffU;
            }
        }
    }

    c->state.target.latest = c->state.target.back;
    return MW_STATUS_OK;
}

uint32_t mw_host_request(mw_ctx *c, const uint8_t *request, uint32_t bytes,
                         uint8_t *reply, uint32_t reply_capacity, uint32_t *reply_bytes)
{
    uint32_t id;

    if (!c || !request || bytes < 4)
        return MW_STATUS_INVALID;

    id = mw_be32(request);

    if (id == MW_PLUGIN_VERSION) {
        static const char version[] = "zz9000-minigl-v29-v9";
        size_t len = strlen(version) + 1;
        if (len > MW_PLUGIN_VERSION_BYTES)
            len = MW_PLUGIN_VERSION_BYTES;
        if (reply_capacity < len)
            return MW_STATUS_CAPACITY;
        if (!reply)
            return MW_STATUS_INVALID;
        memcpy(reply, version, len);
        reply[len - 1] = 0;
        if (reply_bytes)
            *reply_bytes = (uint32_t)len;
        return MW_STATUS_OK;
    }

    if (id == MW_BLIT) {
        uint32_t status = mw_blit(c, request, bytes, reply, reply_capacity);
        if (status == MW_STATUS_OK && reply_bytes)
            *reply_bytes = reply_capacity;
        return status;
    }

    if (id == MW_CAPTURE) {
        uint32_t status = mw_capture(c, request, bytes, reply, reply_capacity);
        if (status == MW_STATUS_OK && reply_bytes)
            *reply_bytes = 0;
        return status;
    }

    if (id == MW_READ_PIXELS) {
        uint32_t format, w, h;
        int32_t x, y;
        uint64_t req_bytes;
        uint32_t *color_buffer;
        uint32_t row;
        int read_idx;

        if (bytes != 24)
            return MW_STATUS_INVALID;

        format = mw_be32(request + 4);
        x = (int32_t)mw_be32(request + 8);
        y = (int32_t)mw_be32(request + 12);
        w = mw_be32(request + 16);
        h = mw_be32(request + 20);

        if ((format != 0x1908 && format != 0x1902) || w > MW_MAX_DIMENSION || h > MW_MAX_DIMENSION)
            return MW_STATUS_INVALID;

        req_bytes = (uint64_t)w * h * 4;
        if (reply_capacity < req_bytes)
            return MW_STATUS_CAPACITY;
        if (req_bytes && !reply)
            return MW_STATUS_INVALID;
        if (!req_bytes) {
            if (reply_bytes) *reply_bytes = 0;
            return MW_STATUS_OK;
        }

        if (!minigl_raster_finish())
            return MW_STATUS_FAILED;

        read_idx = c->state.target.back;
        /* GL semantics: a GL_FRONT read answers the front buffer -- the
         * last presented frame -- whatever the buffer count. A single-
         * buffered context presents by swapping front to the initial back
         * (1), so falling through to the post-swap back would answer a
         * buffer nothing was drawn into: the first frame of every
         * single-buffered context read back black. */
        if (c->state.read_buffer == 0x404 || c->state.read_buffer == 0x400) {
            read_idx = c->state.target.front;
        } else if (c->state.read_buffer == 0) {
            read_idx = -1;
        }
        color_buffer = (read_idx >= 0 && read_idx < 3) ? c->color_buffers[read_idx] : NULL;

        /* GL glReadPixels produces bottom-row-first reply */
        for (row = 0; row < h; ++row) {
            int32_t gl_row = y + (int32_t)row;
            int32_t rast_row = (int32_t)c->height - 1 - gl_row;
            uint8_t *dst_row = reply + (size_t)row * w * 4;
            uint32_t col;

            for (col = 0; col < w; ++col) {
                int32_t gl_col = x + (int32_t)col;
                if (rast_row >= 0 && rast_row < (int32_t)c->height &&
                    gl_col >= 0 && gl_col < (int32_t)c->width) {
                    if (format == 0x1908) { /* GL_RGBA */
                        if (color_buffer) {
                            uint32_t pix = color_buffer[(size_t)rast_row * c->width + gl_col];
                            dst_row[col * 4]     = (uint8_t)(pix >> 24);
                            dst_row[col * 4 + 1] = (uint8_t)(pix >> 16);
                            dst_row[col * 4 + 2] = (uint8_t)(pix >> 8);
                            dst_row[col * 4 + 3] = (uint8_t)pix;
                        } else {
                            memset(dst_row + col * 4, 0, 4);
                        }
                    } else { /* GL_DEPTH_COMPONENT */
                        uint32_t dval = 0;
                        float df;
                        if (c->state.target.depth_bits == 16) {
                            const uint8_t *dp = c->depth_buffer + ((size_t)rast_row * c->width + gl_col) * 2;
                            dval = ((uint32_t)dp[0] << 8) | dp[1];
                            df = (float)dval / 65535.0f;
                        } else if (c->state.target.depth_bits == 24) {
                            const uint8_t *dp = c->depth_buffer + ((size_t)rast_row * c->width + gl_col) * 3;
                            dval = ((uint32_t)dp[0] << 16) | ((uint32_t)dp[1] << 8) | dp[2];
                            df = (float)dval / 16777215.0f;
                        } else {
                            /* The software rasterizer stores every depth
                             * format wider than 16 bits as a packed 24-bit
                             * value. GL's requested 32-bit depth buffer is
                             * therefore read back from that 24-bit backing
                             * store and normalized as such. */
                            const uint8_t *dp = c->depth_buffer + ((size_t)rast_row * c->width + gl_col) * 3;
                            dval = ((uint32_t)dp[0] << 16) | ((uint32_t)dp[1] << 8) | dp[2];
                            df = (float)dval / 16777215.0f;
                        }
                        mw_put_bef(dst_row + col * 4, df);
                    }
                } else {
                    memset(dst_row + col * 4, 0, 4);
                }
            }
        }

        if (reply_bytes)
            *reply_bytes = (uint32_t)req_bytes;
        return MW_STATUS_OK;
    }

    return MW_STATUS_INVALID;
}

uint32_t mw_host_readback_latest_rows(mw_ctx *c, uint8_t *out,
                                      uint32_t capacity, uint32_t first_row,
                                      uint32_t row_count)
{
    uint64_t req_bytes;
    const uint32_t *color_buffer;
    uint32_t y, x;

    if (!c || !out)
        return MW_STATUS_UNAVAILABLE;
    if (first_row > c->height || row_count > c->height - first_row)
        return MW_STATUS_INVALID;
    req_bytes = (uint64_t)c->width * row_count * 4U;
    if (capacity < req_bytes)
        return MW_STATUS_CAPACITY;

    if (!minigl_raster_finish())
        return MW_STATUS_FAILED;
    color_buffer = c->color_buffers[c->state.target.latest];
    for (y = 0; y < row_count; ++y) {
        const uint32_t *src_row =
            color_buffer + (size_t)(first_row + y) * c->width;
        uint8_t *dst_row = out + (size_t)y * c->width * 4U;
        for (x = 0; x < c->width; ++x) {
            uint32_t pix = src_row[x];
            dst_row[x * 4U]     = (uint8_t)(pix >> 24);
            dst_row[x * 4U + 1U] = (uint8_t)(pix >> 16);
            dst_row[x * 4U + 2U] = (uint8_t)(pix >> 8);
            dst_row[x * 4U + 3U] = (uint8_t)pix;
        }
    }
    return MW_STATUS_OK;
}

uint32_t mw_host_readback_rgba8(mw_ctx *c, uint8_t *out, uint32_t capacity)
{
    if (!c)
        return MW_STATUS_UNAVAILABLE;
    return mw_host_readback_latest_rows(c, out, capacity, 0U, c->height);
}
