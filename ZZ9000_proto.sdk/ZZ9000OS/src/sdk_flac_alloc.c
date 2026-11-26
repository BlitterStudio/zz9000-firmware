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

/* The per-stream accounting and owner are selected by the caller before each
 * libFLAC call. The core-1 SDK and the core-0 audio-ring decoder run libFLAC
 * at the same time, so each core keeps its own context: one core's quota,
 * owner or failure flag never leaks into the other's allocations. A decode
 * call is never preempted by another decoder on the same core. */
struct flac_alloc_ctx {
	size_t *used;
	size_t *peak;
	size_t limit;
	uint32_t owner;
	int failed;
};

static struct flac_alloc_ctx g_ctx[2];

static struct flac_alloc_ctx *ctx(void)
{
	return &g_ctx[(unsigned)smp_cpu_id() & 1U];
}

void sdk_flac_alloc_set_quota(size_t *used, size_t *peak, size_t limit)
{
	struct flac_alloc_ctx *c = ctx();

	c->used = used;
	c->peak = peak;
	c->limit = limit;
	c->failed = 0;
}

void sdk_flac_alloc_set_owner(uint32_t owner)
{
	ctx()->owner = owner;
}

int sdk_flac_alloc_failed(void)
{
	return ctx()->failed;
}

void *sdk_flac_malloc(size_t size)
{
	struct flac_alloc_ctx *c = ctx();
	union sdk_flac_alloc_header *header;
	size_t total;

	if (size > SIZE_MAX - sizeof(*header))
		goto fail;
	total = sizeof(*header) + size;
	if (c->used && (total > c->limit ||
	                *c->used > c->limit - total))
		goto fail;
	if (c->owner == 0U && smp_cpu_id() == 1 &&
	    sdk_decode_tracked_count() >= SDK_DECODE_MAX_TRACKED)
		goto fail;
	header = (union sdk_flac_alloc_header *)sdk_decode_heap_alloc_owned(
		total, c->owner != 0U ? c->owner : SDK_DECODE_POOL_OWNER);
	if (!header)
		goto fail;
	header->h.size = total;
	header->h.used = c->used;
	if (header->h.used) {
		*header->h.used += total;
		if (c->peak && *header->h.used > *c->peak)
			*c->peak = *header->h.used;
	}
	return header + 1;

fail:
	c->failed = 1;
	return 0;
}

void *sdk_flac_calloc(size_t count, size_t size)
{
	void *ptr;

	if (count != 0U && size > SIZE_MAX / count) {
		ctx()->failed = 1;
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
