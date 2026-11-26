/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Decode-backend allocation wrappers over the card pool. The owner map is in
 * sdk_compression.h. This unit needs only the card pool, so the host tests
 * run the production wrappers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#include "sdk_compression.h"
#include "card_pool.h"

/* Every large decode backend gets its own card-pool block. Keeping the
 * allocation header immediately before the returned pointer makes free safe
 * without consulting newlib metadata after a worker reset. */
#define DECODE_POOL_MAGIC 0x44454350u
#define DECODE_POOL_OWNER SDK_DECODE_POOL_OWNER

struct decode_pool_header {
	uint32_t magic;
	uint32_t base;
	uint32_t size;
	uint32_t owner;
};

void *sdk_decode_heap_alloc_owned(size_t size, uint32_t owner)
{
	struct decode_pool_header *header;
	uint32_t base;

	if (size == 0U || size > UINT32_MAX - sizeof(*header) || owner == 0U)
		return 0;
	base = card_pool_alloc(&card_pool, (uint32_t)size + sizeof(*header),
	                       owner);
	if (!base)
		return 0;
	header = (struct decode_pool_header *)(uintptr_t)base;
	header->magic = DECODE_POOL_MAGIC;
	header->base = base;
	header->size = (uint32_t)size;
	header->owner = owner;
	return header + 1;
}

void *sdk_decode_heap_alloc(size_t size)
{
	return sdk_decode_heap_alloc_owned(size, DECODE_POOL_OWNER);
}

/* A block whose pool range was revoked (the lent fast-RAM window an Amiga
 * reset takes back) must not be freed through this wrapper: the header is
 * the Amiga's memory by then. Owners tear their decoders down before the
 * revocation; see audio_fabric_codec_reset_quiesce(). */
void sdk_decode_heap_free(void *ptr)
{
	struct decode_pool_header *header;

	if (!ptr)
		return;
	header = ((struct decode_pool_header *)ptr) - 1;
	if (header->magic == DECODE_POOL_MAGIC) {
		(void)card_pool_free(&card_pool, header->base, header->owner);
		return;
	}
	free(ptr);
}

void *sdk_decode_heap_realloc(void *ptr, size_t size)
{
	struct decode_pool_header *header;
	void *fresh;
	size_t copy;

	if (!ptr)
		return sdk_decode_heap_alloc(size);
	if (size == 0U) {
		sdk_decode_heap_free(ptr);
		return 0;
	}
	header = ((struct decode_pool_header *)ptr) - 1;
	if (header->magic != DECODE_POOL_MAGIC)
		return realloc(ptr, size);
	fresh = sdk_decode_heap_alloc(size);
	if (!fresh)
		return 0;
	copy = header->size < size ? header->size : size;
	memcpy(fresh, ptr, copy);
	sdk_decode_heap_free(ptr);
	return fresh;
}

void *sdk_decode_heap_calloc(size_t count, size_t size)
{
	void *ptr;

	if (count != 0U && size > SIZE_MAX / count)
		return 0;
	ptr = sdk_decode_heap_alloc(count * size);
	if (ptr)
		memset(ptr, 0, count * size);
	return ptr;
}
