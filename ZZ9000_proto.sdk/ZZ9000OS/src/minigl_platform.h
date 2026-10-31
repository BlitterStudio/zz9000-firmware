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
int minigl_platform_dma_stream(void *context, uintptr_t source, uint32_t words,
                               uint32_t burst_words);
void minigl_platform_abort_graphics(void);
uint64_t minigl_platform_time_us(void);
void minigl_platform_log(const char *message);
void minigl_platform_logf(const char *format, ...);

#endif /* MINIGL_PLATFORM_H */
