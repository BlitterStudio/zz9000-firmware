/*
 * libogg/Tremor-private allocation arena (force-included into every libogg
 * and Tremor translation unit by build_vorbis.sh).
 *
 * Tremor makes hundreds of small allocations (codebook tables, floor and
 * residue lookups), far more than the 64-slot core-1 decode tracker holds,
 * and it does not check most allocation results. Each stream therefore owns
 * an arena: a few large regions carved by a first-fit allocator with
 * coalescing. The regions come from the card pool when the arena has a pool
 * owner (WebM, WebP), and otherwise from sdk_decode_heap_alloc(), one
 * tracker slot each. The arena is bounded by a per-stream byte limit and
 * region count, and it is released wholesale, so Tremor's own teardown
 * never has to run.
 *
 * An allocation the arena cannot satisfy longjmp()s to the jmp_buf armed by
 * sdk_vorbis_heap_select(): the backend turns that into NO_MEMORY and drops
 * the whole arena, which is leak-free whatever state Tremor was left in.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_VORBIS_ALLOC_H
#define SDK_VORBIS_ALLOC_H
#ifndef __ASSEMBLER__
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Large blocks get regions of their own (see region_add), so a 1080p VP9
 * stream needs about ten. */
#define SDK_VORBIS_HEAP_MAX_REGIONS 16U

struct sdk_vorbis_heap {
	uint8_t *region[SDK_VORBIS_HEAP_MAX_REGIONS];
	uint32_t region_bytes[SDK_VORBIS_HEAP_MAX_REGIONS];
	/* Offset of the lowest block that may be free (a lower bound). */
	uint32_t region_hint[SDK_VORBIS_HEAP_MAX_REGIONS];
	uint32_t region_size;       /* default region size */
	uint32_t limit;             /* ceiling on tracked bytes held */
	uint32_t used;              /* tracked bytes held in regions */
	uint32_t peak;
	/* card_pool owner of the regions; 0 takes them from the decode heap. */
	uint32_t pool_owner;
};

void *sdk_vorbis_malloc(size_t size);
void *sdk_vorbis_calloc(size_t count, size_t size);
void *sdk_vorbis_realloc(void *ptr, size_t size);
void sdk_vorbis_free(void *ptr);

/* region_size: default region allocation; limit: ceiling on held bytes;
 * pool_owner: card_pool owner for the regions, or 0 for the decode heap. */
void sdk_vorbis_heap_init(struct sdk_vorbis_heap *heap, uint32_t region_size,
                          uint32_t limit, uint32_t pool_owner);
/* Route the allocator to heap (NULL: every allocation fails). When fail is
 * non-NULL an allocation failure longjmp()s to it with value 1; otherwise
 * it returns NULL. */
void sdk_vorbis_heap_select(struct sdk_vorbis_heap *heap, jmp_buf *fail);
/* Free every region (on the core that allocated them). */
void sdk_vorbis_heap_release(struct sdk_vorbis_heap *heap);
/* Drop region pointers after a core-1 reclaim (decode heap) or an owner
 * release (card pool) already freed them. */
void sdk_vorbis_heap_forget(struct sdk_vorbis_heap *heap);
/* Number of regions (tracked blocks) currently held. */
unsigned sdk_vorbis_heap_regions(const struct sdk_vorbis_heap *heap);

#ifdef SDK_VORBIS_REPLACE_ALLOCATORS
#define malloc(size) sdk_vorbis_malloc(size)
#define calloc(count, size) sdk_vorbis_calloc((count), (size))
#define realloc(ptr, size) sdk_vorbis_realloc((ptr), (size))
#define free(ptr) sdk_vorbis_free(ptr)
#endif /* SDK_VORBIS_REPLACE_ALLOCATORS */
#endif /* __ASSEMBLER__ */

#endif /* SDK_VORBIS_ALLOC_H */
