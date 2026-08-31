/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef ZZ9000_EHCI_LIFECYCLE_H
#define ZZ9000_EHCI_LIFECYCLE_H

#include <stdint.h>

/* Unsigned subtraction keeps deadlines correct across a 32-bit wrap. */
static inline int ehci_deadline_expired_u32(uint32_t now, uint32_t start,
                                            uint32_t duration)
{
    return (uint32_t)(now - start) >= duration;
}

/* DMA memory is reusable only after hardware can no longer reference it. */
static inline int ehci_dma_reclaimable(int unlink_acknowledged,
                                       int schedule_stopped,
                                       int controller_reset)
{
    return unlink_acknowledged || schedule_stopped || controller_reset;
}

#endif
