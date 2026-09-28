/*
 * Logic checks for zz9k-jpeg.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define ZZ9K_JPEG_NO_MAIN 1
#include "../tools/zz9k-jpeg.c"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	uint32_t width = 0;
	uint32_t height = 0;
	uint8_t invalid[8];

	if (!zz9k_jpeg_read_dimensions(jpeg_2x2, sizeof(jpeg_2x2),
	                               &width, &height)) {
		printf("failed to parse built-in JPEG dimensions\n");
		return 1;
	}
	if (width != 2U || height != 2U) {
		printf("unexpected built-in JPEG dimensions %lu x %lu\n",
		       (unsigned long)width, (unsigned long)height);
		return 2;
	}

	memset(invalid, 0, sizeof(invalid));
	width = 123U;
	height = 456U;
	if (zz9k_jpeg_read_dimensions(invalid, sizeof(invalid),
	                              &width, &height)) {
		printf("accepted invalid JPEG bytes\n");
		return 3;
	}
	if (width != 123U || height != 456U) {
		printf("modified outputs after invalid JPEG parse\n");
		return 4;
	}

	{
		ZZ9KImageSessionResult result;

		memset(&result, 0, sizeof(result));
		result.state = ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT;
		if (zz9k_jpeg_stream_result_made_progress(&result)) {
			printf("accepted no-progress stream result\n");
			return 5;
		}

		result.bytes_written = 128U;
		if (!zz9k_jpeg_stream_result_made_progress(&result)) {
			printf("rejected output-only stream progress\n");
			return 6;
		}

		result.bytes_written = 0U;
		result.bytes_consumed = 64U;
		if (!zz9k_jpeg_stream_result_made_progress(&result)) {
			printf("rejected input stream progress\n");
			return 7;
		}

		memset(&result, 0, sizeof(result));
		result.state = ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT;
		if (zz9k_jpeg_stream_no_progress_is_fatal(&result, 512U,
		                                          4096U, 0)) {
			printf("did not allow append after no-progress stream result\n");
			return 8;
		}
		if (!zz9k_jpeg_stream_no_progress_is_fatal(&result, 4096U,
		                                           4096U, 0)) {
			printf("did not reject full no-progress stream buffer\n");
			return 9;
		}
		if (!zz9k_jpeg_stream_no_progress_is_fatal(&result, 512U,
		                                           4096U, 1)) {
			printf("did not reject eof no-progress stream result\n");
			return 10;
		}
	}

	{
		const char *path = "zz9k-jpeg-fit-test.jpg";
		FILE *file;
		ZZ9KJpegInput input;
		char *fit_argv[] = {
			"zz9k-jpeg", "--fb", "--fit", (char *)path
		};
		char *keep_argv[] = {
			"zz9k-jpeg", "--fb", "--keep", (char *)path
		};
		char *hold_argv[] = {
			"zz9k-jpeg", "--fb", "--hold", "7", (char *)path
		};
		char *resize_argv[] = {
			"zz9k-jpeg", "--fb", "--resize", "--hold", "7",
			(char *)path
		};
		char *resize_nearest_argv[] = {
			"zz9k-jpeg", "--fb", "--resize", "--nearest",
			(char *)path
		};
		char *resize_bilinear_argv[] = {
			"zz9k-jpeg", "--fb", "--resize", "--bilinear",
			(char *)path
		};
		char *view_argv[] = {
			"zz9k-jpeg", "--view", (char *)path
		};
		char *removed_shared_decode_argv[] = {
			"zz9k-jpeg", "--fb", "--resize", "--shared-decode",
			(char *)path
		};
		char *removed_top_pad_argv[] = {
			"zz9k-jpeg", "--fb", "--resize", "--top-pad", "4",
			(char *)path
		};
		char *missing_fb_argv[] = {
			"zz9k-jpeg", "--fit", (char *)path
		};
		char *resize_missing_fb_argv[] = {
			"zz9k-jpeg", "--resize", (char *)path
		};
		char *built_in_fit_argv[] = {
			"zz9k-jpeg", "--fb", "--fit"
		};

		file = fopen(path, "wb");
		if (!file)
			return 11;
		if (fwrite(jpeg_2x2, 1U, sizeof(jpeg_2x2), file) !=
		    sizeof(jpeg_2x2)) {
			fclose(file);
			remove(path);
			return 12;
		}
		fclose(file);

		if (!zz9k_jpeg_parse_args(4, fit_argv, &input)) {
			remove(path);
			printf("rejected valid --fb --fit JPEG file args\n");
			return 13;
		}
		if (!input.use_framebuffer || !input.fit_framebuffer ||
		    !input.restore_framebuffer ||
		    input.resize_framebuffer ||
		    input.framebuffer_hold_ticks !=
		    ZZ9K_JPEG_DEFAULT_FRAMEBUFFER_HOLD_TICKS ||
		    input.built_in || input.width != 2U || input.height != 2U) {
			remove(path);
			printf("did not record --fb --fit JPEG file args\n");
			return 14;
		}
		if (!zz9k_jpeg_parse_args(4, keep_argv, &input)) {
			remove(path);
			printf("rejected valid --fb --keep JPEG file args\n");
			return 17;
		}
		if (input.restore_framebuffer) {
			remove(path);
			printf("did not record --keep JPEG file args\n");
			return 18;
		}
		if (!zz9k_jpeg_parse_args(5, hold_argv, &input)) {
			remove(path);
			printf("rejected valid --fb --hold JPEG file args\n");
			return 19;
		}
		if (!input.restore_framebuffer ||
		    input.resize_framebuffer ||
		    input.framebuffer_hold_ticks != 7U) {
			remove(path);
			printf("did not record --hold JPEG file args\n");
			return 20;
		}
		if (!zz9k_jpeg_parse_args(6, resize_argv, &input)) {
			remove(path);
			printf("rejected valid --fb --resize JPEG file args\n");
			return 34;
		}
		if (!input.use_framebuffer || !input.resize_framebuffer ||
		    !input.restore_framebuffer ||
		    input.scale_filter_override != ZZ9K_JPEG_SCALE_FILTER_AUTO ||
		    input.framebuffer_hold_ticks != 7U) {
			remove(path);
			printf("did not record --fb --resize JPEG file args\n");
			return 35;
		}
		if (!zz9k_jpeg_parse_args(5, resize_nearest_argv, &input)) {
			remove(path);
			printf("rejected valid --fb --resize --nearest args\n");
			return 41;
		}
		if (input.scale_filter_override != ZZ9K_SCALE_NEAREST) {
			remove(path);
			printf("did not record JPEG nearest scale override\n");
			return 42;
		}
		if (!zz9k_jpeg_parse_args(5, resize_bilinear_argv, &input)) {
			remove(path);
			printf("rejected valid --fb --resize --bilinear args\n");
			return 43;
		}
		if (input.scale_filter_override != ZZ9K_SCALE_BILINEAR) {
			remove(path);
			printf("did not record JPEG bilinear scale override\n");
			return 44;
		}
		if (!zz9k_jpeg_parse_args(3, view_argv, &input)) {
			remove(path);
			printf("rejected valid JPEG --view args\n");
			return 48;
		}
		if (!input.use_framebuffer || !input.resize_framebuffer ||
		    !input.view_framebuffer || !input.restore_framebuffer ||
		    input.fit_framebuffer || input.built_in) {
			remove(path);
			printf("did not record JPEG --view args\n");
			return 49;
		}
		if (zz9k_jpeg_parse_args(5, removed_shared_decode_argv, &input)) {
			remove(path);
			printf("accepted removed --shared-decode diagnostic arg\n");
			return 45;
		}
		if (zz9k_jpeg_parse_args(6, removed_top_pad_argv, &input)) {
			remove(path);
			printf("accepted removed --top-pad diagnostic arg\n");
			return 46;
		}
		if (zz9k_jpeg_parse_args(3, missing_fb_argv, &input)) {
			remove(path);
			printf("accepted --fit without --fb\n");
			return 15;
		}
		if (zz9k_jpeg_parse_args(3, resize_missing_fb_argv, &input)) {
			remove(path);
			printf("accepted --resize without --fb\n");
			return 36;
		}
		if (zz9k_jpeg_parse_args(3, built_in_fit_argv, &input)) {
			remove(path);
			printf("accepted --fit without JPEG file\n");
			return 16;
		}
		remove(path);
	}

	{
		ZZ9KSurface framebuffer;
		ZZ9KSurface backup_surface;
		ZZ9KJpegInput input;
		ZZ9KFbRect rect;
		ZZ9KSurfaceCopyDesc save_copy;
		ZZ9KSurfaceCopyDesc restore_copy;

		memset(&framebuffer, 0, sizeof(framebuffer));
		framebuffer.width = 640U;
		framebuffer.height = 480U;
		framebuffer.pitch = 640U * 4U;
		framebuffer.length = framebuffer.pitch * framebuffer.height;
		framebuffer.format = ZZ9K_SURFACE_FORMAT_BGRA8888;

		memset(&input, 0, sizeof(input));
		input.width = 500U;
		input.height = 400U;
		if (!zz9k_jpeg_choose_framebuffer_rect(&framebuffer, &input, &rect) ||
		    rect.w != 500U || rect.h != 400U) {
			printf("did not choose exact non-fit restore rectangle\n");
			return 21;
		}

		input.width = 800U;
		input.height = 600U;
		if (zz9k_jpeg_choose_framebuffer_rect(&framebuffer, &input, &rect)) {
			printf("accepted oversized non-fit restore rectangle\n");
			return 22;
		}

		input.width = 2000U;
		input.height = 1000U;
		input.fit_framebuffer = 1;
		if (!zz9k_jpeg_choose_framebuffer_rect(&framebuffer, &input, &rect) ||
		    rect.w != 640U || rect.h != 320U) {
			printf("did not choose fitted restore rectangle\n");
			return 23;
		}

		input.width = 100U;
		input.height = 50U;
		input.fit_framebuffer = 1;
		if (!zz9k_jpeg_choose_framebuffer_rect(&framebuffer, &input, &rect) ||
		    rect.w != 100U || rect.h != 50U) {
			printf("upscaled small fit restore rectangle\n");
			return 24;
		}

		{
			ZZ9KFbRect area;
			uint32_t decode_width;
			uint32_t decode_height;
			uint32_t left;
			uint32_t top;
			ZZ9KScaleImageDesc scale;
			ZZ9KScaleImageClippedDesc clipped;
			ZZ9KFbRect clip;

			area.x = 50U;
			area.y = 40U;
			area.w = 240U;
			area.h = 120U;
			if (zz9k_jpeg_count_scale_slices(&area) != 2U) {
				printf("did not use larger JPEG window scale slices\n");
				return 47;
			}

			input.width = 500U;
			input.height = 400U;
			input.fit_framebuffer = 1;
			if (!zz9k_jpeg_choose_draw_rect_in_area(&area, 500U, 400U,
			                                        &rect) ||
			    rect.x != 95U || rect.y != 40U ||
			    rect.w != 150U || rect.h != 120U) {
				printf("did not fit JPEG draw rectangle inside window area\n");
				return 28;
			}
			if (!zz9k_jpeg_rect_contains(&area, &rect) ||
			    zz9k_jpeg_scale_needs_clip(&area, &rect)) {
				printf("full JPEG window redraw unexpectedly needs clipped scale\n");
				return 40;
			}

			if (!zz9k_jpeg_choose_window_origin(1280U, 720U, 580U, 500U,
			                                    &left, &top) ||
			    left != 350U || top != 110U) {
				printf("did not choose adjusted centered JPEG window origin\n");
				return 33;
			}
			if (!zz9k_jpeg_choose_window_max_extent(1280U, 720U,
			                                        left, top,
			                                        180U, 120U,
			                                        &decode_width,
			                                        &decode_height) ||
			    decode_width != 930U || decode_height != 610U) {
				printf("did not choose safe JPEG window max extent\n");
				return 37;
			}
			if (!zz9k_jpeg_choose_draw_rect_in_area(&area, 100U, 50U,
			                                        &rect) ||
			    rect.x != 50U || rect.y != 40U ||
			    rect.w != 240U || rect.h != 120U) {
				printf("did not upscale JPEG draw rectangle to window area\n");
				return 29;
			}

			input.width = 4332U;
			input.height = 3295U;
			if (!zz9k_jpeg_choose_decode_surface_size(&input, 1280U, 720U,
			                                          &decode_width,
			                                          &decode_height) ||
			    decode_width != 946U || decode_height != 720U) {
				printf("did not choose bounded ARM decode surface size\n");
				return 30;
			}

			input.width = 500U;
			input.height = 500U;
			if (!zz9k_jpeg_choose_decode_surface_size(&input, 1280U, 720U,
			                                          &decode_width,
			                                          &decode_height) ||
			    decode_width != 500U || decode_height != 500U) {
				printf("scaled a JPEG that already fits the framebuffer\n");
				return 31;
			}

			memset(&scale, 0, sizeof(scale));
			zz9k_jpeg_build_framebuffer_scale_desc(
				&scale, 0x40000023UL, 500U, 400U, &area,
				ZZ9K_SCALE_BILINEAR);
			if (scale.src_surface != 0x40000023UL ||
			    scale.dst_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER ||
			    scale.src_x != 0U || scale.src_y != 0U ||
			    scale.src_w != 500U || scale.src_h != 400U ||
			    scale.dst_x != 95U || scale.dst_y != 40U ||
			    scale.dst_w != 150U || scale.dst_h != 120U ||
			    scale.filter != ZZ9K_SCALE_BILINEAR ||
			    scale.flags != 0U) {
				printf("incorrect JPEG framebuffer scale descriptor\n");
				return 32;
			}

			clip.x = 80U;
			clip.y = 60U;
			clip.w = 100U;
			clip.h = 50U;
			if (zz9k_jpeg_rect_contains(&clip, &rect) ||
			    !zz9k_jpeg_scale_needs_clip(&clip, &rect)) {
				printf("partial JPEG damage redraw did not require clipped scale\n");
				return 41;
			}
			memset(&clipped, 0, sizeof(clipped));
			if (!zz9k_jpeg_build_framebuffer_clipped_scale_desc(
				    &clipped, 0x40000023UL, 500U, 400U,
				    &area, &clip, ZZ9K_SCALE_BILINEAR)) {
				printf("did not build JPEG clipped scale descriptor\n");
				return 38;
			}
			if (clipped.src_surface != 0x40000023UL ||
			    clipped.dst_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER ||
			    clipped.src_w != 500U || clipped.src_h != 400U ||
			    clipped.dst_x != 95U || clipped.dst_y != 40U ||
			    clipped.dst_w != 150U || clipped.dst_h != 120U ||
			    clipped.clip_x != 80U || clipped.clip_y != 60U ||
			    clipped.clip_w != 100U || clipped.clip_h != 50U ||
			    clipped.filter != ZZ9K_SCALE_BILINEAR ||
			    clipped.flags != 0U) {
				printf("incorrect JPEG clipped scale descriptor\n");
				return 39;
			}
		}

		memset(&backup_surface, 0, sizeof(backup_surface));
		backup_surface.handle = 123U;
		rect.x = 12U;
		rect.y = 34U;
		rect.w = 56U;
		rect.h = 78U;
		if (!zz9k_jpeg_make_framebuffer_backup_copy_descs(
			    &backup_surface, &rect, &save_copy, &restore_copy)) {
			printf("did not build framebuffer backup copy descriptors\n");
			return 25;
		}
		if (save_copy.src_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER ||
		    save_copy.dst_surface != 123U ||
		    save_copy.src_x != 12U || save_copy.src_y != 34U ||
		    save_copy.dst_x != 0U || save_copy.dst_y != 0U ||
		    save_copy.width != 56U || save_copy.height != 78U) {
			printf("incorrect framebuffer save copy descriptor\n");
			return 26;
		}
		if (restore_copy.src_surface != 123U ||
		    restore_copy.dst_surface != ZZ9K_SURFACE_HANDLE_FRAMEBUFFER ||
		    restore_copy.src_x != 0U || restore_copy.src_y != 0U ||
		    restore_copy.dst_x != 12U || restore_copy.dst_y != 34U ||
		    restore_copy.width != 56U || restore_copy.height != 78U) {
			printf("incorrect framebuffer restore copy descriptor\n");
			return 27;
		}
	}

	return 0;
}
