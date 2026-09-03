#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "usb/ehci_periodic.h"
#include "usb_proxy_periodic_ring.h"

static void test_high_speed_cadence(void)
{
    struct ehci_periodic_plan plan;

    assert(ehci_periodic_build_plan(EHCI_PERIODIC_SPEED_HIGH, 1, 0,
                                    0, 0, 0, &plan));
    assert(plan.interval_microframes == 1);
    assert(plan.frame_interval == 1);
    assert(plan.start_mask == 0xff);
    assert(plan.complete_mask == 0);

    assert(ehci_periodic_build_plan(EHCI_PERIODIC_SPEED_HIGH, 4, 0,
                                    0, 0, 0, &plan));
    assert(plan.interval_microframes == 4);
    assert(!ehci_periodic_use_software_poll(
        EHCI_PERIODIC_SPEED_HIGH, &plan));
    assert(plan.start_mask == 0x11);

    assert(ehci_periodic_build_plan(EHCI_PERIODIC_SPEED_HIGH, 32768, 0,
                                    0, 0, 0, &plan));
    assert(plan.interval_microframes == 32768);
    assert(plan.frame_interval == 4096);
    assert(ehci_periodic_hardware_frame_interval(&plan) == 1024);
    assert(plan.frame_phase == 0);
    assert(ehci_periodic_frame_due(&plan, 0));
    assert(!ehci_periodic_frame_due(&plan, 1));
    assert(plan.start_mask == 0x01);
    assert(ehci_periodic_use_software_poll(
        EHCI_PERIODIC_SPEED_HIGH, &plan));
    assert(!ehci_periodic_use_software_poll(
        EHCI_PERIODIC_SPEED_FULL, &plan));
    assert(!ehci_periodic_build_plan(EHCI_PERIODIC_SPEED_HIGH, 3, 0,
                                     0, 0, 0, &plan));

    plan.frame_interval = 1024;
    plan.frame_phase = 1023;
    assert(ehci_periodic_frame_due(&plan, 1023));
    assert(!ehci_periodic_frame_due(&plan, 0));
}

static void test_split_masks(void)
{
    struct ehci_periodic_plan plan;

    assert(ehci_periodic_build_plan(EHCI_PERIODIC_SPEED_FULL, 8, 1,
                                    0, 0, 0, &plan));
    assert(plan.interval_microframes == 64);
    assert(plan.frame_interval == 8);
    assert(plan.start_mask == 0x01);
    assert(plan.complete_mask == 0x1c);

    assert(ehci_periodic_build_plan(EHCI_PERIODIC_SPEED_LOW, 10, 1,
                                    2, 1, 3, &plan));
    assert(plan.interval_microframes == 64);
    assert(plan.frame_interval == 8);
    assert(ehci_periodic_frame_due(&plan, 8));
    assert(!ehci_periodic_frame_due(&plan, 10));
    assert(plan.start_mask == 0x02);
    assert(plan.complete_mask == 0xe0);

    assert(ehci_periodic_build_plan(EHCI_PERIODIC_SPEED_FULL, 1, 1,
                                    3, 1, 1, &plan));
    assert(plan.start_mask == 0x01);
    assert(plan.complete_mask == 0xe0);
    assert(!ehci_periodic_build_plan(EHCI_PERIODIC_SPEED_FULL, 1, 1,
                                     4, 0, 0, &plan));
}

static void test_transient_buffer_retirement(void)
{
    assert(ehci_periodic_buffer_reclaimable(0));
    assert(!ehci_periodic_buffer_reclaimable(1));
}

static void test_ready_ring_wrap_and_no_overwrite(void)
{
    uint8_t entries[ZZUSB_PERIODIC_RING_CAPACITY];
    uint8_t head = 0;
    uint8_t count = 0;
    unsigned value;

    memset(entries, 0, sizeof(entries));
    for (value = 0; value < ZZUSB_PERIODIC_RING_CAPACITY; value++)
        assert(zzusb_periodic_ring_push(entries, head, &count,
                                        (uint8_t)value));
    assert(count == ZZUSB_PERIODIC_RING_CAPACITY);
    assert(!zzusb_periodic_ring_push(entries, head, &count, 17));
    assert(!zzusb_periodic_ring_push(entries, head, &count, 3));

    assert(zzusb_periodic_ring_remove(entries, &head, &count, 5));
    assert(count == ZZUSB_PERIODIC_RING_CAPACITY - 1);
    assert(entries[5] == 6);
    assert(zzusb_periodic_ring_push(entries, head, &count, 17));
    assert(entries[ZZUSB_PERIODIC_RING_CAPACITY - 1] == 17);

    while (count)
        assert(zzusb_periodic_ring_remove(entries, &head, &count, 0));
    assert(head == 0);
}

static void test_completed_endpoint_waits_for_host_rearm(void)
{
    assert(!zzusb_periodic_rearm_ready(1, 0));
    assert(zzusb_periodic_rearm_ready(1, 1));
    assert(!zzusb_periodic_rearm_ready(0, 1));
}

int main(void)
{
    test_high_speed_cadence();
    test_split_masks();
    test_transient_buffer_retirement();
    test_ready_ring_wrap_and_no_overwrite();
    test_completed_endpoint_waits_for_host_rearm();
    puts("EHCI periodic cadence, split masks, and bounded ring contract satisfied");
    return 0;
}
