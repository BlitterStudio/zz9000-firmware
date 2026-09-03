/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <string.h>

#include "ehci.h"
#include "ehci-iso.h"
#include "ehci_periodic.h"
#include "io.h"
#include "memalign.h"

#define EHCI_ISO_UFRAME_BUDGET 4915U

static uint16_t iso_bandwidth[1024][8];
static int unlink_descriptor(struct ehci_iso_transfer *transfer,
                             unsigned packet_index);

static uint32_t descriptor_next(union ehci_iso_descriptor *descriptor,
                                int split)
{
    return split ? descriptor->sitd.next : descriptor->itd.next;
}

static uint32_t descriptor_address(union ehci_iso_descriptor *descriptor)
{
    return (uint32_t)(unsigned long)descriptor;
}

static int reserve_bandwidth(struct ehci_iso_transfer *transfer,
                             unsigned packet_index, uint16_t frame,
                             uint8_t mask, uint16_t bytes)
{
    unsigned uframe;
    unsigned slot = frame & EHCI_ISO_LIST_MASK;

    for (uframe = 0; uframe < 8U; uframe++) {
        if (!(mask & (1U << uframe)))
            continue;
        if ((unsigned)iso_bandwidth[slot][uframe] + bytes >
            EHCI_ISO_UFRAME_BUDGET)
            return 0;
    }
    for (uframe = 0; uframe < 8U; uframe++)
        if (mask & (1U << uframe))
            iso_bandwidth[slot][uframe] += bytes;
    transfer->reserved_masks[packet_index] = mask;
    transfer->reserved_bytes[packet_index] = bytes;
    transfer->frame_slots[packet_index] = (uint16_t)slot;
    return 1;
}

static void release_bandwidth(struct ehci_iso_transfer *transfer)
{
    unsigned packet_index;

    for (packet_index = 0; packet_index < transfer->packet_count;
         packet_index++) {
        unsigned uframe;
        unsigned slot = transfer->frame_slots[packet_index];
        uint8_t mask = transfer->reserved_masks[packet_index];
        uint16_t bytes = transfer->reserved_bytes[packet_index];

        for (uframe = 0; uframe < 8U; uframe++) {
            if (!(mask & (1U << uframe)))
                continue;
            if (iso_bandwidth[slot][uframe] >= bytes)
                iso_bandwidth[slot][uframe] -= bytes;
            else
                iso_bandwidth[slot][uframe] = 0;
        }
        transfer->reserved_masks[packet_index] = 0;
        transfer->reserved_bytes[packet_index] = 0;
    }
}

void ehci_iso_reset_bandwidth(void)
{
    memset(iso_bandwidth, 0, sizeof(iso_bandwidth));
}

int ehci_periodic_reserve_bandwidth(
    const struct ehci_periodic_plan *plan, uint8_t mask, uint16_t bytes)
{
    unsigned frame;
    unsigned uframe;

    if (!plan || !mask || !bytes)
        return 0;
    for (frame = 0; frame < 1024U; frame++) {
        if (!ehci_periodic_frame_due(plan, (uint16_t)frame))
            continue;
        for (uframe = 0; uframe < 8U; uframe++) {
            if ((mask & (1U << uframe)) &&
                (unsigned)iso_bandwidth[frame][uframe] + bytes >
                    EHCI_ISO_UFRAME_BUDGET)
                return 0;
        }
    }
    for (frame = 0; frame < 1024U; frame++) {
        if (!ehci_periodic_frame_due(plan, (uint16_t)frame))
            continue;
        for (uframe = 0; uframe < 8U; uframe++)
            if (mask & (1U << uframe))
                iso_bandwidth[frame][uframe] += bytes;
    }
    return 1;
}

void ehci_periodic_release_bandwidth(
    const struct ehci_periodic_plan *plan, uint8_t mask, uint16_t bytes)
{
    unsigned frame;
    unsigned uframe;

    if (!plan || !mask || !bytes)
        return;
    for (frame = 0; frame < 1024U; frame++) {
        if (!ehci_periodic_frame_due(plan, (uint16_t)frame))
            continue;
        for (uframe = 0; uframe < 8U; uframe++) {
            if (!(mask & (1U << uframe)))
                continue;
            if (iso_bandwidth[frame][uframe] >= bytes)
                iso_bandwidth[frame][uframe] -= bytes;
            else
                iso_bandwidth[frame][uframe] = 0;
        }
    }
}

uint16_t ehci_iso_current_frame(struct ehci_ctrl *ctrl)
{
    if (!ctrl || !ctrl->hcor)
        return 0;
    return (uint16_t)((ehci_readl(&ctrl->hcor->or_frindex) >> 3) &
                      EHCI_ISO_FRAME_MASK);
}

static int frame_has_passed(uint16_t current, uint16_t scheduled)
{
    uint16_t distance = ehci_iso_frame_distance(current, scheduled);
    return distance > 0 && distance < 1024U;
}

static uint8_t packet_status_itd(uint32_t transaction, uint16_t requested,
                                 int direction_in, uint16_t *actual)
{
    *actual = ehci_iso_itd_actual(transaction, requested);
    if (transaction & EHCI_ITD_DBE)
        return direction_in ? EHCI_ISO_PACKET_OVERRUN :
                              EHCI_ISO_PACKET_UNDERRUN;
    if (transaction & EHCI_ITD_BABBLE)
        return EHCI_ISO_PACKET_BABBLE;
    if (transaction & EHCI_ITD_XACTERR)
        return EHCI_ISO_PACKET_XACT;
    if (direction_in && *actual < requested)
        return EHCI_ISO_PACKET_SHORT;
    return EHCI_ISO_PACKET_OK;
}

static uint8_t packet_status_sitd(uint32_t results, uint16_t requested,
                                  int direction_in, uint16_t *actual)
{
    uint16_t residual = (uint16_t)((results >> 16) &
                                   EHCI_SITD_LENGTH_MASK);

    *actual = residual <= requested ? (uint16_t)(requested - residual) : 0;
    if (results & EHCI_SITD_MISSED)
        return EHCI_ISO_PACKET_MISSED;
    if (results & EHCI_SITD_DBE)
        return direction_in ? EHCI_ISO_PACKET_OVERRUN :
                              EHCI_ISO_PACKET_UNDERRUN;
    if (results & EHCI_SITD_BABBLE)
        return EHCI_ISO_PACKET_BABBLE;
    if (results & (EHCI_SITD_XACTERR | EHCI_SITD_ERR))
        return EHCI_ISO_PACKET_XACT;
    if (direction_in && *actual < requested)
        return EHCI_ISO_PACKET_SHORT;
    return EHCI_ISO_PACKET_OK;
}

int ehci_iso_schedule(struct ehci_iso_transfer *transfer,
                      struct ehci_ctrl *ctrl,
                      const struct ehci_iso_config *config,
                      struct ehci_iso_packet *packets,
                      unsigned packet_count, uint8_t *buffer)
{
    uint16_t current;
    uint16_t base_distance;
    uint32_t absolute_uframe;
    uint32_t step;
    unsigned packet_index;
    int result = EHCI_ISO_ERR_INVALID;
    int missed;

    if (!transfer || !ctrl || !config || !packets || !buffer ||
        packet_count == 0 || packet_count > EHCI_ISO_MAX_PACKETS ||
        config->endpoint == 0 || config->interval == 0 ||
        packets[0].frame > EHCI_ISO_FRAME_MASK ||
        packets[0].microframe > 7U)
        return EHCI_ISO_ERR_INVALID;
    if (config->speed == 3U) {
        if (config->split || config->interval > 32768U ||
            (config->interval & (config->interval - 1U)) != 0)
            return EHCI_ISO_ERR_INVALID;
        step = config->interval;
    } else {
        if (!config->split || config->speed != 2U ||
            config->interval > 32768U ||
            (config->interval & (config->interval - 1U)) != 0)
            return EHCI_ISO_ERR_INVALID;
        step = (uint32_t)config->interval * 8U;
    }

    memset(transfer, 0, sizeof(*transfer));
    transfer->ctrl = ctrl;
    transfer->config = *config;
    transfer->packet_count = (uint8_t)packet_count;
    absolute_uframe = (uint32_t)packets[0].frame * 8U +
                      packets[0].microframe;
    current = ehci_iso_current_frame(ctrl);
    base_distance = ehci_iso_frame_distance(packets[0].frame, current);
    missed = !ehci_iso_frame_schedulable(packets[0].frame, current);
    for (packet_index = 0; packet_index < packet_count; packet_index++) {
        struct ehci_iso_packet *packet = &transfer->packets[packet_index];
        uint32_t frame_offset =
            (absolute_uframe >> 3) - packets[0].frame;

        *packet = packets[packet_index];
        packet->frame = (uint16_t)((absolute_uframe >> 3) &
                                   EHCI_ISO_FRAME_MASK);
        packet->microframe = (uint8_t)(absolute_uframe & 7U);
        packet->actual = 0;
        packet->status = EHCI_ISO_PACKET_PENDING;
        if ((uint32_t)base_distance + frame_offset >= 1024U)
            missed = 1;
        absolute_uframe += step;
    }
    if (missed) {
        for (packet_index = 0; packet_index < packet_count; packet_index++)
            transfer->packets[packet_index].status =
                EHCI_ISO_PACKET_MISSED;
        transfer->complete = 1;
        return EHCI_ISO_ERR_MISSED;
    }


    for (packet_index = 0; packet_index < packet_count; packet_index++) {
        struct ehci_iso_packet *packet = &transfer->packets[packet_index];
        uint8_t smask = 0;
        uint8_t cmask = 0;
        uint8_t reservation_mask;
        uint16_t reservation_bytes;

        if (config->split) {
            if (!ehci_iso_split_masks(
                    packet->requested, config->direction_in,
                    config->tt_think_time, config->multi_tt,
                    config->endpoint + config->hub_port,
                    &smask, &cmask))
                goto fail;
            reservation_mask = (uint8_t)(smask | cmask);
            reservation_bytes = packet->requested > 188U ?
                                188U : packet->requested;
            if (!reservation_bytes)
                reservation_bytes = 1;
        } else {
            reservation_mask = (uint8_t)(1U << packet->microframe);
            reservation_bytes = packet->requested ? packet->requested : 1;
        }
        if (!reserve_bandwidth(transfer, packet_index, packet->frame,
                               reservation_mask, reservation_bytes)) {
            result = EHCI_ISO_ERR_BANDWIDTH;
            goto fail;
        }
    }


    for (packet_index = 0; packet_index < packet_count; packet_index++) {
        struct ehci_iso_packet *packet = &transfer->packets[packet_index];
        union ehci_iso_descriptor *descriptor =
            &transfer->descriptors[packet_index];
        uint16_t slot = transfer->frame_slots[packet_index];
        uint32_t next = ctrl->periodic_list[slot];
        uint32_t address = (uint32_t)(unsigned long)(buffer + packet->offset);
        int built;

        if (config->split) {
            uint8_t smask = 0;
            uint8_t cmask = 0;
            ehci_iso_split_masks(packet->requested, config->direction_in,
                                 config->tt_think_time, config->multi_tt,
                                 config->endpoint + config->hub_port,
                                 &smask, &cmask);
            built = ehci_iso_build_sitd(&descriptor->sitd, config,
                                        address, packet->requested,
                                        smask, cmask, next);
        } else {
            built = ehci_iso_build_itd(&descriptor->itd, config, address,
                                       packet->requested,
                                       packet->microframe, next);
        }
        if (!built) {
            result = EHCI_ISO_ERR_INVALID;
            goto unlink_partial;
        }
        flush_dcache_range((unsigned long)descriptor,
                           ALIGN_END_ADDR(union ehci_iso_descriptor,
                                          descriptor, 1));
        ctrl->periodic_list[slot] =
            descriptor_address(descriptor) |
            (config->split ? EHCI_ISO_LINK_SITD : EHCI_ISO_LINK_ITD);
        flush_dcache_range((unsigned long)&ctrl->periodic_list[slot],
                           ALIGN_END_ADDR(uint32_t,
                                          &ctrl->periodic_list[slot], 1));
    }
    if (config->direction_in)
        invalidate_dcache_range((unsigned long)buffer,
            ALIGN_END_ADDR(char, buffer,
                           packets[packet_count - 1].offset +
                           packets[packet_count - 1].requested));
    else
        flush_dcache_range((unsigned long)buffer,
            ALIGN_END_ADDR(char, buffer,
                           packets[packet_count - 1].offset +
                           packets[packet_count - 1].requested));

    transfer->linked = 1;
    if (ehci_periodic_schedule_resume(ctrl, 1) < 0)
        return EHCI_ISO_ERR_TIMEOUT;
    return 0;

unlink_partial:
    while (packet_index > 0) {
        packet_index--;
        unlink_descriptor(transfer, packet_index);
    }
    release_bandwidth(transfer);
    memset(transfer, 0, sizeof(*transfer));
    return result;

fail:
    release_bandwidth(transfer);
    memset(transfer, 0, sizeof(*transfer));
    return result;
}

int ehci_iso_poll(struct ehci_iso_transfer *transfer)
{
    uint16_t current;
    unsigned packet_index;
    unsigned pending = 0;

    if (!transfer || !transfer->linked)
        return EHCI_ISO_ERR_INVALID;
    current = ehci_iso_current_frame(transfer->ctrl);
    for (packet_index = 0; packet_index < transfer->packet_count;
         packet_index++) {
        struct ehci_iso_packet *packet = &transfer->packets[packet_index];
        union ehci_iso_descriptor *descriptor =
            &transfer->descriptors[packet_index];
        uint32_t state;

        if (packet->status != EHCI_ISO_PACKET_PENDING)
            continue;
        invalidate_dcache_range((unsigned long)descriptor,
            ALIGN_END_ADDR(union ehci_iso_descriptor, descriptor, 1));
        if (transfer->config.split) {
            state = descriptor->sitd.results;
            if (state & EHCI_SITD_ACTIVE) {
                if (ehci_iso_sitd_frame_expired(current, packet->frame))
                    packet->status = EHCI_ISO_PACKET_MISSED;
                else
                    pending++;
                continue;
            }
            packet->status = packet_status_sitd(
                state, packet->requested, transfer->config.direction_in,
                &packet->actual);
        } else {
            state = descriptor->itd.transaction[packet->microframe];
            if (state & EHCI_ITD_ACTIVE) {
                if (frame_has_passed(current, packet->frame))
                    packet->status = EHCI_ISO_PACKET_MISSED;
                else
                    pending++;
                continue;
            }
            packet->status = packet_status_itd(
                state, packet->requested, transfer->config.direction_in,
                &packet->actual);
        }
    }
    if (pending)
        return 0;
    transfer->complete = 1;
    return 1;
}

static int unlink_descriptor(struct ehci_iso_transfer *transfer,
                             unsigned packet_index)
{
    uint16_t slot = transfer->frame_slots[packet_index];
    uint32_t target = descriptor_address(
        &transfer->descriptors[packet_index]);
    uint32_t *link = &transfer->ctrl->periodic_list[slot];
    unsigned guard = 0;

    while (guard++ < 128U) {
        uint32_t value = *link;
        uint32_t type;
        uint32_t address;

        if (value & EHCI_ISO_LINK_TERMINATE)
            return 0;
        type = value & EHCI_ISO_LINK_TYPE_MASK;
        if (type != EHCI_ISO_LINK_ITD && type != EHCI_ISO_LINK_SITD)
            return 0;
        address = value & ~0x1fU;
        if (address == target) {
            *link = descriptor_next(&transfer->descriptors[packet_index],
                                    transfer->config.split);
            flush_dcache_range((unsigned long)link,
                               ALIGN_END_ADDR(uint32_t, link, 1));
            return 1;
        }
        link = (uint32_t *)(unsigned long)address;
        invalidate_dcache_range((unsigned long)link,
                                ALIGN_END_ADDR(uint32_t, link, 1));
    }
    return 0;
}

int ehci_iso_retire(struct ehci_iso_transfer *transfer,
                    uint8_t unfinished_status)
{
    unsigned packet_index;
    int resume_result;
    int has_pending = 0;
    if (!transfer)
        return EHCI_ISO_ERR_INVALID;
    if (!transfer->linked) {
        release_bandwidth(transfer);
        return 0;
    }
    for (packet_index = 0; packet_index < transfer->packet_count;
         packet_index++)
        if (transfer->packets[packet_index].status ==
            EHCI_ISO_PACKET_PENDING) {
            has_pending = 1;
            break;
        }
    if ((has_pending || transfer->ctrl->periodic_schedules == 1) &&
        ehci_periodic_schedule_pause(transfer->ctrl) < 0)
        return EHCI_ISO_ERR_TIMEOUT;
    for (packet_index = 0; packet_index < transfer->packet_count;
         packet_index++)
        unlink_descriptor(transfer, packet_index);
    resume_result = ehci_periodic_schedule_resume(transfer->ctrl, -1);
    release_bandwidth(transfer);
    transfer->linked = 0;
    for (packet_index = 0; packet_index < transfer->packet_count;
         packet_index++) {
        if (transfer->packets[packet_index].status ==
            EHCI_ISO_PACKET_PENDING)
            transfer->packets[packet_index].status = unfinished_status;
    }
    return resume_result;
}
