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

int main(int argc, char **argv)
{
	char *mailbox_source;
	char *mailbox_header;
	char *makefile_source;
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

	ok &= expect_contains(makefile_source, "Makefile",
	                      "$(SRC_DIR)/sdk_compression.c");
	ok &= expect_contains(makefile_source, "Makefile",
	                      "$(LZMA_SDK_BUILD)/LzmaDec.o");
	ok &= expect_contains(makefile_source, "Makefile",
	                      "$(LZMA_SDK_BUILD)/Lzma2Dec.o");

	free(mailbox_source);
	free(mailbox_header);
	free(makefile_source);
	return ok ? 0 : 1;
}
