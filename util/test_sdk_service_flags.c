/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Source guard for SDK image-service capability advertisement.
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

static int expect_contains(const char *source, const char *label,
                           const char *needle)
{
	if (strstr(source, needle))
		return 1;
	printf("%s: missing %s\n", label, needle);
	return 0;
}

static unsigned count_occurrences(const char *source, const char *needle)
{
	unsigned count = 0U;
	const char *cursor = source;
	size_t len = strlen(needle);

	while ((cursor = strstr(cursor, needle)) != 0) {
		count++;
		cursor += len;
	}
	return count;
}

int main(int argc, char **argv)
{
	char *mailbox_source;
	char *mailbox_header;
	unsigned png_flag_refs;
	int ok = 1;

	if (argc != 3) {
		printf("usage: %s <sdk_mailbox.c> <sdk_mailbox.h>\n", argv[0]);
		return 2;
	}

	mailbox_source = read_file(argv[1]);
	mailbox_header = read_file(argv[2]);
	if (!mailbox_source || !mailbox_header) {
		printf("failed to read input sources\n");
		free(mailbox_source);
		free(mailbox_header);
		return 2;
	}

	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_IMAGE_PNG_DIRECT_BGRA");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_IMAGE_RGB888_OUTPUT");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SURFACE_FORMAT_RGB888");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_AUDIO_MP3_DECODE");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_AUDIO_MP3_STREAM");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_OP_AUDIO_STREAM_BEGIN");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_OP_AUDIO_STREAM_FEED");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_OP_AUDIO_STREAM_READ");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_OP_AUDIO_STREAM_CLOSE");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_VIDEO_MEDIA_SESSION");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_VIDEO_EXPLICIT_PRESENT");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_VIDEO_TIMELINE_90KHZ");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_CAP_AUDIO_DECODE");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_SERVICE_AUDIO");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "case SDK_OP_DECODE_MP3:");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "case SDK_OP_AUDIO_STREAM_BEGIN:");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "handle_audio_stream_feed");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "stream->backpressure");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "bytes = (uint32_t)samples *");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "(uint32_t)info.channels *");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "consumed = (uint32_t)info.frame_bytes");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "max_pcm_this_call");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "stream->high_water_bytes");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_SERVICE_FLAG_AUDIO_MP3_STREAM");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "put_be32(info->flags, service_flags(service));");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_SERVICE_FLAG_IMAGE_RGB888_OUTPUT");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_SERVICE_FLAG_VIDEO_MEDIA_SESSION");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_SERVICE_FLAG_VIDEO_EXPLICIT_PRESENT");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_SERVICE_FLAG_VIDEO_TIMELINE_90KHZ");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_SERVICE_FLAG_VIDEO_MEDIA_MP2");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_SERVICE_FLAG_VIDEO_PCM_RING_STATUS");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "audio_codec_present()");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_SERVICE_FLAG_VIDEO_AUDIO_BIND");

	png_flag_refs = count_occurrences(
		mailbox_source, "SDK_SERVICE_FLAG_IMAGE_PNG_DIRECT_BGRA");
	if (png_flag_refs < 2U) {
		printf("sdk_mailbox.c: PNG flag should be present in both the "
		       "image service descriptor and dynamic service flags "
		       "(found %u)\n",
		       png_flag_refs);
		ok = 0;
	}

	free(mailbox_source);
	free(mailbox_header);
	return ok ? 0 : 1;
}
