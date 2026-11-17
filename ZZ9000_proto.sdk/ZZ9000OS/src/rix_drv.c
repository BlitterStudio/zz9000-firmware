/*
 * RasterIX EF FIFO transport driver for ZZ9000.
 *
 * The bounded DSE packet and texture ordering rules follow XACP librixgpu
 * (0BSD).  This implementation is part of ZZ9000 firmware.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rix_drv.h"
#include "minigl_profile.h"

#define RIX_DSE_STREAM 0x90000000U
#define RIX_MEM_TO_ST1 RIX_DRV_MEMFETCH_ENVELOPE

static uint32_t rix_drv_read(struct rix_drv *driver, uint32_t offset)
{
    ++driver->device_reads;
    return driver->read(driver->context, offset);
}

static void rix_drv_write(const struct rix_drv *driver, uint32_t offset,
                          uint32_t value)
{
    driver->write(driver->context, offset, value);
}

static uint32_t rix_drv_fifo_read(struct rix_drv *driver, uint32_t offset)
{
    ++driver->device_reads;
    return driver->fifo_read(driver->context, offset);
}

static void rix_drv_fifo_write(const struct rix_drv *driver, uint32_t offset,
                               uint32_t value)
{
    driver->fifo_write(driver->context, offset, value);
}

static int rix_drv_usable(const struct rix_drv *driver)
{
    return driver != NULL && driver->available && driver->read != NULL &&
           driver->write != NULL && driver->fifo_read != NULL &&
           driver->fifo_write != NULL;
}

static int rix_drv_wait_idle(struct rix_drv *driver, uint32_t timeout)
{
    uint32_t attempt = 0U;

    for (;;) {
        driver->last_status = rix_drv_read(driver, RIX_DRV_CSR_STATUS);
        if ((driver->last_status & RIX_DRV_STATUS_IDLE) != 0U)
            return RIX_DRV_OK;
        if (attempt++ >= timeout)
            return RIX_DRV_TIMEOUT;
    }
}

static int rix_drv_wait_vacancy(struct rix_drv *driver, uint32_t words,
                                uint32_t timeout)
{
    uint32_t attempt = 0U;
    int result;

    if (rix_drv_fifo_read(driver, RIX_DRV_FIFO_TDFV) >= words)
        return RIX_DRV_OK;
    /* Back-pressure: RasterIX consumes slower than the ARM pushes. Same
     * read/attempt sequence as a plain poll loop. */
    minigl_profile_push(MGLP_STALL);
    for (;;) {
        if (attempt++ >= timeout) {
            result = RIX_DRV_TIMEOUT;
            break;
        }
        if (rix_drv_fifo_read(driver, RIX_DRV_FIFO_TDFV) >= words) {
            result = RIX_DRV_OK;
            break;
        }
    }
    minigl_profile_pop();
    return result;
}

/* TDFV reports usable FIFO words after reset (PG080 reserves four).  The
 * original 512-word core deliberately used 248-word packets; retain that
 * wire behaviour, but use all of a deeper FIFO minus this packet's header. */
static void rix_drv_set_packet_words(struct rix_drv *driver)
{
    uint32_t vacancy = rix_drv_fifo_read(driver, RIX_DRV_FIFO_TDFV);

    driver->fifo_vacancy_estimate = vacancy;
    if (vacancy > driver->fifo_capacity)
        driver->fifo_capacity = vacancy;
    if (vacancy == RIX_DRV_FIFO_LEGACY_VACANCY)
        driver->packet_words = RIX_DRV_FIFO_PACKET_WORDS;
    else if (vacancy > 2U)
        driver->packet_words = vacancy - 2U;
    else
        driver->packet_words = RIX_DRV_FIFO_PACKET_WORDS;
    if (driver->packet_words > RIX_DRV_FIFO_MAX_PACKET_WORDS)
        driver->packet_words = RIX_DRV_FIFO_MAX_PACKET_WORDS;
}

/* Free FIFO words for the next feed packet.  The estimate only shrinks as
 * words are staged while RasterIX keeps draining, so it is a lower bound;
 * TDFV is read again only when the estimate cannot hold `wanted` words. */
static uint32_t rix_drv_feed_vacancy(struct rix_drv *driver, uint32_t wanted)
{
    if (driver->fifo_vacancy_estimate < wanted) {
        driver->fifo_vacancy_estimate = rix_drv_fifo_read(driver, RIX_DRV_FIFO_TDFV);
        minigl_profile_feed_vacancy(driver->fifo_vacancy_estimate,
                                    driver->fifo_capacity);
    }
    return driver->fifo_vacancy_estimate;
}

static uint32_t rix_drv_command_words(uint32_t word)
{
    switch (word >> 28U) {
    case 1U: return 2U; /* OP_RENDER_CONFIG */
    case 2U: return 1U; /* OP_FRAMEBUFFER */
    case 3U: return 1U + ((word & 0x3ffU) / 4U); /* OP_TRIANGLE_STREAM */
    case 4U: return 65U; /* OP_FOG_LUT_STREAM */
    case 5U: return 1U + (word & 0x7ffffU); /* OP_TEXTURE_STREAM */
    default: return 1U; /* OP_NOP */
    }
}

/* The CommandParser ignores envelope TLAST and keeps its command state
 * between envelopes, so a list is cut into fixed 16 KiB pieces (the last
 * one shorter) whatever the command boundaries; the list itself is a whole
 * number of 64-byte bursts. */
static uint32_t rix_drv_memfetch_chunk(uint32_t first, uint32_t command_words)
{
    uint32_t remaining = command_words - first;

    return remaining < RIX_DRV_MEMFETCH_CHUNK_WORDS ? remaining :
           RIX_DRV_MEMFETCH_CHUNK_WORDS;
}

static int rix_drv_memfetch_validate(uintptr_t command_address,
                                     uint32_t command_words)
{
    size_t bytes = (size_t)command_words * sizeof(uint32_t);

    if (command_address == (uintptr_t)0U ||
        command_address > (uintptr_t)UINT32_MAX ||
        (command_address & (RIX_DRV_MEMFETCH_ALIGN_BYTES - 1U)) != 0U ||
        bytes == 0U ||
        (bytes & (RIX_DRV_MEMFETCH_ALIGN_BYTES - 1U)) != 0U)
        return RIX_DRV_BAD_ARGUMENT;
    return RIX_DRV_OK;
}

static int rix_drv_memfetch_commit(struct rix_drv *driver, uintptr_t address,
                                   uint32_t words)
{
    uint32_t bytes = words * sizeof(uint32_t);
    uint32_t isr;

    driver->last_packet_bytes = 2U * sizeof(uint32_t);
    driver->pending_bytes = driver->last_packet_bytes;
    driver->last_packet_dma = 0;
    ++driver->packets;
    minigl_profile_count(MGLP_N_DRV_PACKETS, 1U);
    driver->memfetch_committed = 1U;
    rix_drv_fifo_write(driver, RIX_DRV_FIFO_TDFD, RIX_MEM_TO_ST1 | bytes);
    rix_drv_fifo_write(driver, RIX_DRV_FIFO_TDFD, (uint32_t)address);
    if (driver->fifo_drain != NULL)
        driver->fifo_drain(driver->context);
    rix_drv_fifo_write(driver, RIX_DRV_FIFO_TLF, 2U * sizeof(uint32_t));
    ++driver->memfetch_commit_count;
    driver->fifo_vacancy_estimate = driver->fifo_vacancy_estimate >= 2U ?
                                    driver->fifo_vacancy_estimate - 2U : 0U;
    isr = rix_drv_fifo_read(driver, RIX_DRV_FIFO_ISR);
    if ((isr & RIX_DRV_FIFO_ISR_ERRORS) != 0U) {
        driver->last_error_status = isr;
        driver->last_error_address = (uint32_t)address;
        minigl_profile_feed_errors(isr);
        rix_drv_fifo_write(driver, RIX_DRV_FIFO_ISR,
                           isr & RIX_DRV_FIFO_ISR_ERRORS);
        return RIX_DRV_ERROR;
    }
    return RIX_DRV_OK;
}

/* Ends the in-flight DMA packet: wait for the transfer, then establish its
 * packet boundary.  TLF before the next packet's header keeps the parser's
 * view of packet order intact. */
static int rix_drv_flush_pending(struct rix_drv *driver)
{
    uint32_t isr;

    if (!driver->pending_dma)
        return RIX_DRV_OK;
    if (driver->dma_retire == NULL ||
        !driver->dma_retire(driver->context)) {
        driver->pending_dma = 0;
        return RIX_DRV_ERROR;
    }
    driver->pending_dma = 0;
    if (driver->fifo_drain != NULL)
        driver->fifo_drain(driver->context);
    rix_drv_fifo_write(driver, RIX_DRV_FIFO_TLF, driver->pending_bytes);
    isr = rix_drv_fifo_read(driver, RIX_DRV_FIFO_ISR);
    if ((isr & RIX_DRV_FIFO_ISR_ERRORS) != 0U) {
        driver->last_error_status = isr;
        return RIX_DRV_ERROR;
    }
    return RIX_DRV_OK;
}

static int rix_drv_feed_commit(struct rix_drv *driver, struct rix_drv_feed *feed)
{
    uint32_t isr;
    uint32_t payload_words = feed->staged_bytes / sizeof(uint32_t) - 2U;

    if (driver->fifo_drain != NULL)
        driver->fifo_drain(driver->context);
    rix_drv_fifo_write(driver, RIX_DRV_FIFO_TLF, feed->staged_bytes);
    isr = rix_drv_fifo_read(driver, RIX_DRV_FIFO_ISR);
    if ((isr & RIX_DRV_FIFO_ISR_ERRORS) != 0U) {
        driver->last_error_status = isr;
        minigl_profile_feed_errors(isr);
        rix_drv_fifo_write(driver, RIX_DRV_FIFO_ISR,
                           isr & RIX_DRV_FIFO_ISR_ERRORS);
        return RIX_DRV_FEED_ERROR;
    }
    feed->command_offset += payload_words;
    feed->staged_bytes = 0U;
    feed->pending_dma = 0;
    minigl_profile_feed_chunk(feed->source);
    if (feed->command_offset == feed->command_words) {
        feed->active = 0;
        return RIX_DRV_FEED_DONE;
    }
    return RIX_DRV_FEED_PROGRESS;
}

static int rix_drv_send_packet(struct rix_drv *driver, const uint32_t *commands,
                               uintptr_t command_address, uint32_t words,
                               uint32_t timeout)
{
    uint32_t index, isr;
    int result;
    int dma;

    if (words == 0U || words > driver->packet_words)
        return RIX_DRV_BAD_ARGUMENT;
    /* Profiling only: where the renderer is when the next packet is ready.
     * A full FIFO and a stalled pipeline mean RasterIX is the bottleneck;
     * an empty FIFO means the ARM side is. */
    if (minigl_profile_enabled()) {
        uint32_t status = rix_drv_read(driver, RIX_DRV_CSR_STATUS);

        minigl_profile_count(MGLP_N_HW_SAMPLES, 1U);
        minigl_profile_count(MGLP_N_HW_BUSY, (status & RIX_DRV_STATUS_BUSY) != 0U);
        minigl_profile_count(MGLP_N_HW_STALL, (status & RIX_DRV_STATUS_STALL) != 0U);
        minigl_profile_count(MGLP_N_HW_READS, RIX_DRV_STATUS_READS(status));
        minigl_profile_count(MGLP_N_HW_VACANCY, rix_drv_fifo_read(driver, RIX_DRV_FIFO_TDFV));
    }
    /* Close the previous packet before anything new is staged: TLF
     * carries the byte count staged since the previous TLF, so a header
     * word written first makes the boundary value wrong and the FIFO drops
     * it (ISR.TSE, seen on kit 31) -- the parser then desynchronises and
     * executes garbage.  The retire still overlaps with the caller's work
     * since the launch: the emit stage between two submissions. */
    result = rix_drv_flush_pending(driver);
    if (result != RIX_DRV_OK)
        return result;
    result = rix_drv_wait_vacancy(driver, words + 2U, timeout);
    if (result != RIX_DRV_OK)
        return result;

    dma = (driver->dma_start_async != NULL || driver->dma_stream != NULL) &&
          command_address != (uintptr_t)0U && words >= 16U;
    minigl_profile_count(MGLP_N_DRV_PACKETS, 1U);
    driver->last_packet_bytes = (words + 2U) * sizeof(*commands);
    driver->pending_bytes = driver->last_packet_bytes;
    driver->last_packet_dma = dma;
    ++driver->packets;
    rix_drv_fifo_write(driver, RIX_DRV_FIFO_TDFD,
                       RIX_DSE_STREAM | (words * sizeof(*commands)));
    rix_drv_fifo_write(driver, RIX_DRV_FIFO_TDFD, 0U);
    if (dma && driver->dma_start_async != NULL) {
        ++driver->dma_packets;
        if (!driver->dma_start_async(driver->context, command_address, words, 16U)) {
            driver->last_error_address = (uint32_t)command_address;
            return RIX_DRV_ERROR;
        }
        driver->pending_dma = 1;
        return RIX_DRV_OK;
    }
    if (dma) {
        ++driver->dma_packets;
        if (!driver->dma_stream(driver->context, command_address, words, 16U)) {
            driver->last_error_address = (uint32_t)command_address;
            return RIX_DRV_ERROR;
        }
    } else if (driver->fifo_write_words != NULL) {
        driver->fifo_write_words(driver->context, commands, words);
    } else {
        for (index = 0U; index < words; ++index)
            rix_drv_fifo_write(driver, RIX_DRV_FIFO_TDFD, commands[index]);
    }
    /* The FIFO data aperture is Device bufferable on hardware, and TDFD (the
     * AXI4 data window) and TLF (the register window) are separate GP1
     * interconnect ports.  Ensure all TDFD writes have completed before TLF
     * establishes the packet boundary required by PG080. */
    if (driver->fifo_drain != NULL)
        driver->fifo_drain(driver->context);
    rix_drv_fifo_write(driver, RIX_DRV_FIFO_TLF, driver->last_packet_bytes);
    isr = rix_drv_fifo_read(driver, RIX_DRV_FIFO_ISR);
    if ((isr & RIX_DRV_FIFO_ISR_ERRORS) != 0U) {
        driver->last_error_status = isr;
        driver->last_error_address = (uint32_t)(command_address != (uintptr_t)0U ?
                                                command_address :
                                                (uintptr_t)commands);
        return RIX_DRV_ERROR;
    }
    return RIX_DRV_OK;
}

int rix_drv_init(struct rix_drv *driver, const struct rix_drv_config *config)
{
    uint32_t identity, version;

    if (driver == NULL || config == NULL)
        return RIX_DRV_BAD_ARGUMENT;
    driver->context = config->context;
    driver->read = config->read;
    driver->write = config->write;
    driver->fifo_read = config->fifo_read;
    driver->fifo_write = config->fifo_write;
    driver->fifo_write_words = config->fifo_write_words;
    driver->fifo_drain = config->fifo_drain;
    driver->clean_dcache = config->clean_dcache;
    driver->dma_stream = config->dma_stream;
    driver->dma_start_async = config->dma_start_async;
    driver->dma_retire = config->dma_retire;
    driver->dma_complete = config->dma_complete;
    driver->dma_start_nonblocking = config->dma_start_nonblocking;
    driver->dma_ready = config->dma_ready;
    driver->dma_abort = config->dma_abort;
    driver->submit_mode = config->submit_mode;
    driver->pending_bytes = 0U;
    driver->pending_dma = 0;
    driver->last_status = 0U;
    driver->last_error_status = 0U;
    driver->last_error_address = 0U;
    driver->submitted_target = 0U;
    driver->memfetch_commit_count = 0U;
    driver->last_packet_bytes = 0U;
    driver->packets = 0U;
    driver->dma_packets = 0U;
    driver->last_packet_dma = 0;
    driver->packet_words = RIX_DRV_FIFO_PACKET_WORDS;
    driver->fifo_capacity = RIX_DRV_FIFO_LEGACY_VACANCY;
    driver->available = 0;
    driver->enabled = 0;
    driver->fifo_vacancy_estimate = 0U;
    driver->device_reads = 0U;

    if (!config->hardware_present || config->read == NULL ||
        config->write == NULL || config->fifo_read == NULL ||
        config->fifo_write == NULL ||
        config->submit_mode > RIX_DRV_SUBMIT_MEMFETCH)
        return RIX_DRV_UNAVAILABLE;
    identity = rix_drv_read(driver, RIX_DRV_CSR_IDENTITY);
    version = rix_drv_read(driver, RIX_DRV_CSR_VERSION);
    if (identity != RIX_DRV_IDENTITY_VALUE ||
        (version & 0xffff0000U) != (RIX_DRV_VERSION_VALUE & 0xffff0000U))
        return RIX_DRV_UNAVAILABLE;
    driver->available = 1;
    return RIX_DRV_OK;
}

int rix_drv_enable(struct rix_drv *driver)
{
    uint32_t isr;

    if (!rix_drv_usable(driver))
        return RIX_DRV_UNAVAILABLE;
    driver->pending_dma = 0;
    rix_drv_fifo_write(driver, RIX_DRV_FIFO_TDFR, RIX_DRV_FIFO_RESET_KEY);
    rix_drv_set_packet_words(driver);
    /* TDFR leaves ISR intact; clear a previous context's sticky errors so the
     * per-packet commit check only sees this context's packets. */
    isr = rix_drv_fifo_read(driver, RIX_DRV_FIFO_ISR) & RIX_DRV_FIFO_ISR_ERRORS;
    if (isr != 0U)
        rix_drv_fifo_write(driver, RIX_DRV_FIFO_ISR, isr);
    rix_drv_write(driver, RIX_DRV_CSR_CONTROL, RIX_DRV_CONTROL_ENABLE);
    driver->enabled = 1;
    return RIX_DRV_OK;
}

static int rix_drv_submit_packets(struct rix_drv *driver, const uint32_t *commands,
                                  uintptr_t command_address, uint32_t command_words,
                                  uint32_t timeout)
{
    uint32_t first, count;
    int result;

    first = 0U;
    while (first < command_words) {
        count = 0U;
        while (first + count < command_words) {
            uint32_t words = rix_drv_command_words(commands[first + count]);

            if (words == 0U || words > command_words - first - count)
                return RIX_DRV_BAD_ARGUMENT;
            if (words > driver->packet_words - count)
                break;
            count += words;
        }
        result = rix_drv_send_packet(driver, commands + first,
                                     command_address == (uintptr_t)0U ? 0U :
                                     command_address + first * sizeof(*commands),
                                     count, timeout);
        if (result != RIX_DRV_OK)
            return result;
        first += count;
    }
    return RIX_DRV_OK;
}

static int rix_drv_submit_memfetch(struct rix_drv *driver,
                                   const uint32_t *commands,
                                   uintptr_t command_address,
                                   uint32_t command_words, uint32_t timeout)
{
    uint32_t first = 0U;
    int result;

    result = rix_drv_memfetch_validate(command_address, command_words);
    if (result != RIX_DRV_OK)
        return result;
    while (first < command_words) {
        uint32_t count = rix_drv_memfetch_chunk(first, command_words);

        result = rix_drv_wait_vacancy(driver, 2U, timeout);
        if (result != RIX_DRV_OK)
            return result;
        result = rix_drv_memfetch_commit(
            driver, command_address + first * sizeof(*commands), count);
        if (result != RIX_DRV_OK)
            return result;
        first += count;
    }
    return RIX_DRV_OK;
}

int rix_drv_submit(struct rix_drv *driver, const uint32_t *commands,
                   uintptr_t command_address, uint32_t command_words,
                   uint32_t timeout)
{
    int result;

    if (!rix_drv_usable(driver) || !driver->enabled || commands == NULL ||
        command_words == 0U)
        return RIX_DRV_BAD_ARGUMENT;
    minigl_profile_push(MGLP_FIFO);
    minigl_profile_count(MGLP_N_FIFO_WORDS, command_words);
    minigl_profile_count(MGLP_N_FIFO_SUBMITS, 1U);
    if (driver->clean_dcache != NULL && command_address != (uintptr_t)0U)
        driver->clean_dcache(driver->context, command_address,
                             (size_t)command_words * sizeof(*commands));
    driver->memfetch_committed = 0U;
    result = driver->submit_mode == RIX_DRV_SUBMIT_MEMFETCH ?
             rix_drv_submit_memfetch(driver, commands, command_address,
                                     command_words, timeout) :
             rix_drv_submit_packets(driver, commands, command_address,
                                    command_words, timeout);
    if (result == RIX_DRV_OK)
        result = rix_drv_flush_pending(driver);
    /* The blocking path stages words without tracking them. */
    driver->fifo_vacancy_estimate = 0U;
    minigl_profile_pop();
    if (result != RIX_DRV_OK)
        return result;
    ++driver->submitted_target;
    return RIX_DRV_OK;
}

int rix_drv_feed_begin(struct rix_drv_feed *feed, const uint32_t *commands,
                       uintptr_t command_address, uint32_t command_words,
                       uint32_t source)
{
    if (feed == NULL || commands == NULL || command_words == 0U ||
        source >= RIX_DRV_FEED_SOURCE_COUNT)
        return RIX_DRV_BAD_ARGUMENT;
    feed->commands = commands;
    feed->command_address = command_address;
    feed->command_words = command_words;
    feed->command_offset = 0U;
    feed->staged_bytes = 0U;
    feed->source = source;
    feed->pending_dma = 0;
    feed->pending_launch = 0;
    feed->active = 1;
    return RIX_DRV_OK;
}

void rix_drv_feed_reset(struct rix_drv_feed *feed)
{
    if (feed == NULL)
        return;
    feed->command_offset = 0U;
    feed->staged_bytes = 0U;
    feed->pending_dma = 0;
    feed->pending_launch = 0;
    feed->active = 0;
}

static int rix_drv_try_feed_memfetch(struct rix_drv *driver,
                                     struct rix_drv_feed *feed);

/* Nothing stops the FrameStreamingCore once it has accepted an envelope:
 * resetting it mid-fetch can leave an accepted AXI transaction behind, and
 * an envelope may end inside a command the next one completes.  Aborting a
 * DDR list therefore finishes pushing it; the caller then waits for the
 * list's fence and idle before any command memory is reused. */
static int rix_drv_memfetch_abort(struct rix_drv *driver, struct rix_drv_feed *feed)
{
    int result;

    if (!feed->active)
        return RIX_DRV_FEED_DONE;
    result = rix_drv_try_feed_memfetch(driver, feed);
    if (result == RIX_DRV_FEED_PROGRESS)
        return RIX_DRV_FEED_BLOCKED;
    return result;
}

int rix_drv_feed_abort(struct rix_drv *driver, struct rix_drv_feed *feed)
{
    int state;

    if (!rix_drv_usable(driver) || !driver->enabled || feed == NULL)
        return RIX_DRV_FEED_ERROR;
    if (driver->submit_mode == RIX_DRV_SUBMIT_MEMFETCH)
        return rix_drv_memfetch_abort(driver, feed);
    if (!feed->active)
        return RIX_DRV_FEED_DONE;
    if (feed->pending_launch) {
        /*
         * The FIFO header is already staged, but the debug launch handshake
         * has not completed.  Retrying DMAGO here can keep cancellation
         * pending forever; hand it to the bounded channel-abort state
         * machine instead.
         */
        feed->pending_launch = 0;
        feed->pending_dma = 1;
    }
    if (feed->pending_dma) {
        if (driver->dma_abort == NULL)
            return RIX_DRV_FEED_ERROR;
        state = driver->dma_abort(driver->context);
        if (state == RIX_DRV_DMA_PENDING)
            return RIX_DRV_FEED_BLOCKED;
        if (state != RIX_DRV_DMA_COMPLETE)
            return RIX_DRV_FEED_ERROR;
    }
    rix_drv_fifo_write(driver, RIX_DRV_FIFO_TDFR, RIX_DRV_FIFO_RESET_KEY);
    driver->fifo_vacancy_estimate = 0U;
    state = rix_drv_fifo_read(driver, RIX_DRV_FIFO_ISR) & RIX_DRV_FIFO_ISR_ERRORS;
    if (state != 0)
        rix_drv_fifo_write(driver, RIX_DRV_FIFO_ISR, (uint32_t)state);
    rix_drv_feed_reset(feed);
    return RIX_DRV_FEED_DONE;
}

static int rix_drv_try_feed_memfetch(struct rix_drv *driver,
                                     struct rix_drv_feed *feed)
{
    uintptr_t command_address;
    uint32_t count;
    uint32_t vacancy;
    int result;

    result = rix_drv_memfetch_validate(feed->command_address,
                                       feed->command_words);
    if (result != RIX_DRV_OK)
        return RIX_DRV_FEED_ERROR;
    vacancy = rix_drv_feed_vacancy(driver, 2U);
    if (vacancy < 2U) {
        minigl_profile_feed_retry();
        return RIX_DRV_FEED_BLOCKED;
    }
    count = rix_drv_memfetch_chunk(feed->command_offset, feed->command_words);
    command_address = feed->command_address +
                      feed->command_offset * sizeof(*feed->commands);
    result = rix_drv_memfetch_commit(driver, command_address, count);
    feed->command_offset += count;
    if (feed->command_offset == feed->command_words)
        feed->active = 0;
    if (result != RIX_DRV_OK)
        return RIX_DRV_FEED_ERROR;
    minigl_profile_feed_chunk(feed->source);
    if (!feed->active)
        return RIX_DRV_FEED_DONE;
    return RIX_DRV_FEED_PROGRESS;
}
int rix_drv_try_feed(struct rix_drv *driver, struct rix_drv_feed *feed)
{
    uint32_t vacancy, limit, count, physical_payload;
    uintptr_t command_address;
    int dma;

    if (!rix_drv_usable(driver) || !driver->enabled || feed == NULL)
        return RIX_DRV_FEED_ERROR;
    if (!feed->active)
        return RIX_DRV_FEED_DONE;
    if (driver->submit_mode == RIX_DRV_SUBMIT_MEMFETCH)
        return rix_drv_try_feed_memfetch(driver, feed);
    if (feed->pending_launch) {
        int state;

        if (driver->dma_start_nonblocking == NULL)
            return RIX_DRV_FEED_ERROR;
        ++driver->device_reads;
        state = driver->dma_start_nonblocking(driver->context,
            feed->command_address + feed->command_offset * sizeof(*feed->commands),
            feed->staged_bytes / sizeof(uint32_t) - 2U, 16U);
        if (state == RIX_DRV_DMA_PENDING)
            return RIX_DRV_FEED_BLOCKED;
        if (state != RIX_DRV_DMA_COMPLETE)
            return RIX_DRV_FEED_ERROR;
        feed->pending_launch = 0;
        feed->pending_dma = 1;
        return RIX_DRV_FEED_PROGRESS;
    }
    if (feed->pending_dma) {
        int state;

        if (driver->dma_complete == NULL)
            return RIX_DRV_FEED_ERROR;
        ++driver->device_reads;
        state = driver->dma_complete(driver->context);
        if (state == RIX_DRV_DMA_PENDING)
            return RIX_DRV_FEED_BLOCKED;
        if (state != RIX_DRV_DMA_COMPLETE)
            return RIX_DRV_FEED_ERROR;
        minigl_profile_feed_dma_complete();
        return rix_drv_feed_commit(driver, feed);
    }

    physical_payload = driver->fifo_capacity > 2U ? driver->fifo_capacity - 2U : 0U;
    vacancy = rix_drv_feed_vacancy(driver, RIX_DRV_FEED_MAX_WORDS + 2U);
    if (vacancy <= 2U) {
        driver->fifo_vacancy_estimate = 0U;
        minigl_profile_feed_retry();
        return RIX_DRV_FEED_BLOCKED;
    }
    limit = vacancy - 2U;
    if (limit > RIX_DRV_FEED_MAX_WORDS)
        limit = RIX_DRV_FEED_MAX_WORDS;
    count = 0U;
    while (feed->command_offset + count < feed->command_words) {
        uint32_t words = rix_drv_command_words(
            feed->commands[feed->command_offset + count]);
        if (words == 0U || words > feed->command_words -
                                     feed->command_offset - count ||
            words > physical_payload)
            return RIX_DRV_FEED_ERROR;
        if (words > limit - count) {
            if (count == 0U && words <= vacancy - 2U)
                count = words;
            break;
        }
        count += words;
    }
    if (count == 0U) {
        /* A large command waits for real vacancy, not the estimate. */
        driver->fifo_vacancy_estimate = 0U;
        minigl_profile_feed_retry();
        return RIX_DRV_FEED_BLOCKED;
    }

    command_address = feed->command_address == (uintptr_t)0U ? 0U :
        feed->command_address + feed->command_offset * sizeof(*feed->commands);
    dma = command_address != (uintptr_t)0U && count >= 16U &&
          driver->dma_start_nonblocking != NULL && driver->dma_complete != NULL;
    if (dma && driver->dma_ready != NULL && !driver->dma_ready(driver->context))
        return RIX_DRV_FEED_ERROR;
    feed->staged_bytes = (count + 2U) * sizeof(*feed->commands);
    driver->fifo_vacancy_estimate -= count + 2U;
    rix_drv_fifo_write(driver, RIX_DRV_FIFO_TDFD,
                       RIX_DSE_STREAM | count * sizeof(*feed->commands));
    rix_drv_fifo_write(driver, RIX_DRV_FIFO_TDFD, 0U);
    if (dma) {
        int state;

        ++driver->device_reads;
        state = driver->dma_start_nonblocking(driver->context, command_address,
                                                   count, 16U);

        if (state == RIX_DRV_DMA_PENDING) {
            feed->pending_launch = 1;
            return RIX_DRV_FEED_BLOCKED;
        }
        if (state != RIX_DRV_DMA_COMPLETE) {
            driver->last_error_address = (uint32_t)command_address;
            return RIX_DRV_FEED_ERROR;
        }
        feed->pending_dma = 1;
        return RIX_DRV_FEED_PROGRESS;
    }
    if (driver->fifo_write_words != NULL) {
        driver->fifo_write_words(driver->context,
                                 feed->commands + feed->command_offset, count);
    } else {
        uint32_t index;

        for (index = 0U; index < count; ++index)
            rix_drv_fifo_write(driver, RIX_DRV_FIFO_TDFD,
                               feed->commands[feed->command_offset + index]);
    }
    return rix_drv_feed_commit(driver, feed);
}
int rix_drv_poll(struct rix_drv *driver, uint32_t timeout)
{
    if (!rix_drv_usable(driver))
        return RIX_DRV_UNAVAILABLE;
    return rix_drv_wait_idle(driver, timeout);
}

int rix_drv_recover(struct rix_drv *driver, uint32_t timeout, int soft_reset)
{
    int result;

    if (!rix_drv_usable(driver))
        return RIX_DRV_UNAVAILABLE;
    result = rix_drv_wait_idle(driver, timeout);
    if (result != RIX_DRV_OK)
        return result;
    /* Drop queued packets or envelopes before the renderer leaves reset. */
    rix_drv_fifo_write(driver, RIX_DRV_FIFO_TDFR, RIX_DRV_FIFO_RESET_KEY);
    if (soft_reset)
        rix_drv_write(driver, RIX_DRV_CSR_CONTROL,
                      RIX_DRV_CONTROL_ENABLE | RIX_DRV_CONTROL_SOFT_RESET);
    driver->pending_dma = 0;
    rix_drv_set_packet_words(driver);
    return RIX_DRV_OK;
}

int rix_drv_disable(struct rix_drv *driver, uint32_t timeout)
{
    int result;

    if (!rix_drv_usable(driver))
        return RIX_DRV_UNAVAILABLE;
    result = rix_drv_wait_idle(driver, timeout);
    if (result != RIX_DRV_OK)
        return result;
    rix_drv_write(driver, RIX_DRV_CSR_CONTROL, 0U);
    driver->enabled = 0;
    return RIX_DRV_OK;
}
