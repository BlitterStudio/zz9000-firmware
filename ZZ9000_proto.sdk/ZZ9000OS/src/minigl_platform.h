/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MINIGL_PLATFORM_H
#define MINIGL_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "zzm_runtime.h"

/* The module entry injects the firmware-owned graphics table.  Resident
 * firmware uses the same callbacks through its local hardware adapter. */
int minigl_platform_set_graphics(const zzm_graphics_hw_service_t *graphics,
                                 const zzm_graphics_hw_caps_t *caps,
                                 uint32_t owner);
int minigl_platform_has_rasterix(void);
/* FrameStreamingCore memory fetch is part of every supported RasterIX EF
 * image.  Apps may force the inline FIFO path for an A/B comparison. */
void minigl_platform_set_memfetch_enabled(int enabled);
int minigl_platform_memfetch_enabled(void);
uint32_t minigl_platform_csr_read(void *context, uint32_t offset);
void minigl_platform_csr_write(void *context, uint32_t offset, uint32_t value);
uint32_t minigl_platform_fifo_read(void *context, uint32_t offset);
void minigl_platform_fifo_write(void *context, uint32_t offset, uint32_t value);
void minigl_platform_fifo_write_words(void *context, const uint32_t *words,
                                      uint32_t count);
void minigl_platform_cache_clean(void *context, uintptr_t address, size_t bytes);
void minigl_platform_cache_invalidate(void *context, uintptr_t address,
                                      size_t bytes);

void minigl_platform_barrier(void *context);
void minigl_platform_publish_fence(uint32_t address, uint32_t sequence);
int minigl_platform_dma_stream(void *context, uintptr_t source, uint32_t words,
                               uint32_t burst_words);
int minigl_platform_dma_start_async(void *context, uintptr_t source,
                                    uint32_t words, uint32_t burst_words);
int minigl_platform_dma_retire(void *context);
int minigl_platform_dma_complete(void *context);
int minigl_platform_dma_start_nonblocking(void *context, uintptr_t source,
                                          uint32_t words, uint32_t burst_words);
/* Claims the DMAC channel unless already held: at module start and at each
 * RasterIX context bind, since a stop releases it without claiming it back. */
int minigl_platform_dma_claim(void);
int minigl_platform_dma_ready(void *context);
int minigl_platform_dma_abort(void *context);
/* Stops the DMAC channel and RasterIX and releases the channel.  Nonzero
 * once both are stopped; it does not depend on claiming the channel again. */
int minigl_platform_abort_graphics(void);
uint64_t minigl_platform_time_us(void);
void minigl_platform_log(const char *message);
void minigl_platform_logf(const char *format, ...);

#endif /* MINIGL_PLATFORM_H */
