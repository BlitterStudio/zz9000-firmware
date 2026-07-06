/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Host tests for sdk_decompress_lzh() wrapper-level contracts.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <setjmp.h>
#include <stdio.h>
#include <stdint.h>

#include "lzh/lha.h"
#include "lzh/zz9k_lzh.h"

FILE *infile;
FILE *outfile;

boolean quiet;
boolean text_mode;
boolean verify_mode;
boolean output_to_stdout;
boolean dump_lzss;
boolean extract_broken_archive;
int quiet_mode;
unsigned char *dtext;

int zz9k_lzh_error;
jmp_buf zz9k_lzh_fatal_jmp;

static uint32_t fake_out_count;
static off_t fake_read_size;
static int fake_overflow;
static int fake_method;
static off_t fake_original_size;
static off_t fake_packed_size;

void make_crctable(void) {}

void zz9k_lzh_io_begin(const uint8_t *src, uint32_t src_len,
                       uint8_t *dst, uint32_t dst_cap)
{
	(void)src;
	(void)src_len;
	(void)dst;
	(void)dst_cap;
	infile = (FILE *)0x1111;
	outfile = (FILE *)0x2222;
}

uint32_t zz9k_lzh_io_out_count(void)
{
	return fake_out_count;
}

int zz9k_lzh_io_overflowed(void)
{
	return fake_overflow;
}

const char *zz9k_lzh_fatal_message(void)
{
	return NULL;
}

void zz9k_lzh_disarm_dtext(void)
{
	dtext = NULL;
}

void zz9k_lzh_free_dtext(void)
{
	dtext = NULL;
}

void *zz9k_lzh_dtext_reclaim_base(void)
{
	return NULL;
}

unsigned zz9k_lzh_dtext_reclaim_bytes(void)
{
	return 0u;
}

void zz9k_lzh_flush_dtext_reclaim(void) {}
void zz9k_lzh_invalidate_dtext_reclaim(void) {}
void zz9k_lzh_track_dtext(unsigned char *ptr) { dtext = ptr; }
void zz9k_lzh_reclaim(void) { dtext = NULL; }

int decode_lzhuf(FILE *infp, FILE *outfp,
                 off_t original_size, off_t packed_size,
                 char *name, int method, off_t *read_sizep)
{
	(void)infp;
	(void)outfp;
	(void)name;
	fake_method = method;
	fake_original_size = original_size;
	fake_packed_size = packed_size;
	*read_sizep = fake_read_size;
	return 0x1234;
}

static void reset_fake(uint32_t out_count, off_t read_size, int overflow)
{
	fake_out_count = out_count;
	fake_read_size = read_size;
	fake_overflow = overflow;
	fake_method = 0;
	fake_original_size = 0;
	fake_packed_size = 0;
	dtext = NULL;
}

static int test_rejects_short_source_consumption(void)
{
	uint8_t src[5] = { 1, 2, 3, 4, 0xee };
	uint8_t dst[3] = { 0 };
	struct SDKDecompressResult result;
	uint16_t status;

	reset_fake(sizeof(dst), (off_t)(sizeof(src) - 1u), 0);
	status = sdk_decompress_lzh(SDK_COMPRESSION_LH5,
	                            src, sizeof(src),
	                            dst, sizeof(dst),
	                            &result);

	if (status != SDK_STATUS_BAD_REQUEST) return 1;
	if (result.bytes_consumed != sizeof(src) - 1u) return 2;
	if ((result.flags & SDK_DECOMPRESS_RESULT_STREAM_END) != 0u) return 3;
	if (fake_method != LZHUFF5_METHOD_NUM) return 4;
	if (fake_original_size != (off_t)sizeof(dst)) return 5;
	if (fake_packed_size != (off_t)sizeof(src)) return 6;
	return 0;
}

static int test_accepts_full_source_consumption(void)
{
	uint8_t src[4] = { 1, 2, 3, 4 };
	uint8_t dst[3] = { 0 };
	struct SDKDecompressResult result;
	uint16_t status;

	reset_fake(sizeof(dst), (off_t)sizeof(src), 0);
	status = sdk_decompress_lzh(SDK_COMPRESSION_LH5,
	                            src, sizeof(src),
	                            dst, sizeof(dst),
	                            &result);

	if (status != SDK_STATUS_OK) return 1;
	if (result.bytes_consumed != sizeof(src)) return 2;
	if (result.bytes_written != sizeof(dst)) return 3;
	if ((result.flags & SDK_DECOMPRESS_RESULT_STREAM_END) == 0u) return 4;
	return 0;
}

static int test_accepts_zero_length_member(void)
{
	struct SDKDecompressResult result;
	uint16_t status;

	reset_fake(123u, 456, 0);
	status = sdk_decompress_lzh(SDK_COMPRESSION_LH5,
	                            NULL, 0,
	                            NULL, 0,
	                            &result);

	if (status != SDK_STATUS_OK) return 1;
	if (result.bytes_consumed != 0u) return 2;
	if (result.bytes_written != 0u) return 3;
	if (result.checksum != 0u) return 4;
	if (result.algorithm != SDK_COMPRESSION_LH5) return 5;
	if (result.flags != (SDK_DECOMPRESS_RESULT_CHECKSUM_VALID |
	                     SDK_DECOMPRESS_RESULT_STREAM_END)) return 6;
	if (fake_method != 0) return 7;        /* no decode core call */
	return 0;
}

int main(int argc, char **argv)
{
	struct { const char *name; int (*fn)(void); } tests[] = {
		{ "rejects_short_source_consumption", test_rejects_short_source_consumption },
		{ "accepts_full_source_consumption", test_accepts_full_source_consumption },
		{ "accepts_zero_length_member", test_accepts_zero_length_member },
	};
	unsigned i;
	int failures = 0;

	(void)argc;
	(void)argv;

	for (i = 0u; i < sizeof(tests) / sizeof(tests[0]); i++) {
		int rc = tests[i].fn();

		if (rc != 0) {
			printf("FAIL %s (rc=%d)\n", tests[i].name, rc);
			failures++;
		} else {
			printf("ok   %s\n", tests[i].name);
		}
	}
	if (failures) {
		printf("lzh_decode_contract_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("lzh_decode_contract_test: all passed\n");
	return 0;
}
