/*
 * Source guard for zz9k-png.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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
	char *source;
	int ok;

	if (argc != 2) {
		printf("usage: %s <tools/zz9k-png.c>\n", argv[0]);
		return 2;
	}

	source = read_file(argv[1]);
	if (!source) {
		printf("failed to read %s\n", argv[1]);
		return 2;
	}

	ok = 1;
	ok &= expect_contains(source, "#include \"zz9k/image.h\"");
	ok &= expect_contains(source, "#include \"zz9k/surface.h\"");
	ok &= expect_contains(source, "#include \"zz9k-png-view.h\"");
	ok &= expect_contains(source, "zz9k_image_stream_required_service_flags");
	ok &= expect_contains(source, "zz9k_image_service_supports_clipped_scale");
	ok &= expect_contains(source, "zz9k_image_window_visible_clips");
	ok &= expect_contains(source, "--view");
	ok &= expect_contains(source, "showing until close");
	ok &= expect_contains(source, "zz9k_png_framebuffer_restore_visible");
	ok &= expect_contains(source, "zz9k_surface_native_rtg_format()");
	ok &= expect_contains(source,
	                      "zz9k_surface_is_native_rtg_format(framebuffer->format)");
	ok &= expect_contains(source,
	                      "zz9k_surface_layout(png_input->width, png_input->height,");
	ok &= expect_contains(source, "output_pitch");
	ok &= expect_contains(source, "result.output_format != output_format");
	ok &= expect_contains(source, "zz9k_png_decode_viewer_image");
	ok &= expect_contains(
		source, "output_format = zz9k_picture_viewer_decode_format();");
	ok &= expect_contains(source, "#define ZZ9K_PNG_STAGING_BYTES (32UL * 1024UL)");
	ok &= expect_contains(source, "ZZ9K_ALLOC_HOST_WINDOW, &staging");
	ok &= expect_contains(source, "ZZ9KPictureViewerImage");
	ok &= expect_contains(source, "ZZ9K_PICTURE_VIEWER_CODEC_PNG");
	ok &= expect_not_contains(source, "zz9k_png_output_bytes");
	ok &= expect_not_contains(source, "uint32_t bytes_per_pixel");
	ok &= expect_not_contains(source, "ZZ9K_SURFACE_FORMAT_BGRA8888");

	free(source);
	return ok ? 0 : 1;
}
