/*
 * libFLAC-private allocation wrappers.
 *
 * Every block comes from sdk_decode_heap_alloc(), so on core 1 it is recorded
 * in the decode tracker and reclaimed by a core-1 cold restart. A small
 * header carries the size so realloc can be served without the platform
 * realloc (which would bypass tracking) and so the owning stream's quota and
 * peak can be maintained.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_flac_alloc.h"
#include "sdk_compression.h"
#include "sdk_decode_reclaim.h"
#include "sdk_smp_lock.h"
#include <stdint.h>

union sdk_flac_alloc_header {
	struct {
		size_t size;
		size_t *used;
	} h;
	long long align_ll;
	double align_d;
};

static size_t *active_used;
static size_t *active_peak;
static size_t active_limit;
static int active_failed;

void sdk_flac_alloc_set_quota(size_t *used, size_t *peak, size_t limit)
{
	active_used = used;
	active_peak = peak;
	active_limit = limit;
	active_failed = 0;
}

int sdk_flac_alloc_failed(void)
{
	return active_failed;
}

void *sdk_flac_malloc(size_t size)
{
	union sdk_flac_alloc_header *header;
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
	header = (union sdk_flac_alloc_header *)sdk_decode_heap_alloc(total);
	if (!header)
		goto fail;
	header->h.size = total;
	header->h.used = active_used;
	if (header->h.used) {
		*header->h.used += total;
		if (active_peak && *header->h.used > *active_peak)
			*active_peak = *header->h.used;
	}
	return header + 1;

fail:
	active_failed = 1;
	return 0;
}

void *sdk_flac_calloc(size_t count, size_t size)
{
	void *ptr;

	if (count != 0U && size > SIZE_MAX / count) {
		active_failed = 1;
		return 0;
	}
	ptr = sdk_flac_malloc(count * size);
	if (ptr)
		memset(ptr, 0, count * size);
	return ptr;
}

void sdk_flac_free(void *ptr)
{
	union sdk_flac_alloc_header *header;

	if (!ptr)
		return;
	header = ((union sdk_flac_alloc_header *)ptr) - 1;
	if (header->h.used && *header->h.used >= header->h.size)
		*header->h.used -= header->h.size;
	sdk_decode_heap_free(header);
}

void *sdk_flac_realloc(void *ptr, size_t size)
{
	union sdk_flac_alloc_header *header;
	size_t old_size;
	void *fresh;

	if (!ptr)
		return sdk_flac_malloc(size);
	if (size == 0U) {
		/* POSIX realloc(ptr, 0) semantics libFLAC relies on. */
		sdk_flac_free(ptr);
		return 0;
	}
	header = ((union sdk_flac_alloc_header *)ptr) - 1;
	old_size = header->h.size - sizeof(*header);
	fresh = sdk_flac_malloc(size);
	if (!fresh)
		return 0;	/* original block stays valid, as realloc */
	memcpy(fresh, ptr, old_size < size ? old_size : size);
	sdk_flac_free(ptr);
	return fresh;
}
