/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "zzm_minigl_host.h"
#include "zzm_format.h"
#include "zzm_runtime.h"
#include "memorymap.h"
#include "sdk_aperture_layout.h"


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

#define ZZM_RIX_DMAC_BASE 0xf8004000U
#define ZZM_RIX_DMAC_CHANNEL 6U
#define ZZM_RIX_DMAC_CSR(channel) (0x100U + (channel) * 8U)
#define ZZM_RIX_DMAC_SAR(channel) (0x400U + (channel) * 0x20U)
#define ZZM_RIX_DMAC_DBGSTATUS 0xd00U
#define ZZM_RIX_DMAC_DBGCMD 0xd04U
#define ZZM_RIX_DMAC_DBGINST0 0xd08U
#define ZZM_RIX_DMAC_DBGINST1 0xd0cU
#define ZZM_RIX_DMAC_STOPPED 0U
#define ZZM_RIX_DMAC_FAULTING_COMPLETING 14U
#define ZZM_RIX_DMAC_FAULTING 15U
#define ZZM_RIX_PL330_DMAMOV 0xbcu
#define ZZM_RIX_PL330_DMALP 0x20u
#define ZZM_RIX_PL330_DMALPEND 0x28u
#define ZZM_RIX_PL330_DMALD 0x04u
#define ZZM_RIX_PL330_DMAST 0x08u
#define ZZM_RIX_PL330_DMAWMB 0x13u
#define ZZM_RIX_PL330_DMAEND 0x00u
#define ZZM_RIX_PL330_DMAGO 0xa0u
#define ZZM_RIX_PL330_DMAKILL 0x01u
#define ZZM_RIX_PL330_RD_SAR 0u
#define ZZM_RIX_PL330_RD_CCR 1u
#define ZZM_RIX_PL330_RD_DAR 2u
#define ZZM_RIX_FIFO_TDFD 0x10U
#define ZZM_RIX_DMA_MAX_WORDS 248U
#define ZZM_RIX_IDENTITY 0x52495831U
#define ZZM_RIX_CSR_ABI_V2 0x00020000U

static const zzm_minigl_ops_v1_t *g_ops;
static const zzm_minigl_worker_ops_t *g_worker;
static void *g_context;
static int g_poll_active;
static int g_doorbell_installed;
static uint32_t g_module_capabilities;
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

/* Firmware owns the PL330 program and channel.  Module code can only submit
 * an owned command buffer through the service table. */
#if !defined(TASKQ_HOST_TEST)
static uint8_t g_pl330_program[128] __attribute__((aligned(64)));

static unsigned pl330_word(uint8_t *program, unsigned offset, uint32_t value)
{
    program[offset++] = (uint8_t)value;
    program[offset++] = (uint8_t)(value >> 8U);
    program[offset++] = (uint8_t)(value >> 16U);
    program[offset++] = (uint8_t)(value >> 24U);
    return offset;
}

static uint32_t pl330_ccr(uint32_t words)
{
    return 1U | (2U << 1U) | ((words - 1U) << 4U) | (2U << 15U);
}

static void graphics_pl330_abort(void *context)
{
    (void)context;
    if (g_pl330_claimed) {
        Xil_Out32(ZZM_RIX_DMAC_BASE + ZZM_RIX_DMAC_DBGINST0,
                  (ZZM_RIX_PL330_DMAKILL << 16U) |
                  (ZZM_RIX_DMAC_CHANNEL << 8U) | 1U);
        Xil_Out32(ZZM_RIX_DMAC_BASE + ZZM_RIX_DMAC_DBGINST1, 0U);
        Xil_Out32(ZZM_RIX_DMAC_BASE + ZZM_RIX_DMAC_DBGCMD, 0U);
        graphics_barrier(0);
    }
    g_pl330_claimed = 0;
}

static int graphics_pl330_start(void *context, uintptr_t source, uint32_t words,
                                uint32_t burst_words)
{
    uint8_t *program = g_pl330_program;
    uint32_t blocks = words / 16U, tail = words % 16U, attempt;
    unsigned offset = 0U, body, store, loop;

    (void)context;
    if (!g_pl330_claimed || source == 0U || words == 0U ||
        words > ZZM_RIX_DMA_MAX_WORDS || burst_words != 16U)
        return 0;
    program[offset++] = ZZM_RIX_PL330_DMAMOV;
    program[offset++] = ZZM_RIX_PL330_RD_SAR;
    offset = pl330_word(program, offset, (uint32_t)source);
    program[offset++] = ZZM_RIX_PL330_DMAMOV;
    program[offset++] = ZZM_RIX_PL330_RD_DAR;
    offset = pl330_word(program, offset, ZZM_RIX_FIFO_BASE + ZZM_RIX_FIFO_TDFD);
    if (blocks != 0U) {
        program[offset++] = ZZM_RIX_PL330_DMAMOV;
        program[offset++] = ZZM_RIX_PL330_RD_CCR;
        offset = pl330_word(program, offset, pl330_ccr(16U));
        program[offset++] = ZZM_RIX_PL330_DMALP | 2U;
        program[offset++] = (uint8_t)(blocks - 1U);
        body = offset;
        program[offset++] = ZZM_RIX_PL330_DMALD;
        program[offset++] = ZZM_RIX_PL330_DMALP;
        program[offset++] = 15U;
        store = offset;
        program[offset++] = ZZM_RIX_PL330_DMAST;
        loop = offset;
        program[offset++] = ZZM_RIX_PL330_DMALPEND | 0x10U;
        program[offset++] = (uint8_t)(loop - store);
        loop = offset;
        program[offset++] = ZZM_RIX_PL330_DMALPEND | 0x14U;
        program[offset++] = (uint8_t)(loop - body);
    }
    if (tail != 0U) {
        program[offset++] = ZZM_RIX_PL330_DMAMOV;
        program[offset++] = ZZM_RIX_PL330_RD_CCR;
        offset = pl330_word(program, offset, pl330_ccr(tail));
        program[offset++] = ZZM_RIX_PL330_DMALD;
        program[offset++] = ZZM_RIX_PL330_DMALP;
        program[offset++] = (uint8_t)(tail - 1U);
        store = offset;
        program[offset++] = ZZM_RIX_PL330_DMAST;
        loop = offset;
        program[offset++] = ZZM_RIX_PL330_DMALPEND | 0x10U;
        program[offset++] = (uint8_t)(loop - store);
    }
    program[offset++] = ZZM_RIX_PL330_DMAWMB;
    program[offset++] = ZZM_RIX_PL330_DMAEND;
    if (offset > sizeof(g_pl330_program))
        return 0;
    Xil_DCacheFlushRange((INTPTR)program, (u32)sizeof(g_pl330_program));
    graphics_barrier(0);
    for (attempt = 0U; attempt < 100000U; ++attempt)
        if ((Xil_In32(ZZM_RIX_DMAC_BASE + ZZM_RIX_DMAC_DBGSTATUS) & 1U) == 0U)
            break;
    if (attempt == 100000U)
        return 0;
    Xil_Out32(ZZM_RIX_DMAC_BASE + ZZM_RIX_DMAC_DBGINST0,
              (ZZM_RIX_PL330_DMAGO << 16U) |
              (ZZM_RIX_DMAC_CHANNEL << 24U));
    Xil_Out32(ZZM_RIX_DMAC_BASE + ZZM_RIX_DMAC_DBGINST1,
              (uint32_t)(uintptr_t)program);
    graphics_barrier(0);
    Xil_Out32(ZZM_RIX_DMAC_BASE + ZZM_RIX_DMAC_DBGCMD, 0U);
    graphics_barrier(0);
    for (attempt = 0U; attempt < 100000U; ++attempt)
        if ((Xil_In32(ZZM_RIX_DMAC_BASE + ZZM_RIX_DMAC_DBGSTATUS) & 1U) == 0U)
            break;
    if (attempt == 100000U)
        goto kill;
    for (attempt = 0U; attempt < 1000000U; ++attempt) {
        uint32_t state = Xil_In32(ZZM_RIX_DMAC_BASE +
                                  ZZM_RIX_DMAC_CSR(ZZM_RIX_DMAC_CHANNEL)) & 15U;
        if (state == ZZM_RIX_DMAC_STOPPED)
            return Xil_In32(ZZM_RIX_DMAC_BASE +
                            ZZM_RIX_DMAC_SAR(ZZM_RIX_DMAC_CHANNEL)) ==
                   (uint32_t)source + words * sizeof(uint32_t);
        if (state == ZZM_RIX_DMAC_FAULTING_COMPLETING ||
            state == ZZM_RIX_DMAC_FAULTING)
            break;
    }
kill:
    graphics_pl330_abort(0);
    return 0;
}
#else
static void graphics_pl330_abort(void *context)
{
    (void)context;
    g_pl330_claimed = 0;
}

static int graphics_pl330_start(void *context, uintptr_t source, uint32_t words,
                                uint32_t burst_words)
{
    (void)context;
    (void)source;
    (void)words;
    (void)burst_words;
    return 0;
}
#endif

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

#if defined(ZZM_SERVICE_GRAPHICS_HW)
static int graphics_v3_caps(zzm_graphics_hw_caps_t *out)
{
    if (out == 0)
        return 0;
    out->has_rasterix_ef = g_caps.has_rasterix_ef;
    out->rasterix_csr_abi_major = g_caps.rasterix_csr_abi >> 16U;
    out->rasterix_csr_abi_minor = g_caps.rasterix_csr_abi & 0xffffU;
    out->afi3_ready = g_caps.afi3_64bit;
    out->pl330_available = g_caps.has_pl330;
    return 1;
}

static int graphics_v3_csr_read(uint32_t offset, uint32_t *value_out)
{
#if !defined(TASKQ_HOST_TEST)
    if (!g_caps.has_rasterix_ef || value_out == 0)
        return 0;
    *value_out = graphics_csr_read(0, offset);
    return 1;
#else
    (void)offset;
    (void)value_out;
    return 0;
#endif
}

static int graphics_v3_csr_write(uint32_t offset, uint32_t value)
{
#if !defined(TASKQ_HOST_TEST)
    if (!g_caps.has_rasterix_ef)
        return 0;
    graphics_csr_write(0, offset, value);
    return 1;
#else
    (void)offset;
    (void)value;
    return 0;
#endif
}

static int graphics_v3_fifo_write_words(const uint32_t *words,
                                        uint32_t word_count)
{
#if !defined(TASKQ_HOST_TEST)
    uint32_t i;
    if (!g_caps.has_rasterix_ef || words == 0 || word_count == 0)
        return 0;
    for (i = 0U; i < word_count; ++i)
        graphics_fifo_write(0, ZZM_RIX_FIFO_TDFD, words[i]);
    return 1;
#else
    (void)words;
    (void)word_count;
    return 0;
#endif
}

static int graphics_v3_pl330_claim(uint32_t owner, uint32_t *channel_out)
{
    if (!g_caps.has_pl330 || owner == 0U || channel_out == 0 ||
        g_pl330_claimed)
        return 0;
    g_pl330_claimed = 1;
    *channel_out = ZZM_RIX_DMAC_CHANNEL;
    return 1;
}

static int graphics_v3_pl330_start(uint32_t channel, uint32_t source_address,
                                   uint32_t bytes)
{
    if (channel != ZZM_RIX_DMAC_CHANNEL || (bytes & 3U) != 0U)
        return 0;
    return graphics_pl330_start(0, source_address, bytes / sizeof(uint32_t),
                                16U);
}

static void graphics_v3_pl330_abort(uint32_t channel)
{
    (void)channel;
    graphics_pl330_abort(0);
}

static void graphics_v3_pl330_release(uint32_t channel)
{
    (void)channel;
    graphics_pl330_release(0);
}

static void graphics_v3_barrier(void)
{
#if !defined(TASKQ_HOST_TEST)
    graphics_barrier(0);
#endif
}

static const zzm_graphics_hw_service_t graphics_hw_service = {
    ZZM_SERVICE_GRAPHICS_HW_V3, sizeof(graphics_hw_service),
    graphics_v3_caps, graphics_v3_csr_read, graphics_v3_csr_write,
    graphics_v3_fifo_write_words, graphics_v3_pl330_claim,
    graphics_v3_pl330_start, graphics_v3_pl330_abort,
    graphics_v3_pl330_release, graphics_v3_barrier
};
#endif

#if defined(ZZM_SERVICE_TRANSPORT)
static int transport_publish_descriptor(uint32_t module_id,
                                        const void *descriptor,
                                        uint32_t descriptor_bytes)
{
#if !defined(TASKQ_HOST_TEST)
    volatile uint8_t *slot;
    const uint8_t *source = descriptor;
    uint32_t i;

    if (module_id != ZZM_MODULE_MINIGL || descriptor == 0 ||
        descriptor_bytes == 0U || descriptor_bytes > 256U ||
        !sdk_aperture_runtime_is_zorro3())
        return 0;
    slot = (volatile uint8_t *)(uintptr_t)(SDK_MAILBOX_Z3_ADDRESS +
                                           SDK_MAILBOX_MINIGL_DESC_OFFSET);
    for (i = 0U; i < descriptor_bytes; ++i)
        slot[i] = source[i];
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
#else
    (void)module_id;
    (void)descriptor;
    (void)descriptor_bytes;
    return 0;
#endif
}

static void transport_retract_descriptor(uint32_t module_id)
{
#if !defined(TASKQ_HOST_TEST)
    volatile uint8_t *slot;
    uint32_t i;

    if (module_id != ZZM_MODULE_MINIGL)
        return;
    slot = (volatile uint8_t *)(uintptr_t)(SDK_MAILBOX_Z3_ADDRESS +
                                           SDK_MAILBOX_MINIGL_DESC_OFFSET);
    for (i = 0U; i < 256U; ++i)
        slot[i] = 0U;
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)module_id;
#endif
}

static int transport_install_doorbell(uint32_t module_id)
{
    if (module_id != ZZM_MODULE_MINIGL || !g_ops)
        return 0;
    g_doorbell_installed = 1;
    return 1;
}

static void transport_remove_doorbell(uint32_t module_id)
{
    if (module_id == ZZM_MODULE_MINIGL)
        g_doorbell_installed = 0;
}

static void transport_publish_capabilities(uint32_t module_id,
                                           uint32_t capabilities)
{
    if (module_id == ZZM_MODULE_MINIGL)
        g_module_capabilities = capabilities;
}

static const zzm_transport_service_t transport_service = {
    ZZM_SERVICE_TRANSPORT_V3, sizeof(transport_service),
    transport_publish_descriptor, transport_retract_descriptor,
    transport_install_doorbell, transport_remove_doorbell,
    transport_publish_capabilities
};
#endif

const void *zzm_minigl_host_get_service(uint32_t id, uint32_t min_version)
{
#if defined(ZZM_SERVICE_GRAPHICS_HW)
    if (id == ZZM_SERVICE_GRAPHICS_HW &&
        min_version <= ZZM_SERVICE_GRAPHICS_HW_V3)
        return &graphics_hw_service;
#endif
#if defined(ZZM_SERVICE_TRANSPORT)
    if (id == ZZM_SERVICE_TRANSPORT && min_version <= ZZM_SERVICE_TRANSPORT_V3)
        return &transport_service;
#endif
#if !defined(ZZM_SERVICE_GRAPHICS_HW) && !defined(ZZM_SERVICE_TRANSPORT)
    (void)id;
    (void)min_version;
#endif
    return 0;
}

static int table_has(uint32_t size, size_t end)
{
    return size >= end;
}

int zzm_minigl_register(const zzm_minigl_ops_v1_t *ops, void *context,
                        const zzm_minigl_worker_ops_t *worker_ops)
{
    if (ops == 0 || worker_ops == 0 || ops->version != ZZM_MINIGL_OPS_V1 ||
        worker_ops->version != ZZM_MINIGL_WORKER_OPS_V1 ||
        !table_has(ops->size, offsetof(zzm_minigl_ops_v1_t, app_reclaim) +
                              sizeof(ops->app_reclaim)) ||
        !table_has(worker_ops->size, offsetof(zzm_minigl_worker_ops_t, poll) +
                                      sizeof(worker_ops->poll)) ||
        worker_ops->has_work == 0 || worker_ops->poll == 0 ||
        ops->app_bind == 0 || ops->app_swap == 0 || ops->app_unbind == 0 ||
        ops->app_reclaim == 0) {
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

int zzm_minigl_register_ops(uint32_t module_id, const void *ops,
                            uint32_t ops_size)
{
    const zzm_minigl_registration_v1_t *registration = ops;

    if (module_id != ZZM_MODULE_MINIGL || registration == 0 ||
        ops_size < sizeof(*registration) ||
        registration->version != ZZM_MINIGL_REGISTRATION_V1 ||
        registration->size < sizeof(*registration)) {
        return 0;
    }
    return zzm_minigl_register(registration->ops, registration->context,
                               registration->worker_ops);
}

void zzm_minigl_unregister(void)
{
    if (g_poll_active)
        return;
    g_worker = 0;
    g_ops = 0;
    g_context = 0;
    g_reset_active = 0;
    g_doorbell_installed = 0;
    g_module_capabilities = 0U;
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
    if (g_doorbell_installed && g_ops && g_ops->doorbell)
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
    g_doorbell_installed = 0;
    g_module_capabilities = 0U;
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
        g_caps.has_pl330 = g_caps.has_rasterix_ef;
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
