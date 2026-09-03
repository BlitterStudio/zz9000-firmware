/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZUSB_EHCI_PERIODIC_H
#define ZZUSB_EHCI_PERIODIC_H

#include <stdint.h>

#define EHCI_PERIODIC_SPEED_LOW  1U
#define EHCI_PERIODIC_SPEED_FULL 2U
#define EHCI_PERIODIC_SPEED_HIGH 3U

struct ehci_periodic_plan {
    uint32_t interval_microframes;
    uint16_t frame_interval;
    uint16_t frame_phase;
    uint8_t start_mask;
    uint8_t complete_mask;
};

/*
 * A timed-out transient QH can still reference its endpoint-owned buffer
 * until controller recovery removes every quarantined descriptor.
 */
static inline int ehci_periodic_buffer_reclaimable(
    int controller_recovery_pending)
{
    return !controller_recovery_pending;
}

static inline uint16_t ehci_periodic_hardware_frame_interval(
    const struct ehci_periodic_plan *plan)
{
    if (!plan || plan->frame_interval == 0)
        return 1U;
    return plan->frame_interval > 1024U ? 1024U :
                                             plan->frame_interval;
}

static inline int ehci_periodic_frame_due(
    const struct ehci_periodic_plan *plan, uint16_t frame)
{
    uint16_t interval = ehci_periodic_hardware_frame_interval(plan);
    uint16_t phase = plan ? (uint16_t)(plan->frame_phase % interval) : 0;

    return (uint16_t)((frame + interval - phase) % interval) == 0;
}

/*
 * Persistent QHs have only been hardware-verified for sub-frame high-speed
 * cadence. Sparse high-speed endpoints use bounded one-shot transactions so
 * a hub's long status interval cannot destabilize the periodic frame list.
 */
static inline int ehci_periodic_use_software_poll(
    unsigned speed, const struct ehci_periodic_plan *plan)
{
    return speed == EHCI_PERIODIC_SPEED_HIGH && plan &&
           plan->interval_microframes > 8U;
}

static inline int ehci_periodic_build_plan(
    unsigned speed, unsigned interval, int split,
    unsigned think_time, int multi_tt, unsigned slot_seed,
    struct ehci_periodic_plan *plan)
{
    uint32_t microframes;
    uint8_t start = 0;

    if (!plan || interval == 0)
        return 0;
    plan->frame_phase = 0;

    if (speed == EHCI_PERIODIC_SPEED_HIGH) {
        if (interval > 32768U || (interval & (interval - 1U)) != 0 ||
            split)
            return 0;
        microframes = interval;
        plan->interval_microframes = microframes;
        plan->frame_interval = (uint16_t)(
            microframes > 8U ? microframes / 8U : 1U);
        plan->complete_mask = 0;
        if (microframes >= 8U) {
            plan->start_mask = 0x01U;
        } else {
            uint8_t mask = 0;
            for (unsigned uframe = 0; uframe < 8U;
                 uframe += microframes)
                mask |= (uint8_t)(1U << uframe);
            plan->start_mask = mask;
        }
        return 1;
    }

    if ((speed != EHCI_PERIODIC_SPEED_LOW &&
         speed != EHCI_PERIODIC_SPEED_FULL) || interval > 255U)
        return 0;

    plan->frame_interval = 1U;
    while ((uint32_t)plan->frame_interval * 2U <= interval &&
           plan->frame_interval < 1024U)
        plan->frame_interval = (uint16_t)(plan->frame_interval * 2U);
    plan->interval_microframes =
        (uint32_t)plan->frame_interval * 8U;
    plan->complete_mask = 0;
    if (!split) {
        plan->start_mask = 0x01U;
        return 1;
    }
    if (think_time > 3U)
        return 0;

    if (multi_tt && think_time < 3U)
        start = (uint8_t)(slot_seed & 1U);
    plan->start_mask = (uint8_t)(1U << start);
    start = (uint8_t)(start + 2U + think_time);
    if (start > 5U)
        return 0;
    plan->complete_mask = (uint8_t)(0x07U << start);
    return 1;
}

#endif
