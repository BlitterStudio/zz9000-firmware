/*
 * MiniGL RasterIX backend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MINIGL_RIX_BACKEND_H
#define MINIGL_RIX_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "rix_setup.h"
#include "rix_drv.h"
#include "minigl_renderer.h"
#include "minigl_pool.h"

#define MINIGL_RIX_CSR_BASE 0x83c60000U
#define MINIGL_RIX_FIFO_BASE 0x83c70000U
/* PL330 DMAC (secure APB view) and the channel used by MINIGL_RIX_DMA=1
 * builds to stream packet payloads into TDFD; UG585 Appendix B. */
#define MINIGL_RIX_DMAC_BASE 0xf8004000U
#define MINIGL_RIX_DMAC_CHANNEL 6U
#define MINIGL_RIX_DMAC_DS 0x000U
#define MINIGL_RIX_DMAC_FSRC 0x034U
#define MINIGL_RIX_DMAC_FTR(channel) (0x040U + (channel) * 4U)
#define MINIGL_RIX_DMAC_CSR(channel) (0x100U + (channel) * 8U)
#define MINIGL_RIX_DMAC_CPC(channel) (0x104U + (channel) * 8U)
#define MINIGL_RIX_DMAC_SAR(channel) (0x400U + (channel) * 0x20U)
#define MINIGL_RIX_DMAC_DAR(channel) (0x404U + (channel) * 0x20U)
#define MINIGL_RIX_DMAC_CCR(channel) (0x408U + (channel) * 0x20U)
#define MINIGL_RIX_DMAC_DBGSTATUS 0xd00U
/* S_AXI_HP3 AFI, the RasterIX EF memory port. */
#define MINIGL_RIX_AFI_BASE 0xf800b000U
#define MINIGL_RIX_TEXTURE_PAGE_BYTES 2048U
#define MINIGL_RIX_WHITE_TEXTURE_BYTES MINIGL_RIX_TEXTURE_PAGE_BYTES
/* TextureSampler's sparse mip layout uses a 256x256x16-bit level-zero
 * address space plus a second half for the lower levels. */
#define MINIGL_RIX_TEXTURE_VIRTUAL_BYTES (256U * 256U * 2U * 2U)
#define MINIGL_RIX_TEXTURE_PAGES \
    (MINIGL_RIX_TEXTURE_VIRTUAL_BYTES / MINIGL_RIX_TEXTURE_PAGE_BYTES)
/* RasterIX uses 2 KiB pages and a 32-bit external address space.  Backing
 * begins only on first upload and grows to this card-pool bounded ceiling. */
#define MINIGL_RIX_TEXTURE_POOL_BYTES MINIGL_RIX_TEXTURE_POOL_MAX_BYTES
#define MINIGL_RIX_TEXTURE_POOL_PAGES \
    (MINIGL_RIX_TEXTURE_POOL_BYTES / MINIGL_RIX_TEXTURE_PAGE_BYTES)
/* The CPU rasterizer can retain this many texture identities.  RasterIX
 * residency is page-based and evicts unpinned LRU images on demand. */
#define MINIGL_RIX_MAX_TEXTURES ZZ9K_MINIGL_MAX_TEXTURES
/* Commands are broken at parser-command boundaries into 248-word FIFO packets. */
#define MINIGL_RIX_COMMAND_BYTES 0x00010000U
#define MINIGL_RIX_COMMAND_BUFFERS 2U
#define MINIGL_RIX_MAX_TRIANGLES 1024U
#define MINIGL_RIX_MAX_OPS 1024U
#define MINIGL_RIX_COMMAND_WORD_BUDGET 15000U


typedef uint32_t (*MiniGLRixReadFn)(void *context, uint32_t offset);
typedef void (*MiniGLRixWriteFn)(void *context, uint32_t offset, uint32_t value);
typedef void (*MiniGLRixCacheCleanFn)(void *context, uintptr_t address, size_t bytes);
typedef void (*MiniGLRixCacheInvalidateFn)(void *context, uintptr_t address,
                                           size_t bytes);
typedef void (*MiniGLRixFifoWordsFn)(void *context, const uint32_t *words,
                                     uint32_t count);
typedef void (*MiniGLRixBarrierFn)(void *context);
typedef int (*MiniGLRixDmaStreamFn)(void *context, uintptr_t source,
                                    uint32_t words, uint32_t burst_words);


typedef int (*MiniGLRixTexturePoolGrowFn)(void *context, size_t minimum_bytes,
                                          uint8_t **memory,
                                          uintptr_t *address,
                                          size_t *bytes);

typedef struct MiniGLRixPlatform {
    void *context;
    MiniGLRixReadFn csr_read;
    MiniGLRixWriteFn csr_write;
    MiniGLRixReadFn fifo_read;
    MiniGLRixWriteFn fifo_write;
    MiniGLRixFifoWordsFn fifo_write_words;
    MiniGLRixBarrierFn fifo_drain;
    MiniGLRixDmaStreamFn dma_stream;
    MiniGLRixCacheCleanFn cache_clean;
    MiniGLRixCacheInvalidateFn cache_invalidate;
    uint8_t *command_buffer[MINIGL_RIX_COMMAND_BUFFERS];
    uintptr_t command_address[MINIGL_RIX_COMMAND_BUFFERS];
    uint8_t *white_texture;
    uintptr_t white_texture_address;
    size_t white_texture_bytes;
    uint8_t *texture_memory;
    uintptr_t texture_address;
    size_t texture_bytes;
    size_t command_capacity;
    MiniGLRixTexturePoolGrowFn grow_texture_pool;
} MiniGLRixPlatform;

typedef struct MiniGLRixVertex {
    float x, y, z, reciprocal_w;
    uint32_t rgba;
    float s, t, q;
    float s1, t1, q1;
} MiniGLRixVertex;
typedef struct MiniGLRixTexture {
    uint32_t id;
    uint32_t last_used_fence;
    uint32_t incarnation;
    uint32_t lru_stamp;
    uint16_t levels;
    uint16_t required_levels;
    uint16_t page_count;
    uint16_t first_page;
    uint16_t min_filter;
    uint16_t mag_filter;
    uint16_t wrap_s;
    uint16_t wrap_t;
    uint16_t width;
    uint16_t height;
    uint8_t width_log2;
    uint8_t height_log2;
    uint8_t format;
    uint8_t valid;
    uint8_t deleted;
    uint8_t pin_count;
} MiniGLRixTexture;

typedef enum MiniGLRixRecordedOpType {
    MINIGL_RIX_OP_CLEAR,
    MINIGL_RIX_OP_DRAW
} MiniGLRixRecordedOpType;

typedef struct MiniGLRixRecordedOp {
    ZZ9KMiniGLPacketSetState state;
    size_t first_triangle;
    size_t triangle_count;
    uint32_t color;
    uint32_t depth;
    uint32_t mask;
    uint32_t texture_incarnation;
    uint32_t texture1_incarnation;
    uint16_t texture_slot;
    uint16_t texture1_slot;
    uint16_t min_filter;
    uint16_t mag_filter;
    uint16_t wrap_s;
    uint16_t wrap_t;
    uint16_t texture1_min_filter;
    uint16_t texture1_mag_filter;
    uint16_t texture1_wrap_s;
    uint16_t texture1_wrap_t;
    uint8_t two_pass;
    uint8_t type;
} MiniGLRixRecordedOp;


typedef struct MiniGLRixBackend {
    struct rix_drv driver;
    MiniGLRixPlatform platform;
    RixTriangle triangles[MINIGL_RIX_MAX_TRIANGLES];
    RixTriangle texture1_triangles[MINIGL_RIX_MAX_TRIANGLES];
    MiniGLRixRecordedOp ops[MINIGL_RIX_MAX_OPS];
    ZZ9KMiniGLPacketSetState state;
    uint32_t color_address, depth_address;
    uint16_t width, height;
    uint8_t current_buffer;
    uint8_t next_buffer;
    uint8_t available;
    uint8_t force_cpu;
    uint8_t collecting;
    uint8_t frame_active;
    uint8_t needs_color_read;
    uint8_t needs_depth_read;
    uint8_t surface_rgb565;
    uint8_t error;
    uint32_t pending_fence[MINIGL_RIX_COMMAND_BUFFERS];
    uint32_t submitted_command_words[MINIGL_RIX_COMMAND_BUFFERS];
    uint8_t pending[MINIGL_RIX_COMMAND_BUFFERS];
    uint8_t pending_pins[MINIGL_RIX_COMMAND_BUFFERS][MINIGL_RIX_MAX_TEXTURES];
    uint8_t recorded_pins[MINIGL_RIX_MAX_TEXTURES];
    MiniGLRixTexture textures[MINIGL_RIX_MAX_TEXTURES];
    uint32_t bound_texture;
    uint32_t bound_texture1;
    uint32_t lru_clock;
    uint32_t texture_incarnation_clock;
    size_t triangle_count;
    size_t op_count;
    size_t command_words_estimate;
} MiniGLRixBackend;

/* Probe only the versioned CSR identity contract. This is the sole CSR access
 * permitted before the RasterIX ABI has been validated. */
int minigl_rix_backend_probe_platform(const MiniGLRixPlatform *platform);
/* Validates the RasterIX CSR identity before rix_drv_init can access it. */
void minigl_rix_backend_init(MiniGLRixBackend *backend,
                             const MiniGLRixPlatform *platform);
void minigl_rix_backend_reset(MiniGLRixBackend *backend);
int minigl_rix_backend_available(const MiniGLRixBackend *backend);
void minigl_rix_backend_set_state(MiniGLRixBackend *backend,
                                  const ZZ9KMiniGLPacketSetState *state);
void minigl_rix_backend_bind_texture(MiniGLRixBackend *backend, uint16_t unit, uint32_t id);

/* begin_draw/add_triangle/end_draw record a draw in the active frame.  The
 * frame is band-replayed and submitted at drain or the next required hand-off. */
/* RasterIX addresses a 256 KiB virtual texture: level 0 begins at zero and
 * later mip levels occupy the sparse page layout used by TextureSampler. */
int minigl_rix_backend_upload_texture(MiniGLRixBackend *backend, uint32_t id,
                                      uint16_t level, const uint32_t *rgba,
                                      uint16_t width, uint16_t height,
                                      uint16_t format);
/* Writes an unscaled sub-rectangle to a resident texture.  A pinned texture
 * is copy-on-written where spare pages permit, so earlier command buffers
 * retain their original version. */
int minigl_rix_backend_subimage_texture(MiniGLRixBackend *backend, uint32_t id,
                                        uint16_t level, uint16_t xoffset,
                                        uint16_t yoffset, uint16_t width,
                                        uint16_t height, const uint32_t *rgba,
                                        uint16_t source_width,
                                        uint16_t source_height);
void minigl_rix_backend_tex_params(MiniGLRixBackend *backend, uint32_t id,
                                   uint16_t min_filter, uint16_t mag_filter,
                                   uint16_t wrap_s, uint16_t wrap_t);
void minigl_rix_backend_delete_texture(MiniGLRixBackend *backend, uint32_t id);
/* True only while all RasterIX page mappings for this texture are resident. */
int minigl_rix_backend_texture_resident(const MiniGLRixBackend *backend, uint32_t id);
int minigl_rix_backend_begin_draw(MiniGLRixBackend *backend, uint16_t width,
                                  uint16_t height, uint32_t color_address,
                                  uint32_t depth_address);
int minigl_rix_backend_add_triangle(MiniGLRixBackend *backend,
                                    const MiniGLRixVertex *a,
                                    const MiniGLRixVertex *b,
                                    const MiniGLRixVertex *c);
int minigl_rix_backend_end_draw(MiniGLRixBackend *backend, uint32_t color_address,
                                uint32_t depth_address);
void minigl_rix_backend_abort_draw(MiniGLRixBackend *backend);
int minigl_rix_backend_clear(MiniGLRixBackend *backend, uint16_t width,
                             uint16_t height, uint32_t color_address,
                             uint32_t depth_address, uint32_t mask,
                             uint32_t rgba, uint32_t depth);
/* The CPU buffer and the RasterIX target share storage.  These functions
 * perform the ordered in-place format hand-off around mixed execution. */
int minigl_rix_backend_prepare_target(MiniGLRixBackend *backend,
                                      uint32_t *rgba, size_t pixels);
int minigl_rix_backend_make_cpu_visible(MiniGLRixBackend *backend,
                                        uint32_t *rgba, size_t pixels);
int minigl_rix_backend_surface_is_rgb565(const MiniGLRixBackend *backend);
/* Completion is the ownership hand-off: no CPU access may occur before it. */
int minigl_rix_backend_drain(MiniGLRixBackend *backend);
uint32_t minigl_rix_backend_eval_fallback(const MiniGLRixBackend *backend);
const char *minigl_rix_backend_fallback_name(uint32_t reason);
void minigl_rix_backend_log_fallback(uint32_t reason);
void minigl_rix_backend_log_context_backend(const MiniGLRixBackend *backend);

#endif /* MINIGL_RIX_BACKEND_H */
