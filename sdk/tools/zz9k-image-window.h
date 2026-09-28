/*
 * Shared ARM-side image window scaling helpers for ZZ9000 SDK tools.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_IMAGE_WINDOW_H
#define ZZ9K_IMAGE_WINDOW_H

#include "zz9k-fb-common.h"
#include "zz9k/image_geometry.h"
#include "zz9k/host.h"
#include "zz9k/surface.h"
#include <stdint.h>

#if defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__) || \
	defined(__VBCC__)
#define ZZ9K_IMAGE_WINDOW_AMIGA 1
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <utility/tagitem.h>
#else
#define ZZ9K_IMAGE_WINDOW_AMIGA 0
struct Window;
#endif

#define ZZ9K_IMAGE_WINDOW_SCALE_SLICE_ROWS \
	ZZ9K_IMAGE_SCALE_DEFAULT_SLICE_ROWS
#define ZZ9K_IMAGE_WINDOW_MAX_VISIBLE_CLIPS 32U

typedef struct ZZ9KImageWindowConfig {
	const char *title;
	uint32_t source_width;
	uint32_t source_height;
	uint32_t min_width;
	uint32_t min_height;
	uint32_t margin_x;
	uint32_t margin_y;
	int fit_framebuffer;
	int resizable;
	int close_gadget;
} ZZ9KImageWindowConfig;

typedef struct ZZ9KImageWindow {
	ZZ9KFbRect inner;
	int resizable;
#if ZZ9K_IMAGE_WINDOW_AMIGA
	struct Screen *screen;
	struct Window *window;
#endif
} ZZ9KImageWindow;

typedef struct ZZ9KImageWindowEvent {
	int changed;
	int closed;
	uint32_t vanilla_key;
	uint32_t raw_key;
} ZZ9KImageWindowEvent;

uint32_t zz9k_image_window_scale_slice_rows(void);

uint32_t zz9k_image_window_count_scale_slices(
	const ZZ9KFbRect *clip_rect);

int zz9k_image_window_fit_size_to_area(uint32_t src_w, uint32_t src_h,
                                       uint32_t area_w, uint32_t area_h,
                                       uint32_t *out_w,
                                       uint32_t *out_h);

int zz9k_image_window_choose_draw_rect_in_area(
	const ZZ9KFbRect *area,
	uint32_t src_width,
	uint32_t src_height,
	ZZ9KFbRect *rect);

int zz9k_image_window_build_clipped_scale_desc(
	ZZ9KScaleImageClippedDesc *desc,
	uint32_t source_handle,
	uint32_t source_width,
	uint32_t source_height,
	const ZZ9KFbRect *draw_rect,
	const ZZ9KFbRect *clip_rect,
	uint32_t filter);

int zz9k_image_window_build_scale_desc(
	ZZ9KScaleImageDesc *desc,
	uint32_t source_handle,
	uint32_t source_width,
	uint32_t source_height,
	const ZZ9KFbRect *draw_rect,
	uint32_t filter);

int zz9k_image_window_build_framebuffer_fill_desc(
	ZZ9KSurfaceFillDesc *desc,
	const ZZ9KFbRect *rect,
	uint32_t color,
	uint32_t flags);

int zz9k_image_window_intersect_rect(const ZZ9KFbRect *a,
                                     const ZZ9KFbRect *b,
                                     ZZ9KFbRect *out);

int zz9k_image_window_build_damage_clips(const ZZ9KFbRect *visible_rects,
                                         uint32_t visible_count,
                                         const ZZ9KFbRect *damage_rect,
                                         ZZ9KFbRect *clips,
                                         uint32_t clip_capacity,
                                         uint32_t *clip_count);

int zz9k_image_window_visible_clips(const ZZ9KImageWindow *ui,
                                    const ZZ9KFbRect *damage_rect,
                                    ZZ9KFbRect *clips,
                                    uint32_t clip_capacity,
                                    uint32_t *clip_count);

int zz9k_image_window_visible_clips_for_window(
	struct Window *window,
	const ZZ9KFbRect *damage_rect,
	ZZ9KFbRect *clips,
	uint32_t clip_capacity,
	uint32_t *clip_count);

int zz9k_image_window_loop_done(int wait_for_close,
                                uint32_t elapsed_ticks,
                                uint32_t hold_ticks,
                                int closed);

int zz9k_image_window_scale_sliced(
	ZZ9KContext *ctx,
	uint32_t source_handle,
	uint32_t source_width,
	uint32_t source_height,
	const ZZ9KFbRect *draw_rect,
	const ZZ9KFbRect *clip_rect,
	uint32_t filter);

int zz9k_image_window_choose_origin(uint32_t screen_width,
                                    uint32_t screen_height,
                                    uint32_t window_width,
                                    uint32_t window_height,
                                    uint32_t *left,
                                    uint32_t *top);

int zz9k_image_window_choose_max_extent(uint32_t framebuffer_width,
                                        uint32_t framebuffer_height,
                                        uint32_t left,
                                        uint32_t top,
                                        uint32_t min_width,
                                        uint32_t min_height,
                                        uint32_t *max_width,
                                        uint32_t *max_height);

void zz9k_image_window_config_init(ZZ9KImageWindowConfig *config,
                                   const char *title,
                                   uint32_t source_width,
                                   uint32_t source_height,
                                   int fit_framebuffer,
                                   int resizable,
                                   uint32_t min_width,
                                   uint32_t min_height,
                                   uint32_t margin_x,
                                   uint32_t margin_y);

int zz9k_image_window_choose_size(const ZZ9KSurface *framebuffer,
                                  const ZZ9KImageWindowConfig *config,
                                  uint32_t *out_width,
                                  uint32_t *out_height);

int zz9k_image_window_inner_rect(const ZZ9KSurface *framebuffer,
                                 const ZZ9KImageWindow *ui,
                                 ZZ9KFbRect *inner);

int zz9k_image_window_open(const ZZ9KSurface *framebuffer,
                           const ZZ9KImageWindowConfig *config,
                           ZZ9KImageWindow *ui);

int zz9k_image_window_poll(ZZ9KImageWindow *ui,
                           const ZZ9KSurface *framebuffer,
                           int *changed,
                           int *closed);

int zz9k_image_window_poll_event(ZZ9KImageWindow *ui,
                                 const ZZ9KSurface *framebuffer,
                                 ZZ9KImageWindowEvent *event);

void zz9k_image_window_set_title(ZZ9KImageWindow *ui, const char *title);

void zz9k_image_window_close(ZZ9KImageWindow *ui);

#endif
