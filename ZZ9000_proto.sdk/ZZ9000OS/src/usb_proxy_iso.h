/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZUSB_PROXY_ISO_H
#define ZZUSB_PROXY_ISO_H

#include <stdint.h>

struct ZZUSBCommand;

#define ZZUSB_ISO_MAGIC              0x5a49534fU
#define ZZUSB_ISO_VERSION            1U
#define ZZUSB_ISO_HEADER_SIZE        32U
#define ZZUSB_ISO_PACKET_SIZE        16U
#define ZZUSB_ISO_MAX_PACKETS        32U
#define ZZUSB_ISO_MAX_BATCHES        8U
#define ZZUSB_ISO_DATA_MAX           15840U

#define ZZUSB_ISO_FLAG_ASAP          0x0001U
#define ZZUSB_ISO_PACKET_OK         0U
#define ZZUSB_ISO_PACKET_PENDING    1U
#define ZZUSB_ISO_PACKET_SHORT      2U
#define ZZUSB_ISO_PACKET_MISSED     3U
#define ZZUSB_ISO_PACKET_UNDERRUN   4U
#define ZZUSB_ISO_PACKET_OVERRUN    5U
#define ZZUSB_ISO_PACKET_CANCELLED  6U
#define ZZUSB_ISO_PACKET_OFFLINE    7U
#define ZZUSB_ISO_PACKET_XACT       8U
#define ZZUSB_ISO_PACKET_BABBLE     9U


#define ZZUSB_ISO_HDR_OFF_MAGIC      0U
#define ZZUSB_ISO_HDR_OFF_VERSION    4U
#define ZZUSB_ISO_HDR_OFF_FLAGS      6U
#define ZZUSB_ISO_HDR_OFF_BATCH_ID   8U
#define ZZUSB_ISO_HDR_OFF_START      12U
#define ZZUSB_ISO_HDR_OFF_COUNT      14U
#define ZZUSB_ISO_HDR_OFF_DATA_LEN   16U
#define ZZUSB_ISO_HDR_OFF_START_UFRAME 20U

#define ZZUSB_ISO_PKT_OFF_REQUESTED  0U
#define ZZUSB_ISO_PKT_OFF_ACTUAL     2U
#define ZZUSB_ISO_PKT_OFF_STATUS     4U
#define ZZUSB_ISO_PKT_OFF_FRAME      6U
#define ZZUSB_ISO_PKT_OFF_DATA       8U
#define ZZUSB_ISO_PKT_OFF_UFRAME     12U

uint16_t usb_proxy_iso_handle_queue(volatile struct ZZUSBCommand *cmd,
                                    uint8_t *wire);
uint16_t usb_proxy_iso_handle_reap(volatile struct ZZUSBCommand *cmd,
                                   uint8_t *wire);
uint16_t usb_proxy_iso_handle_stop(volatile struct ZZUSBCommand *cmd);
void usb_proxy_iso_pump(void);
int usb_proxy_iso_stop_all(uint8_t unfinished_status);
void usb_proxy_iso_after_controller_reset(void);
uint32_t usb_proxy_iso_queue_state(void);
uint32_t usb_proxy_iso_schedule_bits(void);

#endif
