/* RasterIX EF FIFO driver: discovery, bounded packets, DMA, and reset. */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "rix_drv.h"
#include "minigl_profile.h"

#define CSR_WORDS 32U
#define FIFO_WORDS 8U
#define LOG_WORDS (RIX_DRV_FIFO_MAX_PACKET_WORDS + 12U)

struct model {
    uint32_t csr[CSR_WORDS];
    uint32_t fifo[FIFO_WORDS];
    uint32_t words[LOG_WORDS];
    uint32_t lengths[64];
    unsigned word_count, packet_count, dma_count, clean_count;
    uintptr_t clean_address;
    size_t clean_bytes;
    uint32_t dma_words[64];
    /* Interleaved transport events: h = header word, t = packet boundary,
     * a = async launch, r = retire. */
    char events[256];
    unsigned event_count;
    char abort_log[16];
    unsigned abort_log_count;
    unsigned async_launches, retires, complete_observations;
    unsigned nonblocking_launches;
    int debug_busy, dma_claimed, dma_abort_phase, record_abort;
    int dma_done;
};

static void log_abort(struct model *model, char event);

static uint32_t csr_read(void *context, uint32_t offset)
{
    return ((struct model *)context)->csr[offset / 4U];
}

/* Abort log: C = command admission stopped, S = renderer soft reset. */
static void csr_write(void *context, uint32_t offset, uint32_t value)
{
    struct model *model = context;

    model->csr[offset / 4U] = value;
    if (offset == RIX_DRV_CSR_CONTROL && model->record_abort)
        log_abort(model, (value & RIX_DRV_CONTROL_SOFT_RESET) != 0U ? 'S' : 'C');
}

static uint32_t fifo_read(void *context, uint32_t offset)
{
    return ((struct model *)context)->fifo[offset / 4U];
}


static void log_abort(struct model *model, char event)
{
    assert(model->abort_log_count < sizeof(model->abort_log));
    model->abort_log[model->abort_log_count++] = event;
}
static void fifo_write(void *context, uint32_t offset, uint32_t value)
{
    struct model *model = context;

    if (offset == RIX_DRV_FIFO_TDFD) {
        assert(model->word_count < LOG_WORDS);
        model->words[model->word_count++] = value;
        if (model->event_count < sizeof(model->events))
            model->events[model->event_count++] = 'h';
        return;
    }
    if (offset == RIX_DRV_FIFO_TDFR && model->record_abort) {
        model->word_count = 0U;
        model->packet_count = 0U;
        log_abort(model, 'F');
    }
    assert(offset / 4U < FIFO_WORDS);
    if (offset == RIX_DRV_FIFO_ISR) {
        model->fifo[0] &= ~value;
        return;
    }
    model->fifo[offset / 4U] = value;
    if (offset == RIX_DRV_FIFO_TLF) {
        assert(model->packet_count < 8U);
        model->lengths[model->packet_count++] = value;
        if (model->event_count < sizeof(model->events))
            model->events[model->event_count++] = 't';
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

static int dma_stream_error(void *context, uintptr_t source, uint32_t words,
                            uint32_t burst_words)
{
    (void)context;
    (void)source;
    (void)words;
    (void)burst_words;
    return 0;
}
static int dma_start_async(void *context, uintptr_t source, uint32_t words,
                           uint32_t burst_words)
{
    struct model *model = context;
    const uint32_t *payload = (const uint32_t *)source;
    uint32_t index;

    assert(burst_words == 16U);
    assert(model->async_launches < 64U);
    ++model->async_launches;
    model->dma_words[model->dma_count++] = words;
    for (index = 0U; index < words; ++index) {
        assert(model->word_count < LOG_WORDS);
        model->words[model->word_count++] = payload[index];
    }
    if (model->event_count < sizeof(model->events))
        model->events[model->event_count++] = 'a';
    return 1;
}

/* The first call represents the debug interface accepting DMAGO.  Until it
 * does, the packet header must remain the only FIFO data staged. */
static int dma_start_nonblocking(void *context, uintptr_t source, uint32_t words,
                                 uint32_t burst_words)
{
    struct model *model = context;
    const uint32_t *payload = (const uint32_t *)source;
    uint32_t index;

    assert(burst_words == 16U);
    ++model->nonblocking_launches;
    if (model->debug_busy)
        return RIX_DRV_DMA_PENDING;
    if (model->record_abort)
        log_abort(model, 'L');
    for (index = 0U; index < words; ++index) {
        assert(model->word_count < LOG_WORDS);
        model->words[model->word_count++] = payload[index];
    }
    return RIX_DRV_DMA_COMPLETE;
}

static int dma_start_error(void *context, uintptr_t source, uint32_t words,
                           uint32_t burst_words)
{
    (void)context;
    (void)source;
    (void)words;
    (void)burst_words;
    return RIX_DRV_DMA_ERROR;
}

static int dma_complete(void *context)
{
    struct model *model = context;

    ++model->complete_observations;
    return model->dma_done ? RIX_DRV_DMA_COMPLETE : RIX_DRV_DMA_PENDING;
}

static int dma_ready(void *context)
{
    return ((struct model *)context)->dma_claimed;
}

static int dma_abort(void *context)
{
    struct model *model = context;

    if (model->dma_abort_phase++ == 0) {
        log_abort(model, 'K');
        return RIX_DRV_DMA_PENDING;
    }
    log_abort(model, 'Q');
    return RIX_DRV_DMA_COMPLETE;
}

static int dma_retire(void *context)
{
    struct model *model = context;

    ++model->retires;
    if (model->event_count < sizeof(model->events))
        model->events[model->event_count++] = 'r';
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
static struct rix_drv_config memfetch_config(struct model *model)
{
    struct rix_drv_config value = config(model, 1);

    value.submit_mode = RIX_DRV_SUBMIT_MEMFETCH;
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

static void test_fifo_depth_packet_boundaries(void)
{
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;
    uint32_t commands[RIX_DRV_FIFO_MAX_PACKET_WORDS + 7U];
    uint32_t index;

    for (index = 0U; index < sizeof(commands) / sizeof(commands[0]); ++index)
        commands[index] = index;

    memset(&model, 0, sizeof(model));
    identify(&model);
    io = config(&model, 1);
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(driver.packet_words == RIX_DRV_FIFO_PACKET_WORDS);
    assert(rix_drv_submit(&driver, commands, 0U, 300U, 0U) == RIX_DRV_OK);
    assert(model.packet_count == 2U);
    assert(model.lengths[0] == (248U + 2U) * 4U);
    assert(model.lengths[1] == (52U + 2U) * 4U);
    assert(model.words[0] == (0x90000000U | 248U * 4U));
    assert(model.words[250] == (0x90000000U | 52U * 4U));
    assert(memcmp(&model.words[2], commands, 248U * sizeof(*commands)) == 0);
    assert(memcmp(&model.words[252], commands + 248U,
                  52U * sizeof(*commands)) == 0);

    memset(&model, 0, sizeof(model));
    identify(&model);
    model.fifo[RIX_DRV_FIFO_TDFV / 4U] = 4092U;
    io = config(&model, 1);
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(driver.packet_words == RIX_DRV_FIFO_MAX_PACKET_WORDS);
    assert(rix_drv_submit(&driver, commands, 0U,
                          (uint32_t)(sizeof(commands) / sizeof(commands[0])),
                          0U) == RIX_DRV_OK);
    assert(model.packet_count == 2U);
    assert(model.lengths[0] == (RIX_DRV_FIFO_MAX_PACKET_WORDS + 2U) * 4U);
    assert(model.lengths[1] == 9U * 4U);
    assert(model.words[0] == (0x90000000U |
                              RIX_DRV_FIFO_MAX_PACKET_WORDS * 4U));
    assert(model.words[RIX_DRV_FIFO_MAX_PACKET_WORDS + 2U] ==
           (0x90000000U | 7U * 4U));
    assert(memcmp(&model.words[2], commands,
                  RIX_DRV_FIFO_MAX_PACKET_WORDS * sizeof(*commands)) == 0);
    assert(memcmp(&model.words[RIX_DRV_FIFO_MAX_PACKET_WORDS + 4U],
                  commands + RIX_DRV_FIFO_MAX_PACKET_WORDS,
                  7U * sizeof(*commands)) == 0);
}

/* The deferred packet boundary: launch returns before the transfer
 * finishes, so the boundary word of packet k must wait for its retire and
 * must precede packet k+1's header -- the parser still sees whole packets
 * strictly in order. */
static void test_async_defers_boundary_until_retire(void)
{
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;
    uint32_t commands[300];
    static const char expected[] = "hha" "rt" "hh" "a" "rt";

    memset(&model, 0, sizeof(model));
    memset(commands, 0, sizeof(commands));
    identify(&model);
    io = config(&model, 1);
    io.dma_start_async = dma_start_async;
    io.dma_retire = dma_retire;
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_submit(&driver, commands, (uintptr_t)commands, 300U, 0U) == RIX_DRV_OK);
    assert(model.async_launches == 2U && model.retires == 2U);
    assert(model.packet_count == 2U);
    assert(model.lengths[0] == (248U + 2U) * 4U);
    assert(model.lengths[1] == (52U + 2U) * 4U);
    assert(model.event_count == sizeof(expected) - 1U);
    assert(memcmp(model.events, expected, sizeof(expected) - 1U) == 0);
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

static void feed_until_done(struct rix_drv *driver, struct rix_drv_feed *feed)
{
    unsigned int steps;

    for (steps = 0U; steps < 128U; ++steps) {
        int result = rix_drv_try_feed(driver, feed);

        if (result == RIX_DRV_FEED_DONE)
            return;
        assert(result == RIX_DRV_FEED_PROGRESS || result == RIX_DRV_FEED_BLOCKED);
        if (feed->pending_dma)
            ((struct model *)driver->context)->dma_done = 1;
    }
    assert(0);
}

static void test_feed_chunk_boundaries_and_lengths(void)
{
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;
    struct rix_drv_feed feed;
    uint32_t commands[1027];
    uint32_t split;

    for (split = 0U; split < sizeof(commands) / sizeof(commands[0]); ++split)
        commands[split] = 0U;
    for (split = 1U; split <= 1025U; ++split) {
        memset(&model, 0, sizeof(model));
        identify(&model);
        model.fifo[RIX_DRV_FIFO_TDFV / 4U] = 4092U;
        io = config(&model, 1);
        assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
        assert(rix_drv_enable(&driver) == RIX_DRV_OK);
        assert(rix_drv_feed_begin(&feed, commands, (uintptr_t)commands, split,
                                  RIX_DRV_FEED_SOURCE_SVC) == RIX_DRV_OK);
        feed_until_done(&driver, &feed);
        assert(model.packet_count == (split <= RIX_DRV_FEED_MAX_WORDS ? 1U : 2U));
        assert(model.lengths[0] ==
               ((split <= RIX_DRV_FEED_MAX_WORDS ? split : RIX_DRV_FEED_MAX_WORDS) + 2U) * 4U);
        if (split > RIX_DRV_FEED_MAX_WORDS)
            assert(model.lengths[1] == (split - RIX_DRV_FEED_MAX_WORDS + 2U) * 4U);
    }
}

static void test_feed_partial_vacancy_and_oversized_command(void)
{
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;
    struct rix_drv_feed feed;
    uint32_t commands[1026];
    uint32_t index;

    memset(&model, 0, sizeof(model));
    identify(&model);
    model.fifo[RIX_DRV_FIFO_TDFV / 4U] = 2U;
    io = config(&model, 1);
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_feed_begin(&feed, commands, 0U, 1U,
                              RIX_DRV_FEED_SOURCE_SVC) == RIX_DRV_OK);
    commands[0] = 0U;
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_BLOCKED);
    assert(model.word_count == 0U && model.packet_count == 0U);
    model.fifo[RIX_DRV_FIFO_TDFV / 4U] = 3U;
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_DONE);
    assert(model.lengths[0] == 12U);

    memset(&model, 0, sizeof(model));
    identify(&model);
    model.fifo[RIX_DRV_FIFO_TDFV / 4U] = 1030U;
    commands[0] = 0x50000400U;
    for (index = 1U; index < sizeof(commands) / sizeof(commands[0]); ++index)
        commands[index] = 0U;
    io = config(&model, 1);
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_feed_begin(&feed, commands, 0U, 1025U,
                              RIX_DRV_FEED_SOURCE_SVC) == RIX_DRV_OK);
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_DONE);
    assert(model.packet_count == 1U && model.lengths[0] == 1027U * 4U);

    memset(&model, 0, sizeof(model));
    identify(&model);
    commands[0] = 0x500001faU; /* 507 words; 508-word FIFO holds 506 payload words. */
    io = config(&model, 1);
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_feed_begin(&feed, commands, 0U, 507U,
                              RIX_DRV_FEED_SOURCE_SVC) == RIX_DRV_OK);
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_ERROR);

    for (index = 0U; index < 2U; ++index) {
        uint32_t error = index == 0U ? RIX_DRV_FIFO_ISR_TSE : RIX_DRV_FIFO_ISR_TPOE;

        memset(&model, 0, sizeof(model));
        identify(&model);
        commands[0] = 0U;
        io = config(&model, 1);
        assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
        assert(rix_drv_enable(&driver) == RIX_DRV_OK);
        model.fifo[RIX_DRV_FIFO_ISR / 4U] = error;
        assert(rix_drv_feed_begin(&feed, commands, 0U, 1U,
                                  RIX_DRV_FEED_SOURCE_SVC) == RIX_DRV_OK);
        assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_ERROR);
        assert(driver.last_error_status == error);
    }
}

static void test_feed_delayed_dma_and_reset(void)
{
    struct model model;
    struct rix_drv driver;
    uint32_t commands[2050];
    struct rix_drv_config io;
    struct rix_drv_feed feed;
    uint32_t index;

    for (index = 0U; index < sizeof(commands) / sizeof(commands[0]); ++index)
        commands[index] = index;
    memset(&model, 0, sizeof(model));
    identify(&model);
    model.fifo[RIX_DRV_FIFO_TDFV / 4U] = 4092U;
    io = config(&model, 1);
    io.dma_start_nonblocking = dma_start_async;
    io.dma_complete = dma_complete;
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_feed_begin(&feed, commands, (uintptr_t)commands,
                              (uint32_t)(sizeof(commands) / sizeof(commands[0])),
                              RIX_DRV_FEED_SOURCE_SCHEDULER) == RIX_DRV_OK);
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_PROGRESS);
    assert(feed.pending_dma && model.packet_count == 0U);
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_BLOCKED);
    assert(model.complete_observations == 1U && model.packet_count == 0U);
    assert(model.words[0] == (0x90000000U | RIX_DRV_FEED_MAX_WORDS * 4U));
    model.dma_done = 1;
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_PROGRESS);
    assert(model.packet_count == 1U &&
           model.lengths[0] == (RIX_DRV_FEED_MAX_WORDS + 2U) * 4U);
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_PROGRESS);
    assert(feed.pending_dma && model.packet_count == 1U);
    rix_drv_feed_reset(&feed);
    assert(model.event_count == 7U && memcmp(model.events, "hhathha", 7U) == 0);
    assert(!feed.active && !feed.pending_dma);
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_DONE);
    assert(model.packet_count == 1U);
}

/* A DMAGO handshake can be busy while the FIFO header is already staged.
 * Retrying that handshake must not stage a second header or close a packet
 * before the transfer has completed. */
static void test_feed_pending_launch_does_not_restage_header(void)
{
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;
    struct rix_drv_feed feed;
    uint32_t commands[16] = { 0U };

    memset(&model, 0, sizeof(model));
    identify(&model);
    model.fifo[RIX_DRV_FIFO_TDFV / 4U] = 4092U;
    model.debug_busy = 1;
    io = config(&model, 1);
    io.dma_start_nonblocking = dma_start_nonblocking;
    io.dma_complete = dma_complete;
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_feed_begin(&feed, commands, (uintptr_t)commands, 16U,
                              RIX_DRV_FEED_SOURCE_SVC) == RIX_DRV_OK);
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_BLOCKED);
    assert(feed.pending_launch && model.word_count == 2U &&
           model.packet_count == 0U);
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_BLOCKED);
    assert(model.nonblocking_launches == 2U && model.word_count == 2U);
    model.debug_busy = 0;
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_PROGRESS);
    assert(feed.pending_dma && model.packet_count == 0U);
    model.dma_done = 1;
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_DONE);
    assert(model.packet_count == 1U && model.lengths[0] == (16U + 2U) * 4U);
}

static unsigned model_command_stream(const struct model *model, uint32_t *out)
{
    unsigned int packet, at = 0U, count = 0U;

    for (packet = 0U; packet < model->packet_count; ++packet) {
        uint32_t words = model->lengths[packet] / sizeof(uint32_t);

        assert(words >= 2U && at + words <= model->word_count);
        assert(model->words[at] == (0x90000000U |
                                    (words - 2U) * sizeof(uint32_t)));
        memcpy(out + count, model->words + at + 2U,
               (words - 2U) * sizeof(*out));
        count += words - 2U;
        at += words;
    }
    assert(at == model->word_count);
    return count;
}

static void test_feed_rejects_unclaimed_dma_before_staging(void)
{
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;
    struct rix_drv_feed feed;
    uint32_t commands[16] = { 0U };

    memset(&model, 0, sizeof(model));
    identify(&model);
    model.fifo[RIX_DRV_FIFO_TDFV / 4U] = 4092U;
    io = config(&model, 1);
    io.dma_start_nonblocking = dma_start_nonblocking;
    io.dma_complete = dma_complete;
    io.dma_ready = dma_ready;
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_feed_begin(&feed, commands, (uintptr_t)commands, 16U,
                              RIX_DRV_FEED_SOURCE_SVC) == RIX_DRV_OK);
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_ERROR);
    assert(model.word_count == 0U && model.packet_count == 0U);
}

static void test_feed_rejects_nonblocking_launch_failure(void)
{
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;
    struct rix_drv_feed feed;
    uint32_t commands[16] = { 0U };

    memset(&model, 0, sizeof(model));
    identify(&model);
    model.fifo[RIX_DRV_FIFO_TDFV / 4U] = 4092U;
    io = config(&model, 1);
    io.dma_start_nonblocking = dma_start_error;
    io.dma_complete = dma_complete;
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_feed_begin(&feed, commands, (uintptr_t)commands, 16U,
                              RIX_DRV_FEED_SOURCE_SVC) == RIX_DRV_OK);
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_ERROR);
    assert(!feed.pending_launch && !feed.pending_dma && model.packet_count == 0U);
}

static void test_submit_propagates_synchronous_dma_failure(void)
{
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;
    uint32_t commands[16] = { 0U };

    memset(&model, 0, sizeof(model));
    identify(&model);
    model.fifo[RIX_DRV_FIFO_TDFV / 4U] = 4092U;
    io = config(&model, 1);
    io.dma_stream = dma_stream_error;
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_submit(&driver, commands, (uintptr_t)commands, 16U, 0U) ==
           RIX_DRV_ERROR);
}

static void test_feed_abort_waits_for_dma_and_resets_in_order(void)
{
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;
    struct rix_drv_feed feed;
    uint32_t commands[16] = { 0U };

    memset(&model, 0, sizeof(model));
    identify(&model);
    model.fifo[RIX_DRV_FIFO_TDFV / 4U] = 4092U;
    model.dma_claimed = 1;
    model.debug_busy = 1;
    io = config(&model, 1);
    io.dma_start_nonblocking = dma_start_nonblocking;
    io.dma_complete = dma_complete;
    io.dma_ready = dma_ready;
    io.dma_abort = dma_abort;
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_feed_begin(&feed, commands, (uintptr_t)commands, 16U,
                              RIX_DRV_FEED_SOURCE_SVC) == RIX_DRV_OK);
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_BLOCKED);
    model.fifo[RIX_DRV_FIFO_ISR / 4U] = RIX_DRV_FIFO_ISR_TSE;
    model.record_abort = 1;
    model.debug_busy = 0;
    assert(rix_drv_feed_abort(&driver, &feed) == RIX_DRV_FEED_BLOCKED);
    assert(memcmp(model.abort_log, "K", 1U) == 0 && model.dma_abort_phase == 1);
    assert(rix_drv_feed_abort(&driver, &feed) == RIX_DRV_FEED_DONE);
    assert(memcmp(model.abort_log, "KQF", 3U) == 0);
    assert((model.fifo[RIX_DRV_FIFO_ISR / 4U] & RIX_DRV_FIFO_ISR_ERRORS) == 0U);
    assert(!feed.active && !feed.pending_launch && !feed.pending_dma);

    model.record_abort = 0;
    assert(rix_drv_feed_begin(&feed, commands, (uintptr_t)commands, 16U,
                              RIX_DRV_FEED_SOURCE_SVC) == RIX_DRV_OK);
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_PROGRESS);
    model.dma_done = 1;
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_DONE);
    assert(model.packet_count == 1U && model.lengths[0] == (16U + 2U) * 4U);
    assert((model.fifo[RIX_DRV_FIFO_ISR / 4U] & RIX_DRV_FIFO_ISR_TSE) == 0U);
}

static void test_feed_matches_synchronous_stream(void)
{
    struct model sync_model, feed_model;
    struct rix_drv sync_driver, feed_driver;
    struct rix_drv_config io;
    struct rix_drv_feed feed;
    uint32_t commands[1200], sync_commands[1200], feed_commands[1200];
    uint32_t random = 0x4d595df4U;
    uint32_t index = 0U;

    while (index < sizeof(commands) / sizeof(commands[0])) {
        random = random * 1664525U + 1013904223U;
        if ((random & 3U) == 0U && index + 2U <=
            sizeof(commands) / sizeof(commands[0])) {
            commands[index++] = 0x10000000U;
            commands[index++] = random;
        } else if ((random & 3U) == 1U) {
            uint32_t payload = (random >> 8U) & 15U;

            if (index + payload + 1U > sizeof(commands) / sizeof(commands[0]))
                payload = (uint32_t)(sizeof(commands) / sizeof(commands[0])) - index - 1U;
            commands[index++] = 0x30000000U | payload * sizeof(uint32_t);
            while (payload-- != 0U)
                commands[index++] = random;
        } else {
            commands[index++] = 0U;
        }
    }
    memset(&sync_model, 0, sizeof(sync_model));
    identify(&sync_model);
    sync_model.fifo[RIX_DRV_FIFO_TDFV / 4U] = 4092U;
    io = config(&sync_model, 1);
    assert(rix_drv_init(&sync_driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&sync_driver) == RIX_DRV_OK);
    assert(rix_drv_submit(&sync_driver, commands, 0U,
                          (uint32_t)(sizeof(commands) / sizeof(commands[0])), 0U) == RIX_DRV_OK);

    memset(&feed_model, 0, sizeof(feed_model));
    identify(&feed_model);
    feed_model.fifo[RIX_DRV_FIFO_TDFV / 4U] = 4092U;
    io = config(&feed_model, 1);
    assert(rix_drv_init(&feed_driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&feed_driver) == RIX_DRV_OK);
    assert(rix_drv_feed_begin(&feed, commands, 0U,
                              (uint32_t)(sizeof(commands) / sizeof(commands[0])),
                              RIX_DRV_FEED_SOURCE_SVC) == RIX_DRV_OK);
    feed_until_done(&feed_driver, &feed);
    assert(model_command_stream(&sync_model, sync_commands) == sizeof(commands) / sizeof(commands[0]));
    assert(model_command_stream(&feed_model, feed_commands) == sizeof(commands) / sizeof(commands[0]));
    assert(memcmp(feed_commands, sync_commands, sizeof(commands)) == 0);
}
/* Envelopes are fixed 16 KiB pieces of the list: a triangle may straddle
 * the cut, which the parser joins (proved in xsim, ef-stress_memfetch-split). */
static void test_memfetch_envelopes_cut_at_fixed_size(void)
{
    static uint32_t commands[4128] __attribute__((aligned(64)));
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;
    struct rix_drv_feed feed;
    uint32_t index;

    memset(commands, 0, sizeof(commands));
    commands[4080] = 0x3000009cU; /* A 40-word triangle across the cut. */
    for (index = 4081U; index < 4120U; ++index)
        commands[index] = index;

    memset(&model, 0, sizeof(model));
    identify(&model);
    io = memfetch_config(&model);
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_submit(&driver, commands, 0x01000000U,
                          (uint32_t)(sizeof(commands) / sizeof(commands[0])),
                          0U) == RIX_DRV_OK);
    assert(model.clean_count == 1U && model.clean_bytes == sizeof(commands));
    assert(model.packet_count == 2U);
    assert(model.lengths[0] == 8U && model.lengths[1] == 8U);
    assert(model.word_count == 4U);
    assert(model.words[0] == (0xb0000000U | 4096U * sizeof(uint32_t)));
    assert(model.words[1] == 0x01000000U);
    assert(model.words[2] == (0xb0000000U | 32U * sizeof(uint32_t)));
    assert(model.words[3] == 0x01004000U);

    memset(&model, 0, sizeof(model));
    identify(&model);
    io = memfetch_config(&model);
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_feed_begin(&feed, commands, 0x01000000U,
                              (uint32_t)(sizeof(commands) / sizeof(commands[0])),
                              RIX_DRV_FEED_SOURCE_SVC) == RIX_DRV_OK);
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_PROGRESS);
    assert(feed.command_offset == 4096U && model.packet_count == 1U);
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_DONE);
    assert(feed.command_offset == 4128U && model.packet_count == 2U);
    assert(model.lengths[0] == 8U && model.lengths[1] == 8U);
    assert(model.words[0] == (0xb0000000U | 4096U * sizeof(uint32_t)));
    assert(model.words[1] == 0x01000000U);
    assert(model.words[2] == (0xb0000000U | 32U * sizeof(uint32_t)));
    assert(model.words[3] == 0x01004000U);
}

static void test_memfetch_rejects_unaligned_lists(void)
{
    static uint32_t commands[16] __attribute__((aligned(64)));
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;

    memset(commands, 0, sizeof(commands));
    memset(&model, 0, sizeof(model));
    identify(&model);
    io = memfetch_config(&model);
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_submit(&driver, commands, 0x01000004U, 16U, 0U) ==
           RIX_DRV_BAD_ARGUMENT);
    assert(rix_drv_submit(&driver, commands, 0x01000000U, 15U, 0U) ==
           RIX_DRV_BAD_ARGUMENT);
    assert(model.word_count == 0U && model.packet_count == 0U);
}

/* An accepted envelope cannot be recalled, and the next one may complete a
 * command it began.  Aborting mid-list pushes the rest of the list and
 * touches neither CONTROL nor the FIFO reset: the caller waits for the
 * list's fence before reusing memory. */
static void test_memfetch_abort_finishes_the_list(void)
{
    static uint32_t commands[3U * RIX_DRV_MEMFETCH_CHUNK_WORDS]
        __attribute__((aligned(64)));
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;
    struct rix_drv_feed feed;

    memset(commands, 0, sizeof(commands));
    memset(&model, 0, sizeof(model));
    identify(&model);
    io = memfetch_config(&model);
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_feed_begin(&feed, commands, 0x01000000U,
                              (uint32_t)(sizeof(commands) / sizeof(commands[0])),
                              RIX_DRV_FEED_SOURCE_SVC) == RIX_DRV_OK);
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_PROGRESS);
    model.record_abort = 1;
    assert(rix_drv_feed_abort(&driver, &feed) == RIX_DRV_FEED_BLOCKED);
    assert(rix_drv_feed_abort(&driver, &feed) == RIX_DRV_FEED_DONE);
    assert(model.packet_count == 3U && !feed.active);
    assert(model.words[4] == (0xb0000000U | RIX_DRV_MEMFETCH_CHUNK_BYTES));
    assert(model.words[5] == 0x01000000U + 2U * RIX_DRV_MEMFETCH_CHUNK_BYTES);
    assert(model.abort_log_count == 0U);
    assert(rix_drv_feed_abort(&driver, &feed) == RIX_DRV_FEED_DONE);
    assert(model.packet_count == 3U && model.abort_log_count == 0U);
}

static unsigned g_tdfv_reads;

static uint32_t counting_fifo_read(void *context, uint32_t offset)
{
    if (offset == RIX_DRV_FIFO_TDFV)
        ++g_tdfv_reads;
    return fifo_read(context, offset);
}

/* Envelopes always fit: the vacancy read once covers many envelopes. */
static void test_memfetch_tracks_fifo_vacancy_in_software(void)
{
    static uint32_t commands[4U * RIX_DRV_MEMFETCH_CHUNK_WORDS]
        __attribute__((aligned(64)));
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;
    struct rix_drv_feed feed;
    int result;

    memset(commands, 0, sizeof(commands));
    memset(&model, 0, sizeof(model));
    identify(&model);
    io = memfetch_config(&model);
    io.fifo_read = counting_fifo_read;
    assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_feed_begin(&feed, commands, 0x01000000U,
                              (uint32_t)(sizeof(commands) / sizeof(commands[0])),
                              RIX_DRV_FEED_SOURCE_SVC) == RIX_DRV_OK);
    g_tdfv_reads = 0U;
    do
        result = rix_drv_try_feed(&driver, &feed);
    while (result == RIX_DRV_FEED_PROGRESS);
    assert(result == RIX_DRV_FEED_DONE && model.packet_count == 4U);
    assert(g_tdfv_reads <= 1U);

    /* An estimate that says full is re-read, never trusted. */
    model.fifo[RIX_DRV_FIFO_TDFV / 4U] = 1U;
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_feed_begin(&feed, commands, 0x01000000U, 64U,
                              RIX_DRV_FEED_SOURCE_SVC) == RIX_DRV_OK);
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_BLOCKED);
    model.fifo[RIX_DRV_FIFO_TDFV / 4U] = 508U;
    assert(rix_drv_try_feed(&driver, &feed) == RIX_DRV_FEED_DONE);
}

static void test_version_compatibility(void)
{
    static const uint32_t accepted[] = { 0x00020000U, 0x00020001U, 0x00020007U };
    static const uint32_t rejected[] = { 0x00010007U, 0x00030000U };
    struct model model;
    struct rix_drv driver;
    struct rix_drv_config io;
    unsigned int index;

    for (index = 0U; index < sizeof(accepted) / sizeof(accepted[0]); ++index) {
        memset(&model, 0, sizeof(model));
        identify(&model);
        model.csr[RIX_DRV_CSR_VERSION / 4U] = accepted[index];
        io = config(&model, 1);
        assert(rix_drv_init(&driver, &io) == RIX_DRV_OK);
    }
    for (index = 0U; index < sizeof(rejected) / sizeof(rejected[0]); ++index) {
        memset(&model, 0, sizeof(model));
        identify(&model);
        model.csr[RIX_DRV_CSR_VERSION / 4U] = rejected[index];
        io = config(&model, 1);
        assert(rix_drv_init(&driver, &io) == RIX_DRV_UNAVAILABLE);
    }
}

static void test_busy_counter_wrap(void)
{
    assert(minigl_profile_counter_delta(7U, 0xfffffff0U) == 23U);
    assert(minigl_profile_counter_delta(0x100U, 0x80U) == 0x80U);
}


int main(void)
{
    test_discovery_and_enable();
    test_cpu_packet_and_backpressure();
    test_dma_chunking();
    test_async_defers_boundary_until_retire();
    test_rejected_commit();
    test_fifo_depth_packet_boundaries();
    test_recovery_and_disable();
    test_version_compatibility();
    test_busy_counter_wrap();
    test_feed_chunk_boundaries_and_lengths();
    test_feed_partial_vacancy_and_oversized_command();
    test_feed_delayed_dma_and_reset();
    test_feed_matches_synchronous_stream();
    test_memfetch_envelopes_cut_at_fixed_size();
    test_memfetch_rejects_unaligned_lists();
    test_memfetch_abort_finishes_the_list();
    test_memfetch_tracks_fifo_vacancy_in_software();
    test_feed_pending_launch_does_not_restage_header();
    test_feed_rejects_unclaimed_dma_before_staging();
    test_feed_rejects_nonblocking_launch_failure();
    test_submit_propagates_synchronous_dma_failure();
    test_feed_abort_waits_for_dma_and_resets_in_order();
    return 0;
}
