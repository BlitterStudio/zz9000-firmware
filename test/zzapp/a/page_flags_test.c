/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "zzapp_runtime.h"

#define X 1u
#define W 2u
#define R 4u

/* Kit 66: hello.elf's .data starts at 0x388 inside the code page, and
 * mapping the data segment last made that page non-executable, so the app
 * took a prefetch abort at its entry point. Every page must carry the
 * flags of all segments that touch it. */
int main(void)
{
	/* hello.elf: R E at 0x0 (0x388 bytes), RW at 0x388 (0x14 bytes). */
	static const uint32_t hello[2][3] = {
		{ 0x000u, 0x388u, R | X },
		{ 0x388u, 0x014u, R | W },
	};
	/* Code ends mid-page, data starts on the same page and runs on,
	 * then a gap page, then a read-only segment. */
	static const uint32_t larger[3][3] = {
		{ 0x0000u, 0x2100u, R | X },
		{ 0x2100u, 0x1f00u, R | W },
		{ 0x5000u, 0x0800u, R },
	};

	assert(zzapp_loader_page_flags(hello, 2u, 0x0000u) == (R | W | X));
	assert(zzapp_loader_page_flags(hello, 2u, 0x1000u) == 0u);

	assert(zzapp_loader_page_flags(larger, 3u, 0x0000u) == (R | X));
	assert(zzapp_loader_page_flags(larger, 3u, 0x1000u) == (R | X));
	assert(zzapp_loader_page_flags(larger, 3u, 0x2000u) == (R | W | X));
	assert(zzapp_loader_page_flags(larger, 3u, 0x3000u) == (R | W));
	/* 0x4000 is the gap: nothing touches it, so it stays unmapped. */
	assert(zzapp_loader_page_flags(larger, 3u, 0x4000u) == 0u);
	assert(zzapp_loader_page_flags(larger, 3u, 0x5000u) == R);
	/* A segment ending exactly on a page boundary does not touch the
	 * next page. */
	assert(zzapp_loader_page_flags(larger, 3u, 0x6000u) == 0u);

	printf("page_flags_test: ok\n");
	return 0;
}
