/*
 * Logic checks for shared ZZ9000 SDK image-window helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-image-window.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int zz9k_image_window_visible_clips(const ZZ9KImageWindow *ui,
                                           const ZZ9KFbRect *damage_rect,
                                           ZZ9KFbRect *clips,
                                           uint32_t clip_capacity,
                                           uint32_t *clip_count);

int main(void)
{
	ZZ9KFbRect area;
	ZZ9KFbRect rect;
	ZZ9KScaleImageDesc scale;
	ZZ9KScaleImageClippedDesc clipped;
	ZZ9KSurfaceFillDesc fill;
	ZZ9KFbRect visible[4];
	ZZ9KFbRect clips[3];
	uint32_t clip_count;
	uint32_t width;
	uint32_t height;
	uint32_t left;
	uint32_t top;
	uint32_t max_width;
	uint32_t max_height;
	uint32_t window_width;
	uint32_t window_height;
	ZZ9KImageWindowConfig config;
	ZZ9KImageWindow window;
	ZZ9KSurface framebuffer;

	if (!zz9k_image_window_fit_size_to_area(720U, 960U, 1280U, 720U,
	                                        &width, &height) ||
	    width != 540U || height != 720U) {
		printf("did not fit portrait image to framebuffer area\n");
		return 1;
	}

	area.x = 50U;
	area.y = 40U;
	area.w = 240U;
	area.h = 120U;
	if (zz9k_image_window_count_scale_slices(&area) != 2U) {
		printf("did not use shared 96-row image window slices\n");
		return 2;
	}
	if (!zz9k_image_window_choose_draw_rect_in_area(&area, 720U, 960U,
	                                               &rect) ||
	    rect.x != 125U || rect.y != 40U ||
	    rect.w != 90U || rect.h != 120U) {
		printf("did not choose centered image window draw rectangle\n");
		return 3;
	}

	memset(&clipped, 0, sizeof(clipped));
	if (!zz9k_image_window_build_clipped_scale_desc(
		    &clipped, 0x40000055UL, 720U, 960U,
		    &rect, &area, ZZ9K_SCALE_BILINEAR)) {
		printf("did not build shared clipped scale descriptor\n");
		return 4;
	}
	if (clipped.src_surface != 0x40000055UL ||
	    clipped.dst_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER ||
	    clipped.src_x != 0U || clipped.src_y != 0U ||
	    clipped.src_w != 720U || clipped.src_h != 960U ||
	    clipped.dst_x != 125U || clipped.dst_y != 40U ||
	    clipped.dst_w != 90U || clipped.dst_h != 120U ||
	    clipped.clip_x != 50U || clipped.clip_y != 40U ||
	    clipped.clip_w != 240U || clipped.clip_h != 120U ||
	    clipped.filter != ZZ9K_SCALE_BILINEAR ||
	    clipped.flags != 0U) {
		printf("incorrect shared clipped scale descriptor\n");
		return 5;
	}

	memset(&scale, 0xff, sizeof(scale));
	if (!zz9k_image_window_build_scale_desc(
		    &scale, 0x40000055UL, 720U, 960U, &rect,
		    ZZ9K_SCALE_NEAREST)) {
		printf("did not build shared framebuffer scale descriptor\n");
		return 16;
	}
	if (scale.src_surface != 0x40000055UL ||
	    scale.dst_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER ||
	    scale.src_x != 0U || scale.src_y != 0U ||
	    scale.src_w != 720U || scale.src_h != 960U ||
	    scale.dst_x != 125U || scale.dst_y != 40U ||
	    scale.dst_w != 90U || scale.dst_h != 120U ||
	    scale.filter != ZZ9K_SCALE_NEAREST ||
	    scale.flags != 0U) {
		printf("incorrect shared framebuffer scale descriptor\n");
		return 17;
	}

	memset(&fill, 0xa5, sizeof(fill));
	if (!zz9k_image_window_build_framebuffer_fill_desc(
		    &fill, &area, 0xff112233UL, 0U)) {
		printf("did not build shared framebuffer fill descriptor\n");
		return 13;
	}
	if (fill.surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER ||
	    fill.x != 50U || fill.y != 40U ||
	    fill.width != 240U || fill.height != 120U ||
	    fill.color != 0xff112233UL || fill.flags != 0U) {
		printf("incorrect shared framebuffer fill descriptor\n");
		return 14;
	}
	rect = area;
	rect.w = 0U;
	if (zz9k_image_window_build_framebuffer_fill_desc(
		    &fill, &rect, 0xff112233UL, 0U)) {
		printf("accepted empty shared framebuffer fill descriptor\n");
		return 15;
	}

	rect.x = 10U;
	rect.y = 20U;
	rect.w = 100U;
	rect.h = 50U;
	area.x = 70U;
	area.y = 40U;
	area.w = 80U;
	area.h = 80U;
	if (!zz9k_image_window_intersect_rect(&rect, &area, &clips[0]) ||
	    clips[0].x != 70U || clips[0].y != 40U ||
	    clips[0].w != 40U || clips[0].h != 30U) {
		printf("did not intersect image window clip rectangles\n");
		return 18;
	}
	area.x = 110U;
	area.y = 20U;
	area.w = 10U;
	area.h = 10U;
	if (zz9k_image_window_intersect_rect(&rect, &area, &clips[0])) {
		printf("accepted edge-touching image window clip rectangles\n");
		return 19;
	}

	visible[0].x = 10U;
	visible[0].y = 10U;
	visible[0].w = 100U;
	visible[0].h = 40U;
	visible[1].x = 120U;
	visible[1].y = 10U;
	visible[1].w = 20U;
	visible[1].h = 40U;
	visible[2].x = 0U;
	visible[2].y = 80U;
	visible[2].w = 90U;
	visible[2].h = 30U;
	visible[3].x = 250U;
	visible[3].y = 0U;
	visible[3].w = 10U;
	visible[3].h = 10U;
	area.x = 50U;
	area.y = 0U;
	area.w = 100U;
	area.h = 100U;
	memset(clips, 0, sizeof(clips));
	clip_count = 0U;
	if (!zz9k_image_window_build_damage_clips(
		    visible, 4U, &area, clips, 3U, &clip_count) ||
	    clip_count != 3U ||
	    clips[0].x != 50U || clips[0].y != 10U ||
	    clips[0].w != 60U || clips[0].h != 40U ||
	    clips[1].x != 120U || clips[1].y != 10U ||
	    clips[1].w != 20U || clips[1].h != 40U ||
	    clips[2].x != 50U || clips[2].y != 80U ||
	    clips[2].w != 40U || clips[2].h != 20U) {
		printf("did not build image window damage clip list\n");
		return 20;
	}
	if (zz9k_image_window_build_damage_clips(
		    visible, 4U, &area, clips, 2U, &clip_count)) {
		printf("accepted undersized image window damage clip list\n");
		return 21;
	}
	area.x = 300U;
	area.y = 300U;
	area.w = 10U;
	area.h = 10U;
	clip_count = 42U;
	if (!zz9k_image_window_build_damage_clips(
		    visible, 4U, &area, clips, 3U, &clip_count) ||
	    clip_count != 0U) {
		printf("did not return empty image window damage clip list\n");
		return 22;
	}

	if (!zz9k_image_window_choose_origin(1280U, 720U, 580U, 500U,
	                                     &left, &top) ||
	    left != 350U || top != 110U) {
		printf("did not choose shared centered window origin\n");
		return 6;
	}
	if (!zz9k_image_window_choose_max_extent(1280U, 720U, left, top,
	                                         180U, 120U, &max_width,
	                                         &max_height) ||
	    max_width != 930U || max_height != 610U) {
		printf("did not choose shared window max extent\n");
		return 7;
	}

	memset(&framebuffer, 0, sizeof(framebuffer));
	framebuffer.width = 1280U;
	framebuffer.height = 720U;
	framebuffer.pitch = 1280U * 4U;
	framebuffer.length = framebuffer.pitch * framebuffer.height;
	framebuffer.format = ZZ9K_SURFACE_FORMAT_BGRA8888;

	zz9k_image_window_config_init(&config, "ZZ9000 SDK Test",
	                              500U, 400U, 0, 0,
	                              180U, 120U, 32U, 48U);
	if (!zz9k_image_window_choose_size(&framebuffer, &config,
	                                   &window_width, &window_height) ||
	    window_width != 580U || window_height != 490U) {
		printf("did not choose small image window size\n");
		return 8;
	}

	zz9k_image_window_config_init(&config, "ZZ9000 SDK Test",
	                              720U, 960U, 1, 1,
	                              180U, 120U, 32U, 48U);
	if (!zz9k_image_window_choose_size(&framebuffer, &config,
	                                   &window_width, &window_height) ||
	    window_width != 1248U || window_height != 672U) {
		printf("did not clamp oversized image window size\n");
		return 9;
	}

	memset(&window, 0, sizeof(window));
	if (!zz9k_image_window_open(&framebuffer, &config, &window) ||
	    !window.resizable || window.inner.x != 0U || window.inner.y != 0U ||
	    window.inner.w != 1280U || window.inner.h != 720U) {
		printf("did not open native fallback image window\n");
		return 10;
	}
	{
		int changed = 1;
		int closed = 1;

		if (!zz9k_image_window_poll(&window, &framebuffer,
		                            &changed, &closed) ||
		    changed || closed) {
			printf("did not poll native fallback image window\n");
			return 11;
		}
	}
	area.x = 100U;
	area.y = 90U;
	area.w = 300U;
	area.h = 200U;
	memset(clips, 0, sizeof(clips));
	clip_count = 0U;
	if (!zz9k_image_window_visible_clips(
		    &window, &area, clips, 3U, &clip_count) ||
	    clip_count != 1U ||
	    clips[0].x != 100U || clips[0].y != 90U ||
	    clips[0].w != 300U || clips[0].h != 200U) {
		printf("did not build native fallback visible window clip\n");
		return 23;
	}
	if (!zz9k_image_window_visible_clips_for_window(
		    0, &area, clips, 3U, &clip_count) ||
	    clip_count != 1U ||
	    clips[0].x != 100U || clips[0].y != 90U ||
	    clips[0].w != 300U || clips[0].h != 200U) {
		printf("did not build native fallback generic window clip\n");
		return 24;
	}
	if (zz9k_image_window_visible_clips(
		    &window, &area, clips, 0U, &clip_count)) {
		printf("accepted undersized native fallback visible window clip\n");
		return 25;
	}
	if (zz9k_image_window_loop_done(0, 0U, 2U, 0) ||
	    zz9k_image_window_loop_done(0, 1U, 2U, 0) ||
	    !zz9k_image_window_loop_done(0, 2U, 2U, 0) ||
	    !zz9k_image_window_loop_done(0, 0U, 2U, 1)) {
		printf("did not stop fixed-duration image window loop correctly\n");
		return 26;
	}
	if (zz9k_image_window_loop_done(1, 0xffffffffUL, 2U, 0) ||
	    !zz9k_image_window_loop_done(1, 0U, 2U, 1)) {
		printf("did not keep viewer image window loop open until close\n");
		return 27;
	}
	zz9k_image_window_close(&window);
	if (window.inner.w != 0U || window.inner.h != 0U || window.resizable) {
		printf("did not close native fallback image window\n");
		return 12;
	}

	return 0;
}
