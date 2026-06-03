/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Source guard for SDK mailbox surface cache policy.
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

static int expect_count(const char *source, const char *needle,
                        unsigned int expected)
{
	const char *cursor = source;
	unsigned int actual = 0;
	size_t needle_len = strlen(needle);

	while ((cursor = strstr(cursor, needle)) != 0) {
		actual++;
		cursor += needle_len;
	}
	if (actual == expected)
		return 1;
	printf("expected %u occurrences of %s, got %u\n",
	       expected, needle, actual);
	return 0;
}

int main(int argc, char **argv)
{
	char *source;
	int ok = 1;

	if (argc != 2) {
		printf("usage: %s <sdk_mailbox.c>\n", argv[0]);
		return 2;
	}

	source = read_file(argv[1]);
	if (!source) {
		printf("failed to read %s\n", argv[1]);
		return 2;
	}

	ok &= expect_contains(source, "static void prepare_surface_for_arm_read");
	ok &= expect_contains(source, "static void flush_surface_rect");
	ok &= expect_contains(source, "surface_is_arm_local(surface)");
	ok &= expect_contains(source, "prepare_surface_for_arm_read(&src);");
	ok &= expect_contains(source,
	                      "flush_surface_rect(&surface, x, y, width, height);");
	ok &= expect_contains(source,
	                      "flush_surface_rect(&dst, dst_x, dst_y, width, height);");
	ok &= expect_count(
		source, "Xil_DCacheFlushRange(SDK_MAILBOX_ADDRESS, SDK_MAILBOX_TOTAL_SIZE);",
		1U);
	ok &= expect_contains(
		source, "Xil_DCacheInvalidateRange(SDK_MAILBOX_ADDRESS, SDK_MAILBOX_TOTAL_SIZE);");
	ok &= expect_not_contains(
		source, "Xil_DCacheInvalidateRange((INTPTR)src.address, src.length);");
	ok &= expect_not_contains(
		source, "Xil_DCacheFlushRange((INTPTR)surface.address, surface.length);");
	ok &= expect_not_contains(
		source, "Xil_DCacheFlushRange((INTPTR)dst.address, dst.length);");
	ok &= expect_contains(source, "static void flush_audio_pcm_written");
	ok &= expect_contains(
		source,
		"flush_audio_pcm_written(stream, pcm_dst, pcm_flush_start,\n"
		"\t\t                        produced_this_call);");
	ok &= expect_not_contains(
		source, "flush_audio_pcm_written(stream, dst, stream->pcm_write, bytes);");
	ok &= expect_not_contains(
		source, "Xil_DCacheFlushRange((INTPTR)dst, stream->pcm_capacity);");
	ok &= expect_contains(source, "uint32_t input_offset;");
	ok &= expect_contains(source, "static void audio_stream_compact_input");
	ok &= expect_contains(
		source, "#define SDK_AUDIO_STREAM_MIN_INPUT_BYTES (16U * 1024U)");
	ok &= expect_contains(source, "static int audio_stream_needs_more_input");
	ok &= expect_contains(
		source, "stream->input_length < SDK_AUDIO_STREAM_MIN_INPUT_BYTES");
	ok &= expect_contains(
		source, "if (audio_stream_needs_more_input(stream))");
	ok &= expect_count(
		source, "if (audio_stream_needs_more_input(stream))", 3U);
	ok &= expect_contains(
		source, "stream->eof && stream->frames_decoded != 0U &&");
	ok &= expect_contains(source, "info.frame_offset != 0");
	ok &= expect_contains(source, "uint32_t output_frame_limit;");
	ok &= expect_contains(source, "int decode_complete;");
	ok &= expect_contains(source, "static int audio_stream_check_vbr_tag");
	ok &= expect_contains(source, "stream->output_frame_limit = frame_count;");
	ok &= expect_contains(source, "stream->frames_decoded >=");
	ok &= expect_contains(source, "stream->decode_complete = 1;");
	ok &= expect_contains(source, "audio_stream_discard_input(stream);");
	ok &= expect_contains(source, "stream->input_offset = 0U;");
	ok &= expect_contains(
		source, "dst + stream->input_offset + stream->input_length");
	ok &= expect_not_contains(
		source, "memmove(input, input + bytes, stream->input_length - bytes);");

	free(source);
	return ok ? 0 : 1;
}
