#ifndef USB_PROXY_H
#define USB_PROXY_H

#include <xil_types.h>

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

#define ZZUSB_SPEED_LOW         0
#define ZZUSB_SPEED_FULL        1
#define ZZUSB_SPEED_HIGH        2

#define ZZUSB_CMD_SIZE          48
#define ZZUSB_DATA_OFFSET       64
#define ZZUSB_MAX_XFER          (24576 - ZZUSB_DATA_OFFSET)

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
    uint8_t  reserved[6];
} __attribute__((packed));

static inline uint16_t be16(const volatile uint16_t *p) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    return ((uint16_t)b[0] << 8) | b[1];
}

/* Read a 16-bit value stored in USB little-endian byte order.
 * The m68k Poseidon driver copies setup packet fields (wValue, wIndex,
 * wLength) directly from its IoUsbHW API structure, which stores them
 * in USB-native little-endian format.  Using be16() on these would
 * incorrectly byte-swap them. */
static inline uint16_t le16(const volatile uint16_t *p) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    return ((uint16_t)b[1] << 8) | b[0];
}

static inline uint32_t be32(const volatile uint32_t *p) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}

static inline void put_be16(volatile uint16_t *p, uint16_t v) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    b[0] = (v >> 8) & 0xff;
    b[1] = v & 0xff;
}

static inline void put_be32(volatile uint32_t *p, uint32_t v) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    b[0] = (v >> 24) & 0xff;
    b[1] = (v >> 16) & 0xff;
    b[2] = (v >> 8) & 0xff;
    b[3] = v & 0xff;
}

uint16_t usb_proxy_handle_command(volatile struct ZZUSBCommand *cmd, uint8_t *data_buf);

#endif
