/* Active MiniGL profile telemetry with deterministic platform samples. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "minigl_profile.h"
#include "minigl_platform.h"
#include "rix_drv.h"

static uint64_t now_us;
static int rasterix_present;
static uint32_t csr[8];
static unsigned busy_csr_reads;
static char report[1024];

uint64_t minigl_platform_time_us(void)
{
    return now_us;
}

int minigl_platform_has_rasterix(void)
{
    return rasterix_present;
}

uint32_t minigl_platform_csr_read(void *context, uint32_t offset)
{
    (void)context;
    if (offset == RIX_DRV_CSR_DBG_BUSY_CYCLES)
        ++busy_csr_reads;
    return csr[offset / sizeof(csr[0])];
}

void minigl_platform_log(const char *message)
{
    (void)snprintf(report, sizeof(report), "%s", message);
}

int minigl_raster_mem_counters(uint32_t *reads, uint32_t *writes)
{
    (void)reads;
    (void)writes;
    return 0;
}

static void reset_profile(uint32_t version, uint32_t busy)
{
    minigl_profile_set_enabled(0);
    memset(csr, 0, sizeof(csr));
    csr[RIX_DRV_CSR_IDENTITY / sizeof(csr[0])] = RIX_DRV_IDENTITY_VALUE;
    csr[RIX_DRV_CSR_VERSION / sizeof(csr[0])] = version;
    csr[RIX_DRV_CSR_DBG_BUSY_CYCLES / sizeof(csr[0])] = busy;
    busy_csr_reads = 0U;
    report[0] = '\0';
    now_us = 0U;
    rasterix_present = 1;
    minigl_profile_set_enabled(1);
}

static void emit_packet(void)
{
    minigl_profile_packet_begin();
    minigl_profile_packet_end(16U, 1U);
}

static void test_busy_csr_abi_gating(void)
{
    reset_profile(RIX_DRV_VERSION_VALUE, 0x12345678U);
    assert(busy_csr_reads == 0U);
    emit_packet();
    now_us = 5000000U;
    minigl_profile_report();
    assert(busy_csr_reads == 0U);
    assert(strstr(report, "busy=n/a") != NULL);
}

static void test_busy_percentage_and_counter_wrap(void)
{
    const uint32_t start = 0xf8000000U;

    reset_profile(RIX_DRV_VERSION_BUSY_CYCLES, start);
    assert(busy_csr_reads == 1U);
    emit_packet();
    csr[RIX_DRV_CSR_DBG_BUSY_CYCLES / sizeof(csr[0])] = start + 250000000U;
    now_us = 5000000U;
    minigl_profile_report();
    assert(busy_csr_reads == 3U);
    assert(strstr(report, "busy=50%") != NULL);
}

static void test_feed_full_empty_and_retry_counters(void)
{
    reset_profile(RIX_DRV_VERSION_BUSY_CYCLES, 0U);
    minigl_profile_feed_vacancy(0U, RIX_DRV_FIFO_LEGACY_VACANCY);
    minigl_profile_feed_vacancy(RIX_DRV_FIFO_LEGACY_VACANCY,
                                RIX_DRV_FIFO_LEGACY_VACANCY);
    minigl_profile_feed_retry();
    minigl_profile_feed_retry();
    emit_packet();
    now_us = 5000000U;
    minigl_profile_report();
    assert(strstr(report, "empty=1 full=1 retry=2") != NULL);
}

static void test_feed_deadline_and_park_telemetry(void)
{
    reset_profile(RIX_DRV_VERSION_BUSY_CYCLES, 0U);
    minigl_profile_feed_queued(7U);
    minigl_profile_feed_deadline_wake();
    minigl_profile_feed_wait(MGLP_FEED_WAIT_RING, 2000U);
    minigl_profile_feed_wait(MGLP_FEED_WAIT_TEXTURE, 3000U);
    emit_packet();
    now_us = 5000000U;
    minigl_profile_report();
    assert(strstr(report, "deadline=0") != NULL);
    assert(strstr(report, "qhi=7 dwake=1") != NULL);
    assert(strstr(report, "wait-ring=1/2ms") != NULL);
    assert(strstr(report, "wait-tex=1/3ms") != NULL);
}

/* Per-frame cost of feed steps: device reads and cache invalidates. */
static void test_feed_step_cost_per_frame(void)
{
    reset_profile(RIX_DRV_VERSION_BUSY_CYCLES, 0U);
    minigl_profile_count(MGLP_N_FRAMES, 2U);
    minigl_profile_count(MGLP_N_FEED_STEPS, 13200U);
    minigl_profile_count(MGLP_N_FEED_DEVICE_READS, 64U);
    minigl_profile_count(MGLP_N_FEED_INVALIDATES, 4U);
    emit_packet();
    now_us = 5000000U;
    minigl_profile_report();
    assert(strstr(report, "steps=6600 step-rd=32 step-inv=2") != NULL);
}

int main(void)
{
    test_busy_csr_abi_gating();
    test_busy_percentage_and_counter_wrap();
    test_feed_full_empty_and_retry_counters();
    test_feed_deadline_and_park_telemetry();
    test_feed_step_cost_per_frame();
    return 0;
}
