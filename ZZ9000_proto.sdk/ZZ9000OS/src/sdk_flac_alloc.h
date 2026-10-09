/*
 * libFLAC-private allocation wrappers (force-included into every libFLAC
 * translation unit by build_flac.sh).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_FLAC_ALLOC_H
#define SDK_FLAC_ALLOC_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

void *sdk_flac_malloc(size_t size);
void *sdk_flac_calloc(size_t count, size_t size);
void *sdk_flac_realloc(void *ptr, size_t size);
void sdk_flac_free(void *ptr);

/* Select the per-stream accounting words before every libFLAC call. used and
 * peak may be NULL (unaccounted); limit bounds *used. */
void sdk_flac_alloc_set_quota(size_t *used, size_t *peak, size_t limit);
/* Nonzero when the current allocator context rejected an allocation. */
int sdk_flac_alloc_failed(void);

#ifdef SDK_FLAC_REPLACE_ALLOCATORS
#define malloc(size) sdk_flac_malloc(size)
#define calloc(count, size) sdk_flac_calloc((count), (size))
#define realloc(ptr, size) sdk_flac_realloc((ptr), (size))
#define free(ptr) sdk_flac_free(ptr)
#endif

#endif /* SDK_FLAC_ALLOC_H */
