/* RasterIX EF FIFO driver: discovery, bounded packets, DMA, and reset. */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "rix_drv.h"

#define CSR_WORDS 32U
#define FIFO_WORDS 8U
#define LOG_WORDS 1024U

struct model {
    uint32_t csr[CSR_WORDS];
    uint32_t fifo[FIFO_WORDS];
    uint32_t words[LOG_WORDS];
    uint32_t lengths[8];
    unsigned word_count, packet_count, dma_count, clean_count;
    uintptr_t clean_address;
    size_t clean_bytes;
    uint32_t dma_words[8];
};

static uint32_t csr_read(void *context, uint32_t offset)
{
    return ((struct model *)context)->csr[offset / 4U];
}

static void csr_write(void *context, uint32_t offset, uint32_t value)
{
    ((struct model *)context)->csr[offset / 4U] = value;
}

static uint32_t fifo_read(void *context, uint32_t offset)
{
    return ((struct model *)context)->fifo[offset / 4U];
}

static void fifo_write(void *context, uint32_t offset, uint32_t value)
{
    struct model *model = context;

    if (offset == RIX_DRV_FIFO_ISR) {
        model->fifo[0] &= ~value;
        return;
    }
    model->fifo[offset / 4U] = value;
    if (offset == RIX_DRV_FIFO_TDFD) {
        assert(model->word_count < LOG_WORDS);
        model->words[model->word_count++] = value;
    } else if (offset == RIX_DRV_FIFO_TLF) {
        assert(model->packet_count < 8U);
        model->lengths[model->packet_count++] = value;
    }
}

static void cache_clean(void *context, uintptr_t address, size_t bytes)
{
    struct model *model = context;

    ++model->clean_count;
    model->clean_address = address;
    model->clean_bytes = bytes;
}

static int dma_stream(void *context, uintptr_t source, uint32_t words,
                      uint32_t burst_words)
{
    struct model *model = context;

    (void)source;
    assert(burst_words == 16U);
    assert(model->dma_count < 8U);
    model->dma_words[model->dma_count++] = words;
    return 1;
}

static struct rix_drv_config config(struct model *model, int present)
{
    struct rix_drv_config value;

    memset(&value, 0, sizeof(value));
    value.context = model;
    value.read = csr_read;
    value.write = csr_write;
    value.fifo_read = fifo_read;
    value.fifo_write = fifo_write;
    value.clean_dcache = cache_clean;
    value.hardware_present = present;
    return value;
}

static void identify(struct model *model)
{
    model->csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model->csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
    model->csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    model->fifo[RIX_DRV_FIFO_TDFV / 4U] = 508U;
}

static void test_discovery_and_enable(void)
{
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;

    memset(&model, 0, sizeof(model));
    io = config(&model, 0);
    assert(rix_drv_init(&driver, &io) == RIX_DRV_UNAVAILABLE);
    identify(&model);
    io = config(&model, 1);
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(model.fifo[RIX_DRV_FIFO_TDFR / 4U] == RIX_DRV_FIFO_RESET_KEY);
    assert(model.csr[RIX_DRV_CSR_CONTROL / 4U] == RIX_DRV_CONTROL_ENABLE);
}

static void test_cpu_packet_and_backpressure(void)
{
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;
    static const uint32_t commands[] = { 0x10000000U, 0x12345678U, 0U };

    memset(&model, 0, sizeof(model));
    identify(&model);
    io = config(&model, 1);
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_submit(&driver, commands, (uintptr_t)commands, 3U, 0U) == RIX_DRV_OK);
    assert(model.clean_count == 1U);
    assert(model.clean_address == (uintptr_t)commands);
    assert(model.clean_bytes == sizeof(commands));
    assert(model.packet_count == 1U);
    assert(model.lengths[0] == 20U);
    assert(model.words[0] == (0x90000000U | sizeof(commands)));
    assert(model.words[1] == 0U);
    assert(model.words[2] == commands[0]);
    model.fifo[RIX_DRV_FIFO_TDFV / 4U] = 0U;
    assert(rix_drv_submit(&driver, commands, (uintptr_t)commands, 3U, 0U) == RIX_DRV_TIMEOUT);
}

static void test_dma_chunking(void)
{
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;
    uint32_t commands[300];

    memset(&model, 0, sizeof(model));
    memset(commands, 0, sizeof(commands));
    identify(&model);
    io = config(&model, 1);
    io.dma_stream = dma_stream;
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_submit(&driver, commands, (uintptr_t)commands, 300U, 0U) == RIX_DRV_OK);
    assert(model.packet_count == 2U);
    assert(model.lengths[0] == (248U + 2U) * 4U);
    assert(model.lengths[1] == (52U + 2U) * 4U);
    assert(model.dma_count == 2U);
    assert(model.dma_words[0] == 248U && model.dma_words[1] == 52U);
    assert(driver.packets == 2U && driver.dma_packets == 2U);
    assert(driver.last_packet_dma && driver.last_packet_bytes == (52U + 2U) * 4U);
    assert(rix_drv_poll(&driver, 0U) == RIX_DRV_OK);
}

/* A TLF that disagrees with the staged byte count is dropped by the FIFO
 * (ISR.TSE); the driver must report it on that packet, and enable must clear
 * a previous context's sticky errors without touching other ISR bits. */
static void test_rejected_commit(void)
{
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;
    static const uint32_t commands[] = { 0x10000000U, 0x12345678U, 0U };
    const uint32_t trc = 1U << 24;

    memset(&model, 0, sizeof(model));
    identify(&model);
    model.fifo[RIX_DRV_FIFO_ISR / 4U] = RIX_DRV_FIFO_ISR_TSE | trc;
    io = config(&model, 1);
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(model.fifo[RIX_DRV_FIFO_ISR / 4U] == trc);
    assert(rix_drv_submit(&driver, commands, (uintptr_t)commands, 3U, 0U) == RIX_DRV_OK);
    assert(driver.last_error_status == 0U && !driver.last_packet_dma);
    model.fifo[RIX_DRV_FIFO_ISR / 4U] |= RIX_DRV_FIFO_ISR_TSE;
    assert(rix_drv_submit(&driver, commands, (uintptr_t)commands, 3U, 0U) == RIX_DRV_ERROR);
    assert(driver.last_error_status == (RIX_DRV_FIFO_ISR_TSE | trc));
    assert(driver.last_error_address == (uint32_t)(uintptr_t)commands);
    assert(driver.packets == 2U && driver.dma_packets == 0U);
}

static void test_recovery_and_disable(void)
{
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;

    memset(&model, 0, sizeof(model));
    identify(&model);
    io = config(&model, 1);
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_recover(&driver, 0U, 1) == RIX_DRV_OK);
    assert(model.csr[RIX_DRV_CSR_CONTROL / 4U] ==
           (RIX_DRV_CONTROL_ENABLE | RIX_DRV_CONTROL_SOFT_RESET));
    assert(rix_drv_disable(&driver, 0U) == RIX_DRV_OK);
    assert(model.csr[RIX_DRV_CSR_CONTROL / 4U] == 0U);
}

int main(void)
{
    test_discovery_and_enable();
    test_cpu_packet_and_backpressure();
    test_dma_chunking();
    test_rejected_commit();
    test_recovery_and_disable();
    return 0;
}
