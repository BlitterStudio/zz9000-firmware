/*
 * RasterIX DDR transport driver for ZZ9000.
 * Ported RasterIX/ZZ9000 code; derived from RasterIX by ToNi3141.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RIX_DRV_H
#define RIX_DRV_H

#include <stddef.h>
#include <stdint.h>

#define RIX_DRV_IDENTITY_VALUE 0x52495831U
#define RIX_DRV_VERSION_VALUE  0x00010001U

#define RIX_DRV_CSR_IDENTITY     0x00U
#define RIX_DRV_CSR_VERSION      0x04U
#define RIX_DRV_CSR_CONTROL      0x08U
#define RIX_DRV_CSR_STATUS       0x0cU
#define RIX_DRV_CSR_CMD_ADDR     0x1cU
#define RIX_DRV_CSR_CMD_WORDS    0x20U
#define RIX_DRV_CSR_CMD_START    0x24U
#define RIX_DRV_CSR_SUBMITTED    0x28U
#define RIX_DRV_CSR_COMPLETED    0x2cU
#define RIX_DRV_CSR_ERROR_STATUS 0x30U
#define RIX_DRV_CSR_ERROR_ADDR   0x34U

#define RIX_DRV_CONTROL_ENABLE     (1U << 0)
#define RIX_DRV_CONTROL_SOFT_RESET (1U << 1)
#define RIX_DRV_CONTROL_ACK_ERROR  (1U << 2)

#define RIX_DRV_STATUS_IDLE          (1U << 1)
#define RIX_DRV_CMD_START_QUEUE_READY (1U << 0)

enum rix_drv_result {
    RIX_DRV_OK = 0,
    RIX_DRV_UNAVAILABLE = -1,
    RIX_DRV_BAD_ARGUMENT = -2,
    RIX_DRV_TIMEOUT = -3,
    RIX_DRV_ERROR = -4
};

typedef uint32_t (*rix_drv_read_fn)(void *context, uint32_t offset);
typedef void (*rix_drv_write_fn)(void *context, uint32_t offset,
                                 uint32_t value);
typedef void (*rix_drv_cache_clean_fn)(void *context, uintptr_t address,
                                       size_t bytes);

/* The caller must set hardware_present from its build-time capability gate.
 * When it is zero, rix_drv_init() does not access the CSR window. */
struct rix_drv_config {
    void *context;
    rix_drv_read_fn read;
    rix_drv_write_fn write;
    rix_drv_cache_clean_fn clean_dcache;
    int hardware_present;
};

struct rix_drv {
    void *context;
    rix_drv_read_fn read;
    rix_drv_write_fn write;
    rix_drv_cache_clean_fn clean_dcache;
    uint32_t last_status;
    uint32_t last_error_status;
    uint32_t last_error_address;
    uint32_t submitted_target;
    int available;
    int enabled;
};

int rix_drv_init(struct rix_drv *driver, const struct rix_drv_config *config);
int rix_drv_enable(struct rix_drv *driver);
int rix_drv_submit(struct rix_drv *driver, uintptr_t command_address,
                   uint32_t command_words, uint32_t timeout);
int rix_drv_poll(struct rix_drv *driver, uint32_t completed_target,
                 uint32_t timeout);
int rix_drv_recover(struct rix_drv *driver, uint32_t timeout, int soft_reset);
int rix_drv_disable(struct rix_drv *driver, uint32_t timeout);

#endif /* RIX_DRV_H */
