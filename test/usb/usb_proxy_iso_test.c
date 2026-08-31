/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "usb_proxy.h"
#include "usb_proxy_diag.h"
#include "usb_proxy_iso.h"
#include "usb/ehci-iso.h"

struct ehci_ctrl {
    int unused;
};

static struct ehci_ctrl fake_ctrl;
static int fake_schedule_result;
static int fake_complete;
static int fake_retire_failure;
static unsigned fake_retire_count;
static unsigned fake_bandwidth_reset_count;
static unsigned fake_refresh_count;
static uint16_t fake_current_frame = 100;
static uint8_t *fake_buffer;
static unsigned fake_schedule_count;
static uint16_t fake_start_frame[ZZUSB_ISO_MAX_BATCHES];
static uint8_t fake_start_microframe[ZZUSB_ISO_MAX_BATCHES];

struct ehci_ctrl *usb_proxy_get_ehci_controller(void)
{
    return &fake_ctrl;
}

uint32_t usb_proxy_get_controller_epoch(void)
{
    return 77;
}

void usb_proxy_refresh_event_irq(void)
{
    fake_refresh_count++;
}


void zzusb_diag_count(enum zzusb_diag_counter counter)
{
    assert(counter == ZZUSB_DIAG_COUNT_ISO_QUEUE ||
           counter == ZZUSB_DIAG_COUNT_ISO_REAP);
}

uint16_t ehci_iso_current_frame(struct ehci_ctrl *ctrl)
{
    assert(ctrl == &fake_ctrl);
    return fake_current_frame;
}

int ehci_iso_schedule(struct ehci_iso_transfer *transfer,
                      struct ehci_ctrl *ctrl,
                      const struct ehci_iso_config *config,
                      struct ehci_iso_packet *packets,
                      unsigned packet_count, uint8_t *buffer)
{
    uint32_t absolute_uframe;
    uint32_t step;

    assert(ctrl == &fake_ctrl);
    assert(packet_count <= EHCI_ISO_MAX_PACKETS);
    memset(transfer, 0, sizeof(*transfer));
    transfer->ctrl = ctrl;
    transfer->config = *config;
    transfer->packet_count = (uint8_t)packet_count;
    absolute_uframe = (uint32_t)packets[0].frame * 8U +
                      packets[0].microframe;
    step = config->speed == 3U ? 1U << (config->interval - 1U) :
           (uint32_t)(1U << (config->interval - 1U)) * 8U;
    assert(fake_schedule_count < ZZUSB_ISO_MAX_BATCHES);
    fake_start_frame[fake_schedule_count] = packets[0].frame;
    fake_start_microframe[fake_schedule_count] = packets[0].microframe;
    fake_schedule_count++;
    for (unsigned index = 0; index < packet_count; index++) {
        transfer->packets[index] = packets[index];
        transfer->packets[index].frame =
            (uint16_t)((absolute_uframe >> 3) & EHCI_ISO_FRAME_MASK);
        transfer->packets[index].microframe =
            (uint8_t)(absolute_uframe & 7U);
        transfer->packets[index].status = EHCI_ISO_PACKET_PENDING;
        absolute_uframe += step;
    }
    fake_buffer = buffer;
    if (fake_schedule_result) {
        if (fake_schedule_result == EHCI_ISO_ERR_TIMEOUT)
            transfer->linked = 1;
        return fake_schedule_result;
    }
    transfer->linked = 1;
    return 0;
}

int ehci_iso_poll(struct ehci_iso_transfer *transfer)
{
    if (!fake_complete)
        return 0;
    for (unsigned index = 0; index < transfer->packet_count; index++) {
        struct ehci_iso_packet *packet = &transfer->packets[index];

        packet->actual = packet->requested;
        packet->status = EHCI_ISO_PACKET_OK;
    }
    for (unsigned index = 0; index < 16; index++)
        fake_buffer[index] = (uint8_t)(0xa0U + index);
    transfer->complete = 1;
    return 1;
}

int ehci_iso_retire(struct ehci_iso_transfer *transfer,
                    uint8_t unfinished_status)
{
    (void)unfinished_status;
    fake_retire_count++;
    if (fake_retire_failure)
        return EHCI_ISO_ERR_TIMEOUT;
    transfer->linked = 0;
    return 0;
}

void ehci_iso_reset_bandwidth(void)
{
    fake_bandwidth_reset_count++;
}

static void make_command(struct ZZUSBCommand *cmd, uint16_t command,
                         uint32_t data_length)
{
    memset(cmd, 0, sizeof(*cmd));
    put_be16(&cmd->cmd, command);
    put_be32(&cmd->dev_addr, 4);
    put_be16(&cmd->endpoint, 2);
    put_be16(&cmd->direction, 0x80);
    put_be16(&cmd->speed, ZZUSB_SPEED_HIGH);
    put_be16(&cmd->max_pkt_size, 1024);
    put_be16(&cmd->interval, 1);
    put_be16(&cmd->reserved, 9);
    put_be32(&cmd->data_length, data_length);
}

static unsigned make_batch(uint8_t *wire, uint32_t batch_id,
                           uint16_t flags, uint16_t start_frame)
{
    unsigned metadata_size = ZZUSB_ISO_HEADER_SIZE +
                             2U * ZZUSB_ISO_PACKET_SIZE;

    memset(wire, 0, ZZUSB_V2_DATA_MAX);
    put_be32(wire + ZZUSB_ISO_HDR_OFF_MAGIC, ZZUSB_ISO_MAGIC);
    put_be16(wire + ZZUSB_ISO_HDR_OFF_VERSION, ZZUSB_ISO_VERSION);
    put_be16(wire + ZZUSB_ISO_HDR_OFF_FLAGS, flags);
    put_be32(wire + ZZUSB_ISO_HDR_OFF_BATCH_ID, batch_id);
    put_be16(wire + ZZUSB_ISO_HDR_OFF_START, start_frame);
    put_be16(wire + ZZUSB_ISO_HDR_OFF_COUNT, 2);
    put_be32(wire + ZZUSB_ISO_HDR_OFF_DATA_LEN, 16);
    put_be16(wire + ZZUSB_ISO_HEADER_SIZE +
             ZZUSB_ISO_PKT_OFF_REQUESTED, 8);
    put_be32(wire + ZZUSB_ISO_HEADER_SIZE +
             ZZUSB_ISO_PKT_OFF_DATA, 0);
    put_be16(wire + ZZUSB_ISO_HEADER_SIZE + ZZUSB_ISO_PACKET_SIZE +
             ZZUSB_ISO_PKT_OFF_REQUESTED, 8);
    put_be32(wire + ZZUSB_ISO_HEADER_SIZE + ZZUSB_ISO_PACKET_SIZE +
             ZZUSB_ISO_PKT_OFF_DATA, 8);
    return metadata_size;
}

static void reset_fixture(void)
{
    fake_schedule_result = 0;
    fake_complete = 0;
    fake_retire_failure = 0;
    fake_retire_count = 0;
    fake_refresh_count = 0;
    fake_current_frame = 100;
    fake_schedule_count = 0;
    memset(fake_start_frame, 0, sizeof(fake_start_frame));
    memset(fake_start_microframe, 0, sizeof(fake_start_microframe));
    fake_buffer = NULL;
    usb_proxy_iso_after_controller_reset();
}

static void test_queue_complete_reap(void)
{
    struct ZZUSBCommand cmd;
    uint8_t wire[ZZUSB_V2_DATA_MAX];
    unsigned metadata_size;

    reset_fixture();
    metadata_size = make_batch(wire, 0x11223344,
                               ZZUSB_ISO_FLAG_ASAP, 0);
    make_command(&cmd, ZZUSB_CMD_ISO_QUEUE, metadata_size);
    assert(usb_proxy_iso_handle_queue(&cmd, wire) == ZZUSB_STATUS_OK);
    assert(usb_proxy_iso_queue_state() == 0x01000000);
    assert(usb_proxy_iso_schedule_bits() == 1);

    fake_complete = 1;
    usb_proxy_iso_pump();
    assert(usb_proxy_iso_queue_state() == 0x11000000);
    assert(fake_refresh_count == 1);

    make_command(&cmd, ZZUSB_CMD_ISO_REAP, sizeof(wire));
    assert(usb_proxy_iso_handle_reap(&cmd, wire) == ZZUSB_STATUS_OK);
    assert(be32(&cmd.actual_length) == metadata_size + 16U);
    assert(be32(wire + ZZUSB_ISO_HDR_OFF_BATCH_ID) == 0x11223344);
    assert(be16(wire + ZZUSB_ISO_HEADER_SIZE +
                ZZUSB_ISO_PKT_OFF_STATUS) == EHCI_ISO_PACKET_OK);
    assert(be16(wire + ZZUSB_ISO_HEADER_SIZE +
                ZZUSB_ISO_PKT_OFF_FRAME) == 104);
    assert(wire[ZZUSB_ISO_HEADER_SIZE +
                ZZUSB_ISO_PKT_OFF_UFRAME] == 0);
    assert(wire[ZZUSB_ISO_HEADER_SIZE + ZZUSB_ISO_PACKET_SIZE +
                ZZUSB_ISO_PKT_OFF_UFRAME] == 1);
    for (unsigned index = 0; index < 16; index++)
        assert(wire[metadata_size + index] == (uint8_t)(0xa0U + index));
    assert(fake_retire_count == 1);
    assert(fake_refresh_count == 2);
    assert(usb_proxy_iso_queue_state() == 0);
}

static void test_asap_batches_chain(void)
{
    struct ZZUSBCommand cmd;
    uint8_t wire[ZZUSB_V2_DATA_MAX];
    unsigned metadata_size;

    reset_fixture();
    metadata_size = make_batch(wire, 10, ZZUSB_ISO_FLAG_ASAP, 0);
    make_command(&cmd, ZZUSB_CMD_ISO_QUEUE, metadata_size);
    assert(usb_proxy_iso_handle_queue(&cmd, wire) == ZZUSB_STATUS_OK);
    metadata_size = make_batch(wire, 11, ZZUSB_ISO_FLAG_ASAP, 0);
    make_command(&cmd, ZZUSB_CMD_ISO_QUEUE, metadata_size);
    assert(usb_proxy_iso_handle_queue(&cmd, wire) == ZZUSB_STATUS_OK);
    assert(fake_schedule_count == 2);
    assert(fake_start_frame[0] == 104);
    assert(fake_start_microframe[0] == 0);
    assert(fake_start_frame[1] == 104);
    assert(fake_start_microframe[1] == 2);
    assert(usb_proxy_iso_stop_all(EHCI_ISO_PACKET_CANCELLED) == 0);
}

static void test_completed_batch_does_not_delay_asap(void)
{
    struct ZZUSBCommand cmd;
    uint8_t wire[ZZUSB_V2_DATA_MAX];
    unsigned metadata_size;

    reset_fixture();
    metadata_size = make_batch(wire, 12, 0, 100);
    make_command(&cmd, ZZUSB_CMD_ISO_QUEUE, metadata_size);
    assert(usb_proxy_iso_handle_queue(&cmd, wire) == ZZUSB_STATUS_OK);
    fake_complete = 1;
    usb_proxy_iso_pump();
    assert(usb_proxy_iso_queue_state() == 0x11000000);

    fake_current_frame = 1200;
    metadata_size = make_batch(wire, 13, ZZUSB_ISO_FLAG_ASAP, 0);
    make_command(&cmd, ZZUSB_CMD_ISO_QUEUE, metadata_size);
    assert(usb_proxy_iso_handle_queue(&cmd, wire) == ZZUSB_STATUS_OK);
    assert(fake_start_frame[1] == 1204);
    assert(fake_start_microframe[1] == 0);
    assert(usb_proxy_iso_stop_all(EHCI_ISO_PACKET_CANCELLED) == 0);
}

static void test_full_speed_asap_batches_use_exponential_interval(void)
{
    struct ZZUSBCommand cmd;
    uint8_t wire[ZZUSB_V2_DATA_MAX];
    unsigned metadata_size;

    reset_fixture();
    metadata_size = make_batch(wire, 20, ZZUSB_ISO_FLAG_ASAP, 0);
    make_command(&cmd, ZZUSB_CMD_ISO_QUEUE, metadata_size);
    put_be16(&cmd.speed, ZZUSB_SPEED_FULL);
    put_be16(&cmd.max_pkt_size, 64);
    put_be16(&cmd.interval, 3);
    put_be16(&cmd.flags, ZZUSB_FLAG_SPLIT);
    put_be16(&cmd.split_hub_addr, 1);
    put_be16(&cmd.split_hub_port, 2);
    assert(usb_proxy_iso_handle_queue(&cmd, wire) == ZZUSB_STATUS_OK);

    metadata_size = make_batch(wire, 21, ZZUSB_ISO_FLAG_ASAP, 0);
    make_command(&cmd, ZZUSB_CMD_ISO_QUEUE, metadata_size);
    put_be16(&cmd.speed, ZZUSB_SPEED_FULL);
    put_be16(&cmd.max_pkt_size, 64);
    put_be16(&cmd.interval, 3);
    put_be16(&cmd.flags, ZZUSB_FLAG_SPLIT);
    put_be16(&cmd.split_hub_addr, 1);
    put_be16(&cmd.split_hub_port, 2);
    assert(usb_proxy_iso_handle_queue(&cmd, wire) == ZZUSB_STATUS_OK);

    assert(fake_schedule_count == 2);
    assert(fake_start_frame[0] == 104);
    assert(fake_start_microframe[0] == 0);
    assert(fake_start_frame[1] == 112);
    assert(fake_start_microframe[1] == 0);
    assert(usb_proxy_iso_stop_all(EHCI_ISO_PACKET_CANCELLED) == 0);
}

static void test_missed_frame_status(void)
{
    struct ZZUSBCommand cmd;
    uint8_t wire[ZZUSB_V2_DATA_MAX];
    unsigned metadata_size;

    reset_fixture();
    fake_schedule_result = EHCI_ISO_ERR_MISSED;
    metadata_size = make_batch(wire, 2, 0, 98);
    make_command(&cmd, ZZUSB_CMD_ISO_QUEUE, metadata_size);
    assert(usb_proxy_iso_handle_queue(&cmd, wire) == ZZUSB_STATUS_OK);
    assert(usb_proxy_iso_queue_state() == 0x11000000);

    make_command(&cmd, ZZUSB_CMD_ISO_REAP, sizeof(wire));
    assert(usb_proxy_iso_handle_reap(&cmd, wire) == ZZUSB_STATUS_OK);
    assert(be16(wire + ZZUSB_ISO_HEADER_SIZE +
                ZZUSB_ISO_PKT_OFF_STATUS) == EHCI_ISO_PACKET_MISSED);
    assert(be16(wire + ZZUSB_ISO_HEADER_SIZE +
                ZZUSB_ISO_PKT_OFF_FRAME) == 98);
    assert(be16(wire + ZZUSB_ISO_HEADER_SIZE + ZZUSB_ISO_PACKET_SIZE +
                ZZUSB_ISO_PKT_OFF_FRAME) == 98);
}

static void test_linked_failure_is_quarantined(void)
{
    struct ZZUSBCommand cmd;
    uint8_t wire[ZZUSB_V2_DATA_MAX];
    unsigned metadata_size;

    reset_fixture();
    fake_schedule_result = EHCI_ISO_ERR_TIMEOUT;
    metadata_size = make_batch(wire, 3, ZZUSB_ISO_FLAG_ASAP, 0);
    make_command(&cmd, ZZUSB_CMD_ISO_QUEUE, metadata_size);
    assert(usb_proxy_iso_handle_queue(&cmd, wire) ==
           ZZUSB_STATUS_HOSTERROR);
    assert(usb_proxy_iso_queue_state() == 0x11000000);

    make_command(&cmd, ZZUSB_CMD_ISO_REAP, sizeof(wire));
    assert(usb_proxy_iso_handle_reap(&cmd, wire) == ZZUSB_STATUS_OK);
    assert(be16(wire + ZZUSB_ISO_HEADER_SIZE +
                ZZUSB_ISO_PKT_OFF_STATUS) == EHCI_ISO_PACKET_OFFLINE);
    assert(fake_retire_count == 1);
    assert(usb_proxy_iso_queue_state() == 0);
}

static void test_ring_backpressure_and_retirement(void)
{
    struct ZZUSBCommand cmd;
    uint8_t wire[ZZUSB_V2_DATA_MAX];
    unsigned metadata_size;

    reset_fixture();
    for (unsigned batch = 1; batch <= ZZUSB_ISO_MAX_BATCHES; batch++) {
        metadata_size = make_batch(wire, batch,
                                   ZZUSB_ISO_FLAG_ASAP, 0);
        make_command(&cmd, ZZUSB_CMD_ISO_QUEUE, metadata_size);
        assert(usb_proxy_iso_handle_queue(&cmd, wire) == ZZUSB_STATUS_OK);
    }
    metadata_size = make_batch(wire, 99, ZZUSB_ISO_FLAG_ASAP, 0);
    make_command(&cmd, ZZUSB_CMD_ISO_QUEUE, metadata_size);
    assert(usb_proxy_iso_handle_queue(&cmd, wire) == ZZUSB_STATUS_BUSY);

    fake_retire_failure = 1;
    assert(usb_proxy_iso_stop_all(EHCI_ISO_PACKET_CANCELLED) < 0);
    assert(usb_proxy_iso_queue_state() == 0x08000000);
    assert(usb_proxy_iso_handle_queue(&cmd, wire) == ZZUSB_STATUS_BUSY);

    fake_retire_failure = 0;
    assert(usb_proxy_iso_stop_all(EHCI_ISO_PACKET_CANCELLED) == 0);
    assert(fake_retire_count == 16);
    assert(usb_proxy_iso_queue_state() == 0);
}

int main(void)
{
    test_queue_complete_reap();
    test_asap_batches_chain();
    test_completed_batch_does_not_delay_asap();
    test_full_speed_asap_batches_use_exponential_interval();
    test_missed_frame_status();
    test_linked_failure_is_quarantined();
    test_ring_backpressure_and_retirement();
    assert(fake_bandwidth_reset_count == 7);
    puts("usb_proxy_iso_test: ok");
    return 0;
}
