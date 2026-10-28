/*
 * RasterIX band binning and dispatch for ZZ9000.
 * Derived from RasterIX by ToNi3141; SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Band protocol (verified against Rasterizer.v and
 * InternalFramebufferCommandHandler.v with the 16,384-pixel band RAM of
 * zz9k_rasterix):
 *  - The rasterizer clamps scan rows with `y < yResolution` and writes band
 *    RAM at ((yResolution - 1) - y) * xResolution + x, so triangle
 *    descriptors must carry band-local y counted upwards from the band's
 *    bottom row, and RENDER_CONFIG resolution must be (width, band lines).
 *  - COMMIT/MEMSET walk band RAM from index 0 to the command size and map
 *    to DDR at the configured buffer address in that order, so RAM row 0 is
 *    the first DDR row of the band and the sizes are band lines * width.
 *  - RasterIX y follows the OpenGL bottom-up convention: absolute y is
 *    (screen height - top-down row).  A band covering top-down rows
 *    [t, t + lines) therefore uses Y offset = height - t - lines, buffer
 *    addresses at base + t * pitch, and descriptors rebased by the same
 *    Y offset.
 */
#ifndef RIX_BIN_H
#define RIX_BIN_H

#include <stddef.h>
#include <stdint.h>

#include "rix_cmd.h"
#include "rix_setup.h"

#define RIX_MAX_BANDS 256U
#define RIX_CLEAR_COLOR 1U
#define RIX_CLEAR_DEPTH 2U

typedef struct RixBand {
    /* Absolute bottom-up GL row range [y_start, y_end) of the band.
     * Band 0 covers the topmost screen rows; the last band may be short. */
    uint16_t y_start, y_end;
    size_t first, triangle_count;
} RixBand;

typedef struct RixBandList {
    RixBand band[RIX_MAX_BANDS];
    RixTriangle **references;
    size_t reference_capacity, reference_count;
    uint16_t count, width, height;
} RixBandList;

typedef struct RixBandConfig {
    uint16_t width, height;
    uint32_t color_addr, depth_addr;
    uint32_t color_pitch, depth_pitch;
    uint32_t clear_color;
    uint16_t clear_depth;
    uint8_t clear_mask;
} RixBandConfig;

/* Per-band RasterIX_IF protocol values derived by rix_band_setup(). */
typedef struct RixBandSetup {
    uint16_t x_resolution; /* render width */
    uint16_t y_resolution; /* band line count */
    uint16_t y_offset;     /* absolute bottom-up first GL row of the band */
    uint32_t color_addr;   /* color base + top-down band row * pitch */
    uint32_t depth_addr;   /* depth base + top-down band row * pitch */
    uint32_t pixels;       /* band line count * width */
} RixBandSetup;

/* RasterIX_IF framebuffer transfers are two RGB565/Z16 pixels per beat, so
 * width must be even.  Unsupported geometry returns zero before recording. */
int rix_band_list_init(RixBandList *list, uint16_t width, uint16_t height,
                       RixTriangle **references, size_t reference_capacity);
int rix_band_list_add(RixBandList *list, RixTriangle *triangle);
void rix_band_setup(RixBandSetup *setup, const RixBandList *list,
                    const RixBandConfig *config, uint16_t band);
int rix_band_emit_header(RixCmdWriter *writer, const RixBandSetup *setup);
int rix_band_emit_frame(RixCmdWriter *writer, const RixBandList *list,
                        const RixBandConfig *config);

#endif
