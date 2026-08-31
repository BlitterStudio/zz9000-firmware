/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZUSB_EHCI_ISO_H
#define ZZUSB_EHCI_ISO_H

#include <stdint.h>
#include <string.h>

#define EHCI_ISO_MAX_PACKETS 32U
#define EHCI_ISO_FRAME_MASK  0x07ffU
#define EHCI_ISO_LIST_MASK   0x03ffU
#define EHCI_ISO_LINK_TERMINATE 0x00000001U
#define EHCI_ISO_LINK_TYPE_MASK 0x00000006U
#define EHCI_ISO_LINK_ITD    0x00000000U
#define EHCI_ISO_LINK_SITD   0x00000004U
#define EHCI_ISO_LINK_QH     0x00000002U

#define EHCI_ISO_ERR_INVALID   (-1)
#define EHCI_ISO_ERR_MISSED    (-2)
#define EHCI_ISO_ERR_BANDWIDTH (-3)
#define EHCI_ISO_ERR_TIMEOUT   (-4)

#define EHCI_ITD_ACTIVE      (1U << 31)
#define EHCI_ITD_DBE         (1U << 30)
#define EHCI_ITD_BABBLE      (1U << 29)
#define EHCI_ITD_XACTERR     (1U << 28)
#define EHCI_ITD_IOC         (1U << 15)
#define EHCI_ITD_LENGTH_MASK 0x0fffU

#define EHCI_SITD_IOC        (1U << 31)
#define EHCI_SITD_ACTIVE     (1U << 7)
#define EHCI_SITD_ERR        (1U << 6)
#define EHCI_SITD_DBE        (1U << 5)
#define EHCI_SITD_BABBLE     (1U << 4)
#define EHCI_SITD_XACTERR    (1U << 3)
#define EHCI_SITD_MISSED     (1U << 2)
#define EHCI_SITD_LENGTH_MASK 0x03ffU

#define EHCI_ISO_PACKET_OK        0U
#define EHCI_ISO_PACKET_PENDING   1U
#define EHCI_ISO_PACKET_SHORT     2U
#define EHCI_ISO_PACKET_MISSED    3U
#define EHCI_ISO_PACKET_UNDERRUN  4U
#define EHCI_ISO_PACKET_OVERRUN   5U
#define EHCI_ISO_PACKET_CANCELLED 6U
#define EHCI_ISO_PACKET_OFFLINE   7U
#define EHCI_ISO_PACKET_XACT      8U
#define EHCI_ISO_PACKET_BABBLE    9U

struct ehci_iso_itd {
    uint32_t next;
    uint32_t transaction[8];
    uint32_t buffer[7];
} __attribute__((aligned(32)));

struct ehci_ctrl;
struct ehci_periodic_plan;

struct ehci_iso_sitd {
    uint32_t next;
    uint32_t endpoint;
    uint32_t uframe;
    uint32_t results;
    uint32_t buffer[2];
    uint32_t back;
    uint32_t reserved;
} __attribute__((aligned(32)));

union ehci_iso_descriptor {
    struct ehci_iso_itd itd;
    struct ehci_iso_sitd sitd;
} __attribute__((aligned(32)));

struct ehci_iso_config {
    uint8_t address;
    uint8_t endpoint;
    uint8_t direction_in;
    uint8_t speed;
    uint8_t split;
    uint8_t multi_tt;
    uint8_t tt_think_time;
    uint8_t interval;
    uint8_t hub_address;
    uint8_t hub_port;
    uint16_t max_packet;
};

struct ehci_iso_packet {
    uint32_t offset;
    uint16_t requested;
    uint16_t actual;
    uint16_t frame;
    uint8_t microframe;
    uint8_t status;
};

struct ehci_iso_transfer {
    struct ehci_ctrl *ctrl;
    struct ehci_iso_config config;
    struct ehci_iso_packet packets[EHCI_ISO_MAX_PACKETS];
    union ehci_iso_descriptor descriptors[EHCI_ISO_MAX_PACKETS];
    uint16_t reserved_bytes[EHCI_ISO_MAX_PACKETS];
    uint16_t frame_slots[EHCI_ISO_MAX_PACKETS];
    uint8_t reserved_masks[EHCI_ISO_MAX_PACKETS];
    uint8_t packet_count;
    uint8_t linked;
    uint8_t complete;
};

static inline uint16_t ehci_iso_frame_distance(uint16_t future,
                                                uint16_t current)
{
    return (uint16_t)((future - current) & EHCI_ISO_FRAME_MASK);
}

static inline int ehci_iso_frame_schedulable(uint16_t future,
                                              uint16_t current)
{
    uint16_t distance = ehci_iso_frame_distance(future, current);

    return distance >= 4U && distance < 1024U;
}

static inline uint16_t ehci_iso_itd_actual(uint32_t transaction,
                                           uint16_t requested)
{
    uint16_t transferred = (uint16_t)((transaction >> 16) &
                                      EHCI_ITD_LENGTH_MASK);

    return transferred <= requested ? transferred : 0;
}

static inline int ehci_iso_split_masks(uint16_t length, int direction_in,
                                       unsigned think_time, int multi_tt,
                                       unsigned seed, uint8_t *smask,
                                       uint8_t *cmask)
{
    unsigned transactions;
    unsigned start = (multi_tt && think_time < 3U) ? (seed & 1U) : 0U;

    if (!smask || !cmask || length > 1023U || think_time > 3U)
        return 0;
    transactions = (length + 187U) / 188U;
    if (transactions == 0)
        transactions = 1;
    if (transactions > 6U)
        return 0;

    if (!direction_in) {
        if (start + transactions > 6U)
            start = 0;
        *smask = (uint8_t)(((1U << transactions) - 1U) << start);
        *cmask = 0;
        return 1;
    }

    *smask = (uint8_t)(1U << start);
    start += 2U + think_time;
    if (start > 5U)
        return 0;
    transactions += 2U;
    if (transactions > 8U - start)
        transactions = 8U - start;
    *cmask = (uint8_t)(((1U << transactions) - 1U) << start);
    return transactions >= 3U;
}

static inline int ehci_iso_build_itd(struct ehci_iso_itd *itd,
                                     const struct ehci_iso_config *config,
                                     uint32_t buffer_address,
                                     uint16_t length, uint8_t microframe,
                                     uint32_t next)
{
    uint32_t page;
    uint32_t base_packet;
    uint32_t multiplier;

    if (!itd || !config || microframe > 7U)
        return 0;
    base_packet = config->max_packet & 0x07ffU;
    multiplier = 1U + ((config->max_packet >> 11) & 0x03U);
    if (!base_packet || multiplier > 3U ||
        length > base_packet * multiplier)
        return 0;

    memset(itd, 0, sizeof(*itd));
    page = buffer_address & 0xfffff000U;
    itd->next = next;
    itd->transaction[microframe] = EHCI_ITD_ACTIVE | EHCI_ITD_IOC |
        ((uint32_t)length << 16) | (buffer_address & 0x0fffU);
    itd->buffer[0] = page | (config->address & 0x7fU) |
                     ((uint32_t)(config->endpoint & 0x0fU) << 8);
    itd->buffer[1] = (page + 0x1000U) | base_packet;
    itd->buffer[2] = (page + 0x2000U) |
                     (config->direction_in ? (1U << 11) : 0U) |
                     multiplier;
    for (unsigned index = 3; index < 7; index++)
        itd->buffer[index] = page + index * 0x1000U;
    return 1;
}

static inline int ehci_iso_build_sitd(struct ehci_iso_sitd *sitd,
                                      const struct ehci_iso_config *config,
                                      uint32_t buffer_address,
                                      uint16_t length, uint8_t smask,
                                      uint8_t cmask, uint32_t next)
{
    unsigned transactions;

    if (!sitd || !config || length > 1023U || !smask)
        return 0;
    memset(sitd, 0, sizeof(*sitd));
    sitd->next = next;
    sitd->endpoint = (config->address & 0x7fU) |
        ((uint32_t)(config->endpoint & 0x0fU) << 8) |
        ((uint32_t)(config->hub_address & 0x7fU) << 16) |
        ((uint32_t)(config->hub_port & 0x7fU) << 24) |
        (config->direction_in ? (1U << 31) : 0U);
    sitd->uframe = smask | ((uint32_t)cmask << 8);
    sitd->results = EHCI_SITD_IOC | EHCI_SITD_ACTIVE |
                    ((uint32_t)length << 16);
    sitd->buffer[0] = buffer_address;
    sitd->buffer[1] = (buffer_address + length) & 0xfffff000U;
    if (!config->direction_in) {
        transactions = (length + 187U) / 188U;
        if (!transactions)
            transactions = 1;
        sitd->buffer[1] |= transactions;
        if (transactions > 1U)
            sitd->buffer[1] |= 1U << 3;
    }
    sitd->back = EHCI_ISO_LINK_TERMINATE;
    return 1;
}

uint16_t ehci_iso_current_frame(struct ehci_ctrl *ctrl);
int ehci_iso_schedule(struct ehci_iso_transfer *transfer,
                      struct ehci_ctrl *ctrl,
                      const struct ehci_iso_config *config,
                      struct ehci_iso_packet *packets,
                      unsigned packet_count, uint8_t *buffer);
int ehci_iso_poll(struct ehci_iso_transfer *transfer);
int ehci_iso_retire(struct ehci_iso_transfer *transfer,
                    uint8_t unfinished_status);
void ehci_iso_reset_bandwidth(void);
int ehci_periodic_reserve_bandwidth(
    const struct ehci_periodic_plan *plan, uint8_t mask, uint16_t bytes);
void ehci_periodic_release_bandwidth(
    const struct ehci_periodic_plan *plan, uint8_t mask, uint16_t bytes);

#endif
