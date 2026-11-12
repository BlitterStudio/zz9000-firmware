#include <assert.h>
#include <stdarg.h>
#include <stdint.h>

#include "minigl_platform.h"
#include "rix_drv.h"
#include "xil_cache_l.h"
#include "zzm_minigl_host.h"

extern const void *zzm_minigl_host_get_service(uint32_t id,
                                                uint32_t min_version);

#define RIX_CSR_BASE 0x83c60000U
#define RIX_FIFO_BASE 0x83c70000U
#define DMAC_BASE 0xf8003000U
#define SLCR_APER_CLK_CTRL 0xf800012cU
#define SLCR_DMAC_RST_CTRL 0xf800020cU
#define DMAC_DSR 0x000U
#define DMAC_CSR(channel) (0x100U + (channel) * 8U)
#define DMAC_DBGSTATUS 0xd00U
#define DMAC_DBGINST0 0xd08U
#define DMAC_CHANNEL 6U
#define PL330_DMAKILL 0x01U
#define FIFO_TDFR 0x08U
#define FIFO_RESET_KEY 0xa5U

static uint32_t debug_busy;
static uint32_t channel_state;
static unsigned kill_writes;
static unsigned fifo_resets;
static unsigned reporter_calls;
static unsigned platform_sync_aborts;
static unsigned platform_fifo_resets;

int printf(const char *format, ...)
{
    va_list args;

    (void)format;
    va_start(args, format);
    va_end(args);
    ++reporter_calls;
    return 0;
}

uint32_t Xil_In32(uintptr_t address)
{
    if (address == SLCR_APER_CLK_CTRL)
        return 1U;
    if (address == SLCR_DMAC_RST_CTRL)
        return 0U;
    if (address == DMAC_BASE + DMAC_DSR)
        return 0U;
    if (address == RIX_CSR_BASE)
        return 0x52495831U;
    if (address == RIX_CSR_BASE + 4U)
        return 0x00020000U;
    if (address == DMAC_BASE + DMAC_DBGSTATUS)
        return debug_busy;
    if (address == DMAC_BASE + DMAC_CSR(DMAC_CHANNEL))
        return channel_state;
    return 0U;
}

void Xil_Out32(uintptr_t address, uint32_t value)
{
    if (address == DMAC_BASE + DMAC_DBGINST0 &&
        (value >> 16U) == PL330_DMAKILL)
        ++kill_writes;
    if (address == RIX_FIFO_BASE + FIFO_TDFR && value == FIFO_RESET_KEY)
        ++fifo_resets;
}

void Xil_DCacheFlushRange(uint32_t address, uint32_t length)
{
    (void)address;
    (void)length;
}

void Xil_DCacheInvalidateRange(uint32_t address, uint32_t length)
{
    (void)address;
    (void)length;
}

void Xil_ICacheInvalidateRange(uint32_t address, uint32_t length)
{
    (void)address;
    (void)length;
}

void Xil_L1DCacheFlushRange(INTPTR address, u32 length)
{
    (void)address;
    (void)length;
}

void Xil_L1DCacheInvalidateRange(INTPTR address, u32 length)
{
    (void)address;
    (void)length;
}

void Xil_SetTlbAttributes(uintptr_t address, uint32_t attributes)
{
    (void)address;
    (void)attributes;
}

void l2_maint_dcache_flush_range(intptr_t address, uint32_t length)
{
    (void)address;
    (void)length;
}

int sdk_aperture_runtime_is_zorro3(void)
{
    return 1;
}

void l2_maint_l2_invalidate_range(intptr_t address, uint32_t length)
{
    (void)address;
    (void)length;
}

void zzm_rt_dcache_clean(uint32_t address, uint32_t length)
{
    (void)address;
    (void)length;
}

void zzm_rt_dcache_invalidate(uint32_t address, uint32_t length)
{
    (void)address;
    (void)length;
}

void zzm_rt_barrier(void)
{
}

uint64_t zzm_rt_time_us(void)
{
    return 0U;
}

void zzm_rt_log(const char *message)
{
    (void)message;
}

static int platform_caps(zzm_graphics_hw_caps_t *caps)
{
    caps->has_rasterix_ef = 1U;
    caps->rasterix_csr_abi_major = 2U;
    caps->pl330_available = 1U;
    return 1;
}

static int platform_claim(uint32_t owner, uint32_t *channel)
{
    (void)owner;
    *channel = 6U;
    return 1;
}

static void platform_abort(uint32_t channel)
{
    assert(channel == 6U);
    ++platform_sync_aborts;
}

static int platform_abort_nonblocking(uint32_t channel)
{
    assert(channel == 6U);
    return RIX_DRV_DMA_PENDING;
}

static int platform_start_nonblocking(uint32_t channel, uint32_t source,
                                      uint32_t bytes)
{
    (void)channel;
    (void)source;
    (void)bytes;
    return RIX_DRV_DMA_PENDING;
}

static int platform_write(uint32_t offset, uint32_t value)
{
    if (offset == RIX_DRV_FIFO_TDFR && value == RIX_DRV_FIFO_RESET_KEY)
        ++platform_fifo_resets;
    return 1;
}

static int platform_read(uint32_t offset, uint32_t *value)
{
    (void)offset;
    *value = 0U;
    return 1;
}

static int platform_write_words(const uint32_t *words, uint32_t count)
{
    (void)words;
    (void)count;
    return 1;
}

static void platform_barrier(void)
{
}

static const zzm_graphics_hw_service_t platform_service = {
    .version = ZZM_SERVICE_GRAPHICS_HW_V3,
    .size = sizeof(platform_service),
    .caps = platform_caps,
    .csr_read = platform_read,
    .csr_write = platform_write,
    .fifo_read = platform_read,
    .fifo_write = platform_write,
    .fifo_write_words = platform_write_words,
    .pl330_claim = platform_claim,
    .pl330_start_nonblocking = platform_start_nonblocking,
    .pl330_abort = platform_abort,
    .barrier = platform_barrier,
    .pl330_abort_nonblocking = platform_abort_nonblocking,
};

static void test_platform_abort_is_synchronous(void)
{
    zzm_graphics_hw_caps_t caps;

    assert(platform_service.caps(&caps));
    assert(minigl_platform_set_graphics(&platform_service, &caps, 1U));
    minigl_platform_abort_graphics();
    assert(platform_sync_aborts == 1U);
    assert(platform_fifo_resets == 1U);
}

static const zzm_graphics_hw_service_t *graphics_service(void)
{
    const zzm_graphics_hw_service_t *service;

    service = zzm_minigl_host_get_service(ZZM_SERVICE_GRAPHICS_HW,
                                           ZZM_SERVICE_GRAPHICS_HW_V3);
    assert(service != NULL);
    return service;
}

static void claim_channel(const zzm_graphics_hw_service_t *service)
{
    uint32_t channel = 0U;

    assert(service->pl330_claim(1U, &channel));
    assert(channel == DMAC_CHANNEL);
    kill_writes = 0U;
    fifo_resets = 0U;
    reporter_calls = 0U;
}

static void test_nonblocking_abort_waits_for_debug_idle(void)
{
    const zzm_graphics_hw_service_t *service = graphics_service();
    uint32_t channel = DMAC_CHANNEL;
    unsigned i;

    claim_channel(service);
    debug_busy = 0U;
    channel_state = 1U;
    assert(service->pl330_start_nonblocking(channel, 0x1000U, 64U) ==
           RIX_DRV_DMA_PENDING);
    debug_busy = 1U;
    for (i = 0U; i < 3U; ++i) {
        assert(service->pl330_abort_nonblocking(channel) == RIX_DRV_DMA_PENDING);
        assert(kill_writes == 0U);
    }
    assert(reporter_calls == 0U);
    debug_busy = 0U;
    assert(service->pl330_abort_nonblocking(channel) == RIX_DRV_DMA_PENDING);
    assert(kill_writes == 1U && fifo_resets == 0U && reporter_calls == 0U);
    channel_state = 0U;
    assert(service->pl330_abort_nonblocking(channel) == RIX_DRV_DMA_COMPLETE);
    assert(kill_writes == 1U && fifo_resets == 0U && reporter_calls == 0U);
}

static void test_nonblocking_abort_timeout_keeps_channel_claimed(void)
{
    const zzm_graphics_hw_service_t *service = graphics_service();
    uint32_t channel = DMAC_CHANNEL;
    unsigned i;

    claim_channel(service);
    debug_busy = 1U;
    channel_state = 1U;
    for (i = 0U; i < 32U; ++i) {
        int result = service->pl330_abort_nonblocking(channel);

        assert(result == (i == 31U ? RIX_DRV_DMA_ERROR : RIX_DRV_DMA_PENDING));
    }
    assert(kill_writes == 0U && fifo_resets == 0U && reporter_calls == 0U);
    debug_busy = 0U;
    assert(service->pl330_abort_nonblocking(channel) == RIX_DRV_DMA_PENDING);
    assert(kill_writes == 1U);
    channel_state = 0U;
    assert(service->pl330_abort_nonblocking(channel) == RIX_DRV_DMA_COMPLETE);
}

/* Once bounded cancellation reports ERROR, teardown switches to the
 * synchronous recovery hook. That hook must discard the poisoned launch
 * state so the next owner can claim and start a fresh channel. */
static void test_abort_error_recovers_for_next_owner(void)
{
    const zzm_graphics_hw_service_t *service = graphics_service();
    uint32_t channel = DMAC_CHANNEL;
    unsigned i;

    claim_channel(service);
    debug_busy = 1U;
    channel_state = 1U;
    for (i = 0U; i < 32U; ++i)
        assert(service->pl330_abort_nonblocking(channel) ==
               (i == 31U ? RIX_DRV_DMA_ERROR : RIX_DRV_DMA_PENDING));
    channel_state = 0U;
    service->pl330_abort(channel);
    assert(kill_writes == 1U);
    debug_busy = 0U;
    assert(service->pl330_claim(2U, &channel));
    assert(channel == DMAC_CHANNEL);
    assert(service->pl330_start_nonblocking(channel, 0x2000U, 64U) ==
           RIX_DRV_DMA_PENDING);
    channel_state = 0U;
    service->pl330_abort(channel);
}

int main(void)
{
    zzm_minigl_boot_caps_init(ZZM_MINIGL_IMAGE_RASTERIX_EF);
    test_platform_abort_is_synchronous();
    test_nonblocking_abort_waits_for_debug_idle();
    test_abort_error_recovers_for_next_owner();
    test_nonblocking_abort_timeout_keeps_channel_claimed();
    return 0;
}
