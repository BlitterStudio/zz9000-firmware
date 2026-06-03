/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Source guard for the firmware PNG streaming policy.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, long *out_length)
{
	FILE *file;
	char *data;
	long length;

	file = fopen(path, "rb");
	if (!file)
		return 0;
	if (fseek(file, 0L, SEEK_END) != 0) {
		fclose(file);
		return 0;
	}
	length = ftell(file);
	if (length < 0L) {
		fclose(file);
		return 0;
	}
	if (fseek(file, 0L, SEEK_SET) != 0) {
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
	fclose(file);
	data[length] = '\0';
	if (out_length)
		*out_length = length;
	return data;
}

int main(void)
{
	const char *source_path = "ZZ9000_proto.sdk/ZZ9000OS/src/sdk_image_stream.c";
	const char *header_path = "ZZ9000_proto.sdk/ZZ9000OS/src/sdk_image_stream.h";
	char *source;
	char *header;
	long length = 0L;
	int rc = 0;

	source = read_file(source_path, &length);
	if (!source) {
		printf("could not read %s\n", source_path);
		return 1;
	}
	header = read_file(header_path, 0);
	if (!header) {
		printf("could not read %s\n", header_path);
		free(source);
		return 1;
	}
	if (strstr(source, "png_process_data_pause") != 0) {
		printf("PNG streaming must not call png_process_data_pause\n");
		rc = 2;
	}
	if (strstr(header, "SDK_IMAGE_STREAM_PNG_MAX_FEED_BYTES") == 0 ||
	    strstr(source, "SDK_IMAGE_STREAM_PNG_MAX_FEED_BYTES") == 0) {
		printf("PNG streaming feed cap is missing\n");
		rc = 3;
	}
	if (strstr(header,
	           "SDK_IMAGE_STREAM_PNG_MAX_FEED_BYTES (256U * 1024U)") == 0) {
		printf("PNG streaming feed cap must match the 256 KiB SDK staging buffer\n");
		rc = 4;
	}
	if (strstr(source, "png_row_rgb888") == 0 ||
	    strstr(source, "memcpy(dst, row, row_bytes)") == 0) {
		printf("PNG RGB888 streaming must keep the direct row-copy path\n");
		rc = 5;
	}
	if (strstr(source, "!has_alpha && !has_trns") == 0) {
		printf("PNG tRNS must be treated as alpha before adding filler bytes\n");
		rc = 6;
	}
	if (strstr(source, "interlaced PNG is not supported") != 0) {
		printf("PNG interlace must be supported, not rejected\n");
		rc = 7;
	}
	if (strstr(source, "png_set_interlace_handling") == 0 ||
	    strstr(source, "png_progressive_combine_row") == 0) {
		printf("PNG interlace support must use libpng progressive row combining\n");
		rc = 8;
	}
	if (strstr(source, "png_interlaced") == 0 ||
	    strstr(source, "png_interlace_buffer") == 0) {
		printf("PNG interlace session state is missing\n");
		rc = 9;
	}
	(void)length;
	free(header);
	free(source);
	return rc;
}
