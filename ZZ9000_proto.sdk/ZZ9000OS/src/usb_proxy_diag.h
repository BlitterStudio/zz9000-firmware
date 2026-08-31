/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZUSB_PROXY_DIAG_H
#define ZZUSB_PROXY_DIAG_H

#include <stddef.h>
#include <stdint.h>

#define ZZUSB_DIAG_MAGIC             0x5a554447UL
#define ZZUSB_DIAG_VERSION           1U
#define ZZUSB_DIAG_PAGE_SIZE         4096U
#define ZZUSB_DIAG_EVENT_COUNT       64U
#define ZZUSB_DIAG_EVENT_SIZE        32U
#define ZZUSB_DIAG_COUNTER_COUNT     16U

#define ZZUSB_DIAG_OFF_MAGIC         0U
#define ZZUSB_DIAG_OFF_GENERATION    4U
#define ZZUSB_DIAG_OFF_VERSION       8U
#define ZZUSB_DIAG_OFF_HEADER_SIZE   10U
#define ZZUSB_DIAG_OFF_TOTAL_SIZE    12U
#define ZZUSB_DIAG_OFF_CAPABILITIES  16U
#define ZZUSB_DIAG_OFF_EPOCH         20U
#define ZZUSB_DIAG_OFF_LAST_ID       24U
#define ZZUSB_DIAG_OFF_EVENT_NEXT    28U
#define ZZUSB_DIAG_OFF_EVENT_COUNT   32U
#define ZZUSB_DIAG_OFF_LOST_EVENTS   36U
#define ZZUSB_DIAG_OFF_QUEUE_STATE   40U
#define ZZUSB_DIAG_OFF_SCHEDULE_BITS 44U
#define ZZUSB_DIAG_OFF_COUNTERS      48U
#define ZZUSB_DIAG_OFF_EVENTS        128U

#define ZZUSB_DIAG_EVT_OFF_SEQUENCE  0U
#define ZZUSB_DIAG_EVT_OFF_REQUEST   4U
#define ZZUSB_DIAG_EVT_OFF_EPOCH     8U
#define ZZUSB_DIAG_EVT_OFF_DETAIL    12U
#define ZZUSB_DIAG_EVT_OFF_TIMESTAMP 16U
#define ZZUSB_DIAG_EVT_OFF_TYPE      20U
#define ZZUSB_DIAG_EVT_OFF_STATUS    22U
#define ZZUSB_DIAG_EVT_OFF_ADDRESS   24U
#define ZZUSB_DIAG_EVT_OFF_TOPOLOGY  26U
#define ZZUSB_DIAG_EVT_OFF_ENDPOINT  28U
#define ZZUSB_DIAG_EVT_OFF_DIRECTION 29U
#define ZZUSB_DIAG_EVT_OFF_SCHEDULE  30U

enum zzusb_diag_counter {
    ZZUSB_DIAG_COUNT_REQUEST = 0,
    ZZUSB_DIAG_COUNT_COMPLETION,
    ZZUSB_DIAG_COUNT_TIMEOUT,
    ZZUSB_DIAG_COUNT_LATE_COMPLETION,
    ZZUSB_DIAG_COUNT_CANCELLATION,
    ZZUSB_DIAG_COUNT_RESET,
    ZZUSB_DIAG_COUNT_EHCI_ERROR,
    ZZUSB_DIAG_COUNT_RECOVERY,
    ZZUSB_DIAG_COUNT_STALE,
    ZZUSB_DIAG_COUNT_QUEUE_HIGH_WATER,
    ZZUSB_DIAG_COUNT_PERIODIC_ARM,
    ZZUSB_DIAG_COUNT_PERIODIC_REAP,
    ZZUSB_DIAG_COUNT_ISO_QUEUE,
    ZZUSB_DIAG_COUNT_ISO_REAP
};

enum zzusb_diag_event_type {
    ZZUSB_DIAG_EVENT_REQUEST = 1,
    ZZUSB_DIAG_EVENT_COMPLETION,
    ZZUSB_DIAG_EVENT_TIMEOUT,
    ZZUSB_DIAG_EVENT_LATE_COMPLETION,
    ZZUSB_DIAG_EVENT_CANCELLATION,
    ZZUSB_DIAG_EVENT_RESET,
    ZZUSB_DIAG_EVENT_EHCI_ERROR,
    ZZUSB_DIAG_EVENT_RECOVERY,
    ZZUSB_DIAG_EVENT_STALE,
    ZZUSB_DIAG_EVENT_HIGH_WATER,
    ZZUSB_DIAG_EVENT_PERIODIC,
    ZZUSB_DIAG_EVENT_ISO
};

struct zzusb_diag_event {
    volatile uint32_t sequence;
    uint32_t request_id;
    uint32_t epoch;
    uint32_t detail;
    uint32_t timestamp;
    uint16_t type;
    uint16_t status;
    uint16_t address;
    uint16_t topology;
    uint8_t endpoint;
    uint8_t direction;
    uint16_t schedule;
};

typedef void (*zzusb_diag_flush_fn)(volatile void *address, size_t length);

void zzusb_diag_reset(void);
void zzusb_diag_count(enum zzusb_diag_counter counter);
void zzusb_diag_high_water(uint32_t depth);
void zzusb_diag_record(uint16_t type, uint16_t status,
                       uint32_t request_id, uint32_t epoch,
                       uint16_t address, uint8_t endpoint,
                       uint8_t direction, uint16_t topology,
                       uint16_t schedule, uint32_t detail,
                       uint32_t timestamp);
void zzusb_diag_publish(volatile void *page, size_t page_size,
                        uint32_t capabilities, uint32_t epoch,
                        uint32_t last_request_id, uint32_t queue_state,
                        uint32_t schedule_bits, zzusb_diag_flush_fn flush);
int zzusb_diag_read_coherent(volatile const void *page, void *snapshot,
                             size_t snapshot_size, unsigned retries);

#endif
