/*
 * RasterIX DDR transport driver host tests for ZZ9000.
 * Ported RasterIX/ZZ9000 code; derived from RasterIX by ToNi3141.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rix_drv.h"

#define CSR_WORDS ((RIX_DRV_CSR_ERROR_ADDR / 4U) + 1U)
#define WRITE_LOG_CAPACITY 16U

struct register_write {
    uint32_t offset;
    uint32_t value;
};

struct register_model {
    uint32_t csr[CSR_WORDS];
    struct register_write writes[WRITE_LOG_CAPACITY];
    uintptr_t clean_address;
    size_t clean_bytes;
    unsigned clean_count;
    unsigned read_count;
    unsigned write_count;
    int accept_start;
};

static uint32_t model_read(void *context, uint32_t offset)
{
    struct register_model *model = context;

    assert((offset & 3U) == 0U);
    assert(offset / 4U < CSR_WORDS);
    ++model->read_count;
    return model->csr[offset / 4U];
}

static void model_write(void *context, uint32_t offset, uint32_t value)
{
    struct register_model *model = context;

    assert((offset & 3U) == 0U);
    assert(offset / 4U < CSR_WORDS);
    assert(model->write_count < WRITE_LOG_CAPACITY);
    model->writes[model->write_count].offset = offset;
    model->writes[model->write_count].value = value;
    ++model->write_count;
    model->csr[offset / 4U] = value;
    if (offset == RIX_DRV_CSR_CMD_START && value == 1U && model->accept_start) {
        ++model->csr[RIX_DRV_CSR_SUBMITTED / 4U];
    }
}

static void model_clean(void *context, uintptr_t address, size_t bytes)
{
    struct register_model *model = context;

    model->clean_address = address;
    model->clean_bytes = bytes;
    ++model->clean_count;
}

static struct rix_drv_config model_config(struct register_model *model,
                                          int hardware_present)
{
    struct rix_drv_config config;

    config.context = model;
    config.read = model_read;
    config.write = model_write;
    config.clean_dcache = model_clean;
    config.hardware_present = hardware_present;
    return config;
}

static void model_identify(struct register_model *model)
{
    model->csr[RIX_DRV_CSR_IDENTITY / 4U] = RIX_DRV_IDENTITY_VALUE;
    model->csr[RIX_DRV_CSR_VERSION / 4U] = RIX_DRV_VERSION_VALUE;
}

static void test_absent_gate_does_not_read(void)
{
    struct register_model model;
    struct rix_drv driver;
    struct rix_drv_config config;

    memset(&model, 0, sizeof(model));
    config = model_config(&model, 0);
    assert(rix_drv_init(&driver, &config) == RIX_DRV_UNAVAILABLE);
    assert(model.read_count == 0U);
    assert(model.write_count == 0U);
}

static void test_identity_mismatch_is_unavailable(void)
{
    struct register_model model;
    struct rix_drv driver;
    struct rix_drv_config config;

    memset(&model, 0, sizeof(model));
    model.csr[RIX_DRV_CSR_IDENTITY / 4U] = 0U;
    config = model_config(&model, 1);
    assert(rix_drv_init(&driver, &config) == RIX_DRV_UNAVAILABLE);
    assert(model.read_count == 1U);
    assert(model.write_count == 0U);
}

static void test_submit_and_poll_success(void)
{
    struct register_model model;
    struct rix_drv driver;
    struct rix_drv_config config;

    memset(&model, 0, sizeof(model));
    model_identify(&model);
    model.csr[RIX_DRV_CSR_CMD_START / 4U] = RIX_DRV_CMD_START_QUEUE_READY;
    model.accept_start = 1;
    config = model_config(&model, 1);
    assert(rix_drv_init(&driver, &config) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(model.writes[0].offset == RIX_DRV_CSR_CONTROL);
    assert(model.writes[0].value == RIX_DRV_CONTROL_ENABLE);
    assert(rix_drv_submit(&driver, (uintptr_t)0x1000U, 6U, 0U) == RIX_DRV_OK);
    assert(model.clean_count == 1U);
    assert(model.clean_address == (uintptr_t)0x1000U);
    assert(model.clean_bytes == 24U);
    assert(model.csr[RIX_DRV_CSR_CMD_ADDR / 4U] == 0x1000U);
    assert(model.csr[RIX_DRV_CSR_CMD_WORDS / 4U] == 6U);
    assert(driver.submitted_target == 1U);
    model.csr[RIX_DRV_CSR_COMPLETED / 4U] = 1U;
    assert(rix_drv_poll(&driver, driver.submitted_target, 0U) == RIX_DRV_OK);
}

static void test_submit_rejects_invalid_descriptors(void)
{
    struct register_model model;
    struct rix_drv driver;
    struct rix_drv_config config;

    memset(&model, 0, sizeof(model));
    model_identify(&model);
    config = model_config(&model, 1);
    assert(rix_drv_init(&driver, &config) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_submit(&driver, (uintptr_t)0x1004U, 1U, 0U) ==
           RIX_DRV_BAD_ARGUMENT);
    assert(rix_drv_submit(&driver, (uintptr_t)0x1000U, 0U, 0U) ==
           RIX_DRV_BAD_ARGUMENT);
    assert(model.clean_count == 0U);
    assert(model.write_count == 1U);
}

static void test_poll_catches_sticky_error(void)
{
    struct register_model model;
    struct rix_drv driver;
    struct rix_drv_config config;

    memset(&model, 0, sizeof(model));
    model_identify(&model);
    model.csr[RIX_DRV_CSR_STATUS / 4U] = 1U << 8;
    model.csr[RIX_DRV_CSR_ERROR_STATUS / 4U] = 1U;
    model.csr[RIX_DRV_CSR_ERROR_ADDR / 4U] = 0x2008U;
    config = model_config(&model, 1);
    assert(rix_drv_init(&driver, &config) == RIX_DRV_OK);
    assert(rix_drv_poll(&driver, 1U, 0U) == RIX_DRV_ERROR);
    assert(driver.last_status == (1U << 8));
    assert(driver.last_error_status == 1U);
    assert(driver.last_error_address == 0x2008U);
}

static void test_poll_times_out(void)
{
    struct register_model model;
    struct rix_drv driver;
    struct rix_drv_config config;

    memset(&model, 0, sizeof(model));
    model_identify(&model);
    config = model_config(&model, 1);
    assert(rix_drv_init(&driver, &config) == RIX_DRV_OK);
    assert(rix_drv_poll(&driver, 1U, 0U) == RIX_DRV_TIMEOUT);
}

static void test_recovery_and_disable_wait_for_idle(void)
{
    struct register_model model;
    struct rix_drv driver;
    struct rix_drv_config config;

    memset(&model, 0, sizeof(model));
    model_identify(&model);
    model.csr[RIX_DRV_CSR_STATUS / 4U] = RIX_DRV_STATUS_IDLE;
    config = model_config(&model, 1);
    assert(rix_drv_init(&driver, &config) == RIX_DRV_OK);
    assert(rix_drv_enable(&driver) == RIX_DRV_OK);
    assert(rix_drv_recover(&driver, 0U, 1) == RIX_DRV_OK);
    assert(model.writes[1].offset == RIX_DRV_CSR_CONTROL);
    assert(model.writes[1].value ==
           (RIX_DRV_CONTROL_ENABLE | RIX_DRV_CONTROL_ACK_ERROR));
    assert(model.writes[2].offset == RIX_DRV_CSR_CONTROL);
    assert(model.writes[2].value ==
           (RIX_DRV_CONTROL_ENABLE | RIX_DRV_CONTROL_SOFT_RESET));
    assert(rix_drv_disable(&driver, 0U) == RIX_DRV_OK);
    assert(model.writes[3].offset == RIX_DRV_CSR_CONTROL);
    assert(model.writes[3].value == 0U);
    assert(driver.enabled == 0);
}

int main(void)
{
    test_absent_gate_does_not_read();
    test_identity_mismatch_is_unavailable();
    test_submit_and_poll_success();
    test_submit_rejects_invalid_descriptors();
    test_poll_catches_sticky_error();
    test_poll_times_out();
    test_recovery_and_disable_wait_for_idle();
    puts("rix_drv_test: ok");
    return 0;
}
