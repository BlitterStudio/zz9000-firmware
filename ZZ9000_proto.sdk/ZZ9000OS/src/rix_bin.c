/*
 * RasterIX band binning and dispatch for ZZ9000.
 * Derived from RasterIX by ToNi3141; SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rix_bin.h"

#include <string.h>

#define RIX_BAND_PIXELS 16384U

int rix_band_list_init(RixBandList *list, uint16_t width, uint16_t height,
                       RixTriangle **references, size_t reference_capacity)
{
    uint32_t rows, count, i;

    if (list == NULL || references == NULL || width == 0U || height == 0U) {
        return 0;
    }
    /* Every scanline is dispatched: full bands of RIX_BAND_PIXELS / width
     * lines plus a final partial band, so no target rows are lost. */
    rows = RIX_BAND_PIXELS / width;
    count = ((uint32_t)height + rows - 1U) / rows;
    if (rows == 0U || count > RIX_MAX_BANDS) {
        return 0;
    }
    memset(list, 0, sizeof(*list));
    list->references = references;
    list->reference_capacity = reference_capacity;
    list->count = (uint16_t)count;
    list->width = width;
    list->height = height;
    for (i = 0U; i < count; ++i) {
        /* Band i covers top-down rows [i * rows, (i + 1) * rows), which is
         * the absolute bottom-up range stored here. */
        uint32_t top = i * rows;
        uint32_t lines = height - top < rows ? height - top : rows;
        uint32_t bottom = (uint32_t)height - top - lines;

        list->band[i].y_start = (uint16_t)bottom;
        list->band[i].y_end = (uint16_t)(bottom + lines);
    }
    return 1;
}

int rix_band_list_add(RixBandList *list, RixTriangle *triangle)
{
    uint16_t i;

    if (list == NULL || triangle == NULL || list->reference_count == list->reference_capacity) {
        return 0;
    }
    list->references[list->reference_count++] = triangle;
    for (i = 0U; i < list->count; ++i) {
        if (rix_triangle_in_band(triangle, list->band[i].y_start, list->band[i].y_end)) {
            ++list->band[i].triangle_count;
        }
    }
    return 1;
}

void rix_band_setup(RixBandSetup *setup, const RixBandList *list,
                    const RixBandConfig *config, uint16_t band)
{
    const RixBand *range = &list->band[band];
    uint16_t lines = range->y_end - range->y_start;
    /* Top-down first row of the band; buffer addresses advance top-down to
     * match the commit order of band RAM row 0 into the target surface. */
    uint16_t top = config->height - range->y_end;

    setup->x_resolution = config->width;
    setup->y_resolution = lines;
    setup->y_offset = range->y_start;
    setup->color_addr = config->color_addr + (uint32_t)top * config->color_pitch;
    setup->depth_addr = config->depth_addr + (uint32_t)top * config->depth_pitch;
    setup->pixels = (uint32_t)config->width * lines;
}

int rix_band_emit_header(RixCmdWriter *writer, const RixBandSetup *setup)
{
    return rix_cmd_resolution(writer, setup->x_resolution, setup->y_resolution) &&
           rix_cmd_y_offset(writer, setup->y_offset) &&
           rix_cmd_buffer_address(writer, RIX_REG_COLOR_BUFFER_ADDR,
                                  setup->color_addr) &&
           rix_cmd_buffer_address(writer, RIX_REG_DEPTH_BUFFER_ADDR,
                                  setup->depth_addr);
}

static int rix_band_emit_triangles(RixCmdWriter *writer, const RixBandList *list, const RixBand *band)
{
    size_t i;

    for (i = 0U; i < list->reference_count; ++i) {
        RixTriangle copy;
        if (!rix_triangle_in_band(list->references[i], band->y_start, band->y_end)) {
            continue;
        }
        copy = *list->references[i];
        if (!rix_triangle_increment(&copy, band->y_start, band->y_end) ||
            !rix_cmd_triangle(writer, copy.words, RIX_TRIANGLE_WORDS)) {
            return 0;
        }
    }
    return 1;
}

int rix_band_emit_frame(RixCmdWriter *writer, const RixBandList *list,
                        const RixBandConfig *config)
{
    uint16_t i;

    if (writer == NULL || list == NULL || config == NULL || list->count == 0U ||
        list->width != config->width || list->height != config->height) {
        return 0;
    }
    if (!rix_cmd_clear_color(writer, config->clear_color) ||
        !rix_cmd_clear_depth(writer, config->clear_depth)) {
        return 0;
    }
    for (i = 0U; i < list->count; ++i) {
        const RixBand *band = &list->band[i];
        RixBandSetup setup;
        uint32_t select = 0U;

        rix_band_setup(&setup, list, config, i);
        if (!rix_band_emit_header(writer, &setup)) {
            return 0;
        }
        if (config->clear_mask & RIX_CLEAR_COLOR) {
            select |= RIX_FRAMEBUFFER_COLOR;
        }
        if (config->clear_mask & RIX_CLEAR_DEPTH) {
            select |= RIX_FRAMEBUFFER_DEPTH;
        }
        if (select != 0U && !rix_cmd_framebuffer(writer, RIX_FRAMEBUFFER_MEMSET | select,
                                                 setup.pixels)) {
            return 0;
        }
        /* Depth is an internal Z16 working surface.  Only color is committed:
         * the wrapper has no depth readback consumer, and a depth commit
         * leaves its AXI stream waiting behind the color transfer. */
        if (!rix_band_emit_triangles(writer, list, band) ||
            !rix_cmd_framebuffer(writer, RIX_FRAMEBUFFER_COMMIT | RIX_FRAMEBUFFER_COLOR,
                                 setup.pixels)) {
            return 0;
        }
    }
    return 1;
}
