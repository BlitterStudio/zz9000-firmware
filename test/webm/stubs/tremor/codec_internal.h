/*
 * Host-test stand-in for the Tremor codebook layout books_ok() inspects.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef STUB_CODEC_INTERNAL_H
#define STUB_CODEC_INTERNAL_H

typedef struct {
	long dim;
	long entries;
	char *lengthlist;
} static_codebook;

typedef struct {
	int books;
	static_codebook *book_param[256];
} codec_setup_info;

#endif
