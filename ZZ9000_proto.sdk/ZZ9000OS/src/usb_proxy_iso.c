/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <string.h>

#include "xil_cache.h"
#include "usb_proxy.h"
#include "usb_proxy_diag.h"
#include "usb_proxy_iso.h"
#include "usb/ehci-iso.h"
_Static_assert(ZZUSB_ISO_PACKET_OK == EHCI_ISO_PACKET_OK,
               "ISO OK status drift");
_Static_assert(ZZUSB_ISO_PACKET_PENDING == EHCI_ISO_PACKET_PENDING,
               "ISO pending status drift");
_Static_assert(ZZUSB_ISO_PACKET_SHORT == EHCI_ISO_PACKET_SHORT,
               "ISO short status drift");
_Static_assert(ZZUSB_ISO_PACKET_MISSED == EHCI_ISO_PACKET_MISSED,
               "ISO missed status drift");
_Static_assert(ZZUSB_ISO_PACKET_UNDERRUN == EHCI_ISO_PACKET_UNDERRUN,
               "ISO underrun status drift");
_Static_assert(ZZUSB_ISO_PACKET_OVERRUN == EHCI_ISO_PACKET_OVERRUN,
               "ISO overrun status drift");
_Static_assert(ZZUSB_ISO_PACKET_CANCELLED == EHCI_ISO_PACKET_CANCELLED,
               "ISO cancelled status drift");
_Static_assert(ZZUSB_ISO_PACKET_OFFLINE == EHCI_ISO_PACKET_OFFLINE,
               "ISO offline status drift");
_Static_assert(ZZUSB_ISO_PACKET_XACT == EHCI_ISO_PACKET_XACT,
               "ISO transaction status drift");
_Static_assert(ZZUSB_ISO_PACKET_BABBLE == EHCI_ISO_PACKET_BABBLE,
               "ISO babble status drift");


#ifndef ALIGN
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#endif

#define ZZUSB_ISO_PUMP_BUDGET 2U

struct usb_proxy_iso_key {
    uint32_t epoch;
    uint16_t generation;
    uint16_t address;
    uint16_t endpoint;
    uint16_t direction;
};

struct usb_proxy_iso_batch {
    struct usb_proxy_iso_key key;
    struct ehci_iso_transfer transfer;
    uint32_t batch_id;
    uint32_t total_data;
    uint16_t flags;
    uint8_t used;
    uint8_t ready;
    uint8_t data[ZZUSB_ISO_DATA_MAX] __attribute__((aligned(32)));
} __attribute__((aligned(32)));

static struct usb_proxy_iso_batch iso_batches[ZZUSB_ISO_MAX_BATCHES]
    __attribute__((aligned(32)));
static uint8_t iso_pump_cursor;

static struct ehci_ctrl *iso_controller(void)
{
    return usb_proxy_get_ehci_controller();
}

static int iso_identity_matches(const struct usb_proxy_iso_batch *batch,
                                volatile struct ZZUSBCommand *cmd)
{
    return batch->used &&
           batch->key.epoch == usb_proxy_get_controller_epoch() &&
           batch->key.generation == be16(&cmd->reserved) &&
           batch->key.address == be32(&cmd->dev_addr) &&
           batch->key.endpoint == be16(&cmd->endpoint) &&
           batch->key.direction == be16(&cmd->direction);
}

static unsigned iso_metadata_size(unsigned packet_count)
{
    return ZZUSB_ISO_HEADER_SIZE + packet_count * ZZUSB_ISO_PACKET_SIZE;
}

static uint32_t iso_interval_uframes(const struct ehci_iso_config *config)
{
    return (1U << (config->interval - 1U)) *
           (config->speed == 3U ? 1U : 8U);
}

static void iso_choose_asap(const struct usb_proxy_iso_batch *new_batch,
                            struct ehci_ctrl *ctrl,
                            uint16_t *start_frame,
                            uint8_t *start_microframe)
{
    uint32_t current = (uint32_t)ehci_iso_current_frame(ctrl) * 8U;
    uint32_t best_distance = 32U;
    unsigned slot;

    for (slot = 0; slot < ZZUSB_ISO_MAX_BATCHES; slot++) {
        const struct usb_proxy_iso_batch *batch = &iso_batches[slot];
        const struct ehci_iso_packet *last;
        uint32_t candidate;
        uint32_t distance;

        if (batch == new_batch || !batch->used || batch->ready ||
            batch->key.epoch != new_batch->key.epoch ||
            batch->key.generation != new_batch->key.generation ||
            batch->key.address != new_batch->key.address ||
            batch->key.endpoint != new_batch->key.endpoint ||
            batch->key.direction != new_batch->key.direction ||
            !batch->transfer.packet_count)
            continue;
        last = &batch->transfer.packets[
            batch->transfer.packet_count - 1U];
        candidate = ((uint32_t)last->frame * 8U +
                     last->microframe +
                     iso_interval_uframes(&batch->transfer.config)) &
                    0x3fffU;
        distance = (candidate - current) & 0x3fffU;
        if (distance >= 32U && distance < 8192U &&
            distance > best_distance)
            best_distance = distance;
    }

    current = (current + best_distance) & 0x3fffU;
    *start_frame = (uint16_t)(current >> 3);
    *start_microframe = (uint8_t)(current & 7U);
}

static void iso_mark_packets(struct usb_proxy_iso_batch *batch,
                             uint8_t status)
{
    unsigned packet;

    batch->transfer.complete = 1;
    for (packet = 0; packet < batch->transfer.packet_count; packet++) {
        batch->transfer.packets[packet].actual = 0;
        batch->transfer.packets[packet].status = status;
    }
    batch->ready = 1;
    usb_proxy_refresh_event_irq();
}

static void iso_write_packet(uint8_t *wire, unsigned packet_index,
                             const struct ehci_iso_packet *packet)
{
    uint8_t *entry = wire + ZZUSB_ISO_HEADER_SIZE +
                     packet_index * ZZUSB_ISO_PACKET_SIZE;

    put_be16(entry + ZZUSB_ISO_PKT_OFF_REQUESTED, packet->requested);
    put_be16(entry + ZZUSB_ISO_PKT_OFF_ACTUAL, packet->actual);
    put_be16(entry + ZZUSB_ISO_PKT_OFF_STATUS, packet->status);
    put_be16(entry + ZZUSB_ISO_PKT_OFF_FRAME, packet->frame);
    put_be32(entry + ZZUSB_ISO_PKT_OFF_DATA, packet->offset);
    entry[ZZUSB_ISO_PKT_OFF_UFRAME] = packet->microframe;
    entry[ZZUSB_ISO_PKT_OFF_UFRAME + 1U] = 0;
    entry[ZZUSB_ISO_PKT_OFF_UFRAME + 2U] = 0;
    entry[ZZUSB_ISO_PKT_OFF_UFRAME + 3U] = 0;
}

uint16_t usb_proxy_iso_handle_queue(volatile struct ZZUSBCommand *cmd,
                                    uint8_t *wire)
{
    struct ehci_ctrl *ctrl = iso_controller();
    struct usb_proxy_iso_batch *batch = NULL;
    struct ehci_iso_config config;
    struct ehci_iso_packet packets[ZZUSB_ISO_MAX_PACKETS];
    uint32_t wire_length = be32(&cmd->data_length);
    uint32_t total_data;
    uint32_t batch_id;
    uint32_t expected_offset = 0;
    uint16_t packet_count;
    uint16_t start_frame;
    uint8_t start_microframe;
    uint16_t flags;
    unsigned metadata_size;
    unsigned slot;
    unsigned packet;
    int schedule_result;

    if (!ctrl || !wire || wire_length < ZZUSB_ISO_HEADER_SIZE ||
        be32(wire + ZZUSB_ISO_HDR_OFF_MAGIC) != ZZUSB_ISO_MAGIC ||
        be16(wire + ZZUSB_ISO_HDR_OFF_VERSION) != ZZUSB_ISO_VERSION)
        return ZZUSB_STATUS_BADPARAM;

    flags = be16(wire + ZZUSB_ISO_HDR_OFF_FLAGS);
    batch_id = be32(wire + ZZUSB_ISO_HDR_OFF_BATCH_ID);
    start_frame = be16(wire + ZZUSB_ISO_HDR_OFF_START) &
                  EHCI_ISO_FRAME_MASK;
    start_microframe = wire[ZZUSB_ISO_HDR_OFF_START_UFRAME];
    packet_count = be16(wire + ZZUSB_ISO_HDR_OFF_COUNT);
    total_data = be32(wire + ZZUSB_ISO_HDR_OFF_DATA_LEN);
    if (!batch_id || !packet_count ||
        packet_count > ZZUSB_ISO_MAX_PACKETS ||
        total_data > ZZUSB_ISO_DATA_MAX || start_microframe > 7U)
        return ZZUSB_STATUS_BADPARAM;
    metadata_size = iso_metadata_size(packet_count);
    if (wire_length < metadata_size ||
        (be16(&cmd->direction) == 0 &&
         wire_length != metadata_size + total_data) ||
        (be16(&cmd->direction) == 0x80 && wire_length != metadata_size))
        return ZZUSB_STATUS_BADPARAM;

    for (slot = 0; slot < ZZUSB_ISO_MAX_BATCHES; slot++) {
        if (!iso_batches[slot].used && !batch)
            batch = &iso_batches[slot];
        if (iso_identity_matches(&iso_batches[slot], cmd) &&
            iso_batches[slot].batch_id == batch_id)
            return ZZUSB_STATUS_STALE;
    }
    if (!batch)
        return ZZUSB_STATUS_BUSY;

    memset(batch, 0, sizeof(*batch));
    batch->key.epoch = usb_proxy_get_controller_epoch();
    batch->key.generation = be16(&cmd->reserved);
    batch->key.address = (uint16_t)be32(&cmd->dev_addr);
    batch->key.endpoint = be16(&cmd->endpoint);
    batch->key.direction = be16(&cmd->direction);
    batch->batch_id = batch_id;
    batch->total_data = total_data;
    batch->flags = flags;
    batch->used = 1;

    for (packet = 0; packet < packet_count; packet++) {
        uint8_t *entry = wire + ZZUSB_ISO_HEADER_SIZE +
                         packet * ZZUSB_ISO_PACKET_SIZE;
        uint16_t requested = be16(entry + ZZUSB_ISO_PKT_OFF_REQUESTED);
        uint32_t offset = be32(entry + ZZUSB_ISO_PKT_OFF_DATA);

        if (offset != expected_offset ||
            offset + requested > total_data) {
            memset(batch, 0, sizeof(*batch));
            return ZZUSB_STATUS_BADPARAM;
        }
        memset(&packets[packet], 0, sizeof(packets[packet]));
        packets[packet].requested = requested;
        packets[packet].offset = offset;
        expected_offset += requested;
    }
    if (expected_offset != total_data) {
        memset(batch, 0, sizeof(*batch));
        return ZZUSB_STATUS_BADPARAM;
    }
    if (be16(&cmd->direction) == 0 && total_data)
        memcpy(batch->data, wire + metadata_size, total_data);
    else if (total_data)
        memset(batch->data, 0, total_data);

    memset(&config, 0, sizeof(config));
    config.address = batch->key.address;
    config.endpoint = batch->key.endpoint;
    config.direction_in = batch->key.direction == 0x80;
    config.speed = be16(&cmd->speed) == ZZUSB_SPEED_HIGH ? 3U :
                   be16(&cmd->speed) == ZZUSB_SPEED_FULL ? 2U : 1U;
    config.split = (be16(&cmd->flags) & ZZUSB_FLAG_SPLIT) != 0;
    config.multi_tt = (be16(&cmd->flags) & ZZUSB_FLAG_MULTI_TT) != 0;
    config.tt_think_time = (uint8_t)(
        (be16(&cmd->flags) & ZZUSB_FLAG_TT_THINK_MASK) >>
        ZZUSB_FLAG_TT_THINK_SHIFT);
    config.interval = (uint8_t)be16(&cmd->interval);
    config.hub_address = (uint8_t)be16(&cmd->split_hub_addr);
    config.hub_port = (uint8_t)be16(&cmd->split_hub_port);
    config.max_packet = be16(&cmd->max_pkt_size);

    if (flags & ZZUSB_ISO_FLAG_ASAP)
        iso_choose_asap(batch, ctrl, &start_frame, &start_microframe);
    packets[0].frame = start_frame;
    packets[0].microframe = start_microframe;
    schedule_result = ehci_iso_schedule(&batch->transfer, ctrl, &config,
                                        packets, packet_count, batch->data);
    if (schedule_result == EHCI_ISO_ERR_MISSED) {
        iso_mark_packets(batch, EHCI_ISO_PACKET_MISSED);
    } else if (schedule_result < 0) {
        if (batch->transfer.linked) {
            iso_mark_packets(batch, EHCI_ISO_PACKET_OFFLINE);
            return ZZUSB_STATUS_HOSTERROR;
        }
        memset(batch, 0, sizeof(*batch));
        return schedule_result == EHCI_ISO_ERR_BANDWIDTH ?
               ZZUSB_STATUS_BUSY : ZZUSB_STATUS_BADPARAM;
    }

    zzusb_diag_count(ZZUSB_DIAG_COUNT_ISO_QUEUE);
    put_be32(&cmd->actual_length, 0);
    return ZZUSB_STATUS_OK;
}

void usb_proxy_iso_pump(void)
{
    unsigned visited;

    for (visited = 0; visited < ZZUSB_ISO_PUMP_BUDGET; visited++) {
        struct usb_proxy_iso_batch *batch =
            &iso_batches[iso_pump_cursor];

        iso_pump_cursor = (uint8_t)((iso_pump_cursor + 1U) %
                                    ZZUSB_ISO_MAX_BATCHES);
        if (!batch->used || batch->ready || !batch->transfer.linked)
            continue;
        if (ehci_iso_poll(&batch->transfer) > 0) {
            batch->ready = 1;
            usb_proxy_refresh_event_irq();
        }
    }
}

uint16_t usb_proxy_iso_handle_reap(volatile struct ZZUSBCommand *cmd,
                                   uint8_t *wire)
{
    uint32_t capacity = be32(&cmd->data_length);
    unsigned slot;

    for (slot = 0; slot < ZZUSB_ISO_MAX_BATCHES; slot++) {
        struct usb_proxy_iso_batch *batch = &iso_batches[slot];
        unsigned metadata_size;
        unsigned output_size;
        unsigned packet;

        if (!batch->ready || !iso_identity_matches(batch, cmd))
            continue;
        metadata_size = iso_metadata_size(batch->transfer.packet_count);
        output_size = metadata_size +
            (batch->key.direction == 0x80 ? batch->total_data : 0U);
        if (!wire || capacity < output_size)
            return ZZUSB_STATUS_BADPARAM;
        if (batch->transfer.linked &&
            ehci_iso_retire(&batch->transfer,
                            EHCI_ISO_PACKET_CANCELLED) < 0)
            return ZZUSB_STATUS_HOSTERROR;

        memset(wire, 0, output_size);
        put_be32(wire + ZZUSB_ISO_HDR_OFF_MAGIC, ZZUSB_ISO_MAGIC);
        put_be16(wire + ZZUSB_ISO_HDR_OFF_VERSION, ZZUSB_ISO_VERSION);
        put_be16(wire + ZZUSB_ISO_HDR_OFF_FLAGS, batch->flags);
        put_be32(wire + ZZUSB_ISO_HDR_OFF_BATCH_ID, batch->batch_id);
        put_be16(wire + ZZUSB_ISO_HDR_OFF_START,
                 batch->transfer.packets[0].frame);
        put_be16(wire + ZZUSB_ISO_HDR_OFF_COUNT,
                 batch->transfer.packet_count);
        put_be32(wire + ZZUSB_ISO_HDR_OFF_DATA_LEN, batch->total_data);
        wire[ZZUSB_ISO_HDR_OFF_START_UFRAME] =
            batch->transfer.packets[0].microframe;
        for (packet = 0; packet < batch->transfer.packet_count; packet++)
            iso_write_packet(wire, packet,
                             &batch->transfer.packets[packet]);
        if (batch->key.direction == 0x80 && batch->total_data) {
            Xil_DCacheInvalidateRange((u32)(uintptr_t)batch->data,
                                      ALIGN(batch->total_data, 32));
            memcpy(wire + metadata_size, batch->data, batch->total_data);
        }
        put_be32(&cmd->actual_length, output_size);
        zzusb_diag_count(ZZUSB_DIAG_COUNT_ISO_REAP);
        memset(batch, 0, sizeof(*batch));
        usb_proxy_refresh_event_irq();
        return ZZUSB_STATUS_OK;
    }
    put_be32(&cmd->actual_length, 0);
    return ZZUSB_STATUS_NAK;
}

uint16_t usb_proxy_iso_handle_stop(volatile struct ZZUSBCommand *cmd)
{
    unsigned slot;
    int stopped = 0;

    for (slot = 0; slot < ZZUSB_ISO_MAX_BATCHES; slot++) {
        struct usb_proxy_iso_batch *batch = &iso_batches[slot];

        if (!iso_identity_matches(batch, cmd))
            continue;
        if (batch->transfer.linked &&
            ehci_iso_retire(&batch->transfer,
                            EHCI_ISO_PACKET_CANCELLED) < 0)
            return ZZUSB_STATUS_HOSTERROR;
        memset(batch, 0, sizeof(*batch));
        stopped = 1;
    }
    usb_proxy_refresh_event_irq();
    put_be32(&cmd->actual_length, 0);
    return stopped ? ZZUSB_STATUS_OK : ZZUSB_STATUS_NAK;
}

int usb_proxy_iso_stop_all(uint8_t unfinished_status)
{
    unsigned slot;
    int result = 0;

    for (slot = 0; slot < ZZUSB_ISO_MAX_BATCHES; slot++) {
        struct usb_proxy_iso_batch *batch = &iso_batches[slot];

        if (!batch->used)
            continue;
        if (batch->transfer.linked &&
            ehci_iso_retire(&batch->transfer, unfinished_status) < 0) {
            result = -1;
            continue;
        }
        memset(batch, 0, sizeof(*batch));
    }
    usb_proxy_refresh_event_irq();
    return result;
}

void usb_proxy_iso_after_controller_reset(void)
{
    memset(iso_batches, 0, sizeof(iso_batches));
    ehci_iso_reset_bandwidth();
    iso_pump_cursor = 0;
}

uint32_t usb_proxy_iso_queue_state(void)
{
    uint32_t active = 0;
    uint32_t ready = 0;
    unsigned slot;

    for (slot = 0; slot < ZZUSB_ISO_MAX_BATCHES; slot++) {
        if (iso_batches[slot].used)
            active++;
        if (iso_batches[slot].used && iso_batches[slot].ready)
            ready++;
    }
    return (active << 24) | (ready << 28);
}

uint32_t usb_proxy_iso_schedule_bits(void)
{
    uint32_t high_speed = 0;
    uint32_t split = 0;
    unsigned slot;

    for (slot = 0; slot < ZZUSB_ISO_MAX_BATCHES; slot++) {
        if (!iso_batches[slot].used)
            continue;
        if (iso_batches[slot].transfer.config.split)
            split++;
        else
            high_speed++;
    }
    return (high_speed & 0xffU) | ((split & 0xffU) << 8);
}
