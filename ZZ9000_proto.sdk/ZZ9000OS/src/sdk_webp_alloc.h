/*
 * WebP-private allocation wrappers.
 *
 * libwebp's demuxer allocates per animation frame, far more blocks than the
 * 64-slot core-1 decode tracker holds. Every allocation of an image session
 * is therefore carved from that session's arena (the codec arena in
 * sdk_vorbis_alloc.h): a few large tracked regions, released wholesale.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_WEBP_ALLOC_H
#define SDK_WEBP_ALLOC_H

#include <stddef.h>
#include <stdlib.h>

struct sdk_vorbis_heap;

void *sdk_webp_malloc(size_t size);
void *sdk_webp_calloc(size_t count, size_t size);
void sdk_webp_free(void *ptr);

/* The image session selects its arena before every libwebp call (frees
 * included); NULL makes every allocation fail. Clears the failure flag. */
void sdk_webp_alloc_select(struct sdk_vorbis_heap *heap);
/* Nonzero when an allocation failed since the last select. */
int sdk_webp_alloc_failed(void);

#ifdef SDK_WEBP_REPLACE_ALLOCATORS
#define malloc(size) sdk_webp_malloc(size)
#define calloc(count, size) sdk_webp_calloc((count), (size))
#define free(ptr) sdk_webp_free(ptr)
#endif

#endif /* SDK_WEBP_ALLOC_H */
