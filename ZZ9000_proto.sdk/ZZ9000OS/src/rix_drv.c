/*
 * RasterIX DDR transport driver for ZZ9000.
 * Ported RasterIX/ZZ9000 code; derived from RasterIX by ToNi3141.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rix_drv.h"

#if !defined(MINIGL_HOST_TEST)
#include "xil_cache.h"
#endif

static uint32_t rix_drv_read(const struct rix_drv *driver, uint32_t offset)
{
    return driver->read(driver->context, offset);
}

static void rix_drv_write(const struct rix_drv *driver, uint32_t offset,
                          uint32_t value)
{
    driver->write(driver->context, offset, value);
}

#if !defined(MINIGL_HOST_TEST)
static void rix_drv_platform_cache_clean(void *context, uintptr_t address,
                                         size_t bytes)
{
    (void)context;
    Xil_DCacheFlushRange((INTPTR)address, (u32)bytes);
}
#endif

static int rix_drv_usable(const struct rix_drv *driver)
{
    return driver != NULL && driver->available && driver->read != NULL &&
           driver->write != NULL;
}

static uint32_t rix_drv_control_value(const struct rix_drv *driver)
{
    return driver->enabled ? RIX_DRV_CONTROL_ENABLE : 0U;
}

static int rix_drv_capture_error(struct rix_drv *driver,
                                 uint32_t error_status)
{
    driver->last_error_status = error_status;
    driver->last_status = rix_drv_read(driver, RIX_DRV_CSR_STATUS);
    driver->last_error_address = rix_drv_read(driver, RIX_DRV_CSR_ERROR_ADDR);
    return RIX_DRV_ERROR;
}

static int rix_drv_check_error(struct rix_drv *driver)
{
    uint32_t error_status = rix_drv_read(driver, RIX_DRV_CSR_ERROR_STATUS);

    if (error_status != 0U) {
        return rix_drv_capture_error(driver, error_status);
    }
    return RIX_DRV_OK;
}

static int rix_drv_wait_idle(struct rix_drv *driver, uint32_t timeout)
{
    uint32_t attempt = 0U;

    for (;;) {
        driver->last_status = rix_drv_read(driver, RIX_DRV_CSR_STATUS);
        if ((driver->last_status & RIX_DRV_STATUS_IDLE) != 0U) {
            return RIX_DRV_OK;
        }
        if (attempt++ >= timeout) {
            return RIX_DRV_TIMEOUT;
        }
    }
}

int rix_drv_init(struct rix_drv *driver, const struct rix_drv_config *config)
{
    uint32_t identity;

    if (driver == NULL || config == NULL) {
        return RIX_DRV_BAD_ARGUMENT;
    }

    driver->context = config->context;
    driver->read = config->read;
    driver->write = config->write;
    driver->clean_dcache = config->clean_dcache;
    driver->last_status = 0U;
    driver->last_error_status = 0U;
    driver->last_error_address = 0U;
    driver->submitted_target = 0U;
    driver->available = 0;
    driver->enabled = 0;

    if (!config->hardware_present || config->read == NULL ||
        config->write == NULL) {
        return RIX_DRV_UNAVAILABLE;
    }

#if !defined(MINIGL_HOST_TEST)
    if (driver->clean_dcache == NULL) {
        driver->clean_dcache = rix_drv_platform_cache_clean;
    }
#endif

    identity = rix_drv_read(driver, RIX_DRV_CSR_IDENTITY);
    if (identity != RIX_DRV_IDENTITY_VALUE ||
        rix_drv_read(driver, RIX_DRV_CSR_VERSION) != RIX_DRV_VERSION_VALUE) {
        return RIX_DRV_UNAVAILABLE;
    }

    driver->available = 1;
    return RIX_DRV_OK;
}

int rix_drv_enable(struct rix_drv *driver)
{
    if (!rix_drv_usable(driver)) {
        return RIX_DRV_UNAVAILABLE;
    }

    rix_drv_write(driver, RIX_DRV_CSR_CONTROL, RIX_DRV_CONTROL_ENABLE);
    driver->enabled = 1;
    return RIX_DRV_OK;
}

int rix_drv_submit(struct rix_drv *driver, uintptr_t command_address,
                   uint32_t command_words, uint32_t timeout)
{
    uint32_t submitted_before;
    uint32_t submitted_after;
    uint32_t attempt = 0U;
    size_t command_bytes;
    int result;

    if (!rix_drv_usable(driver) || !driver->enabled ||
        driver->clean_dcache == NULL) {
        return RIX_DRV_UNAVAILABLE;
    }
    if (command_address == (uintptr_t)0U ||
        (command_address & (uintptr_t)7U) != (uintptr_t)0U ||
        command_words == 0U || command_words > UINT32_MAX / 4U) {
        return RIX_DRV_BAD_ARGUMENT;
    }

    command_bytes = (size_t)command_words * 4U;
    driver->clean_dcache(driver->context, command_address, command_bytes);

    for (;;) {
        result = rix_drv_check_error(driver);
        if (result != RIX_DRV_OK) {
            return result;
        }
        if ((rix_drv_read(driver, RIX_DRV_CSR_CMD_START) &
             RIX_DRV_CMD_START_QUEUE_READY) != 0U) {
            break;
        }
        if (attempt++ >= timeout) {
            return RIX_DRV_TIMEOUT;
        }
    }

    submitted_before = rix_drv_read(driver, RIX_DRV_CSR_SUBMITTED);
    rix_drv_write(driver, RIX_DRV_CSR_CMD_ADDR, (uint32_t)command_address);
    rix_drv_write(driver, RIX_DRV_CSR_CMD_WORDS, command_words);
    rix_drv_write(driver, RIX_DRV_CSR_CMD_START, 1U);
    submitted_after = rix_drv_read(driver, RIX_DRV_CSR_SUBMITTED);
    if (submitted_after != submitted_before + 1U) {
        result = rix_drv_check_error(driver);
        if (result != RIX_DRV_OK) {
            return result;
        }
        return RIX_DRV_TIMEOUT;
    }

    driver->submitted_target = submitted_after;
    return RIX_DRV_OK;
}

int rix_drv_poll(struct rix_drv *driver, uint32_t completed_target,
                 uint32_t timeout)
{
    uint32_t attempt = 0U;
    int result;

    if (!rix_drv_usable(driver)) {
        return RIX_DRV_UNAVAILABLE;
    }

    for (;;) {
        result = rix_drv_check_error(driver);
        if (result != RIX_DRV_OK) {
            return result;
        }
        if ((int32_t)(rix_drv_read(driver, RIX_DRV_CSR_COMPLETED) -
                      completed_target) >= 0) {
            return RIX_DRV_OK;
        }
        if (attempt++ >= timeout) {
            driver->last_status = rix_drv_read(driver, RIX_DRV_CSR_STATUS);
            return RIX_DRV_TIMEOUT;
        }
    }
}

int rix_drv_recover(struct rix_drv *driver, uint32_t timeout, int soft_reset)
{
    int result;

    if (!rix_drv_usable(driver)) {
        return RIX_DRV_UNAVAILABLE;
    }

    result = rix_drv_wait_idle(driver, timeout);
    if (result != RIX_DRV_OK) {
        return result;
    }

    rix_drv_write(driver, RIX_DRV_CSR_CONTROL,
                  rix_drv_control_value(driver) | RIX_DRV_CONTROL_ACK_ERROR);
    driver->last_error_status = 0U;

    if (soft_reset) {
        rix_drv_write(driver, RIX_DRV_CSR_CONTROL,
                      rix_drv_control_value(driver) |
                      RIX_DRV_CONTROL_SOFT_RESET);
        return rix_drv_wait_idle(driver, timeout);
    }
    return RIX_DRV_OK;
}

int rix_drv_disable(struct rix_drv *driver, uint32_t timeout)
{
    int result;

    if (!rix_drv_usable(driver)) {
        return RIX_DRV_UNAVAILABLE;
    }

    rix_drv_write(driver, RIX_DRV_CSR_CONTROL, 0U);
    driver->enabled = 0;
    result = rix_drv_wait_idle(driver, timeout);
    return result;
}
