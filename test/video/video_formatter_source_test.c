/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Source-level regression checks for video_formatter.v invariants that are
 * difficult to exercise without a Verilog simulator in the host test setup.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIDEO_FORMATTER_PATH "../../video_formatter.v"

static char *read_file(const char *path)
{
	FILE *fp = fopen(path, "rb");
	char *buffer;
	long size;

	if (!fp) {
		perror(path);
		return NULL;
	}

	if (fseek(fp, 0, SEEK_END) != 0) {
		perror("fseek");
		fclose(fp);
		return NULL;
	}

	size = ftell(fp);
	if (size < 0) {
		perror("ftell");
		fclose(fp);
		return NULL;
	}

	if (fseek(fp, 0, SEEK_SET) != 0) {
		perror("fseek");
		fclose(fp);
		return NULL;
	}

	buffer = malloc((size_t)size + 1U);
	if (!buffer) {
		fclose(fp);
		return NULL;
	}

	if (fread(buffer, 1U, (size_t)size, fp) != (size_t)size) {
		perror("fread");
		free(buffer);
		fclose(fp);
		return NULL;
	}

	buffer[size] = '\0';
	fclose(fp);
	return buffer;
}

static int require_contains(const char *text, const char *needle)
{
	if (!strstr(text, needle)) {
		printf("video_formatter.v: missing expected source pattern: %s\n", needle);
		return 0;
	}

	return 1;
}

static int require_absent(const char *text, const char *needle)
{
	if (strstr(text, needle)) {
		printf("video_formatter.v: stale TKEEP-gated source pattern remains: %s\n", needle);
		return 0;
	}

	return 1;
}

static int test_valid_64bit_beat_writes_both_formatter_words(const char *text)
{
	int ok = 1;

	ok &= require_contains(text, "wire pixin_lo_valid = 1'b1;");
	ok &= require_contains(text, "wire pixin_hi_valid = 1'b1;");
	ok &= require_contains(text, "wire [1:0] pixin_word_count = 2'd2;");
	ok &= require_absent(text, "pixin_word_count = pixin_hi_valid ?");

	return ok ? 0 : 1;
}

int main(void)
{
	char *text = read_file(VIDEO_FORMATTER_PATH);
	int result;

	if (!text)
		return 1;

	result = test_valid_64bit_beat_writes_both_formatter_words(text);
	free(text);

	return result;
}
