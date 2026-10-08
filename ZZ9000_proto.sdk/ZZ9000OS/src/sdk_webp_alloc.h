/*
 * WebP-private allocation wrappers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_WEBP_ALLOC_H
#define SDK_WEBP_ALLOC_H

#include <stddef.h>
#include <stdlib.h>

void *sdk_webp_malloc(size_t size);
void *sdk_webp_calloc(size_t count, size_t size);
void sdk_webp_free(void *ptr);

/* The image session selects its quota before calling libwebp. */
void sdk_webp_alloc_set_quota(size_t *used, size_t limit);
/* Nonzero when the current allocator context rejected an allocation. */
int sdk_webp_alloc_failed(void);

#ifdef SDK_WEBP_REPLACE_ALLOCATORS
#define malloc(size) sdk_webp_malloc(size)
#define calloc(count, size) sdk_webp_calloc((count), (size))
#define free(ptr) sdk_webp_free(ptr)
#endif

#endif /* SDK_WEBP_ALLOC_H */
