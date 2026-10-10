/*
 * WebP-private allocation wrappers over the per-session codec arena.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_webp_alloc.h"
#include "sdk_vorbis_alloc.h"

static int active_failed;

void sdk_webp_alloc_select(struct sdk_vorbis_heap *heap)
{
	/* No jump buffer: libwebp checks every allocation result, so a refused
	 * allocation returns NULL and the session faults NO_MEMORY. */
	sdk_vorbis_heap_select(heap, 0);
	active_failed = 0;
}

int sdk_webp_alloc_failed(void)
{
	return active_failed;
}

void *sdk_webp_malloc(size_t size)
{
	void *ptr = sdk_vorbis_malloc(size);

	if (!ptr)
		active_failed = 1;
	return ptr;
}

void *sdk_webp_calloc(size_t count, size_t size)
{
	void *ptr = sdk_vorbis_calloc(count, size);

	if (!ptr)
		active_failed = 1;
	return ptr;
}

void sdk_webp_free(void *ptr)
{
	sdk_vorbis_free(ptr);
}
