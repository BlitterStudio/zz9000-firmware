#ifndef ZZM_WEBM_MODULE_ALLOC_H
#define ZZM_WEBM_MODULE_ALLOC_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#include "sdk_vorbis_alloc.h"
#include "zzm_runtime.h"

void webm_module_heap_init(struct sdk_vorbis_heap *heap, uint32_t owner);

#endif /* ZZM_WEBM_MODULE_ALLOC_H */
