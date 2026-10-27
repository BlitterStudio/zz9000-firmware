#ifndef ZZM_WEBP_ALLOC_H
#define ZZM_WEBP_ALLOC_H

#include "zzm_runtime.h"
#include "sdk_webp_module.h"

void zzm_webp_alloc_init(const zzm_services_t *services);
void zzm_webp_alloc_select(struct zzm_webp_arena *arena);
int zzm_webp_alloc_failed(void);
void zzm_webp_alloc_release(struct zzm_webp_arena *arena);
void *sdk_webp_malloc(size_t size);
void *sdk_webp_calloc(size_t count, size_t size);
void sdk_webp_free(void *ptr);

#endif /* ZZM_WEBP_ALLOC_H */
