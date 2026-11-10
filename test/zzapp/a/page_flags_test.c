/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "zzapp_runtime.h"

#define X 1u
#define W 2u
#define R 4u

/* Each image page carries the flags of every segment that touches it, and
 * the page holding the end of the image is writable because the heap
 * starts there. Both layouts below faulted on the card in kit 66. */
int main(void)
{
	/* hello.elf: R E at 0x0 (0x388 bytes), RW at 0x388 (0x14 bytes).
	 * Its .data shares the code page, which ended up non-executable and
	 * faulted at the entry point. */
	static const uint32_t hello[2][3] = {
		{ 0x000u, 0x388u, R | X },
		{ 0x388u, 0x014u, R | W },
	};
	/* swquake.elf: text, then .data/.bss, then a read-only .rela.dyn
	 * segment that ends at 0xfdad4. The first malloc wrote just past it
	 * (0xfdb7c) and took a write permission fault. */
	static const uint32_t swquake[3][3] = {
		{ 0x00000u, 0x7dea4u, R | X },
		{ 0x7dea8u, 0x7b510u, R | W },
		{ 0xf93b8u, 0x0471cu, R },
	};
	const uint32_t swquake_end = 0xfdae0u; /* image_end after rounding */

	assert(zzapp_loader_page_flags(hello, 2u, 0x3a0u, 0x0000u) ==
	       (R | W | X));
	assert(zzapp_loader_page_flags(hello, 2u, 0x3a0u, 0x1000u) == 0u);

	assert(zzapp_loader_page_flags(swquake, 3u, swquake_end, 0x00000u) ==
	       (R | X));
	/* Code ends and .data starts in page 0x7d000. */
	assert(zzapp_loader_page_flags(swquake, 3u, swquake_end, 0x7d000u) ==
	       (R | W | X));
	assert(zzapp_loader_page_flags(swquake, 3u, swquake_end, 0x80000u) ==
	       (R | W));
	/* .bss ends and .rela.dyn starts in page 0xf9000. */
	assert(zzapp_loader_page_flags(swquake, 3u, swquake_end, 0xf9000u) ==
	       (R | W));
	assert(zzapp_loader_page_flags(swquake, 3u, swquake_end, 0xfa000u) == R);
	/* The heap starts in the last image page. */
	assert(zzapp_loader_page_flags(swquake, 3u, swquake_end, 0xfd000u) ==
	       (R | W));
	assert(zzapp_loader_page_flags(swquake, 3u, swquake_end, 0xfe000u) == 0u);

	/* A gap page that no segment touches stays unmapped, and an image
	 * that ends on a page boundary adds nothing to the next page. */
	{
		static const uint32_t gap[2][3] = {
			{ 0x0000u, 0x1000u, R | X },
			{ 0x2000u, 0x1000u, R | W },
		};

		assert(zzapp_loader_page_flags(gap, 2u, 0x3000u, 0x1000u) == 0u);
		assert(zzapp_loader_page_flags(gap, 2u, 0x3000u, 0x3000u) == 0u);
	}

	printf("page_flags_test: ok\n");
	return 0;
}
