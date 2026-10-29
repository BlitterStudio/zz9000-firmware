/*
 * RasterIX EF FIFO transport driver for ZZ9000.
 *
 * The bounded DSE packet and texture ordering rules follow XACP librixgpu
 * (0BSD).  This implementation is part of ZZ9000 firmware.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rix_drv.h"

#define RIX_DSE_STREAM 0x90000000U

static uint32_t rix_drv_read(const struct rix_drv *driver, uint32_t offset)
{
    return driver->read(driver->context, offset);
}

static void rix_drv_write(const struct rix_drv *driver, uint32_t offset,
                          uint32_t value)
{
    driver->write(driver->context, offset, value);
}

static uint32_t rix_drv_fifo_read(const struct rix_drv *driver, uint32_t offset)
{
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

    for (;;) {
        if (rix_drv_fifo_read(driver, RIX_DRV_FIFO_TDFV) >= words)
            return RIX_DRV_OK;
        if (attempt++ >= timeout)
            return RIX_DRV_TIMEOUT;
    }
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

static int rix_drv_send_packet(struct rix_drv *driver, const uint32_t *commands,
                               uintptr_t command_address, uint32_t words,
                               uint32_t timeout)
{
    uint32_t index;
    int result;

    if (words == 0U || words > RIX_DRV_FIFO_PACKET_WORDS)
        return RIX_DRV_BAD_ARGUMENT;
    result = rix_drv_wait_vacancy(driver, words + 2U, timeout);
    if (result != RIX_DRV_OK)
        return result;

    rix_drv_fifo_write(driver, RIX_DRV_FIFO_TDFD,
                       RIX_DSE_STREAM | (words * sizeof(*commands)));
    rix_drv_fifo_write(driver, RIX_DRV_FIFO_TDFD, 0U);
    if (driver->dma_stream != NULL && command_address != (uintptr_t)0U &&
        words >= 16U) {
        if (!driver->dma_stream(driver->context, command_address, words, 16U))
            return RIX_DRV_ERROR;
    } else {
        for (index = 0U; index < words; ++index)
            rix_drv_fifo_write(driver, RIX_DRV_FIFO_TDFD, commands[index]);
    }
    /* TLF is the packet boundary.  The callback has completed every fixed
     * destination write before this commit, as required by PG080. */
    rix_drv_fifo_write(driver, RIX_DRV_FIFO_TLF, (words + 2U) * sizeof(*commands));
    return RIX_DRV_OK;
}

int rix_drv_init(struct rix_drv *driver, const struct rix_drv_config *config)
{
    uint32_t identity;

    if (driver == NULL || config == NULL)
        return RIX_DRV_BAD_ARGUMENT;
    driver->context = config->context;
    driver->read = config->read;
    driver->write = config->write;
    driver->fifo_read = config->fifo_read;
    driver->fifo_write = config->fifo_write;
    driver->clean_dcache = config->clean_dcache;
    driver->dma_stream = config->dma_stream;
    driver->last_status = 0U;
    driver->last_error_status = 0U;
    driver->last_error_address = 0U;
    driver->submitted_target = 0U;
    driver->available = 0;
    driver->enabled = 0;

    if (!config->hardware_present || config->read == NULL || config->write == NULL ||
        config->fifo_read == NULL || config->fifo_write == NULL)
        return RIX_DRV_UNAVAILABLE;
    identity = rix_drv_read(driver, RIX_DRV_CSR_IDENTITY);
    if (identity != RIX_DRV_IDENTITY_VALUE ||
        rix_drv_read(driver, RIX_DRV_CSR_VERSION) != RIX_DRV_VERSION_VALUE)
        return RIX_DRV_UNAVAILABLE;
    driver->available = 1;
    return RIX_DRV_OK;
}

int rix_drv_enable(struct rix_drv *driver)
{
    if (!rix_drv_usable(driver))
        return RIX_DRV_UNAVAILABLE;
    rix_drv_fifo_write(driver, RIX_DRV_FIFO_TDFR, RIX_DRV_FIFO_RESET_KEY);
    rix_drv_write(driver, RIX_DRV_CSR_CONTROL, RIX_DRV_CONTROL_ENABLE);
    driver->enabled = 1;
    return RIX_DRV_OK;
}

int rix_drv_submit(struct rix_drv *driver, const uint32_t *commands,
                   uintptr_t command_address, uint32_t command_words,
                   uint32_t timeout)
{
    uint32_t first, count;
    int result;

    if (!rix_drv_usable(driver) || !driver->enabled || commands == NULL ||
        command_words == 0U)
        return RIX_DRV_BAD_ARGUMENT;
    if (driver->clean_dcache != NULL && command_address != (uintptr_t)0U)
        driver->clean_dcache(driver->context, command_address,
                             (size_t)command_words * sizeof(*commands));

    first = 0U;
    while (first < command_words) {
        count = 0U;
        while (first + count < command_words) {
            uint32_t words = rix_drv_command_words(commands[first + count]);

            if (words == 0U || words > command_words - first - count ||
                words > RIX_DRV_FIFO_PACKET_WORDS - count)
                return RIX_DRV_BAD_ARGUMENT;
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
    ++driver->submitted_target;
    return RIX_DRV_OK;
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
    if (soft_reset)
        rix_drv_write(driver, RIX_DRV_CSR_CONTROL,
                      RIX_DRV_CONTROL_ENABLE | RIX_DRV_CONTROL_SOFT_RESET);
    rix_drv_fifo_write(driver, RIX_DRV_FIFO_TDFR, RIX_DRV_FIFO_RESET_KEY);
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
