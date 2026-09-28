/*
 * Logic checks for shared zz9k-view picture viewer helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "../tools/zz9k-picture-viewer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path)
{
	FILE *file;
	long length;
	char *data;

	file = fopen(path, "rb");
	if (!file)
		return 0;
	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return 0;
	}
	length = ftell(file);
	if (length < 0) {
		fclose(file);
		return 0;
	}
	if (fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return 0;
	}

	data = (char *)malloc((size_t)length + 1U);
	if (!data) {
		fclose(file);
		return 0;
	}
	if (fread(data, 1U, (size_t)length, file) != (size_t)length) {
		free(data);
		fclose(file);
		return 0;
	}

	data[length] = '\0';
	fclose(file);
	return data;
}

static int expect_contains(const char *source, const char *needle)
{
	if (strstr(source, needle))
		return 1;

	printf("missing %s\n", needle);
	return 0;
}

static int expect_not_contains(const char *source, const char *needle)
{
	if (!strstr(source, needle))
		return 1;

	printf("unexpected %s\n", needle);
	return 0;
}

int main(int argc, char **argv)
{
	uint8_t jpeg_header[4] = {0xff, 0xd8, 0xff, 0xe0};
	uint8_t png_header[8] = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
	};
	uint8_t unknown_header[8] = {0};
	char *viewer_argv[4] = {
		"zz9k-view",
		"Work:Pictures/test.jpg",
		"Work:Pictures/alpha test.png",
		"RAM:bad.bin"
	};
	char *empty_argv[1] = {"zz9k-view"};
	char *option_argv[2] = {"zz9k-view", "--resize"};
	char *null_file_argv[2] = {"zz9k-view", 0};
	char *empty_file_argv[2] = {"zz9k-view", ""};
	ZZ9KPictureViewerArgs args;
	ZZ9KPictureViewerImage image;
	char title[128];
	char *source;
	int ok;

	if (argc != 2) {
		printf("usage: %s <tools/zz9k-view.c>\n", argv[0]);
		return 2;
	}

	if (zz9k_picture_viewer_detect_codec(jpeg_header, sizeof(jpeg_header)) !=
	    ZZ9K_PICTURE_VIEWER_CODEC_JPEG) {
		printf("did not detect JPEG header\n");
		return 1;
	}
	if (zz9k_picture_viewer_detect_codec(png_header, sizeof(png_header)) !=
	    ZZ9K_PICTURE_VIEWER_CODEC_PNG) {
		printf("did not detect PNG header\n");
		return 2;
	}
	if (zz9k_picture_viewer_detect_codec(unknown_header,
	                                     sizeof(unknown_header)) !=
	    ZZ9K_PICTURE_VIEWER_CODEC_UNKNOWN) {
		printf("accepted unknown picture header\n");
		return 3;
	}

	memset(&args, 0, sizeof(args));
	if (!zz9k_picture_viewer_parse_args(4, viewer_argv, &args) ||
	    args.file_count != 3U ||
	    strcmp(args.files[1], "Work:Pictures/alpha test.png") != 0) {
		printf("did not parse viewer file list\n");
		return 4;
	}
	if (zz9k_picture_viewer_parse_args(1, empty_argv, &args)) {
		printf("accepted empty viewer file list\n");
		return 5;
	}
	if (zz9k_picture_viewer_parse_args(2, option_argv, &args)) {
		printf("accepted unsupported viewer option\n");
		return 6;
	}
	if (zz9k_picture_viewer_parse_args(2, 0, &args)) {
		printf("accepted null viewer argv\n");
		return 11;
	}
	if (zz9k_picture_viewer_parse_args(4, viewer_argv, 0)) {
		printf("accepted null viewer args output\n");
		return 12;
	}
	if (zz9k_picture_viewer_parse_args(2, null_file_argv, &args)) {
		printf("accepted null viewer file arg\n");
		return 13;
	}
	if (zz9k_picture_viewer_parse_args(2, empty_file_argv, &args)) {
		printf("accepted empty viewer file arg\n");
		return 14;
	}
	if (zz9k_picture_viewer_decode_format() !=
	        ZZ9K_SURFACE_FORMAT_BGRA8888 ||
	    !zz9k_picture_viewer_framebuffer_format_supported(
	        ZZ9K_SURFACE_FORMAT_BGRA8888) ||
	    !zz9k_picture_viewer_framebuffer_format_supported(
	        ZZ9K_SURFACE_FORMAT_RGB565) ||
	    !zz9k_picture_viewer_framebuffer_format_supported(
	        ZZ9K_SURFACE_FORMAT_RGB555) ||
	    zz9k_picture_viewer_framebuffer_format_supported(
	        ZZ9K_SURFACE_FORMAT_INDEX8) ||
	    zz9k_picture_viewer_framebuffer_format_supported(
	        ZZ9K_SURFACE_FORMAT_ARGB8888)) {
		printf("did not classify viewer framebuffer formats\n");
		return 15;
	}
	if (zz9k_picture_viewer_framebuffer_requires_conversion(
	        ZZ9K_SURFACE_FORMAT_BGRA8888) ||
	    !zz9k_picture_viewer_framebuffer_requires_conversion(
	        ZZ9K_SURFACE_FORMAT_RGB565) ||
	    !zz9k_picture_viewer_framebuffer_requires_conversion(
	        ZZ9K_SURFACE_FORMAT_RGB555) ||
	    zz9k_picture_viewer_framebuffer_requires_conversion(
	        ZZ9K_SURFACE_FORMAT_INDEX8)) {
		printf("did not classify viewer framebuffer conversion\n");
		return 16;
	}
	{
		uint32_t scale_flags =
			ZZ9K_SERVICE_FLAG_IMAGE_SCALE_BILINEAR |
			ZZ9K_SERVICE_FLAG_IMAGE_SCALE_CLIPPED;

		if (!zz9k_picture_viewer_image_service_supported(
		        8U, scale_flags, ZZ9K_SURFACE_FORMAT_BGRA8888) ||
		    zz9k_picture_viewer_image_service_supported(
		        8U, scale_flags, ZZ9K_SURFACE_FORMAT_RGB565) ||
		    zz9k_picture_viewer_image_service_supported(
		        8U, scale_flags, ZZ9K_SURFACE_FORMAT_RGB555) ||
		    !zz9k_picture_viewer_image_service_supported(
		        8U,
		        scale_flags |
		        ZZ9K_SERVICE_FLAG_IMAGE_SCALE_BGRA_TO_RGB555_RGB565,
		        ZZ9K_SURFACE_FORMAT_RGB565) ||
		    !zz9k_picture_viewer_image_service_supported(
		        8U,
		        scale_flags |
		        ZZ9K_SERVICE_FLAG_IMAGE_SCALE_BGRA_TO_RGB555_RGB565,
		        ZZ9K_SURFACE_FORMAT_RGB555) ||
		    zz9k_picture_viewer_image_service_supported(
		        7U, scale_flags, ZZ9K_SURFACE_FORMAT_BGRA8888) ||
		    zz9k_picture_viewer_image_service_supported(
		        8U, 0U, ZZ9K_SURFACE_FORMAT_BGRA8888) ||
		    zz9k_picture_viewer_image_service_supported(
		        8U, scale_flags, ZZ9K_SURFACE_FORMAT_INDEX8)) {
			printf("did not gate viewer image service by format\n");
			return 17;
		}
	}

	if (strcmp(zz9k_picture_viewer_basename("Work:Pictures/test.jpg"),
	           "test.jpg") != 0 ||
	    strcmp(zz9k_picture_viewer_basename("SYS:Prefs/alpha test.png"),
	           "alpha test.png") != 0 ||
	    strcmp(zz9k_picture_viewer_basename(
		           "Work:Pictures\\nested\\test.png"),
	           "test.png") != 0 ||
	    strcmp(zz9k_picture_viewer_basename("plain.jpg"),
	           "plain.jpg") != 0) {
		printf("did not format viewer basenames\n");
		return 7;
	}

	zz9k_picture_viewer_image_init(&image);
	image.codec = ZZ9K_PICTURE_VIEWER_CODEC_PNG;
	image.path = "Work:Pictures/alpha test.png";
	image.width = 640U;
	image.height = 480U;
	if (!zz9k_picture_viewer_format_title(title, sizeof(title), 2U, 3U,
	                                      &image) ||
	    strcmp(title,
	           "ZZ9000 View 2/3 PNG 640 x 480 - alpha test.png") != 0) {
		printf("did not format viewer title: %s\n", title);
		return 8;
	}

	if (zz9k_picture_viewer_next_index(2U, 3U) != 0U ||
	    zz9k_picture_viewer_previous_index(0U, 3U) != 2U ||
	    zz9k_picture_viewer_next_index(0U, 0U) != 0U ||
	    zz9k_picture_viewer_previous_index(0U, 0U) != 0U ||
	    zz9k_picture_viewer_next_index(0U, 1U) != 0U ||
	    zz9k_picture_viewer_previous_index(0U, 1U) != 0U) {
		printf("did not wrap viewer navigation\n");
		return 9;
	}

	if (zz9k_picture_viewer_action_from_keys('q', 0U) !=
	        ZZ9K_PICTURE_VIEWER_ACTION_QUIT ||
	    zz9k_picture_viewer_action_from_keys('Q', 0U) !=
	        ZZ9K_PICTURE_VIEWER_ACTION_QUIT ||
	    zz9k_picture_viewer_action_from_keys(0x1bU, 0U) !=
	        ZZ9K_PICTURE_VIEWER_ACTION_QUIT ||
	    zz9k_picture_viewer_action_from_keys(0U, 0x45U) !=
	        ZZ9K_PICTURE_VIEWER_ACTION_QUIT ||
	    zz9k_picture_viewer_action_from_keys(' ', 0U) !=
	        ZZ9K_PICTURE_VIEWER_ACTION_NEXT ||
	    zz9k_picture_viewer_action_from_keys(0U, 0x4dU) !=
	        ZZ9K_PICTURE_VIEWER_ACTION_NEXT ||
	    zz9k_picture_viewer_action_from_keys(0U, 0x4eU) !=
	        ZZ9K_PICTURE_VIEWER_ACTION_NEXT ||
	    zz9k_picture_viewer_action_from_keys(0x08U, 0U) !=
	        ZZ9K_PICTURE_VIEWER_ACTION_PREVIOUS ||
	    zz9k_picture_viewer_action_from_keys(0U, 0x41U) !=
	        ZZ9K_PICTURE_VIEWER_ACTION_PREVIOUS ||
	    zz9k_picture_viewer_action_from_keys(0U, 0x4cU) !=
	        ZZ9K_PICTURE_VIEWER_ACTION_PREVIOUS ||
	    zz9k_picture_viewer_action_from_keys(0U, 0x4fU) !=
	        ZZ9K_PICTURE_VIEWER_ACTION_PREVIOUS ||
	    zz9k_picture_viewer_action_from_keys('r', 0U) !=
	        ZZ9K_PICTURE_VIEWER_ACTION_REDRAW ||
	    zz9k_picture_viewer_action_from_keys('R', 0U) !=
	        ZZ9K_PICTURE_VIEWER_ACTION_REDRAW ||
	    zz9k_picture_viewer_action_from_keys('x', 0U) !=
	        ZZ9K_PICTURE_VIEWER_ACTION_NONE) {
		printf("did not map viewer keys\n");
		return 10;
	}

	source = read_file(argv[1]);
	if (!source) {
		printf("failed to read %s\n", argv[1]);
		return 22;
	}

	ok = 1;
	ok &= expect_not_contains(source, "system(");
	ok &= expect_not_contains(source, "zz9k_view_build_command");
	ok &= expect_not_contains(source, "zz9k-jpeg --view");
	ok &= expect_not_contains(source, "zz9k-png --view");
	ok &= expect_contains(source, "zz9k_jpeg_decode_viewer_image");
	ok &= expect_contains(source, "zz9k_png_decode_viewer_image");
	ok &= expect_contains(source, "zz9k_image_window_poll_event");
	ok &= expect_contains(source, "zz9k_picture_viewer_render_image");
	ok &= expect_contains(source, "zz9k_image_window_set_title");
	ok &= expect_contains(source, "zz9k_view_format_window_title");
	ok &= expect_contains(source, "ZZ9000 View %lu/%lu %s");
	ok &= expect_contains(
		source, "zz9k_picture_viewer_framebuffer_format_supported");
	ok &= expect_contains(
		source, "zz9k_picture_viewer_image_service_supported");

	free(source);
	if (!ok)
		return 23;

	return 0;
}
