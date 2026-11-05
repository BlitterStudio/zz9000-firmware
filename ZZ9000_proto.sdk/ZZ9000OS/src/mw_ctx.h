/*
 * ZZ9000 MiniGL Wire Protocol v9 Context
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MW_CTX_H
#define MW_CTX_H

#include <stdint.h>
#include <stddef.h>
#include "mw_host.h"
#include "mw_types.h"
#include "mw_state.h"
#include "mw_texture.h"

#ifdef __cplusplus
extern "C" {
#endif


#define MW_DIAG_UPLOADS 8U
#define MW_DIAG_BUCKETS 6U

typedef struct mw_diag_bucket {
    uint32_t blend_src;
    uint32_t blend_dst;
    uint32_t texture;
    uint32_t color;
    uint16_t tex_env;
    uint16_t tex_width;
    uint16_t tex_height;
    uint8_t tex_internal_rgb;
    uint8_t blend_enabled;
    uint8_t texture_enabled;
    uint8_t alpha_test;
    uint8_t depth_test;
    uint32_t count;
} mw_diag_bucket;

typedef struct mw_diag_upload {
    uint32_t texture;
    uint32_t internal_format;
    uint32_t format;
    uint32_t type;
    uint32_t raw0;
    uint32_t rgba0;
    uint16_t width;
    uint16_t height;
    uint8_t level;
    uint8_t subimage;
    uint8_t internal_rgb;
} mw_diag_upload;

typedef struct mw_frame_diag {
    uint64_t began_us;
    uint64_t ended_us;
    uint32_t draws;
    uint32_t texture_uploads;
    uint32_t texture_upload_bytes;
    uint32_t texture_upload_format;
    uint8_t upload_count;
    mw_diag_upload uploads[MW_DIAG_UPLOADS];
    uint32_t cpu_draws;
    uint32_t framebuffer_readbacks;
    uint32_t sync_wait_us;
    uint32_t target_wait_us;
    uint32_t drain_wait_us;
    uint32_t copy_wait_us;
    uint32_t flip_wait_us;
    uint8_t bucket_count;
    mw_diag_bucket buckets[MW_DIAG_BUCKETS];
} mw_frame_diag;
struct mw_ctx {
    uint32_t width;
    uint32_t height;
    mw_state state;
    /* First asynchronous command error; packet transport remains healthy. */
    uint32_t error;
    mw_texture_manager texmgr;
    uint8_t  palette[256][4];
    uint32_t palette_width;
    uint32_t *color_buffers[3];
    /* Optional RGB565 P96 targets selected per colour slot.  The matching
     * color_buffers[] allocation remains available for CPU fallback. */
    uint16_t *direct_color_buffers[3];
    uint8_t color_direct[3];
    /* Per colour buffer: contents are in RasterIX's RGB565 layout and a CPU
     * reader must convert through minigl_raster_buffer_to_rgba first.  The
     * layout belongs to the buffer and survives an undefined-content swap. */
    uint8_t  color_rgb565[3];
    /* zzapp_gl marks targets retained for an asynchronous P96 copy. */
    uint8_t  present_queued[3];
    uint8_t  present_submit_rgb565[3];
    uint32_t present_submit_frame[3];
    uint8_t  *depth_buffer;
    uint8_t  *texture_pool;
    uint32_t texture_pool_bytes;

    /* RasterIX storage is independent from CPU texture residency.  It belongs
     * to this context's card-pool owner and is absent in normal firmware. */
    uint8_t  *rix_command_buffers[2];
    uint8_t  *rix_white_texture;
    uint8_t  *rix_texture_pool;
    uint32_t rix_command_buffer_addresses[2];
    uint32_t rix_white_texture_address;
    uint32_t rix_texture_pool_address;
    uint32_t rix_texture_pool_bytes;
    uint8_t  rix_enabled;
    /* A disabled RasterIX can retain unbounded DMA writes to its old target.
     * That allocation deliberately outlives this context. */
    uint8_t  rix_target_quarantined;
    uint32_t *rix_quarantined_color_buffers[3];
    uint8_t  *rix_quarantined_depth_buffer;
    uint32_t rix_quarantined_color_addresses[3];
    uint32_t rix_quarantined_depth_address;
    float *vertex_scratch;
    uint32_t vertex_scratch_capacity;

    uint32_t handle;
    uint32_t generation;
    uint32_t card_pool_owner;
    uint32_t color_buffer_addresses[3];
    uint32_t depth_buffer_address;
    uint32_t texture_pool_address;
    uint32_t vertex_scratch_address;
    uint8_t card_pool_backed;

    /* Indexed P96 presentation cache.  Each byte maps one RGB555 colour to
     * the closest current primary-display CLUT entry. */
    uint8_t   presented_clut[32U * 32U * 32U];
    uint32_t  presented_clut_generation;
    /* ARM-app-only capture of the wire state used to build a presented frame. */
    mw_frame_diag diag_frame;
    mw_frame_diag present_diag[3];
    uint64_t diag_last_log_us;
    uint32_t diag_framebuffer_readbacks;
    uint8_t diag_frame_ready[3];
};

/* zzapp_gl adds presentation timing and reads the physical RGB565 samples. */
void mw_ctx_diag_add_wait(mw_ctx *c, uint32_t copy_us, uint32_t flip_us,
                          uint32_t drain_us, uint32_t target_us);
void mw_ctx_diag_add_present_wait(mw_ctx *c, uint32_t source, uint32_t copy_us,
                                  uint32_t flip_us, uint32_t drain_us);
void mw_ctx_diag_presented(mw_ctx *c, uint32_t source, const uint8_t *pixels,
                           uint32_t pitch, uint32_t format, uint32_t width,
                           uint32_t height);

#ifdef __cplusplus
}
#endif

#endif /* MW_CTX_H */
