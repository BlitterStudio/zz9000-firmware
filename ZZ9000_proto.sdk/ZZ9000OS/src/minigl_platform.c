/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "minigl_platform.h"
#include "rix_drv.h"


#include <stdarg.h>
#include <stdio.h>

#if defined(__arm__) && !defined(MINIGL_MODULE) && !defined(MINIGL_HOST_TEST)
#include "xil_cache.h"
#include "xtime_l.h"
#endif

#if defined(MINIGL_MODULE)
static const zzm_graphics_hw_service_t *g_graphics;
static zzm_graphics_hw_caps_t g_graphics_caps;
static uint32_t g_graphics_owner;
static uint32_t g_pl330_channel;
static int g_pl330_claimed;
#endif

int minigl_platform_set_graphics(const zzm_graphics_hw_service_t *graphics,
                                 const zzm_graphics_hw_caps_t *caps,
                                 uint32_t owner)
{
#if defined(MINIGL_MODULE)
    if (!graphics || !caps || !owner || !graphics->caps || !graphics->csr_read ||
        !graphics->csr_write || !graphics->fifo_read || !graphics->fifo_write ||
        !graphics->fifo_write_words || !graphics->barrier)
        return 0;
    g_graphics = graphics;
    g_graphics_caps = *caps;
    g_graphics_owner = owner;
#else
    (void)graphics;
    (void)caps;
    (void)owner;
#endif
    return 1;
}

int minigl_platform_has_rasterix(void)
{
#if defined(MINIGL_MODULE)
    return g_graphics && g_graphics_caps.has_rasterix_ef &&
           g_graphics_caps.rasterix_csr_abi_major == 2U;
#elif defined(__arm__) && !defined(MINIGL_HOST_TEST)
    return 1;
#else
    return 0;
#endif
}

uint32_t minigl_platform_csr_read(void *context, uint32_t offset)
{
    uint32_t value = 0U;
    (void)context;
#if defined(MINIGL_MODULE)
    if (g_graphics && g_graphics->csr_read)
        (void)g_graphics->csr_read(offset, &value);
#else
    (void)offset;
#endif
    return value;
}

void minigl_platform_csr_write(void *context, uint32_t offset, uint32_t value)
{
    (void)context;
#if defined(MINIGL_MODULE)
    if (g_graphics && g_graphics->csr_write)
        (void)g_graphics->csr_write(offset, value);
#else
    (void)offset;
    (void)value;
#endif
}

uint32_t minigl_platform_fifo_read(void *context, uint32_t offset)
{
    uint32_t value = 0U;
    (void)context;
#if defined(MINIGL_MODULE)
    if (g_graphics && g_graphics->fifo_read)
        (void)g_graphics->fifo_read(offset, &value);
#else
    (void)offset;
#endif
    return value;
}

void minigl_platform_fifo_write(void *context, uint32_t offset, uint32_t value)
{
    (void)context;
#if defined(MINIGL_MODULE)
    if (g_graphics && g_graphics->fifo_write)
        (void)g_graphics->fifo_write(offset, value);
#else
    (void)offset;
    (void)value;
#endif
}

void minigl_platform_fifo_write_words(void *context, const uint32_t *words,
                                      uint32_t count)
{
    (void)context;
#if defined(MINIGL_MODULE)
    if (g_graphics && g_graphics->fifo_write_words)
        (void)g_graphics->fifo_write_words(words, count);
#else
    (void)words;
    (void)count;
#endif
}

void minigl_platform_cache_clean(void *context, uintptr_t address, size_t bytes)
{
    (void)context;
#if defined(MINIGL_MODULE)
    zzm_rt_dcache_clean((uint32_t)address, (uint32_t)bytes);
#elif defined(__arm__) && !defined(MINIGL_HOST_TEST)
    Xil_DCacheFlushRange((INTPTR)address, (u32)bytes);
#else
    (void)address;
    (void)bytes;
#endif
}

void minigl_platform_cache_invalidate(void *context, uintptr_t address,
                                      size_t bytes)
{
    (void)context;
#if defined(MINIGL_MODULE)
    (void)zzm_rt_dcache_invalidate((uint32_t)address, (uint32_t)bytes);
#elif defined(__arm__) && !defined(MINIGL_HOST_TEST)
    Xil_DCacheInvalidateRange((INTPTR)address, (u32)bytes);
#else
    (void)address;
    (void)bytes;
#endif
}

void minigl_platform_barrier(void *context)
{
    (void)context;
#if defined(MINIGL_MODULE)
    zzm_rt_barrier();
#elif defined(__arm__) && !defined(MINIGL_HOST_TEST)
    __asm__ volatile ("dsb sy" ::: "memory");
#endif
}

int minigl_platform_dma_stream(void *context, uintptr_t source, uint32_t words,
                               uint32_t burst_words)
{
    (void)context;
#if defined(MINIGL_MODULE)
    if (!g_graphics || !g_graphics_caps.pl330_available || !g_graphics->pl330_claim ||
        !g_graphics->pl330_start || source == 0U || words == 0U ||
        burst_words != 16U)
        return 0;
    if (!g_pl330_claimed) {
        if (!g_graphics->pl330_claim(g_graphics_owner, &g_pl330_channel))
            return 0;
        g_pl330_claimed = 1;
    }
    minigl_platform_cache_clean(0, source, (size_t)words * sizeof(uint32_t));
    minigl_platform_barrier(0);
    return g_graphics->pl330_start(g_pl330_channel, (uint32_t)source,
                                   words * sizeof(uint32_t));
#else
    (void)source;
    (void)words;
    (void)burst_words;
    return 0;
#endif
}

void minigl_platform_abort_graphics(void)
{
#if defined(MINIGL_MODULE)
    if (g_graphics) {
        if (g_pl330_claimed && g_graphics->pl330_abort)
            g_graphics->pl330_abort(g_pl330_channel);
        if (g_pl330_claimed && g_graphics->pl330_release)
            g_graphics->pl330_release(g_pl330_channel);
        g_pl330_claimed = 0;
        g_graphics->csr_write(RIX_DRV_CSR_CONTROL, 0U);
        g_graphics->fifo_write(RIX_DRV_FIFO_TDFR, RIX_DRV_FIFO_RESET_KEY);
        g_graphics->barrier();
    }
#endif
}

uint64_t minigl_platform_time_us(void)
{
#if defined(MINIGL_MODULE)
    return zzm_rt_time_us();
#elif defined(__arm__) && !defined(MINIGL_HOST_TEST)
    XTime now;
    XTime_GetTime(&now);
    return (uint64_t)now * 1000000U / COUNTS_PER_SECOND;
#else
    return 0U;
#endif
}

void minigl_platform_log(const char *message)
{
#if defined(MINIGL_MODULE)
    zzm_rt_log(message);
#else
    printf("%s\n", message ? message : "");
#endif
}

void minigl_platform_logf(const char *format, ...)
{
    char message[768];
    va_list args;

    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);
#if defined(MINIGL_MODULE)
    zzm_rt_log(message);
#else
    fputs(message, stdout);
#endif
}
