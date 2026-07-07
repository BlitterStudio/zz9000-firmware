/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * libjpeg-turbo system memory backend routed through the SDK decode-reclaim
 * tracking wrappers, replacing the library's jmemnobs. This object is listed
 * before libjpeg.a on the link line, so it satisfies jmemmgr's
 * jpeg_get_small/large imports and the archive's jmemnobs.c.obj is never
 * pulled in (it defines nothing else).
 *
 * Why: JPEG decodes are moving onto the core-1 scheduler. A core-1 fault
 * mid-decode cannot unwind (record_fault halts the worker), so every heap
 * block the decoder holds must be in the decode-reclaim table for
 * sdk_compression_reclaim_core1_decode() to free after the cold reset --
 * the same guarantee the zlib/LZMA decompress backends already have. On
 * core 0 the wrappers are plain malloc/free.
 *
 * Behaviour mirrors jmemnobs.c exactly (no backing store; mem_available
 * honours max_memory_to_use).
 */

#define JPEG_INTERNALS
#include "jinclude.h"
#include "jpeglib.h"
#include "jmemsys.h"

#include "sdk_compression.h"

GLOBAL(void *)
jpeg_get_small(j_common_ptr cinfo, size_t sizeofobject)
{
  (void)cinfo;
  return sdk_decode_heap_alloc(sizeofobject);
}

GLOBAL(void)
jpeg_free_small(j_common_ptr cinfo, void *object, size_t sizeofobject)
{
  (void)cinfo;
  (void)sizeofobject;
  sdk_decode_heap_free(object);
}

GLOBAL(void *)
jpeg_get_large(j_common_ptr cinfo, size_t sizeofobject)
{
  (void)cinfo;
  return sdk_decode_heap_alloc(sizeofobject);
}

GLOBAL(void)
jpeg_free_large(j_common_ptr cinfo, void *object, size_t sizeofobject)
{
  (void)cinfo;
  (void)sizeofobject;
  sdk_decode_heap_free(object);
}

GLOBAL(size_t)
jpeg_mem_available(j_common_ptr cinfo, size_t min_bytes_needed,
                   size_t max_bytes_needed, size_t already_allocated)
{
  (void)min_bytes_needed;
  if (cinfo->mem->max_memory_to_use) {
    if ((size_t)cinfo->mem->max_memory_to_use > already_allocated)
      return cinfo->mem->max_memory_to_use - already_allocated;
    else
      return 0;
  } else {
    return max_bytes_needed;
  }
}

GLOBAL(void)
jpeg_open_backing_store(j_common_ptr cinfo, backing_store_ptr info,
                        long total_bytes_needed)
{
  (void)info;
  (void)total_bytes_needed;
  ERREXIT(cinfo, JERR_NO_BACKING_STORE);
}

GLOBAL(long)
jpeg_mem_init(j_common_ptr cinfo)
{
  (void)cinfo;
  return 0;
}

GLOBAL(void)
jpeg_mem_term(j_common_ptr cinfo)
{
  (void)cinfo;
}
