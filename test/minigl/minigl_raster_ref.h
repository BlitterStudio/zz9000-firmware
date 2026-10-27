#ifndef MINIGL_RASTER_REF_H
#define MINIGL_RASTER_REF_H

#include <stdint.h>
#include "../../ZZ9000_proto.sdk/ZZ9000OS/src/minigl_raster.h"

int ref_minigl_raster_configure_buffers(const MiniGLRasterBuffers *buffers);
void ref_minigl_raster_release_buffers(void);
const uint32_t *ref_minigl_raster_color_buffer(void);
uint16_t ref_minigl_raster_width(void);
uint16_t ref_minigl_raster_height(void);
uint32_t ref_minigl_raster_get_error(void);
int ref_minigl_raster_take_dirty_region(MiniGLRasterDirtyRegion *region);
int ref_minigl_raster_configure(uint32_t width, uint32_t height);
void ref_minigl_raster_reset(void);
void ref_minigl_raster_init(void);
void ref_minigl_raster_clear(uint32_t mask, uint32_t color_rgba, uint32_t depth_value);
void ref_minigl_raster_set_state(const ZZ9KMiniGLPacketSetState *state);
void ref_minigl_raster_load_matrix(uint16_t mode, const float *m);
void ref_minigl_raster_multiply_matrix(uint16_t mode, const float *m);
int ref_minigl_raster_push_matrix(uint16_t mode);
int ref_minigl_raster_pop_matrix(uint16_t mode);
void ref_minigl_raster_bind_texture(uint16_t unit, uint16_t target, uint32_t texture_id);
void ref_minigl_raster_upload_texture(uint32_t texture_id, uint16_t level, uint16_t width, uint16_t height,
                                      uint16_t format, uint16_t staging_slot, uint32_t size_bytes);
void ref_minigl_raster_tex_params(uint32_t texture_id, uint16_t min_filter, uint16_t mag_filter,
                                  uint16_t wrap_s, uint16_t wrap_t);
void ref_minigl_raster_tex_subimage(uint32_t texture_id, uint16_t level, uint16_t xoffset, uint16_t yoffset,
                                    uint16_t width, uint16_t height, uint16_t format,
                                    uint16_t staging_slot, uint32_t size_bytes);
void ref_minigl_raster_delete_texture(uint32_t texture_id);
void ref_minigl_raster_draw_arrays(uint16_t mode, uint16_t attr_code, uint16_t vertex_stride,
                                   uint32_t first, uint32_t count, uint32_t vertex_base_offset);
void ref_minigl_raster_draw_elements(uint16_t mode, uint16_t attr_code, uint16_t vertex_stride,
                                     uint32_t count, uint32_t vertex_base_offset,
                                     uint32_t index_base_offset);
void ref_minigl_raster_present(const ZZ9KMiniGLPacketPresent *present);

#endif /* MINIGL_RASTER_REF_H */
