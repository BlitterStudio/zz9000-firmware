/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ZZ9000 USB Proxy — command mailbox protocol between the m68k
 * Poseidon USB driver and the ARM firmware's EHCI host stack.
 *
 * Copyright (C) 2026 Dimitris Panokostas <midwan@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef USB_PROXY_H
#define USB_PROXY_H

#include <xil_types.h>

struct ehci_ctrl;

#define ZZUSB_CMD_CONTROL_XFER  0x01
#define ZZUSB_CMD_BULK_XFER     0x02
#define ZZUSB_CMD_INT_XFER      0x03
#define ZZUSB_CMD_ISO_XFER      0x04
#define ZZUSB_CMD_RESET_PORT    0x05
#define ZZUSB_CMD_RESUME_PORT   0x06
#define ZZUSB_CMD_SUSPEND_PORT  0x07
#define ZZUSB_CMD_ENUMERATE     0x08
#define ZZUSB_CMD_QUERY_DEVICE  0x09
#define ZZUSB_CMD_SET_ADDRESS   0x0A
#define ZZUSB_CMD_CLEAR_STALL   0x0B
#define ZZUSB_CMD_CHECK_PORT    0x0C
#define ZZUSB_CMD_QUERY_CAPS     0x0D
#define ZZUSB_CMD_RETIRE_EP      0x0E
#define ZZUSB_CMD_CANCEL_EP      0x0F
#define ZZUSB_CMD_DIAG_SNAPSHOT  0x10
#define ZZUSB_CMD_PERIODIC_ARM    0x11
#define ZZUSB_CMD_PERIODIC_REAP   0x12
#define ZZUSB_CMD_PERIODIC_STOP   0x13
#define ZZUSB_CMD_ISO_QUEUE       0x14
#define ZZUSB_CMD_ISO_REAP        0x15
#define ZZUSB_CMD_ISO_STOP        0x16

#define ZZUSB_STATUS_OK         0x00
#define ZZUSB_STATUS_PENDING    0x01
#define ZZUSB_STATUS_ERROR      0xFF
#define ZZUSB_STATUS_TIMEOUT    0xFE
#define ZZUSB_STATUS_STALL      0xFD
#define ZZUSB_STATUS_NAK        0xFC
#define ZZUSB_STATUS_CRC        0xFB
#define ZZUSB_STATUS_BABBLE     0xFA
#define ZZUSB_STATUS_OVERRUN    0xF9
#define ZZUSB_STATUS_UNDERRUN   0xF8
#define ZZUSB_STATUS_OFFLINE    0xF7
#define ZZUSB_STATUS_BADPARAM   0xF6
#define ZZUSB_STATUS_UNSUPPORTED 0xF5
#define ZZUSB_STATUS_STALE       0xF4
#define ZZUSB_STATUS_CANCELLED   0xF3
#define ZZUSB_STATUS_HOSTERROR   0xF2
#define ZZUSB_STATUS_BUSY        0xF1
#define ZZUSB_STATUS_NOMEM       0xF0

#define ZZUSB_SPEED_LOW         0
#define ZZUSB_SPEED_FULL        1
#define ZZUSB_SPEED_HIGH        2

#define ZZUSB_FLAG_SPLIT        0x0001
#define ZZUSB_FLAG_RESET_FSLS   0x0002
#define ZZUSB_FLAG_MULTI_TT     0x0004
#define ZZUSB_FLAG_TT_THINK_SHIFT 4
#define ZZUSB_FLAG_TT_THINK_MASK  0x0030

#define ZZUSB_XFER_CONTROL      0
#define ZZUSB_XFER_BULK         1
#define ZZUSB_XFER_INTERRUPT    2
#define ZZUSB_XFER_ISO          3

#define ZZUSB_PROTOCOL_VERSION   2
#define ZZUSB_CMD_SIZE           48
#define ZZUSB_V2_HEADER_SIZE     64
#define ZZUSB_DATA_OFFSET        64
#define ZZUSB_APERTURE_SIZE      24576
#define ZZUSB_MAX_XFER           (ZZUSB_APERTURE_SIZE - ZZUSB_DATA_OFFSET)
#define ZZUSB_V2_DATA_MAX        16384
#define ZZUSB_DIAG_SIZE          4096
#define ZZUSB_DIAG_OFFSET        (ZZUSB_APERTURE_SIZE - ZZUSB_DIAG_SIZE)
#define ZZUSB_DOORBELL_V2        0x8000

#define ZZUSB_CAP_PROTOCOL_V2      (1U << 0)
#define ZZUSB_CAP_REQUEST_ID       (1U << 1)
#define ZZUSB_CAP_CONTROLLER_EPOCH (1U << 2)
#define ZZUSB_CAP_VALIDATION       (1U << 3)
#define ZZUSB_CAP_DIAGNOSTICS      (1U << 4)
#define ZZUSB_CAP_PERIODIC         (1U << 5)
#define ZZUSB_CAP_ISO_SIMPLE       (1U << 6)
#define ZZUSB_CAP_ISO_REALTIME     (1U << 7)
#define ZZUSB_CAP_EVENT_IRQ        (1U << 8)
#define ZZUSB_CAP_PRECISE_ERRORS   (1U << 9)
#define ZZUSB_CAP_BASE (ZZUSB_CAP_PROTOCOL_V2 | ZZUSB_CAP_REQUEST_ID | \
                        ZZUSB_CAP_CONTROLLER_EPOCH | ZZUSB_CAP_VALIDATION | \
                        ZZUSB_CAP_DIAGNOSTICS | ZZUSB_CAP_PERIODIC | \
                        ZZUSB_CAP_ISO_SIMPLE | ZZUSB_CAP_ISO_REALTIME | \
                        ZZUSB_CAP_EVENT_IRQ | ZZUSB_CAP_PRECISE_ERRORS)

struct ZZUSBCommand {
    uint16_t cmd;
    uint16_t status;
    uint32_t dev_addr;
    uint16_t endpoint;
    uint16_t direction;
    uint16_t xfer_type;
    uint16_t max_pkt_size;
    uint32_t data_length;
    uint32_t actual_length;
    uint32_t timeout_ms;
    uint16_t speed;
    uint16_t interval;
    uint8_t  setup_bRequestType;
    uint8_t  setup_bRequest;
    uint16_t setup_wValue;
    uint16_t setup_wIndex;
    uint16_t setup_wLength;
    uint16_t split_hub_addr;
    uint16_t split_hub_port;
    uint16_t flags;
    uint16_t reserved;
} __attribute__((packed));

struct ZZUSBProtocolExtension {
    uint16_t version;
    uint16_t header_size;
    uint32_t request_id;
    uint32_t controller_epoch;
    uint32_t capabilities;
} __attribute__((packed));

typedef char ZZUSBCommand_size_must_match_protocol[
    (sizeof(struct ZZUSBCommand) == ZZUSB_CMD_SIZE) ? 1 : -1];
typedef char ZZUSBProtocolExtension_size_must_fill_gap[
    (sizeof(struct ZZUSBProtocolExtension) ==
     (ZZUSB_DATA_OFFSET - ZZUSB_CMD_SIZE)) ? 1 : -1];

static inline uint16_t be16(const volatile void *p) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    return ((uint16_t)b[0] << 8) | b[1];
}

/* Read a 16-bit value stored in USB little-endian byte order.
 * The m68k Poseidon driver copies setup packet fields (wValue, wIndex,
 * wLength) directly from its IoUsbHW API structure, which stores them
 * in USB-native little-endian format.  Using be16() on these would
 * incorrectly byte-swap them. */
static inline uint16_t le16(const volatile void *p) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    return ((uint16_t)b[1] << 8) | b[0];
}

static inline uint32_t be32(const volatile void *p) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}

static inline void put_be16(volatile void *p, uint16_t v) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    b[0] = (v >> 8) & 0xff;
    b[1] = v & 0xff;
}

static inline void put_be32(volatile void *p, uint32_t v) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    b[0] = (v >> 24) & 0xff;
    b[1] = (v >> 16) & 0xff;
    b[2] = (v >> 8) & 0xff;
    b[3] = v & 0xff;
}

static inline int zzusb_command_is_transfer(uint16_t command) {
    return command == ZZUSB_CMD_CONTROL_XFER ||
           command == ZZUSB_CMD_BULK_XFER ||
           command == ZZUSB_CMD_INT_XFER ||
           command == ZZUSB_CMD_ISO_XFER;
}

static inline uint16_t zzusb_validate_command(
    const volatile struct ZZUSBCommand *cmd,
    const volatile struct ZZUSBProtocolExtension *ext,
    int is_v2, uint32_t controller_epoch) {
    uint16_t command;
    uint16_t direction;
    uint16_t endpoint;
    uint16_t speed;
    uint16_t flags;
    uint16_t xfer_type;
    uint16_t max_packet;
    uint32_t data_length;
    uint32_t max_length;

    if (!cmd)
        return ZZUSB_STATUS_BADPARAM;

    command = be16(&cmd->cmd);
    direction = be16(&cmd->direction);
    endpoint = be16(&cmd->endpoint);
    speed = be16(&cmd->speed);
    flags = be16(&cmd->flags);
    xfer_type = be16(&cmd->xfer_type);
    max_packet = be16(&cmd->max_pkt_size);
    data_length = be32(&cmd->data_length);
    max_length = is_v2 ? ZZUSB_V2_DATA_MAX : ZZUSB_MAX_XFER;

    if (is_v2) {
        if (!ext ||
            be16(&ext->version) != ZZUSB_PROTOCOL_VERSION ||
            be16(&ext->header_size) != ZZUSB_V2_HEADER_SIZE ||
            be32(&ext->request_id) == 0)
            return ZZUSB_STATUS_BADPARAM;
        if (command != ZZUSB_CMD_QUERY_CAPS &&
            be32(&ext->controller_epoch) != controller_epoch)
            return ZZUSB_STATUS_STALE;
    }

    if (be32(&cmd->dev_addr) > 127 || endpoint > 15 ||
        (direction != 0 && direction != 0x80) ||
        speed > ZZUSB_SPEED_HIGH || data_length > max_length)
        return ZZUSB_STATUS_BADPARAM;

    if (flags & ZZUSB_FLAG_SPLIT) {
        uint16_t hub = be16(&cmd->split_hub_addr);
        uint16_t port = be16(&cmd->split_hub_port);
        if (speed == ZZUSB_SPEED_HIGH || hub == 0 || hub > 127 ||
            port == 0 || port > 127)
            return ZZUSB_STATUS_BADPARAM;
    } else if (is_v2 &&
               (be16(&cmd->split_hub_addr) != 0 ||
                be16(&cmd->split_hub_port) != 0 ||
                (flags & (ZZUSB_FLAG_MULTI_TT |
                          ZZUSB_FLAG_TT_THINK_MASK)) != 0)) {
        return ZZUSB_STATUS_BADPARAM;
    }

    switch (command) {
    case ZZUSB_CMD_CONTROL_XFER:
        if ((is_v2 && xfer_type != ZZUSB_XFER_CONTROL) ||
            endpoint != 0 || max_packet == 0 || max_packet > 64 ||
            le16(&cmd->setup_wLength) != data_length)
            return ZZUSB_STATUS_BADPARAM;
        break;
    case ZZUSB_CMD_BULK_XFER:
        if ((is_v2 && xfer_type != ZZUSB_XFER_BULK) ||
            endpoint == 0 || max_packet == 0 || max_packet > 512)
            return ZZUSB_STATUS_BADPARAM;
        break;
    case ZZUSB_CMD_INT_XFER:
        if ((is_v2 && xfer_type != ZZUSB_XFER_INTERRUPT) ||
            endpoint == 0 || max_packet == 0 || max_packet > 1024 ||
            be16(&cmd->interval) == 0)
            return ZZUSB_STATUS_BADPARAM;
        break;
    case ZZUSB_CMD_ISO_XFER:
        if ((is_v2 && xfer_type != ZZUSB_XFER_ISO) ||
            endpoint == 0 || max_packet == 0 || max_packet > 0x13ff ||
            be16(&cmd->interval) == 0)
            return ZZUSB_STATUS_BADPARAM;
        break;
    case ZZUSB_CMD_QUERY_CAPS:
        if (!is_v2 || data_length != 0)
            return ZZUSB_STATUS_BADPARAM;
        break;
    case ZZUSB_CMD_RESET_PORT:
    case ZZUSB_CMD_RESUME_PORT:
    case ZZUSB_CMD_SUSPEND_PORT:
    case ZZUSB_CMD_ENUMERATE:
    case ZZUSB_CMD_QUERY_DEVICE:
    case ZZUSB_CMD_SET_ADDRESS:
    case ZZUSB_CMD_CLEAR_STALL:
    case ZZUSB_CMD_CHECK_PORT:
    case ZZUSB_CMD_RETIRE_EP:
    case ZZUSB_CMD_CANCEL_EP:
    case ZZUSB_CMD_DIAG_SNAPSHOT:
        break;
    case ZZUSB_CMD_PERIODIC_ARM:
    case ZZUSB_CMD_PERIODIC_REAP:
        if (xfer_type != ZZUSB_XFER_INTERRUPT ||
            endpoint == 0 || max_packet == 0 || max_packet > 1024 ||
            data_length == 0 || data_length > 1024 ||
            be16(&cmd->interval) == 0 ||
            (speed == ZZUSB_SPEED_HIGH &&
             be16(&cmd->interval) > 16))
            return ZZUSB_STATUS_BADPARAM;
        break;
    case ZZUSB_CMD_PERIODIC_STOP:
        if (xfer_type != ZZUSB_XFER_INTERRUPT || endpoint == 0)
            return ZZUSB_STATUS_BADPARAM;
        break;
    case ZZUSB_CMD_ISO_QUEUE:
    case ZZUSB_CMD_ISO_REAP:
        if (!is_v2 || xfer_type != ZZUSB_XFER_ISO ||
            endpoint == 0 || max_packet == 0 ||
            data_length < 32 || data_length > ZZUSB_V2_DATA_MAX ||
            be16(&cmd->interval) == 0 ||
            speed == ZZUSB_SPEED_LOW ||
            (speed == ZZUSB_SPEED_HIGH &&
             (max_packet > 0x13ff || be16(&cmd->interval) > 16 ||
              (flags & ZZUSB_FLAG_SPLIT))) ||
            (speed == ZZUSB_SPEED_FULL &&
             (max_packet > 1023 || be16(&cmd->interval) > 16 ||
              !(flags & ZZUSB_FLAG_SPLIT))))
            return ZZUSB_STATUS_BADPARAM;
        break;
    case ZZUSB_CMD_ISO_STOP:
        if (!is_v2 || xfer_type != ZZUSB_XFER_ISO ||
            endpoint == 0 || data_length != 0)
            return ZZUSB_STATUS_BADPARAM;
        break;
    default:
        return ZZUSB_STATUS_BADPARAM;
    }

    return ZZUSB_STATUS_OK;
}

uint32_t usb_proxy_get_controller_epoch(void);
struct ehci_ctrl *usb_proxy_get_ehci_controller(void);
void usb_proxy_advance_controller_epoch(void);
void usb_proxy_periodic_pump(void);
uint32_t usb_proxy_periodic_queue_state(void);
uint32_t usb_proxy_periodic_schedule_bits(void);
void usb_proxy_refresh_event_irq(void);
void usb_proxy_iso_pump(void);
uint16_t usb_proxy_handle_command(volatile struct ZZUSBCommand *cmd,
                                  volatile struct ZZUSBProtocolExtension *ext,
                                  uint8_t *data_buf, int is_v2);
void usb_proxy_publish_diagnostics(volatile void *aperture,
                                   uint32_t queue_state);
void usb_proxy_note_late_completion(
    volatile struct ZZUSBCommand *cmd,
    volatile struct ZZUSBProtocolExtension *ext, int is_v2);

#endif
