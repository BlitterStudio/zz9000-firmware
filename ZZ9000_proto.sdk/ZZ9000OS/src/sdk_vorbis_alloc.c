/*
 * libogg/Tremor-private allocation arena; see sdk_vorbis_alloc.h.
 *
 * An arena with a card pool owner takes its regions from the card pool,
 * which reclaims them by owner. Otherwise every region comes from
 * sdk_decode_heap_alloc(), so on core 1 it is recorded in the decode
 * tracker and reclaimed by a core-1 cold restart.
 * Inside a region, blocks carry an 8-byte boundary tag (size with an in-use
 * bit, size of the previous block); free neighbours are always coalesced and
 * an all-free region is returned to its source immediately.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_vorbis_alloc.h"
#include "sdk_compression.h"
#include "sdk_decode_reclaim.h"
#include "sdk_smp_lock.h"
#include "card_pool.h"

#define BLK_HDR  8U
#define BLK_USED 1U
#define BLK_MIN  16U

struct blk {
	uint32_t size;      /* bytes including this header; bit 0: in use */
	uint32_t prev;      /* size of the preceding block, 0 for the first */
};

static struct sdk_vorbis_heap *active;
static jmp_buf *active_fail;

static struct blk *blk_at(uint8_t *base, uint32_t off)
{
	return (struct blk *)(void *)(base + off);
}

static uint32_t blk_size(const struct blk *b)
{
	return b->size & ~BLK_USED;
}

static void *alloc_fail(void)
{
	if (active_fail)
		longjmp(*active_fail, 1);
	return 0;
}

static int region_of(const struct sdk_vorbis_heap *h, const void *p)
{
	const uint8_t *q = (const uint8_t *)p;
	unsigned r;

	for (r = 0U; r < SDK_VORBIS_HEAP_MAX_REGIONS; r++)
		if (h->region[r] && q >= h->region[r] &&
		    q < h->region[r] + h->region_bytes[r])
			return (int)r;
	return -1;
}

static void region_drop(struct sdk_vorbis_heap *h, unsigned r)
{
	if (h->pool_owner)
		card_pool_free(&card_pool, (uint32_t)(uintptr_t)h->region[r],
		               h->pool_owner);
	else
		sdk_decode_heap_free(h->region[r]);
	h->used -= h->region_bytes[r];
	h->region[r] = 0;
	h->region_bytes[r] = 0U;
	h->region_hint[r] = 0U;
}

/* Add a region with one free block of at least need bytes. Small blocks
 * share default-size regions. A block of half a region or more gets a
 * region of its own size: packed with others it would strand the tail of
 * a default region (two 3.4 MB VP9 frames in an 8 MB region leave 1.2 MB
 * that no frame fits), and its region is returned as soon as it is freed.
 * The last region may be smaller than the default, so the whole limit is
 * usable rather than only whole default regions. */
static int region_add(struct sdk_vorbis_heap *h, uint32_t need)
{
	uint32_t bytes = need + BLK_HDR;   /* + end sentinel */
	struct blk *b, *end;
	uint8_t *base;
	unsigned r;

	for (r = 0U; r < SDK_VORBIS_HEAP_MAX_REGIONS; r++)
		if (!h->region[r])
			break;
	if (r == SDK_VORBIS_HEAP_MAX_REGIONS ||
	    bytes > h->limit || h->used > h->limit - bytes)
		return -1;
	if (bytes < h->region_size && need < h->region_size / 2U) {
		bytes = h->region_size;
		if (bytes > h->limit - h->used)
			bytes = (h->limit - h->used) & ~7U;
	}
	if (h->pool_owner) {
		base = (uint8_t *)(uintptr_t)card_pool_alloc(&card_pool, bytes,
		                                             h->pool_owner);
	} else {
		if (smp_cpu_id() == 1 &&
		    sdk_decode_tracked_count() >= SDK_DECODE_MAX_TRACKED)
			return -1;
		base = (uint8_t *)sdk_decode_heap_alloc(bytes);
	}
	if (!base)
		return -1;
	b = blk_at(base, 0U);
	b->size = bytes - BLK_HDR;
	b->prev = 0U;
	end = blk_at(base, bytes - BLK_HDR);
	end->size = BLK_USED;              /* size 0, in use: stops scans */
	end->prev = b->size;
	h->region[r] = base;
	h->region_bytes[r] = bytes;
	h->region_hint[r] = 0U;
	h->used += bytes;
	if (h->used > h->peak)
		h->peak = h->used;
	return (int)r;
}

/* Mark the free block at off in use, splitting off a free tail. */
static void *take(uint8_t *base, uint32_t off, uint32_t need)
{
	struct blk *b = blk_at(base, off);
	uint32_t size = blk_size(b);

	if (size - need >= BLK_MIN) {
		struct blk *rest = blk_at(base, off + need);

		/* The old successor is in use: free blocks never touch. */
		rest->size = size - need;
		rest->prev = need;
		blk_at(base, off + size)->prev = rest->size;
		b->size = need;
	}
	b->size |= BLK_USED;
	return b + 1;
}

static uint32_t block_need(size_t size)
{
	uint32_t need = (((uint32_t)size + 7U) & ~7U) + BLK_HDR;

	return need < BLK_MIN ? BLK_MIN : need;
}

void *sdk_vorbis_malloc(size_t size)
{
	struct sdk_vorbis_heap *h = active;
	uint32_t need;
	unsigned r;
	int slot;

	if (!h || size > h->limit)
		return alloc_fail();
	need = block_need(size);
	for (r = 0U; r < SDK_VORBIS_HEAP_MAX_REGIONS; r++) {
		uint8_t *base = h->region[r];
		uint32_t off, first_free = UINT32_MAX;

		if (!base)
			continue;
		for (off = h->region_hint[r];;) {
			struct blk *b = blk_at(base, off);
			uint32_t s = blk_size(b);

			if (s == 0U)
				break;
			if ((b->size & BLK_USED) == 0U) {
				if (first_free == UINT32_MAX)
					first_free = off;
				if (s >= need) {
					void *p = take(base, off, need);

					h->region_hint[r] = first_free == off ?
					    off + blk_size(b) : first_free;
					return p;
				}
			}
			off += s;
		}
		h->region_hint[r] = first_free == UINT32_MAX ? off : first_free;
	}
	slot = region_add(h, need);
	if (slot < 0)
		return alloc_fail();
	{
		uint8_t *base = h->region[slot];
		void *p = take(base, 0U, need);

		h->region_hint[slot] = blk_size(blk_at(base, 0U));
		return p;
	}
}

void *sdk_vorbis_calloc(size_t count, size_t size)
{
	void *ptr;

	if (count != 0U && size > SIZE_MAX / count)
		return alloc_fail();
	ptr = sdk_vorbis_malloc(count * size);
	if (ptr)
		memset(ptr, 0, count * size);
	return ptr;
}

void sdk_vorbis_free(void *ptr)
{
	struct sdk_vorbis_heap *h = active;
	struct blk *b, *next;
	uint8_t *base;
	uint32_t off, size;
	int r;

	if (!ptr || !h)
		return;
	b = (struct blk *)ptr - 1;
	r = region_of(h, b);
	if (r < 0)
		return;
	base = h->region[r];
	off = (uint32_t)((uint8_t *)b - base);
	size = blk_size(b);
	next = blk_at(base, off + size);
	if ((next->size & BLK_USED) == 0U)
		size += next->size;
	if (b->prev != 0U) {
		struct blk *prev = blk_at(base, off - b->prev);

		if ((prev->size & BLK_USED) == 0U) {
			off -= b->prev;
			size += prev->size;
			b = prev;
		}
	}
	b->size = size;
	blk_at(base, off + size)->prev = size;
	if (off < h->region_hint[r])
		h->region_hint[r] = off;
	if (off == 0U && blk_size(blk_at(base, size)) == 0U)
		region_drop(h, (unsigned)r);
}

void *sdk_vorbis_realloc(void *ptr, size_t size)
{
	struct sdk_vorbis_heap *h = active;
	struct blk *b, *next;
	uint8_t *base;
	uint32_t off, cur, need;
	int r;

	if (!ptr)
		return sdk_vorbis_malloc(size);
	if (size == 0U) {
		sdk_vorbis_free(ptr);
		return 0;
	}
	if (!h || size > h->limit)
		return alloc_fail();
	b = (struct blk *)ptr - 1;
	r = region_of(h, b);
	if (r < 0)
		return alloc_fail();
	base = h->region[r];
	off = (uint32_t)((uint8_t *)b - base);
	cur = blk_size(b);
	need = block_need(size);
	if (cur < need) {
		next = blk_at(base, off + cur);
		if ((next->size & BLK_USED) != 0U || cur + next->size < need) {
			void *fresh = sdk_vorbis_malloc(size);

			if (!fresh)
				return 0;	/* original block stays valid */
			memcpy(fresh, ptr, cur - BLK_HDR);
			sdk_vorbis_free(ptr);
			return fresh;
		}
		/* Grow in place into the free successor. If the scan hint
		 * pointed at it, move it past the grown block: it must stay
		 * on a block boundary. */
		if (h->region_hint[r] == off + cur)
			h->region_hint[r] = off + cur + next->size;
		cur += next->size;
		b->size = cur | BLK_USED;
		blk_at(base, off + cur)->prev = cur;
	}
	if (cur - need >= BLK_MIN) {
		struct blk *rest = blk_at(base, off + need);
		uint32_t rest_size = cur - need;

		next = blk_at(base, off + cur);
		if ((next->size & BLK_USED) == 0U)
			rest_size += next->size;
		b->size = need | BLK_USED;
		rest->size = rest_size;
		rest->prev = need;
		blk_at(base, off + need + rest_size)->prev = rest_size;
		if (off + need < h->region_hint[r])
			h->region_hint[r] = off + need;
	}
	return ptr;
}

void sdk_vorbis_heap_init(struct sdk_vorbis_heap *heap, uint32_t region_size,
                          uint32_t limit, uint32_t pool_owner)
{
	memset(heap, 0, sizeof(*heap));
	heap->region_size = (region_size + 7U) & ~7U;
	heap->limit = limit;
	heap->pool_owner = pool_owner;
}

void sdk_vorbis_heap_select(struct sdk_vorbis_heap *heap, jmp_buf *fail)
{
	active = heap;
	active_fail = fail;
}

void sdk_vorbis_heap_release(struct sdk_vorbis_heap *heap)
{
	unsigned r;

	if (!heap)
		return;
	for (r = 0U; r < SDK_VORBIS_HEAP_MAX_REGIONS; r++)
		if (heap->region[r])
			region_drop(heap, r);
	if (active == heap) {
		active = 0;
		active_fail = 0;
	}
}

void sdk_vorbis_heap_forget(struct sdk_vorbis_heap *heap)
{
	if (!heap)
		return;
	memset(heap->region, 0, sizeof(heap->region));
	memset(heap->region_bytes, 0, sizeof(heap->region_bytes));
	memset(heap->region_hint, 0, sizeof(heap->region_hint));
	heap->used = 0U;
	if (active == heap) {
		active = 0;
		active_fail = 0;
	}
}

unsigned sdk_vorbis_heap_regions(const struct sdk_vorbis_heap *heap)
{
	unsigned r, n = 0U;

	for (r = 0U; r < SDK_VORBIS_HEAP_MAX_REGIONS; r++)
		if (heap->region[r])
			n++;
	return n;
}
