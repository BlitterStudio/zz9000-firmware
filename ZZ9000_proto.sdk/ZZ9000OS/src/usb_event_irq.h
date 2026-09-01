/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZUSB_EVENT_IRQ_H
#define ZZUSB_EVENT_IRQ_H

#include <stdint.h>

#define ZZUSB_EVENT_INTERRUPT_BIT (1U << 4)
#define ZZUSB_EVENT_ACK_GATE      (1U << 3)
#define ZZUSB_EVENT_ACK_USB       (1U << 8)

static inline uint32_t zzusb_event_ack_mask(uint16_t write_value)
{
    if ((write_value & (ZZUSB_EVENT_ACK_GATE | ZZUSB_EVENT_ACK_USB)) ==
        (ZZUSB_EVENT_ACK_GATE | ZZUSB_EVENT_ACK_USB))
        return ZZUSB_EVENT_INTERRUPT_BIT;
    return 0;
}

#endif
