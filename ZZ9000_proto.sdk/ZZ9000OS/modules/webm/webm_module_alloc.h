#ifndef ZZM_WEBM_MODULE_ALLOC_H
#define ZZM_WEBM_MODULE_ALLOC_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#include "sdk_vorbis_alloc.h"
#include "zzm_runtime.h"

void webm_module_alloc_init(const zzm_services_t *services);
void webm_module_heap_init(struct sdk_vorbis_heap *heap, uint32_t owner);

void *__wrap_malloc(size_t size);
void __wrap_free(void *ptr);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *ptr, size_t size);
void *__wrap_memalign(size_t alignment, size_t size);
int __wrap_posix_memalign(void **memptr, size_t alignment, size_t size);
void *__wrap_aligned_alloc(size_t alignment, size_t size);

#endif /* ZZM_WEBM_MODULE_ALLOC_H */
