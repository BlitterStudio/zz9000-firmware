/*
 * Shared ARM-side image window scaling helpers for ZZ9000 SDK tools.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-image-window.h"

#include <stdio.h>
#include <string.h>

#if ZZ9K_IMAGE_WINDOW_AMIGA
#include <graphics/clip.h>
#include <graphics/layers.h>
struct IntuitionBase *IntuitionBase;
#endif

static void zz9k_image_window_to_rect(const ZZ9KFbRect *source,
                                      ZZ9KRect *target)
{
	target->x = source->x;
	target->y = source->y;
	target->w = source->w;
	target->h = source->h;
}

static void zz9k_image_window_from_rect(const ZZ9KRect *source,
                                        ZZ9KFbRect *target)
{
	target->x = source->x;
	target->y = source->y;
	target->w = source->w;
	target->h = source->h;
}

uint32_t zz9k_image_window_scale_slice_rows(void)
{
	return ZZ9K_IMAGE_WINDOW_SCALE_SLICE_ROWS;
}

uint32_t zz9k_image_window_count_scale_slices(
	const ZZ9KFbRect *clip_rect)
{
	ZZ9KRect rect;

	if (!clip_rect)
		return 0U;
	zz9k_image_window_to_rect(clip_rect, &rect);
	return zz9k_image_count_scale_slices(
		&rect, zz9k_image_window_scale_slice_rows());
}

static int zz9k_image_window_rect_extents(const ZZ9KFbRect *rect,
                                          uint32_t *right,
                                          uint32_t *bottom)
{
	if (!rect || !right || !bottom ||
	    rect->w == 0U || rect->h == 0U ||
	    rect->x > (0xffffffffU - rect->w) ||
	    rect->y > (0xffffffffU - rect->h)) {
		return 0;
	}
	*right = rect->x + rect->w;
	*bottom = rect->y + rect->h;
	return 1;
}

int zz9k_image_window_intersect_rect(const ZZ9KFbRect *a,
                                     const ZZ9KFbRect *b,
                                     ZZ9KFbRect *out)
{
	uint32_t a_right;
	uint32_t a_bottom;
	uint32_t b_right;
	uint32_t b_bottom;
	uint32_t left;
	uint32_t top;
	uint32_t right;
	uint32_t bottom;

	if (!out ||
	    !zz9k_image_window_rect_extents(a, &a_right, &a_bottom) ||
	    !zz9k_image_window_rect_extents(b, &b_right, &b_bottom)) {
		return 0;
	}

	left = a->x > b->x ? a->x : b->x;
	top = a->y > b->y ? a->y : b->y;
	right = a_right < b_right ? a_right : b_right;
	bottom = a_bottom < b_bottom ? a_bottom : b_bottom;
	if (right <= left || bottom <= top)
		return 0;

	out->x = left;
	out->y = top;
	out->w = right - left;
	out->h = bottom - top;
	return 1;
}

int zz9k_image_window_build_damage_clips(const ZZ9KFbRect *visible_rects,
                                         uint32_t visible_count,
                                         const ZZ9KFbRect *damage_rect,
                                         ZZ9KFbRect *clips,
                                         uint32_t clip_capacity,
                                         uint32_t *clip_count)
{
	uint32_t damage_right;
	uint32_t damage_bottom;
	uint32_t i;

	if (!clip_count || !damage_rect ||
	    (visible_count != 0U && !visible_rects) ||
	    (clip_capacity != 0U && !clips)) {
		return 0;
	}
	*clip_count = 0U;
	if (!zz9k_image_window_rect_extents(
		    damage_rect, &damage_right, &damage_bottom)) {
		return 0;
	}

	for (i = 0U; i < visible_count; i++) {
		ZZ9KFbRect clip;

		if (!zz9k_image_window_intersect_rect(
			    &visible_rects[i], damage_rect, &clip)) {
			continue;
		}
		if (*clip_count >= clip_capacity) {
			*clip_count = 0U;
			return 0;
		}
		clips[*clip_count] = clip;
		(*clip_count)++;
	}
	return 1;
}

#if ZZ9K_IMAGE_WINDOW_AMIGA
static int zz9k_image_window_cliprect_to_rect(
	const struct ClipRect *clip_rect,
	ZZ9KFbRect *rect)
{
	int32_t min_x;
	int32_t min_y;
	int32_t max_x;
	int32_t max_y;

	if (!clip_rect || !rect)
		return 0;
	if (clip_rect->obscured)
		return 0;
	min_x = (int32_t)clip_rect->bounds.MinX;
	min_y = (int32_t)clip_rect->bounds.MinY;
	max_x = (int32_t)clip_rect->bounds.MaxX;
	max_y = (int32_t)clip_rect->bounds.MaxY;
	if (max_x < min_x || max_y < min_y ||
	    max_x < 0 || max_y < 0) {
		return 0;
	}
	if (min_x < 0)
		min_x = 0;
	if (min_y < 0)
		min_y = 0;

	rect->x = (uint32_t)min_x;
	rect->y = (uint32_t)min_y;
	rect->w = (uint32_t)(max_x - min_x) + 1U;
	rect->h = (uint32_t)(max_y - min_y) + 1U;
	return rect->w != 0U && rect->h != 0U;
}
#endif

int zz9k_image_window_visible_clips(const ZZ9KImageWindow *ui,
                                    const ZZ9KFbRect *damage_rect,
                                    ZZ9KFbRect *clips,
                                    uint32_t clip_capacity,
                                    uint32_t *clip_count)
{
	if (!ui)
		return 0;
#if ZZ9K_IMAGE_WINDOW_AMIGA
	return zz9k_image_window_visible_clips_for_window(
		ui->window, damage_rect, clips, clip_capacity, clip_count);
#else
	return zz9k_image_window_build_damage_clips(
		&ui->inner, 1U, damage_rect, clips, clip_capacity,
		clip_count);
#endif
}

int zz9k_image_window_visible_clips_for_window(
	struct Window *window,
	const ZZ9KFbRect *damage_rect,
	ZZ9KFbRect *clips,
	uint32_t clip_capacity,
	uint32_t *clip_count)
{
	uint32_t damage_right;
	uint32_t damage_bottom;

	if (!clip_count || !damage_rect ||
	    (clip_capacity != 0U && !clips)) {
		return 0;
	}
	*clip_count = 0U;
	if (!zz9k_image_window_rect_extents(
		    damage_rect, &damage_right, &damage_bottom)) {
		return 0;
	}

#if ZZ9K_IMAGE_WINDOW_AMIGA
	{
		const struct ClipRect *clip_rect;
		int ok = 1;

		if (!window || !window->WLayer)
			return 0;
		Forbid();
		for (clip_rect = window->WLayer->ClipRect;
		     clip_rect != 0; clip_rect = clip_rect->Next) {
			ZZ9KFbRect visible;
			ZZ9KFbRect clip;

			if (!zz9k_image_window_cliprect_to_rect(
				    clip_rect, &visible) ||
			    !zz9k_image_window_intersect_rect(
				    &visible, damage_rect, &clip)) {
				continue;
			}
			if (*clip_count >= clip_capacity) {
				*clip_count = 0U;
				ok = 0;
				break;
			}
			clips[*clip_count] = clip;
			(*clip_count)++;
		}
		Permit();
		return ok;
	}
#else
	(void)window;
	return zz9k_image_window_build_damage_clips(
		damage_rect, 1U, damage_rect, clips, clip_capacity,
		clip_count);
#endif
}

int zz9k_image_window_loop_done(int wait_for_close,
                                uint32_t elapsed_ticks,
                                uint32_t hold_ticks,
                                int closed)
{
	if (closed)
		return 1;
	if (wait_for_close)
		return 0;
	return elapsed_ticks >= hold_ticks;
}

int zz9k_image_window_fit_size_to_area(uint32_t src_w, uint32_t src_h,
                                       uint32_t area_w, uint32_t area_h,
                                       uint32_t *out_w,
                                       uint32_t *out_h)
{
	return zz9k_image_fit_size_to_area(src_w, src_h, area_w, area_h,
	                                   out_w, out_h);
}

static int zz9k_image_window_clamp_size(uint32_t framebuffer_size,
                                        uint32_t desired,
                                        uint32_t margin,
                                        uint32_t minimum,
                                        uint32_t *out)
{
	uint32_t available;

	if (!out || framebuffer_size < minimum)
		return 0;
	available = framebuffer_size > margin ? framebuffer_size - margin :
	                                        framebuffer_size;
	if (available < minimum)
		available = framebuffer_size;
	if (desired < minimum)
		desired = minimum;
	if (desired > available)
		desired = available;
	*out = desired;
	return desired != 0U;
}

#if ZZ9K_IMAGE_WINDOW_AMIGA
static void zz9k_image_window_drain_messages(struct Window *window)
{
	struct IntuiMessage *msg;

	if (!window || !window->UserPort)
		return;
	while ((msg = (struct IntuiMessage *)GetMsg(window->UserPort)) != 0) {
		ReplyMsg((struct Message *)msg);
	}
}
#endif

int zz9k_image_window_choose_draw_rect_in_area(
	const ZZ9KFbRect *area,
	uint32_t src_width,
	uint32_t src_height,
	ZZ9KFbRect *rect)
{
	ZZ9KRect public_area;
	ZZ9KRect public_rect;

	if (!area || !rect)
		return 0;
	zz9k_image_window_to_rect(area, &public_area);
	if (!zz9k_image_choose_draw_rect_in_area(&public_area, src_width,
	                                         src_height, &public_rect)) {
		return 0;
	}
	zz9k_image_window_from_rect(&public_rect, rect);
	return 1;
}

int zz9k_image_window_build_clipped_scale_desc(
	ZZ9KScaleImageClippedDesc *desc,
	uint32_t source_handle,
	uint32_t source_width,
	uint32_t source_height,
	const ZZ9KFbRect *draw_rect,
	const ZZ9KFbRect *clip_rect,
	uint32_t filter)
{
	ZZ9KRect public_draw_rect;
	ZZ9KRect public_clip_rect;

	if (!draw_rect || !clip_rect)
		return 0;
	zz9k_image_window_to_rect(draw_rect, &public_draw_rect);
	zz9k_image_window_to_rect(clip_rect, &public_clip_rect);
	return zz9k_image_build_clipped_scale_desc(
		desc, source_handle, source_width, source_height,
		&public_draw_rect, &public_clip_rect, filter);
}

int zz9k_image_window_build_scale_desc(
	ZZ9KScaleImageDesc *desc,
	uint32_t source_handle,
	uint32_t source_width,
	uint32_t source_height,
	const ZZ9KFbRect *draw_rect,
	uint32_t filter)
{
	ZZ9KRect public_draw_rect;

	if (!draw_rect)
		return 0;
	zz9k_image_window_to_rect(draw_rect, &public_draw_rect);
	return zz9k_image_build_framebuffer_scale_desc(
		desc, source_handle, source_width, source_height,
		&public_draw_rect, filter);
}

int zz9k_image_window_build_framebuffer_fill_desc(
	ZZ9KSurfaceFillDesc *desc,
	const ZZ9KFbRect *rect,
	uint32_t color,
	uint32_t flags)
{
	ZZ9KRect public_rect;

	if (!rect)
		return 0;
	zz9k_image_window_to_rect(rect, &public_rect);
	return zz9k_surface_build_framebuffer_fill_desc(
		desc, &public_rect, color, flags);
}

int zz9k_image_window_scale_sliced(
	ZZ9KContext *ctx,
	uint32_t source_handle,
	uint32_t source_width,
	uint32_t source_height,
	const ZZ9KFbRect *draw_rect,
	const ZZ9KFbRect *clip_rect,
	uint32_t filter)
{
	uint32_t slice_rows;
	uint32_t end_y;
	uint32_t y;

	if (!ctx || source_handle == ZZ9K_INVALID_HANDLE ||
	    source_width == 0U || source_height == 0U ||
	    !draw_rect || draw_rect->w == 0U || draw_rect->h == 0U ||
	    !clip_rect || clip_rect->w == 0U || clip_rect->h == 0U ||
	    clip_rect->y > (0xffffffffU - clip_rect->h)) {
		return ZZ9K_STATUS_BAD_REQUEST;
	}

	slice_rows = zz9k_image_window_scale_slice_rows();
	if (slice_rows == 0U)
		return ZZ9K_STATUS_BAD_REQUEST;

	end_y = clip_rect->y + clip_rect->h;
	for (y = clip_rect->y; y < end_y;) {
		ZZ9KScaleImageClippedDesc clipped_scale;
		ZZ9KFbRect slice = *clip_rect;
		uint32_t remaining = end_y - y;
		int status;

		slice.y = y;
		slice.h = remaining > slice_rows ? slice_rows : remaining;
		if (!zz9k_image_window_build_clipped_scale_desc(
			    &clipped_scale, source_handle, source_width,
			    source_height, draw_rect, &slice, filter)) {
			return ZZ9K_STATUS_BAD_REQUEST;
		}
		status = zz9k_scale_image_clipped(ctx, &clipped_scale);
		if (status != ZZ9K_STATUS_OK)
			return status;
		y += slice.h;
	}

	return ZZ9K_STATUS_OK;
}

int zz9k_image_window_choose_origin(uint32_t screen_width,
                                    uint32_t screen_height,
                                    uint32_t window_width,
                                    uint32_t window_height,
                                    uint32_t *left,
                                    uint32_t *top)
{
	if (!left || !top || screen_width == 0U || screen_height == 0U ||
	    window_width == 0U || window_height == 0U) {
		return 0;
	}
	*left = screen_width > window_width ?
	        ((screen_width - window_width) / 2U) : 0U;
	*top = screen_height > window_height ?
	       ((screen_height - window_height) / 2U) : 0U;
	return 1;
}

int zz9k_image_window_choose_max_extent(uint32_t framebuffer_width,
                                        uint32_t framebuffer_height,
                                        uint32_t left,
                                        uint32_t top,
                                        uint32_t min_width,
                                        uint32_t min_height,
                                        uint32_t *max_width,
                                        uint32_t *max_height)
{
	if (!max_width || !max_height || framebuffer_width == 0U ||
	    framebuffer_height == 0U || min_width == 0U ||
	    min_height == 0U || left >= framebuffer_width ||
	    top >= framebuffer_height) {
		return 0;
	}

	*max_width = framebuffer_width - left;
	*max_height = framebuffer_height - top;
	if (*max_width < min_width || *max_height < min_height)
		return 0;
	return 1;
}

void zz9k_image_window_config_init(ZZ9KImageWindowConfig *config,
                                   const char *title,
                                   uint32_t source_width,
                                   uint32_t source_height,
                                   int fit_framebuffer,
                                   int resizable,
                                   uint32_t min_width,
                                   uint32_t min_height,
                                   uint32_t margin_x,
                                   uint32_t margin_y)
{
	if (!config)
		return;
	memset(config, 0, sizeof(*config));
	config->title = title;
	config->source_width = source_width;
	config->source_height = source_height;
	config->fit_framebuffer = fit_framebuffer;
	config->resizable = resizable;
	config->min_width = min_width;
	config->min_height = min_height;
	config->margin_x = margin_x;
	config->margin_y = margin_y;
	config->close_gadget = 1;
}

int zz9k_image_window_choose_size(const ZZ9KSurface *framebuffer,
                                  const ZZ9KImageWindowConfig *config,
                                  uint32_t *out_width,
                                  uint32_t *out_height)
{
	uint32_t desired_width;
	uint32_t desired_height;

	if (!framebuffer || !config || !out_width || !out_height ||
	    framebuffer->width == 0U || framebuffer->height == 0U ||
	    config->source_width == 0U || config->source_height == 0U) {
		return 0;
	}

	desired_width = config->source_width + 80U;
	desired_height = config->source_height + 90U;
	if (config->fit_framebuffer ||
	    desired_width < config->source_width ||
	    desired_height < config->source_height ||
	    config->source_width > framebuffer->width ||
	    config->source_height > framebuffer->height) {
		desired_width = framebuffer->width;
		desired_height = framebuffer->height;
	}

	if (!zz9k_image_window_clamp_size(framebuffer->width, desired_width,
	                                  config->margin_x, config->min_width,
	                                  out_width) ||
	    !zz9k_image_window_clamp_size(framebuffer->height, desired_height,
	                                  config->margin_y, config->min_height,
	                                  out_height)) {
		return 0;
	}
	return 1;
}

int zz9k_image_window_inner_rect(const ZZ9KSurface *framebuffer,
                                 const ZZ9KImageWindow *ui,
                                 ZZ9KFbRect *inner)
{
#if ZZ9K_IMAGE_WINDOW_AMIGA
	uint32_t left;
	uint32_t top;
	uint32_t width;
	uint32_t height;
	uint32_t border_x;
	uint32_t border_y;

	if (!framebuffer || !ui || !ui->window || !inner)
		return 0;

	left = (uint32_t)ui->window->LeftEdge +
	       (uint32_t)ui->window->BorderLeft;
	top = (uint32_t)ui->window->TopEdge +
	      (uint32_t)ui->window->BorderTop;
	border_x = (uint32_t)ui->window->BorderLeft +
	           (uint32_t)ui->window->BorderRight;
	border_y = (uint32_t)ui->window->BorderTop +
	           (uint32_t)ui->window->BorderBottom;
	if ((uint32_t)ui->window->Width <= border_x ||
	    (uint32_t)ui->window->Height <= border_y) {
		return 0;
	}
	width = (uint32_t)ui->window->Width - border_x;
	height = (uint32_t)ui->window->Height - border_y;
	if (left >= framebuffer->width || top >= framebuffer->height ||
	    width > (framebuffer->width - left) ||
	    height > (framebuffer->height - top)) {
		return 0;
	}

	inner->x = left;
	inner->y = top;
	inner->w = width;
	inner->h = height;
	return 1;
#else
	if (!framebuffer || !ui || !inner ||
	    framebuffer->width == 0U || framebuffer->height == 0U) {
		return 0;
	}
	(void)ui;
	inner->x = 0U;
	inner->y = 0U;
	inner->w = framebuffer->width;
	inner->h = framebuffer->height;
	return 1;
#endif
}

#ifndef ZZ9K_IMAGE_WINDOW_NO_UI
int zz9k_image_window_open(const ZZ9KSurface *framebuffer,
                           const ZZ9KImageWindowConfig *config,
                           ZZ9KImageWindow *ui)
{
#if ZZ9K_IMAGE_WINDOW_AMIGA
	uint32_t win_width;
	uint32_t win_height;
	uint32_t max_width;
	uint32_t max_height;
	uint32_t left;
	uint32_t top;
	ULONG idcmp;

	if (!framebuffer || !config || !ui)
		return 0;
	memset(ui, 0, sizeof(*ui));
	ui->resizable = config->resizable;

	IntuitionBase = (struct IntuitionBase *)OpenLibrary(
		(CONST_STRPTR)"intuition.library", 39L);
	if (!IntuitionBase) {
		printf("zz9k-image-window: open intuition.library failed\n");
		return 0;
	}

	ui->screen = LockPubScreen(NULL);
	if (!ui->screen) {
		printf("zz9k-image-window: lock public screen failed\n");
		zz9k_image_window_close(ui);
		return 0;
	}

	if (!zz9k_image_window_choose_size(framebuffer, config,
	                                   &win_width, &win_height)) {
		printf("zz9k-image-window: framebuffer too small for window\n");
		zz9k_image_window_close(ui);
		return 0;
	}

	if (!zz9k_image_window_choose_origin(
		    (uint32_t)ui->screen->Width, (uint32_t)ui->screen->Height,
		    win_width, win_height, &left, &top)) {
		printf("zz9k-image-window: invalid window origin\n");
		zz9k_image_window_close(ui);
		return 0;
	}

	idcmp = (ULONG)(IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
			IDCMP_VANILLAKEY | IDCMP_RAWKEY);
	if (config->resizable) {
		idcmp |= IDCMP_NEWSIZE;
	}
	if (config->resizable) {
		if (!zz9k_image_window_choose_max_extent(
			    framebuffer->width, framebuffer->height, left, top,
			    config->min_width, config->min_height, &max_width,
			    &max_height)) {
			printf("zz9k-image-window: invalid resize limits\n");
			zz9k_image_window_close(ui);
			return 0;
		}
		ui->window = OpenWindowTags(
			NULL,
			WA_PubScreen, (ULONG)ui->screen,
			WA_Left, (ULONG)left,
			WA_Top, (ULONG)top,
			WA_Width, (ULONG)win_width,
			WA_Height, (ULONG)win_height,
			WA_Title, (ULONG)(config->title ? config->title :
			                  "ZZ9000 SDK Image"),
			WA_DragBar, TRUE,
			WA_DepthGadget, TRUE,
			WA_CloseGadget, config->close_gadget ? TRUE : FALSE,
			WA_SizeGadget, TRUE,
			WA_SizeBRight, TRUE,
			WA_SizeBBottom, TRUE,
			WA_MinWidth, (ULONG)config->min_width,
			WA_MinHeight, (ULONG)config->min_height,
			WA_MaxWidth, (ULONG)max_width,
			WA_MaxHeight, (ULONG)max_height,
			WA_SmartRefresh, TRUE,
			WA_Activate, TRUE,
			WA_RMBTrap, TRUE,
			WA_IDCMP, idcmp,
			TAG_DONE);
	} else {
		ui->window = OpenWindowTags(
			NULL,
			WA_PubScreen, (ULONG)ui->screen,
			WA_Left, (ULONG)left,
			WA_Top, (ULONG)top,
			WA_Width, (ULONG)win_width,
			WA_Height, (ULONG)win_height,
			WA_Title, (ULONG)(config->title ? config->title :
			                  "ZZ9000 SDK Image"),
			WA_DragBar, TRUE,
			WA_DepthGadget, TRUE,
			WA_CloseGadget, config->close_gadget ? TRUE : FALSE,
			WA_SizeGadget, FALSE,
			WA_SmartRefresh, TRUE,
			WA_Activate, TRUE,
			WA_RMBTrap, TRUE,
			WA_IDCMP, idcmp,
			TAG_DONE);
	}
	if (!ui->window) {
		printf("zz9k-image-window: open window failed\n");
		zz9k_image_window_close(ui);
		return 0;
	}
	if (!zz9k_image_window_inner_rect(framebuffer, ui, &ui->inner)) {
		printf("zz9k-image-window: invalid window geometry\n");
		zz9k_image_window_close(ui);
		return 0;
	}
	Delay(5L);
	return 1;
#else
	if (!framebuffer || !config || !ui)
		return 0;
	memset(ui, 0, sizeof(*ui));
	ui->resizable = config->resizable;
	return zz9k_image_window_inner_rect(framebuffer, ui, &ui->inner);
#endif
}
#endif /* ZZ9K_IMAGE_WINDOW_NO_UI */

int zz9k_image_window_poll_event(ZZ9KImageWindow *ui,
                                 const ZZ9KSurface *framebuffer,
                                 ZZ9KImageWindowEvent *event)
{
#if ZZ9K_IMAGE_WINDOW_AMIGA
	struct IntuiMessage *msg;

	if (!ui || !ui->window || !framebuffer || !event)
		return 0;
	memset(event, 0, sizeof(*event));
	while ((msg = (struct IntuiMessage *)GetMsg(ui->window->UserPort)) != 0) {
		ULONG klass = msg->Class;
		uint32_t code = (uint32_t)msg->Code;

		if (klass == IDCMP_CLOSEWINDOW) {
			event->closed = 1;
		} else if (klass == IDCMP_REFRESHWINDOW) {
			event->changed = 1;
		} else if (ui->resizable && klass == IDCMP_NEWSIZE) {
			event->changed = 1;
		} else if (klass == IDCMP_VANILLAKEY) {
			event->vanilla_key = code;
		} else if (klass == IDCMP_RAWKEY && (code & 0x80U) == 0U) {
			event->raw_key = code & 0x7fU;
		}
		ReplyMsg((struct Message *)msg);
	}
	if (event->changed &&
	    !zz9k_image_window_inner_rect(framebuffer, ui, &ui->inner)) {
		return 0;
	}
	return 1;
#else
	if (!ui || !framebuffer || !event)
		return 0;
	(void)ui;
	(void)framebuffer;
	memset(event, 0, sizeof(*event));
	return 1;
#endif
}

int zz9k_image_window_poll(ZZ9KImageWindow *ui,
                           const ZZ9KSurface *framebuffer,
                           int *changed,
                           int *closed)
{
	ZZ9KImageWindowEvent event;

	if (!changed || !closed)
		return 0;
	if (!zz9k_image_window_poll_event(ui, framebuffer, &event))
		return 0;
	*changed = event.changed;
	*closed = event.closed;
	return 1;
}

void zz9k_image_window_set_title(ZZ9KImageWindow *ui, const char *title)
{
#if ZZ9K_IMAGE_WINDOW_AMIGA
	if (ui && ui->window && title) {
		SetWindowTitles(ui->window, (CONST_STRPTR)title, (CONST_STRPTR)-1);
	}
#else
	(void)ui;
	(void)title;
#endif
}

void zz9k_image_window_close(ZZ9KImageWindow *ui)
{
	if (!ui)
		return;
#if ZZ9K_IMAGE_WINDOW_AMIGA
	if (ui->window) {
		zz9k_image_window_drain_messages(ui->window);
		ModifyIDCMP(ui->window, 0L);
		CloseWindow(ui->window);
		ui->window = 0;
	}
	if (ui->screen) {
		UnlockPubScreen(NULL, ui->screen);
		ui->screen = 0;
	}
	if (IntuitionBase) {
		CloseLibrary((struct Library *)IntuitionBase);
		IntuitionBase = 0;
	}
#endif
	memset(&ui->inner, 0, sizeof(ui->inner));
	ui->resizable = 0;
}
