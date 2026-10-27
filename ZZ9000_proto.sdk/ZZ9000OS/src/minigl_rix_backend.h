/*
 * MiniGL RasterIX backend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MINIGL_RIX_BACKEND_H
#define MINIGL_RIX_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "rix_bin.h"
#include "rix_drv.h"
#include "minigl_renderer.h"

#define MINIGL_RIX_CSR_BASE 0x83c60000U
#define MINIGL_RIX_MNTZORRO_IDENTITY_OFFSET 0x38U
#define MINIGL_RIX_TEXTURE_PAGE_BYTES 2048U
#define MINIGL_RIX_WHITE_TEXTURE_BYTES MINIGL_RIX_TEXTURE_PAGE_BYTES
#define MINIGL_RIX_TEXTURE_VIRTUAL_BYTES (256U * 256U * 2U * 2U)
#define MINIGL_RIX_TEXTURE_PAGES \
    (MINIGL_RIX_TEXTURE_VIRTUAL_BYTES / MINIGL_RIX_TEXTURE_PAGE_BYTES)
/* GLQuake's working set needs the same 24 MiB as the native RasterIX port. */
#define MINIGL_RIX_TEXTURE_POOL_BYTES (24U * 1024U * 1024U)
#define MINIGL_RIX_MAX_TEXTURES \
    (MINIGL_RIX_TEXTURE_POOL_BYTES / MINIGL_RIX_TEXTURE_VIRTUAL_BYTES)
/* The former 0xef800-byte fixed tails each rounded to this pool page size. */
#define MINIGL_RIX_COMMAND_BYTES 0x00400000U
#define MINIGL_RIX_COMMAND_BUFFERS 2U
#define MINIGL_RIX_MAX_TRIANGLES 1024U
#define MINIGL_RIX_MAX_OPS 1024U
#define MINIGL_RIX_COMMAND_WORD_BUDGET 7000U


typedef uint32_t (*MiniGLRixReadFn)(void *context, uint32_t offset);
typedef void (*MiniGLRixWriteFn)(void *context, uint32_t offset, uint32_t value);
typedef void (*MiniGLRixCacheCleanFn)(void *context, uintptr_t address, size_t bytes);
typedef void (*MiniGLRixCacheInvalidateFn)(void *context, uintptr_t address,
                                           size_t bytes);

typedef struct MiniGLRixPlatform {
    void *context;
    /* This is the already-decoded MNTZorro slave, never the RasterIX CSR. */
    MiniGLRixReadFn mntzorro_read;
    MiniGLRixReadFn csr_read;
    MiniGLRixWriteFn csr_write;
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
} MiniGLRixPlatform;

typedef struct MiniGLRixVertex {
    float x, y, z, reciprocal_w;
    uint32_t rgba;
    float s, t, q;
} MiniGLRixVertex;
typedef struct MiniGLRixTexture {
    uint32_t id;
    uint32_t offset;
    uint32_t last_used_fence;
    uint32_t incarnation;
    uint32_t lru_stamp;
    uint16_t levels;
    uint16_t required_levels;
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
    uint32_t texture_id;
    uint8_t type;
} MiniGLRixRecordedOp;


typedef struct MiniGLRixBackend {
    struct rix_drv driver;
    MiniGLRixPlatform platform;
    RixTriangle triangles[MINIGL_RIX_MAX_TRIANGLES];
    RixTriangle *references[MINIGL_RIX_MAX_TRIANGLES];
    RixBandList bands;
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
    uint8_t pending_pins[MINIGL_RIX_COMMAND_BUFFERS][MINIGL_RIX_MAX_TEXTURES];
    uint8_t recorded_pins[MINIGL_RIX_MAX_TEXTURES];
    MiniGLRixTexture textures[MINIGL_RIX_MAX_TEXTURES];
    uint32_t bound_texture;
    uint32_t lru_clock;
    size_t triangle_count;
    size_t op_count;
    size_t command_words_estimate;
} MiniGLRixBackend;

/* Validates MNTZorro identity before rix_drv_init can access the GP1 CSR. */
void minigl_rix_backend_init(MiniGLRixBackend *backend,
                             const MiniGLRixPlatform *platform);
void minigl_rix_backend_reset(MiniGLRixBackend *backend);
int minigl_rix_backend_available(const MiniGLRixBackend *backend);
void minigl_rix_backend_set_state(MiniGLRixBackend *backend,
                                  const ZZ9KMiniGLPacketSetState *state);
void minigl_rix_backend_bind_texture(MiniGLRixBackend *backend, uint32_t id);

/* begin_draw/add_triangle/end_draw record a draw in the active frame.  The
 * frame is band-replayed and submitted at drain or the next required hand-off. */
/* RasterIX addresses a 256 KiB virtual texture: level 0 begins at zero and
 * later mip levels occupy the sparse page layout used by TextureSampler. */
int minigl_rix_backend_upload_texture(MiniGLRixBackend *backend, uint32_t id,
                                      uint16_t level, const uint32_t *rgba,
                                      uint16_t width, uint16_t height,
                                      uint16_t format);
void minigl_rix_backend_tex_params(MiniGLRixBackend *backend, uint32_t id,
                                   uint16_t min_filter, uint16_t mag_filter,
                                   uint16_t wrap_s, uint16_t wrap_t);
void minigl_rix_backend_delete_texture(MiniGLRixBackend *backend, uint32_t id);
int minigl_rix_backend_begin_draw(MiniGLRixBackend *backend, uint16_t width,
                                  uint16_t height, uint32_t color_address,
                                  uint32_t depth_address);
int minigl_rix_backend_add_triangle(MiniGLRixBackend *backend,
                                    const MiniGLRixVertex *a,
                                    const MiniGLRixVertex *b,
                                    const MiniGLRixVertex *c);
int minigl_rix_backend_end_draw(MiniGLRixBackend *backend, uint32_t color_address,
                                uint32_t depth_address);
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
