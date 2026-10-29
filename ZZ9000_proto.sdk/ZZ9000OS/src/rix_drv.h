/*
 * RasterIX EF FIFO transport driver for ZZ9000.
 * The command path follows librixgpu's bounded AXI-stream packets.  The
 * original chunk/fence discipline is 0BSD; this driver is GPL-3.0-or-later.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RIX_DRV_H
#define RIX_DRV_H

#include <stddef.h>
#include <stdint.h>

#define RIX_DRV_IDENTITY_VALUE 0x52495831U
#define RIX_DRV_VERSION_VALUE  0x00020000U

#define RIX_DRV_CSR_IDENTITY     0x00U
#define RIX_DRV_CSR_VERSION      0x04U
#define RIX_DRV_CSR_CONTROL      0x08U
#define RIX_DRV_CSR_STATUS       0x0cU
#define RIX_DRV_CSR_DBG_STREAM   0x10U
#define RIX_DRV_CSR_DBG_RESPONSE 0x14U
#define RIX_DRV_CSR_DBG_AW       0x38U
#define RIX_DRV_CSR_DBG_W        0x3cU
#define RIX_DRV_CSR_DBG_B        0x40U
#define RIX_DRV_CSR_DBG_AR       0x44U
#define RIX_DRV_CSR_DBG_R        0x48U
#define RIX_DRV_CSR_DBG_LAST_AR  0x4cU
#define RIX_DRV_CSR_DBG_LAST_ARID 0x50U

#define RIX_DRV_FIFO_ISR         0x00U
#define RIX_DRV_FIFO_IER         0x04U
#define RIX_DRV_FIFO_TDFR        0x08U
#define RIX_DRV_FIFO_TDFV        0x0cU
#define RIX_DRV_FIFO_TDFD        0x10U
#define RIX_DRV_FIFO_TLF         0x14U
#define RIX_DRV_FIFO_RESET_KEY   0xa5U
#define RIX_DRV_FIFO_PACKET_WORDS 248U

/* PG080 ISR bits.  A TLF value that differs from the bytes written to TDFD
 * since the last accepted TLF is dropped and sets TSE; the staged words then
 * never reach the stream, and every later TLF mismatches too. */
#define RIX_DRV_FIFO_ISR_TPOE    (1U << 28)
#define RIX_DRV_FIFO_ISR_TSE     (1U << 25)
#define RIX_DRV_FIFO_ISR_ERRORS  (RIX_DRV_FIFO_ISR_TPOE | RIX_DRV_FIFO_ISR_TSE)

#define RIX_DRV_CONTROL_ENABLE     (1U << 0)
#define RIX_DRV_CONTROL_SOFT_RESET (1U << 1)

#define RIX_DRV_STATUS_IDLE        (1U << 1)

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
typedef int (*rix_drv_dma_stream_fn)(void *context, uintptr_t source,
                                     uint32_t words, uint32_t burst_words);

/* The caller must set hardware_present from its build-time capability gate.
 * When it is zero, rix_drv_init() does not access the CSR window. */
struct rix_drv_config {
    void *context;
    rix_drv_read_fn read;
    rix_drv_write_fn write;
    rix_drv_read_fn fifo_read;
    rix_drv_write_fn fifo_write;
    rix_drv_cache_clean_fn clean_dcache;
    rix_drv_dma_stream_fn dma_stream;
    int hardware_present;
};

struct rix_drv {
    void *context;
    rix_drv_read_fn read;
    rix_drv_write_fn write;
    rix_drv_read_fn fifo_read;
    rix_drv_write_fn fifo_write;
    rix_drv_cache_clean_fn clean_dcache;
    rix_drv_dma_stream_fn dma_stream;
    uint32_t last_status;
    /* FIFO ISR after a rejected packet commit, else zero. */
    uint32_t last_error_status;
    /* Source address of the rejected packet, else zero. */
    uint32_t last_error_address;
    uint32_t submitted_target;
    /* Transport record of the last packet started, for diagnostics. */
    uint32_t last_packet_bytes;
    uint32_t packets;
    uint32_t dma_packets;
    int last_packet_dma;
    int available;
    int enabled;
};

int rix_drv_init(struct rix_drv *driver, const struct rix_drv_config *config);
int rix_drv_enable(struct rix_drv *driver);
/* Streams parser words as bounded DSE packets. `command_address` is the
 * non-cacheable physical source used by optional PL330 transport. */
int rix_drv_submit(struct rix_drv *driver, const uint32_t *commands,
                   uintptr_t command_address, uint32_t command_words,
                   uint32_t timeout);
/* Waits for the renderer to become idle after a pixel fence has passed. */
int rix_drv_poll(struct rix_drv *driver, uint32_t timeout);
int rix_drv_recover(struct rix_drv *driver, uint32_t timeout, int soft_reset);
int rix_drv_disable(struct rix_drv *driver, uint32_t timeout);

#endif /* RIX_DRV_H */
