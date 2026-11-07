/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EHCI_VBUS_CYCLE_H
#define EHCI_VBUS_CYCLE_H

#include <stdint.h>

/* EHCI PORTSC current-connect-status bit. */
#define ZZUSB_PORTSC_CCS (1U << 0)

static inline int zzusb_vbus_cycle_needed(uint32_t portsc)
{
    return (portsc & ZZUSB_PORTSC_CCS) == 0U;
}

#endif
