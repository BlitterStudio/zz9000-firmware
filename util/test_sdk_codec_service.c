/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Source guard for SDK codec-service mailbox wiring.
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
	size_t length = strlen(needle);

	while ((cursor = strstr(cursor, needle)) != 0) {
		count++;
		cursor += length;
	}
	return count;
}

int main(int argc, char **argv)
{
	char *mailbox_source;
	char *mailbox_header;
	char *makefile_source;
	unsigned legacy_owner_checks;
	int ok = 1;

	if (argc != 4) {
		printf("usage: %s <sdk_mailbox.c> <sdk_mailbox.h> <Makefile>\n",
		       argv[0]);
		return 2;
	}

	mailbox_source = read_file(argv[1]);
	mailbox_header = read_file(argv[2]);
	makefile_source = read_file(argv[3]);
	if (!mailbox_source || !mailbox_header || !makefile_source) {
		printf("failed to read input sources\n");
		free(mailbox_source);
		free(mailbox_header);
		free(makefile_source);
		return 2;
	}

	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_CAP_COMPRESSION");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_CODEC");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_OP_DECOMPRESS");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_OP_DECOMPRESS_STREAM_BEGIN");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_OP_DECOMPRESS_STREAM_READ");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_OP_DECOMPRESS_STREAM_CLOSE");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_OP_DECOMPRESS_STREAM_FEED");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_CODEC_DEFLATE_RAW");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_CODEC_ZLIB");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_CODEC_GZIP");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_CODEC_LZMA_ALONE");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_CODEC_LZMA2");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_CODEC_CHECKSUM");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_CODEC_DECOMPRESS_STREAM");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_CODEC_DECOMPRESS_FEED");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_CODEC_DEFLATE_FEED");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_CODEC_ZLIB_FEED");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_CODEC_GZIP_FEED");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_DECOMPRESS_FLAG_FEED_INPUT");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_DECOMPRESS_RESULT_NEED_INPUT");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_COMPRESSION_DEFLATE_RAW");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_COMPRESSION_LZMA2");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_COMPRESSION_LH1");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_COMPRESSION_LH5");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_COMPRESSION_LH6");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_COMPRESSION_LH7");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_SERVICE_FLAG_CODEC_LZH");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_DECOMPRESS_RESULT_STREAM_END");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_CAP_MEDIA_SESSION");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "#define SDK_OP_MEDIA_SESSION_BEGIN");
	ok &= expect_contains(mailbox_header, "sdk_mailbox.h",
	                      "SDKMediaSessionMainResultPayload");

	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "#include \"sdk_compression.h\"");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_CAP_COMPRESSION");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_SERVICE_CODEC");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_OP_DECOMPRESS");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_OP_DECOMPRESS_STREAM_BEGIN");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_OP_DECOMPRESS_STREAM_FEED");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "handle_decompress_stream_begin");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "handle_decompress_stream_read");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "handle_decompress_stream_feed");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "handle_decompress_stream_close");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "handle_decompress");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "sdk_decompress_buffer");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "sdk_decompress_stream_begin");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "sdk_decompress_stream_read");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "sdk_decompress_stream_feed");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "sdk_decompress_stream_close");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_SERVICE_FLAG_CODEC_LZMA_ALONE");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_SERVICE_FLAG_CODEC_LZMA2");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_SERVICE_FLAG_CODEC_DEFLATE_FEED");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_SERVICE_FLAG_CODEC_ZLIB_FEED");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_SERVICE_FLAG_CODEC_GZIP_FEED");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_SERVICE_FLAG_CODEC_LZH");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDKDecompressPayload_must_be_48_bytes");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDKDecompressResultPayload_must_be_48_bytes");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDKDecompressStreamResultPayload_must_be_48_bytes");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "#include \"sdk_media_session.h\"");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "handle_media_session_begin");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_OP_MEDIA_SESSION_PRESENT");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "handle_media_session_write");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "handle_media_session_deferred_simple");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "handle_media_session_present_or_discard");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "handle_media_session_status");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "handle_media_session_audio_read");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "handle_media_session_audio_unsupported");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "bytes_are_zero(payload->reserved");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "sdk_media_session_audio_read");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "find_shared_buffer(begin.pcm_ring_handle)");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "uint32_t pin_count;");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "if (buffer->pin_count != 0U)");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "shared_buffer_pin(pcm_ring)");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "release_media_pcm_ring(session)");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_SERVICE_FLAG_VIDEO_MEDIA_MP2");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "SDK_SERVICE_FLAG_VIDEO_PCM_RING_STATUS");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "case SDK_OP_MEDIA_SESSION_WRITE:");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "case SDK_OP_MEDIA_SESSION_DECODE:");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "case SDK_OP_MEDIA_SESSION_CLOSE:");
	ok &= expect_contains(mailbox_source, "sdk_mailbox.c",
	                      "sdk_media_session_close_retired");

	legacy_owner_checks = count_occurrences(
		mailbox_source, "SDK_VIDEO_STREAM_OWNER_LEGACY");
	if (legacy_owner_checks < 3U) {
		printf("sdk_mailbox.c: legacy VIDEO_SESSION write/decode/close "
		       "must reject media-owned sessions (found %u owner checks)\n",
		       legacy_owner_checks);
		ok = 0;
	}

	ok &= expect_contains(makefile_source, "Makefile",
	                      "$(SRC_DIR)/sdk_compression.c");
	ok &= expect_contains(makefile_source, "Makefile",
	                      "$(SRC_DIR)/sdk_media_session.c");
	ok &= expect_contains(makefile_source, "Makefile",
	                      "$(SRC_DIR)/sdk_media_timeline.c");
	ok &= expect_contains(makefile_source, "Makefile",
	                      "$(LZMA_SDK_BUILD)/LzmaDec.o");
	ok &= expect_contains(makefile_source, "Makefile",
	                      "$(LZMA_SDK_BUILD)/Lzma2Dec.o");

	free(mailbox_source);
	free(mailbox_header);
	free(makefile_source);
	return ok ? 0 : 1;
}
