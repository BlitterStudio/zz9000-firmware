/*
 * Logic checks for zz9k-png.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define ZZ9K_PNG_NO_MAIN 1
#include "../tools/zz9k-png.c"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint8_t png_header_2x3[] = {
	0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
	0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
	0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03,
	0x08, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00
};

int main(void)
{
	ZZ9KPngHeader header;
	uint8_t invalid[sizeof(png_header_2x3)];

	memset(&header, 0, sizeof(header));
	if (!zz9k_png_read_header(png_header_2x3, sizeof(png_header_2x3),
	                          &header)) {
		printf("failed to parse PNG header\n");
		return 1;
	}
	if (header.width != 2U || header.height != 3U ||
	    header.bit_depth != 8U || header.color_type != 6U ||
	    header.compression != 0U || header.filter != 0U ||
	    header.interlace != 0U) {
		printf("unexpected PNG header fields\n");
		return 2;
	}
	if (!zz9k_png_header_supported(&header)) {
		printf("rejected supported PNG header\n");
		return 3;
	}

	header.interlace = 1U;
	if (zz9k_png_header_supported(&header)) {
		printf("accepted interlaced PNG header\n");
		return 4;
	}

	memset(invalid, 0, sizeof(invalid));
	header.width = 123U;
	header.height = 456U;
	if (zz9k_png_read_header(invalid, sizeof(invalid), &header)) {
		printf("accepted invalid PNG bytes\n");
		return 5;
	}
	if (header.width != 123U || header.height != 456U) {
		printf("modified header after invalid PNG parse\n");
		return 6;
	}
	{
		const char *path = "zz9k-png-parse-test.png";
		FILE *file;
		ZZ9KPngInput input;
		char *fb_hold_argv[] = {
			"zz9k-png", "--fb", "--hold", "7", (char *)path
		};
		char *keep_argv[] = {
			"zz9k-png", "--fb", "--keep", (char *)path
		};
		char *fb_fit_argv[] = {
			"zz9k-png", "--fb", "--fit", (char *)path
		};
		char *fb_window_argv[] = {
			"zz9k-png", "--fb", "--window", (char *)path
		};
		char *fb_resize_argv[] = {
			"zz9k-png", "--fb", "--resize", (char *)path
		};
		char *view_argv[] = {
			"zz9k-png", "--view", (char *)path
		};
		char *surface_argv[] = {
			"zz9k-png", (char *)path
		};
		char *trace_argv[] = {
			"zz9k-png", "--trace", "--stop-after", "3",
			(char *)path
		};
		char *chunk_argv[] = {
			"zz9k-png", "--trace", "--chunk", "33",
			"--stop-after", "5", (char *)path
		};
		char *bad_stop_argv[] = {
			"zz9k-png", "--stop-after", "0", (char *)path
		};
		char *bad_chunk_argv[] = {
			"zz9k-png", "--chunk", "0", (char *)path
		};
		char *bad_fit_argv[] = {
			"zz9k-png", "--fit", (char *)path
		};
		char *bad_window_argv[] = {
			"zz9k-png", "--window", (char *)path
		};
		char *bad_resize_argv[] = {
			"zz9k-png", "--resize", (char *)path
		};
		char *missing_file_argv[] = {
			"zz9k-png", "--fb"
		};

		file = fopen(path, "wb");
		if (!file)
			return 7;
		if (fwrite(png_header_2x3, 1U, sizeof(png_header_2x3), file) !=
		    sizeof(png_header_2x3)) {
			fclose(file);
			remove(path);
			return 8;
		}
		fclose(file);

		if (!zz9k_png_parse_args(5, fb_hold_argv, &input)) {
			remove(path);
			printf("rejected valid --fb --hold PNG file args\n");
			return 9;
		}
		if (!input.use_framebuffer || !input.restore_framebuffer ||
		    input.hold_ticks != 7U || input.width != 2U ||
		    input.height != 3U || strcmp(input.path, path) != 0) {
			remove(path);
			printf("did not record --fb --hold PNG file args\n");
			return 10;
		}

		if (!zz9k_png_parse_args(4, keep_argv, &input)) {
			remove(path);
			printf("rejected valid --keep PNG file args\n");
			return 11;
		}
		if (!input.use_framebuffer || input.restore_framebuffer) {
			remove(path);
			printf("did not record --keep PNG file args\n");
			return 12;
		}
		if (!zz9k_png_parse_args(4, fb_fit_argv, &input)) {
			remove(path);
			printf("rejected valid --fb --fit PNG file args\n");
			return 24;
		}
		if (!input.use_framebuffer || !input.fit_framebuffer ||
		    !input.restore_framebuffer) {
			remove(path);
			printf("did not record --fb --fit PNG file args\n");
			return 25;
		}
		if (zz9k_png_parse_args(3, bad_fit_argv, &input)) {
			remove(path);
			printf("accepted --fit without --fb\n");
			return 26;
		}
		if (!zz9k_png_parse_args(4, fb_window_argv, &input)) {
			remove(path);
			printf("rejected valid --fb --window PNG file args\n");
			return 32;
		}
		if (!input.use_framebuffer || !input.window_framebuffer ||
		    input.resize_framebuffer) {
			remove(path);
			printf("did not record --fb --window PNG file args\n");
			return 33;
		}
		if (!zz9k_png_parse_args(4, fb_resize_argv, &input)) {
			remove(path);
			printf("rejected valid --fb --resize PNG file args\n");
			return 34;
		}
		if (!input.use_framebuffer || !input.window_framebuffer ||
		    !input.resize_framebuffer) {
			remove(path);
			printf("did not record --fb --resize PNG file args\n");
			return 35;
		}
		if (zz9k_png_parse_args(3, bad_window_argv, &input)) {
			remove(path);
			printf("accepted --window without --fb\n");
			return 36;
		}
		if (zz9k_png_parse_args(3, bad_resize_argv, &input)) {
			remove(path);
			printf("accepted --resize without --fb\n");
			return 37;
		}
		if (!zz9k_png_parse_args(3, view_argv, &input)) {
			remove(path);
			printf("rejected valid PNG --view args\n");
			return 44;
		}
		if (!input.use_framebuffer || !input.window_framebuffer ||
		    !input.resize_framebuffer || !input.view_framebuffer ||
		    !input.restore_framebuffer || input.fit_framebuffer) {
			remove(path);
			printf("did not record PNG --view args\n");
			return 45;
		}

		if (!zz9k_png_parse_args(2, surface_argv, &input)) {
			remove(path);
			printf("rejected valid PNG surface args\n");
			return 13;
		}
		if (input.use_framebuffer || !input.restore_framebuffer ||
		    input.width != 2U || input.height != 3U ||
		    input.chunk_bytes != ZZ9K_PNG_DEFAULT_CHUNK_BYTES) {
			remove(path);
			printf("did not record PNG surface args\n");
			return 14;
		}
		if (!zz9k_png_parse_args(5, trace_argv, &input)) {
			remove(path);
			printf("rejected valid PNG trace args\n");
			return 18;
		}
		if (!input.trace || input.stop_after_step != 3U ||
		    !zz9k_png_should_stop_after(&input, 3U) ||
		    zz9k_png_should_stop_after(&input, 2U)) {
			remove(path);
			printf("did not record PNG trace args\n");
			return 19;
		}
		if (zz9k_png_parse_args(4, bad_stop_argv, &input)) {
			remove(path);
			printf("accepted invalid --stop-after value\n");
			return 20;
		}
		if (!zz9k_png_parse_args(7, chunk_argv, &input)) {
			remove(path);
			printf("rejected valid PNG chunk trace args\n");
			return 21;
		}
		if (!input.trace || input.chunk_bytes != 33U ||
		    input.stop_after_step != 5U) {
			remove(path);
			printf("did not record PNG chunk trace args\n");
			return 22;
		}
		if (zz9k_png_parse_args(4, bad_chunk_argv, &input)) {
			remove(path);
			printf("accepted invalid --chunk value\n");
			return 23;
		}

		if (zz9k_png_parse_args(2, missing_file_argv, &input)) {
			remove(path);
			printf("accepted PNG args without a file\n");
			return 15;
		}

		remove(path);
	}
	{
		ZZ9KSurface framebuffer;
		ZZ9KPngInput input;
		ZZ9KFbRect rect;
		ZZ9KFbRect area;
		ZZ9KScaleImageClippedDesc clipped;
		ZZ9KScaleImageDesc scale;
		uint32_t left;
		uint32_t top;
		uint32_t max_width;
		uint32_t max_height;

		memset(&framebuffer, 0, sizeof(framebuffer));
		framebuffer.width = 1280U;
		framebuffer.height = 720U;
		framebuffer.pitch = 1280U * 4U;
		framebuffer.length = framebuffer.pitch * framebuffer.height;
		framebuffer.format = ZZ9K_SURFACE_FORMAT_BGRA8888;

		memset(&input, 0, sizeof(input));
		input.width = 720U;
		input.height = 960U;
		input.fit_framebuffer = 1;
		if (!zz9k_png_choose_framebuffer_rect(&framebuffer, &input, &rect) ||
		    rect.x != 370U || rect.y != 0U ||
		    rect.w != 540U || rect.h != 720U) {
			printf("did not choose centered fitted PNG framebuffer rect\n");
			return 27;
		}

		input.width = 500U;
		input.height = 500U;
		if (!zz9k_png_choose_framebuffer_rect(&framebuffer, &input, &rect) ||
		    rect.x != 390U || rect.y != 110U ||
		    rect.w != 500U || rect.h != 500U) {
			printf("did not center already-fitting PNG framebuffer rect\n");
			return 28;
		}

		input.width = 1290U;
		input.height = 720U;
		input.fit_framebuffer = 0;
		if (zz9k_png_choose_framebuffer_rect(&framebuffer, &input, &rect)) {
			printf("accepted oversized non-fit PNG framebuffer rect\n");
			return 29;
		}

		rect.x = 370U;
		rect.y = 0U;
		rect.w = 540U;
		rect.h = 720U;
		memset(&scale, 0, sizeof(scale));
		if (!zz9k_png_build_framebuffer_scale_desc(
			    &scale, 0x40000055UL, 720U, 960U, &rect)) {
			printf("did not build PNG framebuffer scale descriptor\n");
			return 30;
		}
		if (scale.src_surface != 0x40000055UL ||
		    scale.dst_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER ||
		    scale.src_x != 0U || scale.src_y != 0U ||
		    scale.src_w != 720U || scale.src_h != 960U ||
		    scale.dst_x != rect.x || scale.dst_y != rect.y ||
		    scale.dst_w != rect.w || scale.dst_h != rect.h ||
		    scale.filter != ZZ9K_SCALE_BILINEAR || scale.flags != 0U) {
			printf("incorrect PNG framebuffer scale descriptor\n");
			return 31;
		}

		area.x = 50U;
		area.y = 40U;
		area.w = 240U;
		area.h = 120U;
		if (zz9k_png_count_scale_slices(&area) != 2U) {
			printf("did not use larger PNG window scale slices\n");
			return 38;
		}
		if (!zz9k_png_choose_draw_rect_in_area(&area, 720U, 960U,
		                                       &rect) ||
		    rect.x != 125U || rect.y != 40U ||
		    rect.w != 90U || rect.h != 120U) {
			printf("did not fit PNG draw rectangle inside window area\n");
			return 39;
		}
		if (!zz9k_png_build_framebuffer_clipped_scale_desc_for_rect(
			    &clipped, 0x40000055UL, 720U, 960U,
			    &rect, &area)) {
			printf("did not build PNG clipped scale descriptor\n");
			return 40;
		}
		if (clipped.src_surface != 0x40000055UL ||
		    clipped.dst_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER ||
		    clipped.src_w != 720U || clipped.src_h != 960U ||
		    clipped.dst_x != 125U || clipped.dst_y != 40U ||
		    clipped.dst_w != 90U || clipped.dst_h != 120U ||
		    clipped.clip_x != 50U || clipped.clip_y != 40U ||
		    clipped.clip_w != 240U || clipped.clip_h != 120U ||
		    clipped.filter != ZZ9K_SCALE_BILINEAR ||
		    clipped.flags != 0U) {
			printf("incorrect PNG clipped scale descriptor\n");
			return 41;
		}
		if (!zz9k_png_choose_window_origin(1280U, 720U, 580U, 500U,
		                                   &left, &top) ||
		    left != 350U || top != 110U) {
			printf("did not choose centered PNG window origin\n");
			return 42;
		}
		if (!zz9k_png_choose_window_max_extent(1280U, 720U,
		                                       left, top,
		                                       180U, 120U,
		                                       &max_width,
		                                       &max_height) ||
		    max_width != 930U || max_height != 610U) {
			printf("did not choose safe PNG window max extent\n");
			return 43;
		}
	}

	return 0;
}
