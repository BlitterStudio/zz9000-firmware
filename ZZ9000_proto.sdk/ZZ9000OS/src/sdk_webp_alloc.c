/*
 * WebP-private allocation wrappers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_webp_alloc.h"
#include "sdk_compression.h"
#include "sdk_decode_reclaim.h"
#include "sdk_smp_lock.h"
#include <stdint.h>
#include <string.h>

struct sdk_webp_alloc_header {
	size_t size;
	size_t *used;
};

static size_t *active_used;
static size_t active_limit;
static int active_failed;

void sdk_webp_alloc_set_quota(size_t *used, size_t limit)
{
	active_used = used;
	active_limit = limit;
	active_failed = 0;
}

int sdk_webp_alloc_failed(void)
{
	return active_failed;
}

void *sdk_webp_malloc(size_t size)
{
	struct sdk_webp_alloc_header *header;
	size_t total;

	if (size > SIZE_MAX - sizeof(*header))
		goto fail;
	total = sizeof(*header) + size;
	if (active_used && (total > active_limit ||
	                    *active_used > active_limit - total))
		goto fail;
	if (smp_cpu_id() == 1 &&
	    sdk_decode_tracked_count() >= SDK_DECODE_MAX_TRACKED)
		goto fail;
	header = (struct sdk_webp_alloc_header *)sdk_decode_heap_alloc(total);
	if (!header)
		goto fail;
	header->size = total;
	header->used = active_used;
	if (header->used)
		*header->used += total;
	return header + 1;

fail:
	active_failed = 1;
	return 0;
}

void *sdk_webp_calloc(size_t count, size_t size)
{
	void *ptr;

	if (count != 0U && size > SIZE_MAX / count) {
		active_failed = 1;
		return 0;
	}
	ptr = sdk_webp_malloc(count * size);
	if (ptr)
		memset(ptr, 0, count * size);
	return ptr;
}

void sdk_webp_free(void *ptr)
{
	struct sdk_webp_alloc_header *header;

	if (!ptr)
		return;
	header = ((struct sdk_webp_alloc_header *)ptr) - 1;
	if (header->used && *header->used >= header->size)
		*header->used -= header->size;
	sdk_decode_heap_free(header);
}
