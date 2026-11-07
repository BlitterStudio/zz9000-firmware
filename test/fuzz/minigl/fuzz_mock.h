#ifndef FUZZ_MOCK_H
#define FUZZ_MOCK_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "mw_types.h"
#include "mw_parse.h"
#include "mw_ctx.h"
#include "mw_host.h"
#include "minigl_platform.h"
#include "minigl_raster.h"
#include "rix_cmd.h"
#include "rix_drv.h"

struct fuzz_mock_platform {
    uint32_t csr[32];
    uint32_t fifo[6];
    uint32_t packet[RIX_DRV_FIFO_MAX_PACKET_WORDS + 2U];
    unsigned packet_words;
    uint32_t committed[8][RIX_DRV_FIFO_MAX_PACKET_WORDS + 2U];
    unsigned committed_words[8];
    uint32_t color_address;
    uint32_t clear_color;
    uint8_t cmd_buf[2][64 * 1024];
    uint8_t white_tex[MINIGL_RIX_WHITE_TEXTURE_BYTES];
    uint8_t tex_pool[MINIGL_RIX_TEXTURE_POOL_BYTES];
    unsigned csr_reads;
    unsigned submissions;
    int complete_submissions;
};

static inline uint16_t fuzz_mock_fence_color(uint32_t rgba)
{
    return (uint16_t)(((rgba >> 27U) << 11U) |
                      ((rgba >> 18U) << 5U) | (rgba >> 11U));
}

static inline void fuzz_mock_render_packet(struct fuzz_mock_platform *p)
{
    unsigned index;
    for (index = 2U; index < p->packet_words; ++index) {
        uint32_t command = p->packet[index];
        if ((command & 0xf0000000U) == RIX_OP_TRIANGLE_STREAM) {
            uint32_t words = (command & 0x3ffU) / 4U;
            if (index + words >= p->packet_words)
                break;
            index += words;
            continue;
        }
        if ((command & 0xf0000000U) == RIX_OP_RENDER_CONFIG) {
            if (index + 1U >= p->packet_words)
                break;
            if (command == (RIX_OP_RENDER_CONFIG | RIX_REG_COLOR_BUFFER_ADDR))
                p->color_address = p->packet[index + 1U];
            else if (command == (RIX_OP_RENDER_CONFIG | RIX_REG_CLEAR_COLOR))
                p->clear_color = fuzz_mock_fence_color(p->packet[index + 1U]);
            ++index;
        } else if ((command & 0xf0000000U) == RIX_OP_FRAMEBUFFER &&
                   (command & 0xffU) == (RIX_FRAMEBUFFER_MEMSET |
                                         RIX_FRAMEBUFFER_COLOR) &&
                   ((command >> 8U) & 0xfffffU) == 1U && p->complete_submissions) {
            if (p->color_address == 0x100000U + sizeof(p->cmd_buf[0]) -
                                    MINIGL_RIX_TEXTURE_PAGE_BYTES)
                *(uint16_t *)(p->cmd_buf[0] + sizeof(p->cmd_buf[0]) -
                              MINIGL_RIX_TEXTURE_PAGE_BYTES) = (uint16_t)p->clear_color;
            else if (p->color_address == 0x110000U + sizeof(p->cmd_buf[1]) -
                                         MINIGL_RIX_TEXTURE_PAGE_BYTES)
                *(uint16_t *)(p->cmd_buf[1] + sizeof(p->cmd_buf[1]) -
                              MINIGL_RIX_TEXTURE_PAGE_BYTES) = (uint16_t)p->clear_color;
        }
    }
}

static inline uint32_t fuzz_mock_csr_read(void *ctx, uint32_t offset)
{
    struct fuzz_mock_platform *p = (struct fuzz_mock_platform *)ctx;
    ++p->csr_reads;
    if (offset / 4U < sizeof(p->csr) / sizeof(p->csr[0]))
        return p->csr[offset / 4U];
    return 0U;
}

static inline void fuzz_mock_csr_write(void *ctx, uint32_t offset, uint32_t value)
{
    struct fuzz_mock_platform *p = (struct fuzz_mock_platform *)ctx;
    if (offset / 4U < sizeof(p->csr) / sizeof(p->csr[0]))
        p->csr[offset / 4U] = value;
}

static inline uint32_t fuzz_mock_fifo_read(void *ctx, uint32_t offset)
{
    struct fuzz_mock_platform *p = (struct fuzz_mock_platform *)ctx;
    if (offset / 4U < sizeof(p->fifo) / sizeof(p->fifo[0]))
        return p->fifo[offset / 4U];
    return 0U;
}

static inline void fuzz_mock_fifo_write(void *ctx, uint32_t offset, uint32_t value)
{
    struct fuzz_mock_platform *p = (struct fuzz_mock_platform *)ctx;
    if (offset != RIX_DRV_FIFO_TDFD) {
        if (offset / 4U < sizeof(p->fifo) / sizeof(p->fifo[0]))
            p->fifo[offset / 4U] = value;
    }
    if (offset == RIX_DRV_FIFO_TDFR) {
        p->packet_words = 0U;
    } else if (offset == RIX_DRV_FIFO_TDFD) {
        if (p->packet_words < sizeof(p->packet) / sizeof(p->packet[0]))
            p->packet[p->packet_words++] = value;
    } else if (offset == RIX_DRV_FIFO_TLF) {
        if (p->submissions < sizeof(p->committed_words) / sizeof(p->committed_words[0]) &&
            p->packet_words >= 2U) {
            p->committed_words[p->submissions] = p->packet_words;
            memcpy(p->committed[p->submissions], p->packet,
                   p->packet_words * sizeof(p->packet[0]));
            fuzz_mock_render_packet(p);
            ++p->submissions;
        }
        p->packet_words = 0U;
    }
}

static inline void fuzz_mock_cache_noop(void *ctx, uintptr_t addr, size_t bytes)
{
    (void)ctx;
    (void)addr;
    (void)bytes;
}

static inline void setup_fuzz_mock_platform(struct fuzz_mock_platform *p, MiniGLRixPlatform *out)
{
    memset(p, 0, sizeof(*p));
    memset(out, 0, sizeof(*out));
    p->complete_submissions = 1;
    p->csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    p->csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    p->csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    p->fifo[RIX_DRV_FIFO_TDFV / 4U] = RIX_DRV_FIFO_LEGACY_VACANCY;
    out->context = p;
    out->csr_read = fuzz_mock_csr_read;
    out->csr_write = fuzz_mock_csr_write;
    out->fifo_read = fuzz_mock_fifo_read;
    out->fifo_write = fuzz_mock_fifo_write;
    out->cache_clean = fuzz_mock_cache_noop;
    out->cache_invalidate = fuzz_mock_cache_noop;
    out->command_buffer[0] = p->cmd_buf[0];
    out->command_buffer[1] = p->cmd_buf[1];
    out->command_address[0] = 0x100000;
    out->command_address[1] = 0x110000;
    out->command_capacity = sizeof(p->cmd_buf[0]);
    out->white_texture = p->white_tex;
    out->white_texture_address = 0x200000;
    out->white_texture_bytes = sizeof(p->white_tex);
    out->texture_memory = p->tex_pool;
    out->texture_address = 0x300000;
    out->texture_bytes = sizeof(p->tex_pool);
}

static struct fuzz_mock_platform s_fuzz_mock;
static MiniGLRixPlatform s_fuzz_rix_platform;

static inline void enable_rasterix_mock(void)
{
    setup_fuzz_mock_platform(&s_fuzz_mock, &s_fuzz_rix_platform);
    minigl_raster_set_rix_platform(&s_fuzz_rix_platform);
}

static inline void disable_rasterix_mock(void)
{
    minigl_raster_set_rix_platform(NULL);
}

/* Most random packets die at the frame header (magic, version, total
 * length). Fix those three words so mutations reach the command decoders;
 * the command count and everything after it stay fuzzed. */
static inline uint32_t fuzz_submit_fixed(mw_ctx *c, const uint8_t *data, size_t len)
{
    uint8_t *packet;
    uint32_t status;

    if (len < 16 || len > (1u << 20))
        return mw_host_submit(c, data, (uint32_t)len);
    packet = (uint8_t *)malloc(len);
    if (!packet)
        return MW_STATUS_INVALID;
    memcpy(packet, data, len);
    mw_put_be32(packet, MW_MAGIC);
    mw_put_be32(packet + 4, MW_VERSION);
    mw_put_be32(packet + 8, (uint32_t)len);
    status = mw_host_submit(c, packet, (uint32_t)len);
    free(packet);
    return status;
}

static inline uint32_t fuzz_size_reply_capacity(const uint8_t *req, size_t req_bytes)
{
    if (!req || req_bytes < 4)
        return 64U;
    uint32_t id = mw_be32(req);
    if (id == MW_PLUGIN_VERSION)
        return MW_PLUGIN_VERSION_BYTES;
    if (id == MW_READ_PIXELS) {
        if (req_bytes >= 24) {
            uint32_t w = mw_be32(req + 16);
            uint32_t h = mw_be32(req + 20);
            uint64_t needed = (uint64_t)w * h * 4ULL;
            if (needed > 256ULL * 1024ULL)
                needed = 256ULL * 1024ULL;
            return (uint32_t)(needed == 0 ? 1U : (uint32_t)needed);
        }
        return 64U;
    }
    if (id == MW_BLIT || id == MW_CAPTURE) {
        if (req_bytes >= 24) {
            uint32_t format = mw_be32(req + 4);
            uint32_t bpp = (format == 1) ? 1U :
                           (format <= 3) ? 3U :
                           (format >= 6 && format <= 9) ? 4U : 2U;
            uint32_t pitch = mw_be32(req + 8);
            uint32_t count = mw_be32(req + 20);
            if (count > MW_BLIT_MAX_RECTS)
                count = MW_BLIT_MAX_RECTS;
            uint64_t max_span = 0;
            for (uint32_t i = 0; i < count && 24 + (i + 1) * 16 <= req_bytes; ++i) {
                uint32_t rx1 = mw_be32(req + 24 + i * 16 + 8);
                uint32_t ry1 = mw_be32(req + 24 + i * 16 + 12);
                uint64_t end = (uint64_t)(ry1 > 0 ? ry1 - 1 : 0) * pitch +
                               (uint64_t)rx1 * bpp;
                if (end > max_span)
                    max_span = end;
            }
            if (max_span > 256ULL * 1024ULL)
                max_span = 256ULL * 1024ULL;
            if (max_span < 64ULL)
                max_span = 64ULL;
            return (uint32_t)max_span;
        }
        return 64U;
    }
    return 64U;
}

#endif /* FUZZ_MOCK_H */
