/*
 * ZZ9000 ARM Software 3D Rasterizer & Pipeline
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MINIGL_RASTER_H
#define MINIGL_RASTER_H

#include <stdint.h>
#include "minigl_renderer.h"
#include "minigl_rix_backend.h"



typedef struct MiniGLRasterBuffers {
    uint32_t *color;
    uint32_t color_bytes;
    uint8_t *depth;
    uint32_t depth_bytes;
    uint8_t *vertices;
    uint32_t vertices_bytes;
    uint8_t *texture_staging;
    uint32_t texture_staging_bytes;
    uint8_t *texture_pool;
    uint32_t texture_pool_bytes;
    uint16_t width;
    uint16_t height;
} MiniGLRasterBuffers;

typedef struct MiniGLRasterDirtyRegion { uint16_t x, y, width, height; } MiniGLRasterDirtyRegion;

typedef struct MiniGLRasterRixDiagnostics {
    uint32_t status;
    uint32_t completed_fence;
    uint32_t last_fence;
    uint8_t active;
} MiniGLRasterRixDiagnostics;
typedef void (*MiniGLRasterTraceHook)(uint32_t phase);

/* Trace hook phase 1 brackets a vertex transform and attribute preparation. */
void minigl_raster_set_trace_hook(MiniGLRasterTraceHook hook);


/* Returns zero if any supplied board-memory region cannot contain the target. */
int minigl_raster_configure_buffers(const MiniGLRasterBuffers *buffers);
void minigl_raster_release_buffers(void);
const uint32_t *minigl_raster_color_buffer(void);
uint16_t minigl_raster_width(void);
uint16_t minigl_raster_height(void);
uint32_t minigl_raster_get_error(void);
int minigl_raster_take_dirty_region(MiniGLRasterDirtyRegion *region);
int minigl_raster_configure(uint32_t width, uint32_t height);
void minigl_raster_reset(void);

void minigl_raster_init(void);
/* The core-1 service supplies the private, card-pool-owned CPU texture
 * residency before initialization. */
void minigl_raster_set_texture_pool(uint8_t *pool, uint32_t bytes);
/* Called by the core-1 service before initialization; it is safe to omit on
 * old bitstreams and host tests, which leaves RasterIX unavailable. */
void minigl_raster_set_rix_platform(const MiniGLRixPlatform *platform);
/* Drains RasterIX and restores the shared target to CPU RGBA before readback. */
int minigl_raster_finish(void);
/* Retires all queued engine work before its card-pool buffers are released. */
int minigl_raster_quiesce(void);
void minigl_raster_clear(uint32_t mask, uint32_t color_rgba, uint32_t depth_value);
void minigl_raster_set_state(const ZZ9KMiniGLPacketSetState *state);
void minigl_raster_load_matrix(uint16_t mode, const float *m);
void minigl_raster_multiply_matrix(uint16_t mode, const float *m);
int minigl_raster_push_matrix(uint16_t mode);
int minigl_raster_pop_matrix(uint16_t mode);
void minigl_raster_bind_texture(uint16_t unit, uint16_t target, uint32_t texture_id);
void minigl_raster_upload_texture(uint32_t texture_id, uint16_t level, uint16_t width, uint16_t height,
                                  uint16_t format, uint16_t staging_slot, uint32_t size_bytes);
void minigl_raster_tex_params(uint32_t texture_id, uint16_t min_filter, uint16_t mag_filter,
                              uint16_t wrap_s, uint16_t wrap_t);
void minigl_raster_tex_subimage(uint32_t texture_id, uint16_t level, uint16_t xoffset, uint16_t yoffset,
                                uint16_t width, uint16_t height, uint16_t format,
                                uint16_t staging_slot, uint32_t size_bytes);
void minigl_raster_delete_texture(uint32_t texture_id);
void minigl_raster_draw_arrays(uint16_t mode, uint16_t attr_code, uint16_t vertex_stride,
                               uint32_t first, uint32_t count, uint32_t vertex_base_offset);
void minigl_raster_draw_elements(uint16_t mode, uint16_t attr_code, uint16_t vertex_stride,
                                 uint32_t count, uint32_t vertex_base_offset,
                                 uint32_t index_base_offset);
void minigl_raster_present(const ZZ9KMiniGLPacketPresent *present);
uint32_t minigl_raster_draws_hardware(void);
uint32_t minigl_raster_draws_software(void);
uint32_t minigl_raster_first_fallback_reason(void);
void minigl_raster_reset_frame_counters(void);
int minigl_raster_hardware_available(void);
/* Snapshot the RasterIX CSR state without changing command ownership. */
int minigl_raster_get_rix_diagnostics(MiniGLRasterRixDiagnostics *out);
int minigl_raster_upload_rgba8(uint32_t id, uint16_t level, uint16_t width, uint16_t height,
                               const uint32_t *rgba_pixels, int internal_rgb);
int minigl_raster_subimage_rgba8(uint32_t id, uint16_t level, uint16_t xoffset, uint16_t yoffset,
                                 uint16_t width, uint16_t height, const uint32_t *rgba_pixels,
                                 int internal_rgb);
void minigl_raster_draw_wire_v9(uint16_t mode, const float *object_vertices,
                                uint32_t count, uint32_t flags,
                                const uint32_t *edge_mask);
struct mw_state;
void minigl_raster_set_wire_v9_state(const struct mw_state *state);

#endif /* MINIGL_RASTER_H */
