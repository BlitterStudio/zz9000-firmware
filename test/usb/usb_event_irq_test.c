#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "usb_event_irq.h"

static uint32_t acknowledge(uint32_t pending, uint16_t value)
{
    return pending & ~zzusb_event_ack_mask(value);
}

int main(void)
{
    uint32_t shared = 1U | 2U | 4U | ZZUSB_EVENT_INTERRUPT_BIT;

    assert(zzusb_event_ack_mask(ZZUSB_EVENT_ACK_USB) == 0);
    assert(zzusb_event_ack_mask(ZZUSB_EVENT_ACK_GATE) == 0);
    assert(zzusb_event_ack_mask(ZZUSB_EVENT_ACK_GATE |
                                ZZUSB_EVENT_ACK_USB) ==
           ZZUSB_EVENT_INTERRUPT_BIT);
    assert(acknowledge(shared, ZZUSB_EVENT_ACK_GATE |
                      ZZUSB_EVENT_ACK_USB) == (1U | 2U | 4U));
    assert(acknowledge(shared, ZZUSB_EVENT_ACK_GATE | 16U) == shared);
    assert((shared | ZZUSB_EVENT_INTERRUPT_BIT) == shared);

    puts("USB event IRQ acknowledge and shared-source coexistence satisfied");
    return 0;
}
