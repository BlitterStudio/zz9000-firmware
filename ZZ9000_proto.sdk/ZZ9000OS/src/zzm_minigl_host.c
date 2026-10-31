/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "zzm_minigl_host.h"

#include <string.h>

#if !defined(TASKQ_HOST_TEST)
#include "xil_cache.h"
#include "xil_io.h"
#include "xil_mmu.h"
#endif

#define ZZM_RIX_CSR_BASE 0x83c60000U
#define ZZM_RIX_FIFO_BASE 0x83c70000U
#define ZZM_RIX_AFI3_BASE 0xf800b000U
#define ZZM_RIX_AFI_RD_CTRL 0x00U
#define ZZM_RIX_AFI_WR_CTRL 0x14U
#define ZZM_RIX_CSR_IDENTITY 0x00U
#define ZZM_RIX_CSR_VERSION 0x04U
#define ZZM_RIX_IDENTITY 0x52495831U
#define ZZM_RIX_CSR_ABI_V2 0x00020000U

static const zzm_minigl_ops_v1_t *g_ops;
static const zzm_minigl_worker_ops_t *g_worker;
static void *g_context;
static int g_poll_active;
static int g_reset_active;
static int g_pl330_claimed;
static zzm_minigl_boot_caps_t g_caps;

#if !defined(TASKQ_HOST_TEST)
static uint32_t graphics_csr_read(void *context, uint32_t offset)
{
    (void)context;
    return Xil_In32(ZZM_RIX_CSR_BASE + offset);
}

static void graphics_csr_write(void *context, uint32_t offset, uint32_t value)
{
    (void)context;
    Xil_Out32(ZZM_RIX_CSR_BASE + offset, value);
}

static uint32_t graphics_fifo_read(void *context, uint32_t offset)
{
    (void)context;
    return Xil_In32(ZZM_RIX_FIFO_BASE + offset);
}

static void graphics_fifo_write(void *context, uint32_t offset, uint32_t value)
{
    (void)context;
    Xil_Out32(ZZM_RIX_FIFO_BASE + offset, value);
}

static void graphics_cache_clean(void *context, uintptr_t address, size_t bytes)
{
    (void)context;
    Xil_DCacheFlushRange((INTPTR)address, (u32)bytes);
}

static void graphics_cache_invalidate(void *context, uintptr_t address, size_t bytes)
{
    (void)context;
    Xil_DCacheInvalidateRange((INTPTR)address, (u32)bytes);
}

static void graphics_barrier(void *context)
{
    (void)context;
    __asm__ volatile("dsb sy" ::: "memory");
}
#endif

/* PL330 programming is module-owned but the firmware reserves its channel and
 * retains the sole emergency-abort authority.  The first module revision uses
 * MMIO FIFO writes when no claimed PL330 implementation is provided. */
static int graphics_pl330_start(void *context, uintptr_t source, uint32_t words,
                                uint32_t burst_words)
{
    (void)context;
    (void)source;
    (void)words;
    (void)burst_words;
    return 0;
}

static void graphics_pl330_abort(void *context)
{
    (void)context;
    g_pl330_claimed = 0;
}

static void graphics_pl330_release(void *context)
{
    (void)context;
    g_pl330_claimed = 0;
}

static const zzm_graphics_hw_t g_graphics_hw = {
    ZZM_GRAPHICS_HW_V1, sizeof(g_graphics_hw), &g_caps, 0,
#if !defined(TASKQ_HOST_TEST)
    graphics_csr_read, graphics_csr_write, graphics_fifo_read, graphics_fifo_write,
#else
    0, 0, 0, 0,
#endif
    graphics_pl330_start, graphics_pl330_abort, graphics_pl330_release,
#if !defined(TASKQ_HOST_TEST)
    graphics_cache_clean, graphics_cache_invalidate, graphics_barrier
#else
    0, 0, 0
#endif
};

static int table_has(uint32_t size, size_t end)
{
    return size >= end;
}

int zzm_minigl_register(const zzm_minigl_ops_v1_t *ops, void *context,
                        const zzm_minigl_worker_ops_t *worker_ops)
{
    if (ops == 0 || worker_ops == 0 || ops->version != ZZM_MINIGL_OPS_V1 ||
        worker_ops->version != ZZM_MINIGL_WORKER_OPS_V1 ||
        !table_has(ops->size, offsetof(zzm_minigl_ops_v1_t, fini) + sizeof(ops->fini)) ||
        !table_has(worker_ops->size, offsetof(zzm_minigl_worker_ops_t, poll) +
                                      sizeof(worker_ops->poll)) ||
        worker_ops->has_work == 0 || worker_ops->poll == 0) {
        return 0;
    }
    if (g_ops != 0)
        return 0;
    g_ops = ops;
    g_worker = worker_ops;
    g_context = context;
    g_reset_active = 0;
    return 1;
}

void zzm_minigl_unregister(void)
{
    if (g_poll_active)
        return;
    g_worker = 0;
    g_ops = 0;
    g_context = 0;
    g_reset_active = 0;
}

const zzm_minigl_ops_v1_t *zzm_minigl_ops(void **context)
{
    if (context)
        *context = g_context;
    return g_ops;
}

int zzm_minigl_worker_registered(void)
{
    return g_worker != 0;
}

int zzm_minigl_has_work(void)
{
    return g_worker != 0 && g_worker->has_work(g_context);
}

void zzm_minigl_doorbell(void)
{
    if (g_ops && g_ops->doorbell)
        g_ops->doorbell(g_context);
}

int zzm_minigl_transport_acquire(uint32_t *generation)
{
    if (!g_ops || !g_ops->transport_acquire)
        return 0;
    return g_ops->transport_acquire(g_context, generation);
}

int zzm_minigl_transport_release(void)
{
    if (!g_ops || !g_ops->transport_release)
        return 0;
    return g_ops->transport_release(g_context);
}

void zzm_minigl_service_tick(int in_api)
{
    if (in_api || !g_worker || g_poll_active || !g_worker->has_work(g_context))
        return;
    g_poll_active = 1;
    (void)g_worker->poll(g_context, ZZM_MINIGL_POLL_BUDGET);
    g_poll_active = 0;
}

int zzm_minigl_begin_reset(void)
{
    if (g_reset_active)
        return ZZM_MINIGL_RESET_BUSY;
    if (!g_ops || !g_ops->begin_reset)
        return ZZM_MINIGL_RESET_QUIESCENT;
    g_reset_active = 1;
    return g_ops->begin_reset(g_context);
}

int zzm_minigl_reset_poll(void)
{
    int result;
    if (!g_reset_active)
        return ZZM_MINIGL_RESET_QUIESCENT;
    if (!g_ops || !g_ops->reset_poll)
        return ZZM_MINIGL_RESET_FAILED;
    result = g_ops->reset_poll(g_context);
    if (result != ZZM_MINIGL_RESET_BUSY)
        g_reset_active = 0;
    return result;
}

void zzm_minigl_abort_reset(void)
{
    if (g_ops && g_ops->abort_reset)
        g_ops->abort_reset(g_context);
    g_reset_active = 0;
    graphics_pl330_abort(0);
}

void zzm_minigl_on_core1_fault(void)
{
    /* Never call module code on the faulting stack. */
    g_worker = 0;
    g_ops = 0;
    g_context = 0;
    g_reset_active = 0;
    g_poll_active = 0;
    graphics_pl330_abort(0);
}

void zzm_minigl_boot_caps_init(uint32_t image_flavour)
{
    memset(&g_caps, 0, sizeof(g_caps));
    g_caps.version = ZZM_GRAPHICS_HW_V1;
    g_caps.size = sizeof(g_caps);
    g_caps.image_flavour = image_flavour;
    if (image_flavour != ZZM_MINIGL_IMAGE_RASTERIX_EF)
        return;
#if !defined(TASKQ_HOST_TEST)
    /* AFI3 is widened only in the RasterIX EF image.  Do not read its CSR
     * window until the selected image has passed this firmware-owned gate. */
    Xil_Out32(ZZM_RIX_AFI3_BASE + ZZM_RIX_AFI_RD_CTRL,
              Xil_In32(ZZM_RIX_AFI3_BASE + ZZM_RIX_AFI_RD_CTRL) & ~1U);
    Xil_Out32(ZZM_RIX_AFI3_BASE + ZZM_RIX_AFI_WR_CTRL,
              Xil_In32(ZZM_RIX_AFI3_BASE + ZZM_RIX_AFI_WR_CTRL) & ~1U);
    g_caps.afi3_64bit = 1U;
    if (Xil_In32(ZZM_RIX_CSR_BASE + ZZM_RIX_CSR_IDENTITY) == ZZM_RIX_IDENTITY) {
        g_caps.rasterix_csr_abi = Xil_In32(ZZM_RIX_CSR_BASE + ZZM_RIX_CSR_VERSION);
        g_caps.has_rasterix_ef = (g_caps.rasterix_csr_abi >= ZZM_RIX_CSR_ABI_V2);
    }
#endif
}

const zzm_minigl_boot_caps_t *zzm_minigl_boot_caps(void)
{
    return &g_caps;
}

const zzm_graphics_hw_t *zzm_minigl_graphics_hw(void)
{
    return g_caps.has_rasterix_ef ? &g_graphics_hw : 0;
}
