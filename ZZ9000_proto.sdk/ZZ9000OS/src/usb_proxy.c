/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ZZ9000 USB Proxy — ARM-side USB host controller command handler
 * for the Poseidon USB stack running on the m68k Amiga.
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

#include <string.h>
#include <sleep.h>
#include <xtime_l.h>
#include "xil_cache.h"
#include "usb_proxy.h"
#include "usb_port_reset_sm.h"
#include "usb/usb.h"
#include "usb_proxy_diag.h"
#include "usb_proxy_iso.h"
#include "usb_proxy_periodic_ring.h"
#include "usb/ehci.h"
#include "usb/ehci-iso.h"
#include "usb/ehci_periodic.h"
#include "interrupt.h"
#include "memorymap.h"

#ifndef ALIGN
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#endif

#define ZZUSB_PROXY_MAX_TIMEOUT_MS 1000

/* PORTSC bits (Zynq/ChipIdea extensions beyond standard EHCI) */
#define PORTSC_PFSC     (1U << 24)  /* Port Force Full Speed Connect */
#define PORTSC_PHCD     (1U << 23)  /* PHY Clock Disable (suspend) */
#define PORTSC_LS_K     (1U << 10)  /* Line status K = low-speed attach */
#define PORTSC_LS_MASK  0x00000C00

static unsigned int toggle_bits[128][2];
static uint8_t dma_buf[24576] __attribute__((aligned(32)));
static int root_port_speed_zz = ZZUSB_SPEED_FULL;
static int root_port_connected;
static uint32_t controller_epoch = 1;
static uint32_t last_request_id = 0;
static uint32_t diag_detail;

static struct zzusb_root_reset root_reset;
static uint32_t root_reset_reg;
static int root_reset_fsls;
static struct zzusb_set_address_recovery set_address_recovery;

/*
 * One control, bulk or CLEAR_STALL transfer is in flight at a time (the
 * async schedule holds one QH). The command returns PENDING and finishes
 * on a later main-loop pass, so the core-0 loop keeps answering Zorro
 * register reads while a device NAKs; the device, setup packet and
 * completion context therefore outlive the handler.
 */
enum proxy_xfer_kind {
    PROXY_XFER_CONTROL,
    PROXY_XFER_BULK,
    PROXY_XFER_CLEAR_STALL
};

static struct proxy_xfer {
    int active;
    enum proxy_xfer_kind kind;
    struct usb_device dev;
    struct usb_device split_hub;
    struct devrequest setup __attribute__((aligned(32)));
    int dev_addr;
    int is_in;
    int data_len;
    uint8_t *data_buf;
    unsigned long target_pipe;
    uint32_t diag_detail;
} proxy_xfer;


static void periodic_stop_all(void);
static void reset_port_abort(void);
static uint16_t diag_complete_command(
    volatile struct ZZUSBCommand *cmd,
    volatile struct ZZUSBProtocolExtension *ext, int is_v2,
    uint16_t result);

uint32_t usb_proxy_get_controller_epoch(void)
{
    return controller_epoch;
}

void usb_proxy_advance_controller_epoch(void)
{
    reset_port_abort();
    zzusb_set_address_recovery_abort(&set_address_recovery);
    /* The request that owns the transfer is stale after the fence. */
    if (proxy_xfer.active) {
        ehci_submit_async_cancel();
        proxy_xfer.active = 0;
    }
    periodic_stop_all();
    usb_proxy_iso_stop_all(EHCI_ISO_PACKET_OFFLINE);
    controller_epoch++;
    if (controller_epoch == 0)
        controller_epoch = 1;
    last_request_id = 0;
}

static int request_id_after(uint32_t request_id, uint32_t previous)
{
    return previous == 0 || (int32_t)(request_id - previous) > 0;
}

static struct ehci_ctrl *get_ehci_ctrl(void)
{
    struct usb_device *root = usb_get_dev_index(0);
    if (!root) return NULL;
    return (struct ehci_ctrl *)root->controller;
}

struct ehci_ctrl *usb_proxy_get_ehci_controller(void)
{
    return get_ehci_ctrl();
}
int usb_proxy_can_stage_payload(void)
{
    return !ehci_controller_needs_recovery();
}


static uint64_t reset_now_us(void)
{
    XTime now;

    XTime_GetTime(&now);
    return zzusb_ticks_to_us((uint64_t)now, COUNTS_PER_SECOND);
}

static uint64_t proxy_now_ticks(void)
{
    XTime now;

    XTime_GetTime(&now);
    return (uint64_t)now;
}

static int zz_speed_to_usb(int zz)
{
    switch (zz) {
    case ZZUSB_SPEED_LOW:  return USB_SPEED_LOW;
    case ZZUSB_SPEED_HIGH: return USB_SPEED_HIGH;
    case ZZUSB_SPEED_FULL: return USB_SPEED_FULL;
    default:               return USB_SPEED_FULL;
    }
}

static int portsc_to_zz_speed(uint32_t reg)
{
    int pspd = PORTSC_PSPD(reg);

    if (pspd == PORTSC_PSPD_HS)
        return ZZUSB_SPEED_HIGH;
    if (pspd == PORTSC_PSPD_LS || (reg & PORTSC_LS_MASK) == PORTSC_LS_K)
        return ZZUSB_SPEED_LOW;
    return ZZUSB_SPEED_FULL;
}

static int read_data_length(volatile struct ZZUSBCommand *cmd,
                            uint32_t *data_len)
{
    *data_len = be32(&cmd->data_length);
    if (*data_len > ZZUSB_MAX_XFER) {
        diag_detail = *data_len;
        put_be32(&cmd->actual_length, 0);
        return 0;
    }
    return 1;
}

static unsigned long read_timeout_ms(volatile struct ZZUSBCommand *cmd)
{
    uint32_t timeout_ms = be32(&cmd->timeout_ms);

    /*
     * The Amiga side is blocked while a proxy command is in flight.
     * Respect explicit request timeouts, but cap them so a dead device
     * cannot hold the single primary mailbox for several seconds.
     */
    if (timeout_ms > ZZUSB_PROXY_MAX_TIMEOUT_MS)
        timeout_ms = ZZUSB_PROXY_MAX_TIMEOUT_MS;

    return timeout_ms;
}

static int read_split(volatile struct ZZUSBCommand *cmd,
                      int *hub_addr, int *hub_port)
{
    uint16_t flags = be16(&cmd->flags);

    *hub_addr = be16(&cmd->split_hub_addr);
    *hub_port = be16(&cmd->split_hub_port);

    if (!(flags & ZZUSB_FLAG_SPLIT))
        return 0;
    if (*hub_addr <= 0 || *hub_addr >= 128)
        return 0;
    if (*hub_port <= 0 || *hub_port > 127)
        return 0;

    return 1;
}

static void drop_direct_root_split(struct ehci_ctrl *ctrl,
                                   const char *tag,
                                   int speed_usb,
                                   struct usb_device **split_hub_ptr,
                                   int *split_hub_addr,
                                   int *split_hub_port)
{
    uint32_t portsc_now;
    (void)tag;

    if (!ctrl || !split_hub_ptr || !*split_hub_ptr)
        return;
    if (speed_usb != USB_SPEED_LOW && speed_usb != USB_SPEED_FULL)
        return;

    portsc_now = ehci_readl(&ctrl->hcor->or_portsc[0]);
    if (PORTSC_PSPD(portsc_now) == PORTSC_PSPD_HS)
        return;

    diag_detail = portsc_now;
    *split_hub_ptr = NULL;
    *split_hub_addr = 0;
    *split_hub_port = 0;
}

static void prep_dev(struct usb_device *dev, int addr, int speed_usb,
                     int maxpkt, int endpoint,
                     struct usb_device *split_hub,
                     int split_hub_addr, int split_hub_port)
{
    struct usb_device *root;

    memset(dev, 0, sizeof(*dev));
    dev->devnum = addr;
    dev->speed = speed_usb;
    /* create_pipe() encodes maxpacketsize in bits 0-6 (max 127).
     * Keep it at EP0 default (64) to avoid overflowing into devnum bits.
     * Per-endpoint maxpkt goes into epmaxpacketin/out[] instead. */
    dev->maxpacketsize = (endpoint == 0) ? maxpkt : 64;
    dev->controller = get_ehci_ctrl();
    dev->status = USB_ST_NOT_PROC;
    root = usb_get_dev_index(0);

    if (root && split_hub && (speed_usb == USB_SPEED_LOW || speed_usb == USB_SPEED_FULL)) {
        memset(split_hub, 0, sizeof(*split_hub));
        split_hub->devnum = split_hub_addr;
        split_hub->speed = USB_SPEED_HIGH;
        split_hub->maxpacketsize = 64;
        split_hub->controller = dev->controller;
        split_hub->parent = root;
        split_hub->portnr = 1;
        dev->parent = split_hub;
        dev->portnr = split_hub_port;
    } else if (root && (speed_usb == USB_SPEED_LOW || speed_usb == USB_SPEED_FULL)) {
        dev->parent = root;
        dev->portnr = 1;
    }
    if (addr >= 0 && addr < 128) {
        dev->toggle[0] = toggle_bits[addr][0];
        dev->toggle[1] = toggle_bits[addr][1];
    }
    dev->epmaxpacketin[0] = (endpoint == 0) ? maxpkt : 64;
    dev->epmaxpacketout[0] = (endpoint == 0) ? maxpkt : 64;
    if (endpoint > 0 && endpoint < 16) {
        dev->epmaxpacketin[endpoint] = maxpkt;
        dev->epmaxpacketout[endpoint] = maxpkt;
    }
}

static void save_toggle(int addr, struct usb_device *dev)
{
    if (addr >= 0 && addr < 128) {
        toggle_bits[addr][0] = dev->toggle[0];
        toggle_bits[addr][1] = dev->toggle[1];
    }
}

static uint16_t usb_status_to_zz(unsigned long status, int direction_in)
{
    if (status & USB_ST_BABBLE_DET)
        return ZZUSB_STATUS_BABBLE;
    if (status & USB_ST_BUF_ERR)
        return direction_in ? ZZUSB_STATUS_OVERRUN :
                              ZZUSB_STATUS_UNDERRUN;
    if (status & USB_ST_TIMEOUT)
        return ZZUSB_STATUS_TIMEOUT;
    if (status & USB_ST_CRC_ERR)
        return ZZUSB_STATUS_CRC;
    if (status & USB_ST_STALLED)
        return ZZUSB_STATUS_STALL;
    if (status & USB_ST_NAK_REC)
        return ZZUSB_STATUS_NAK;
    return ZZUSB_STATUS_HOSTERROR;
}

#define ZZUSB_PERIODIC_ENDPOINTS ZZUSB_PERIODIC_RING_CAPACITY
#define ZZUSB_PERIODIC_PUMP_BUDGET 4U
#define ZZUSB_PERIODIC_STALL_GRACE_TICKS (COUNTS_PER_SECOND / 20U)

struct periodic_key {
    uint32_t epoch;
    uint16_t generation;
    uint16_t address;
    uint16_t endpoint;
    uint16_t direction;
    uint16_t speed;
    uint16_t max_packet;
    uint16_t interval;
    uint16_t hub_address;
    uint16_t hub_port;
    uint16_t flags;
};

struct periodic_endpoint {
    struct periodic_key key;
    struct usb_device dev;
    struct usb_device split_hub;
    struct int_queue *queue;
    struct ehci_periodic_plan plan;
    struct ehci_periodic_plan bandwidth_plan;
    uint64_t interval_ticks;
    uint64_t next_due;
    uint32_t length;
    uint32_t actual;
    uint16_t completion_status;
    uint8_t used;
    uint8_t completion_pending;
    uint8_t needs_rearm;
    uint8_t rearm_requested;
    uint8_t failed;
    uint8_t stall_deferred;
    uint8_t software_polled;
    uint8_t bandwidth_reserved;
    uint8_t reservation_mask;
    uint16_t reservation_bytes;
    uint8_t buffer[1024] __attribute__((aligned(32)));
} __attribute__((aligned(32)));

static struct periodic_endpoint periodic_endpoints[ZZUSB_PERIODIC_ENDPOINTS]
    __attribute__((aligned(32)));
static uint8_t periodic_ready[ZZUSB_PERIODIC_ENDPOINTS];
static uint8_t periodic_ready_head;
static uint8_t periodic_ready_count;
static uint8_t periodic_pump_cursor;

static int periodic_identity_matches(
    const struct periodic_endpoint *endpoint,
    volatile struct ZZUSBCommand *cmd)
{
    return endpoint->used &&
           endpoint->key.epoch == controller_epoch &&
           endpoint->key.generation == be16(&cmd->reserved) &&
           endpoint->key.address == be32(&cmd->dev_addr) &&
           endpoint->key.endpoint == be16(&cmd->endpoint) &&
           endpoint->key.direction == be16(&cmd->direction);
}

static int periodic_keys_equal(const struct periodic_key *left,
                               const struct periodic_key *right)
{
    return memcmp(left, right, sizeof(*left)) == 0;
}

static void periodic_remove_ready(unsigned position)
{
    zzusb_periodic_ring_remove(periodic_ready, &periodic_ready_head,
                               &periodic_ready_count, position);
}


static void periodic_release_bandwidth(struct periodic_endpoint *endpoint)
{
    if (!endpoint->bandwidth_reserved)
        return;
    ehci_periodic_release_bandwidth(
        &endpoint->bandwidth_plan, endpoint->reservation_mask,
        endpoint->reservation_bytes);
    endpoint->bandwidth_reserved = 0;
}
static int periodic_release_slot(unsigned index)
{
    struct periodic_endpoint *endpoint = &periodic_endpoints[index];
    unsigned position;

    if (!endpoint->used)
        return 0;
    if (endpoint->software_polled) {
        if (!ehci_periodic_buffer_reclaimable(
                ehci_controller_needs_recovery())) {
            endpoint->failed = 1;
            return -1;
        }
    } else {
        if (!endpoint->queue)
            return -1;
        if (destroy_int_queue(&endpoint->dev, endpoint->queue) < 0) {
            /* The queue is quarantined until controller reset. Preserve the
             * endpoint-owned DMA buffer and suppress its pump meanwhile. */
            endpoint->queue = NULL;
            endpoint->failed = 1;
            return -1;
        }
        save_toggle(endpoint->key.address, &endpoint->dev);
    }
    for (position = 0; position < periodic_ready_count; position++) {
        if (periodic_ready[(periodic_ready_head + position) %
                           ZZUSB_PERIODIC_ENDPOINTS] == index) {
            periodic_remove_ready(position);
            break;
        }
    }
    periodic_release_bandwidth(endpoint);
    memset(endpoint, 0, sizeof(*endpoint));
    return 0;
}

static void periodic_stop_all(void)
{
    unsigned index;

    for (index = 0; index < ZZUSB_PERIODIC_ENDPOINTS; index++)
        periodic_release_slot(index);
    periodic_ready_head = 0;
    periodic_ready_count = 0;
    amiga_interrupt_clear(AMIGA_INTERRUPT_USB);
}

static void periodic_after_controller_reset(void)
{
    memset(periodic_endpoints, 0, sizeof(periodic_endpoints));
    memset(periodic_ready, 0, sizeof(periodic_ready));
    periodic_ready_head = 0;
    periodic_ready_count = 0;
    periodic_pump_cursor = 0;
    amiga_interrupt_clear(AMIGA_INTERRUPT_USB);
}

static void periodic_queue_completion(unsigned index, uint16_t status,
                                      uint32_t actual)
{
    struct periodic_endpoint *endpoint = &periodic_endpoints[index];

    if (endpoint->completion_pending ||
        !zzusb_periodic_ring_push(periodic_ready, periodic_ready_head,
                                  &periodic_ready_count, (uint8_t)index))
        return;
    endpoint->completion_pending = 1;
    endpoint->completion_status = status;
    endpoint->actual = actual;
    zzusb_diag_high_water(periodic_ready_count);
    amiga_interrupt_set(AMIGA_INTERRUPT_USB);
}

/*
 * A hub change report is the removal fence for stalled split children on the
 * indicated ports. Retire those periodic schedules before exposing the
 * report to Poseidon; otherwise its ensuing AbortIO has to unlink a halted
 * child QH while class and device teardown are already in progress.
 */
static void periodic_quiesce_changed_children(unsigned parent_index,
                                               uint32_t actual)
{
    struct periodic_endpoint *parent = &periodic_endpoints[parent_index];
    unsigned index;

    for (index = 0; index < ZZUSB_PERIODIC_ENDPOINTS; index++) {
        struct periodic_endpoint *child = &periodic_endpoints[index];

        if (!zzusb_periodic_child_on_changed_hub_port(
                (child->key.flags & ZZUSB_FLAG_SPLIT) != 0,
                child->stall_deferred != 0, child->key.hub_address,
                child->key.hub_port, parent->key.address,
                parent->buffer, actual))
            continue;
        /*
         * A failure leaves the endpoint quarantined for controller recovery.
         * The parent report must still reach Poseidon so logical child
         * teardown can proceed.
         */
        (void)periodic_release_slot(index);
    }
}

static uint64_t periodic_now(void)
{
    XTime now;

    XTime_GetTime(&now);
    return (uint64_t)now;
}
static uint16_t periodic_poll_software(struct periodic_endpoint *endpoint,
                                       uint32_t *actual)
{
    unsigned long pipe;
    int result;

    if (endpoint->key.direction & 0x80)
        pipe = usb_rcvintpipe(&endpoint->dev, endpoint->key.endpoint);
    else
        pipe = usb_sndintpipe(&endpoint->dev, endpoint->key.endpoint);
    endpoint->dev.status = 0;
    endpoint->dev.act_len = 0;
    result = submit_int_msg_once(&endpoint->dev, pipe, endpoint->buffer,
                                 (int)endpoint->length);
    save_toggle(endpoint->key.address, &endpoint->dev);
    if (result >= 0 && endpoint->dev.status == 0) {
        *actual = endpoint->dev.act_len;
        return ZZUSB_STATUS_OK;
    }
    *actual = 0;
    return usb_status_to_zz(endpoint->dev.status,
                            endpoint->key.direction & 0x80);
}


void usb_proxy_periodic_pump(void)
{
    uint64_t now = periodic_now();
    unsigned visited;

    for (visited = 0; visited < ZZUSB_PERIODIC_PUMP_BUDGET; visited++) {
        unsigned index = periodic_pump_cursor;
        struct periodic_endpoint *endpoint = &periodic_endpoints[index];
        void *completed;
        uint16_t completion_status;

        periodic_pump_cursor =
            (uint8_t)((periodic_pump_cursor + 1U) %
                      ZZUSB_PERIODIC_ENDPOINTS);
        if (!endpoint->used || endpoint->completion_pending)
            continue;
        if (endpoint->needs_rearm) {
            if (!zzusb_periodic_rearm_ready(
                    endpoint->needs_rearm, endpoint->rearm_requested) ||
                now < endpoint->next_due)
                continue;
            if (!endpoint->software_polled &&
                rearm_int_queue(&endpoint->dev, endpoint->queue) < 0) {
                endpoint->failed = 1;
                periodic_queue_completion(index, ZZUSB_STATUS_HOSTERROR, 0);
                continue;
            }
            endpoint->needs_rearm = 0;
            endpoint->rearm_requested = 0;
        }
        if (endpoint->software_polled) {
            uint16_t status;
            uint32_t actual;

            /*
             * A software-polled endpoint creates and retires a temporary
             * periodic QH. Do not mutate that schedule from inside an active
             * async control/bulk transfer; defer the hub poll to the next
             * pump pass instead.
             */
            if (ehci_async_schedule_active(endpoint->dev.controller))
                continue;
            if (now < endpoint->next_due)
                continue;
            endpoint->next_due = now + endpoint->interval_ticks;
            actual = 0;
            status = periodic_poll_software(endpoint, &actual);
            if (actual > endpoint->length)
                actual = endpoint->length;
            if (status == ZZUSB_STATUS_OK) {
                endpoint->stall_deferred = 0;
                if (endpoint->key.direction == 0 || actual != 0) {
                    if (endpoint->key.direction != 0 && actual != 0)
                        periodic_quiesce_changed_children(index, actual);
                    periodic_queue_completion(index, status, actual);
                }
            } else if (status != ZZUSB_STATUS_NAK) {
                if (zzusb_periodic_defer_split_stall(
                        status == ZZUSB_STATUS_STALL,
                        (endpoint->key.flags & ZZUSB_FLAG_SPLIT) != 0,
                        endpoint->key.direction != 0,
                        endpoint->stall_deferred)) {
                    /*
                     * A disappearing split child can halt its interrupt qTD
                     * before the hub change endpoint is serviced. Defer the
                     * first STALL for one full round-robin pass so Poseidon
                     * can receive the hub report and abort this IOR instead
                     * of entering hid.class's clear-halt retry loop.
                     */
                    endpoint->stall_deferred = 1;
                    endpoint->next_due =
                        now + ZZUSB_PERIODIC_STALL_GRACE_TICKS;
                } else {
                    endpoint->failed = 1;
                    periodic_queue_completion(index, status, 0);
                }
            }
            continue;
        }
        if (!endpoint->queue)
            continue;

        completed = poll_int_queue(&endpoint->dev, endpoint->queue);
        if (!completed)
            continue;
        save_toggle(endpoint->key.address, &endpoint->dev);
        endpoint->next_due = now + endpoint->interval_ticks;
        if (endpoint->dev.status == USB_ST_NAK_REC) {
            /*
             * A missed microframe retires the qTD without attempting the
             * interrupt transaction. Rearm the same host request internally;
             * there is no completion for the Amiga side to reap or rearm.
             */
            if (rearm_int_queue(&endpoint->dev, endpoint->queue) < 0) {
                endpoint->failed = 1;
                periodic_queue_completion(
                    index, ZZUSB_STATUS_HOSTERROR, 0);
            }
            continue;
        }
        if (endpoint->dev.status != 0) {
            completion_status = usb_status_to_zz(
                endpoint->dev.status, endpoint->key.direction & 0x80);
            if (zzusb_periodic_defer_split_stall(
                    completion_status == ZZUSB_STATUS_STALL,
                    (endpoint->key.flags & ZZUSB_FLAG_SPLIT) != 0,
                    endpoint->key.direction != 0,
                    endpoint->stall_deferred)) {
                endpoint->stall_deferred = 1;
                endpoint->next_due =
                    now + ZZUSB_PERIODIC_STALL_GRACE_TICKS;
                endpoint->needs_rearm = 1;
                endpoint->rearm_requested = 1;
            } else {
                endpoint->failed = 1;
                periodic_queue_completion(index, completion_status, 0);
            }
        } else {
            endpoint->stall_deferred = 0;
            if (endpoint->dev.act_len > 0) {
                if (endpoint->key.direction != 0)
                    periodic_quiesce_changed_children(
                        index, endpoint->dev.act_len);
                periodic_queue_completion(index, ZZUSB_STATUS_OK,
                                          endpoint->dev.act_len);
            } else {
                endpoint->needs_rearm = 1;
                endpoint->rearm_requested = 1;
            }
        }
    }
}
uint32_t usb_proxy_periodic_queue_state(void)
{
    unsigned index;
    uint32_t active = 0;

    for (index = 0; index < ZZUSB_PERIODIC_ENDPOINTS; index++)
        if (periodic_endpoints[index].used)
            active++;
    return (active << 16) | ((uint32_t)periodic_ready_count << 8);
}

uint32_t usb_proxy_periodic_schedule_bits(void)
{
    unsigned index;
    uint32_t masks = 0;

    for (index = 0; index < ZZUSB_PERIODIC_ENDPOINTS; index++) {
        if (!periodic_endpoints[index].used)
            continue;
        masks |= periodic_endpoints[index].plan.start_mask;
        masks |= (uint32_t)periodic_endpoints[index].plan.complete_mask << 8;
    }
    return masks;
}

void usb_proxy_refresh_event_irq(void)
{
    if (periodic_ready_count ||
        (usb_proxy_iso_queue_state() & 0xf0000000U))
        amiga_interrupt_set(AMIGA_INTERRUPT_USB);
    else
        amiga_interrupt_clear(AMIGA_INTERRUPT_USB);
}

static uint16_t handle_periodic_arm(volatile struct ZZUSBCommand *cmd,
                                    uint8_t *data_buf)
{
    struct ehci_ctrl *ctrl = get_ehci_ctrl();
    struct periodic_key key;
    struct periodic_endpoint *endpoint = NULL;
    struct usb_device dev;
    struct usb_device split_hub;
    struct usb_device *split_hub_ptr;
    struct ehci_periodic_plan plan;
    unsigned long pipe;
    uint32_t length;
    uint16_t flags;
    int split_hub_addr = 0;
    int split_hub_port = 0;
    int speed_usb;
    int is_in;
    unsigned index;
    unsigned free_index = ZZUSB_PERIODIC_ENDPOINTS;

    if (!ctrl || !read_data_length(cmd, &length) ||
        length == 0 || length > 1024U)
        return ZZUSB_STATUS_BADPARAM;

    flags = be16(&cmd->flags);
    speed_usb = zz_speed_to_usb(be16(&cmd->speed));
    is_in = (be16(&cmd->direction) & 0x80) != 0;
    split_hub_ptr =
        read_split(cmd, &split_hub_addr, &split_hub_port) ?
        &split_hub : NULL;
    drop_direct_root_split(ctrl, "periodic", speed_usb, &split_hub_ptr,
                           &split_hub_addr, &split_hub_port);
    if (!split_hub_ptr)
        flags &= (uint16_t)~(ZZUSB_FLAG_SPLIT | ZZUSB_FLAG_MULTI_TT |
                            ZZUSB_FLAG_TT_THINK_MASK);

    memset(&key, 0, sizeof(key));
    key.epoch = controller_epoch;
    key.generation = be16(&cmd->reserved);
    key.address = (uint16_t)be32(&cmd->dev_addr);
    key.endpoint = be16(&cmd->endpoint);
    key.direction = be16(&cmd->direction);
    key.speed = be16(&cmd->speed);
    key.max_packet = be16(&cmd->max_pkt_size);
    key.interval = be16(&cmd->interval);
    key.hub_address = (uint16_t)split_hub_addr;
    key.hub_port = (uint16_t)split_hub_port;
    key.flags = flags;

    for (index = 0; index < ZZUSB_PERIODIC_ENDPOINTS; index++) {
        if (!periodic_endpoints[index].used) {
            if (free_index == ZZUSB_PERIODIC_ENDPOINTS)
                free_index = index;
            continue;
        }
        if (periodic_keys_equal(&periodic_endpoints[index].key, &key)) {
            if (periodic_endpoints[index].needs_rearm)
                periodic_endpoints[index].rearm_requested = 1;
            if (periodic_endpoints[index].completion_pending)
                amiga_interrupt_set(AMIGA_INTERRUPT_USB);
            put_be32(&cmd->actual_length, 0);
            return ZZUSB_STATUS_OK;
        }
        if (periodic_identity_matches(&periodic_endpoints[index], cmd)) {
            if (periodic_release_slot(index) < 0)
                return ZZUSB_STATUS_HOSTERROR;
            if (free_index == ZZUSB_PERIODIC_ENDPOINTS)
                free_index = index;
        }
    }
    if (free_index == ZZUSB_PERIODIC_ENDPOINTS)
        return ZZUSB_STATUS_BUSY;

    prep_dev(&dev, key.address, speed_usb, key.max_packet, key.endpoint,
             split_hub_ptr, split_hub_addr, split_hub_port);
    dev.tt_think_time =
        (uint8_t)((flags & ZZUSB_FLAG_TT_THINK_MASK) >>
                  ZZUSB_FLAG_TT_THINK_SHIFT);
    dev.tt_multi = (flags & ZZUSB_FLAG_MULTI_TT) != 0;
    if (!ehci_periodic_build_plan(
            dev.speed, key.interval, split_hub_ptr != NULL,
            dev.tt_think_time, dev.tt_multi,
            key.endpoint + key.hub_port, &plan))
        return ZZUSB_STATUS_BADPARAM;

    endpoint = &periodic_endpoints[free_index];
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->key = key;
    endpoint->dev = dev;
    endpoint->plan = plan;
    endpoint->length = length;
    endpoint->interval_ticks =
        ((uint64_t)plan.interval_microframes * COUNTS_PER_SECOND + 7999ULL) /
        8000ULL;
    if (!endpoint->interval_ticks)
        endpoint->interval_ticks = 1;
    endpoint->bandwidth_plan = plan;
    ehci_periodic_prepare_bandwidth_plan(
        dev.speed, &endpoint->bandwidth_plan);
    endpoint->reservation_mask =
        (uint8_t)(plan.start_mask | plan.complete_mask);
    endpoint->reservation_bytes = key.max_packet;
    if (split_hub_ptr && endpoint->reservation_bytes > 188U)
        endpoint->reservation_bytes = 188U;
    if (!ehci_periodic_reserve_bandwidth(
            &endpoint->bandwidth_plan, endpoint->reservation_mask,
            endpoint->reservation_bytes)) {
        memset(endpoint, 0, sizeof(*endpoint));
        return ZZUSB_STATUS_BUSY;
    }
    endpoint->bandwidth_reserved = 1;
    if (split_hub_ptr) {
        endpoint->split_hub = split_hub;
        endpoint->dev.parent = &endpoint->split_hub;
    }
    if (!is_in)
        memcpy(endpoint->buffer, data_buf, length);
    if (ehci_periodic_use_software_poll(dev.speed, &plan)) {
        endpoint->software_polled = 1;
        endpoint->next_due = periodic_now();
        endpoint->used = 1;
        zzusb_diag_count(ZZUSB_DIAG_COUNT_PERIODIC_ARM);
        put_be32(&cmd->actual_length, 0);
        return ZZUSB_STATUS_OK;
    }
    if (is_in)
        pipe = usb_rcvintpipe(&endpoint->dev, key.endpoint);
    else
        pipe = usb_sndintpipe(&endpoint->dev, key.endpoint);
    endpoint->queue = create_int_queue(
        &endpoint->dev, pipe, 1, (int)length, endpoint->buffer,
        key.interval);
    if (!endpoint->queue) {
        if (ehci_controller_needs_recovery()) {
            endpoint->used = 1;
            endpoint->failed = 1;
            return ZZUSB_STATUS_HOSTERROR;
        }
        periodic_release_bandwidth(endpoint);
        memset(endpoint, 0, sizeof(*endpoint));
        return ZZUSB_STATUS_NOMEM;
    }
    endpoint->used = 1;
    zzusb_diag_count(ZZUSB_DIAG_COUNT_PERIODIC_ARM);
    put_be32(&cmd->actual_length, 0);
    return ZZUSB_STATUS_OK;
}

static uint16_t handle_periodic_reap(volatile struct ZZUSBCommand *cmd,
                                     uint8_t *data_buf)
{
    uint32_t capacity = be32(&cmd->data_length);
    unsigned position;

    for (position = 0; position < periodic_ready_count; position++) {
        unsigned index = periodic_ready[
            (periodic_ready_head + position) % ZZUSB_PERIODIC_ENDPOINTS];
        struct periodic_endpoint *endpoint = &periodic_endpoints[index];
        uint16_t status;
        uint32_t actual;

        if (!periodic_identity_matches(endpoint, cmd))
            continue;
        status = endpoint->completion_status;
        actual = endpoint->actual;
        if (status == ZZUSB_STATUS_OK && actual > capacity)
            return ZZUSB_STATUS_BADPARAM;
        if (status == ZZUSB_STATUS_OK && actual)
            memcpy(data_buf, endpoint->buffer, actual);
        put_be32(&cmd->actual_length,
                 status == ZZUSB_STATUS_OK ? actual : 0);
        periodic_remove_ready(position);
        endpoint->completion_pending = 0;
        zzusb_diag_count(ZZUSB_DIAG_COUNT_PERIODIC_REAP);
        if (endpoint->failed || endpoint->key.direction == 0) {
            if (periodic_release_slot(index) < 0)
                status = ZZUSB_STATUS_HOSTERROR;
        } else if (zzusb_periodic_completion_needs_rearm(
                       endpoint->failed,
                       endpoint->key.direction != 0)) {
            /*
             * A hub change bit remains asserted until its class driver
             * clears the port feature. This applies equally to persistent
             * interrupt queues and long-interval software polling: wait for
             * the next Amiga IOR before polling again.
             */
            endpoint->needs_rearm = 1;
            endpoint->rearm_requested = 0;
        }
        if (periodic_ready_count)
            amiga_interrupt_set(AMIGA_INTERRUPT_USB);
        usb_proxy_refresh_event_irq();
        return status;
    }

    put_be32(&cmd->actual_length, 0);
    return ZZUSB_STATUS_NAK;
}

static uint16_t handle_periodic_stop(volatile struct ZZUSBCommand *cmd)
{
    unsigned index;
    int stopped = 0;

    for (index = 0; index < ZZUSB_PERIODIC_ENDPOINTS; index++) {
        if (periodic_identity_matches(&periodic_endpoints[index], cmd)) {
            if (periodic_release_slot(index) < 0)
                return ZZUSB_STATUS_HOSTERROR;
            stopped = 1;
        }
    }
    usb_proxy_refresh_event_irq();
    put_be32(&cmd->actual_length, 0);
    return stopped ? ZZUSB_STATUS_OK : ZZUSB_STATUS_NAK;
}

static uint16_t handle_cancel_ep(volatile struct ZZUSBCommand *cmd)
{
    uint16_t periodic = handle_periodic_stop(cmd);
    uint16_t iso = usb_proxy_iso_handle_stop(cmd);

    if (periodic == ZZUSB_STATUS_HOSTERROR ||
        iso == ZZUSB_STATUS_HOSTERROR)
        return ZZUSB_STATUS_HOSTERROR;
    if (periodic == ZZUSB_STATUS_OK || iso == ZZUSB_STATUS_OK)
        return ZZUSB_STATUS_OK;
    return ZZUSB_STATUS_NAK;
}

static void reset_port_assert(struct ehci_ctrl *ctrl, uint32_t reg_pre)
{
    uint32_t *portsc = (uint32_t *)&ctrl->hcor->or_portsc[0];

    if ((reg_pre & PORTSC_LS_MASK) == PORTSC_LS_K)
        root_reset_fsls = 1;
    ehci_zynq_set_phy_mode(root_reset_fsls);
    usleep(100);

    root_reset_reg = reg_pre & ~EHCI_PS_CLEAR;
    root_reset_reg |= EHCI_PS_PR;
    root_reset_reg &= ~EHCI_PS_PE;
    if (root_reset_fsls)
        root_reset_reg |= PORTSC_PFSC;
    else
        root_reset_reg &= ~PORTSC_PFSC;
    ehci_writel(portsc, root_reset_reg);
}

static void reset_port_abort(void)
{
    struct ehci_ctrl *ctrl;

    if (!zzusb_root_reset_active(&root_reset))
        return;
    ctrl = get_ehci_ctrl();
    if (ctrl && (root_reset_reg & EHCI_PS_PR)) {
        root_reset_reg &= ~EHCI_PS_PR;
        ehci_writel(&ctrl->hcor->or_portsc[0], root_reset_reg);
    }
    if (ctrl)
        ehci_zynq_set_phy_speed(USB_SPEED_HIGH);
    zzusb_root_reset_abort(&root_reset);
}

static uint16_t handle_reset_port(volatile struct ZZUSBCommand *cmd)
{
    struct ehci_ctrl *ctrl = get_ehci_ctrl();
    uint32_t *portsc;
    uint32_t reg_pre;
    enum zzusb_root_reset_action action;

    if (!ctrl)
        return ZZUSB_STATUS_ERROR;
    periodic_stop_all();
    usb_proxy_iso_stop_all(EHCI_ISO_PACKET_CANCELLED);
    if (ehci_controller_needs_recovery()) {
        if (ehci_controller_recover() < 0)
            return ZZUSB_STATUS_HOSTERROR;
        usb_proxy_iso_after_controller_reset();
        periodic_after_controller_reset();
    }
    memset(toggle_bits, 0, sizeof(toggle_bits));
    root_port_connected = 0;

    portsc = (uint32_t *)&ctrl->hcor->or_portsc[0];
    reg_pre = ehci_readl(portsc);
    root_reset_fsls = (be16(&cmd->flags) & ZZUSB_FLAG_RESET_FSLS) ? 1 : 0;
    action = zzusb_root_reset_start(&root_reset, reset_now_us(), reg_pre);
    if (action == ZZUSB_ROOT_RESET_DROP_STALE) {
        root_reset_reg = reg_pre & ~EHCI_PS_CLEAR;
        if (root_reset_fsls)
            root_reset_reg |= PORTSC_PFSC;
        else
            root_reset_reg &= ~PORTSC_PFSC;
        root_reset_reg &= ~EHCI_PS_PR;
        ehci_writel(portsc, root_reset_reg);
    } else {
        reset_port_assert(ctrl, reg_pre);
    }
    /* The PR-clear budget or the 50 ms hold runs from the PORTSC write. */
    zzusb_root_reset_arm(&root_reset, reset_now_us());
    return ZZUSB_STATUS_PENDING;
}

int usb_proxy_reset_port_pending(void)
{
    return zzusb_root_reset_active(&root_reset);
}

int usb_proxy_set_address_recovery_pending(void)
{
    return zzusb_set_address_recovery_active(&set_address_recovery);
}

uint16_t usb_proxy_poll_set_address_recovery(
    volatile struct ZZUSBCommand *cmd,
    volatile struct ZZUSBProtocolExtension *ext, int is_v2)
{
    if (!zzusb_set_address_recovery_active(&set_address_recovery))
        return ZZUSB_STATUS_ERROR;
    if (!zzusb_set_address_recovery_elapsed(
            &set_address_recovery, proxy_now_ticks(), COUNTS_PER_SECOND))
        return ZZUSB_STATUS_PENDING;

    zzusb_set_address_recovery_abort(&set_address_recovery);
    if (is_v2) {
        put_be16(&ext->version, ZZUSB_PROTOCOL_VERSION);
        put_be16(&ext->header_size, ZZUSB_V2_HEADER_SIZE);
        put_be32(&ext->controller_epoch, controller_epoch);
        put_be32(&ext->capabilities, ZZUSB_CAP_BASE);
    }
    return diag_complete_command(cmd, ext, is_v2, ZZUSB_STATUS_OK);
}

uint16_t usb_proxy_poll_reset_port(volatile struct ZZUSBCommand *cmd,
                                   volatile struct ZZUSBProtocolExtension *ext,
                                   int is_v2)
{
    struct ehci_ctrl *ctrl = get_ehci_ctrl();
    uint32_t *portsc;
    uint32_t reg;
    enum zzusb_root_reset_state state;
    enum zzusb_root_reset_action action;
    uint16_t result = ZZUSB_STATUS_PENDING;

    if (!ctrl || !zzusb_root_reset_active(&root_reset))
        return ZZUSB_STATUS_ERROR;
    portsc = (uint32_t *)&ctrl->hcor->or_portsc[0];
    state = root_reset.state;
    reg = ehci_readl(portsc);
    action = zzusb_root_reset_step(&root_reset, reset_now_us(), reg);
    switch (action) {
    case ZZUSB_ROOT_RESET_NONE:
        return ZZUSB_STATUS_PENDING;
    case ZZUSB_ROOT_RESET_ASSERT:
        reset_port_assert(ctrl, reg);
        zzusb_root_reset_arm(&root_reset, reset_now_us());
        return ZZUSB_STATUS_PENDING;
    case ZZUSB_ROOT_RESET_DEASSERT:
        /*
         * Use the value written to assert PR, not a PORTSC reread: sticky
         * ChipIdea bits may have changed while the PHY classified the link.
         */
        root_reset_reg &= ~EHCI_PS_PR;
        ehci_writel(portsc, root_reset_reg);
        zzusb_root_reset_arm(&root_reset, reset_now_us());
        return ZZUSB_STATUS_PENDING;
    case ZZUSB_ROOT_RESET_PORT_READY:
        if (!(reg & EHCI_PS_CS)) {
            root_port_speed_zz = ZZUSB_SPEED_FULL;
            ehci_zynq_set_phy_speed(USB_SPEED_HIGH);
            diag_detail = reg;
            zzusb_root_reset_abort(&root_reset);
            result = ZZUSB_STATUS_ERROR;
            break;
        }
        if (!(reg & EHCI_PS_PE)) {
            diag_detail = reg;
            zzusb_root_reset_abort(&root_reset);
            result = ZZUSB_STATUS_ERROR;
            break;
        }
        root_port_speed_zz = portsc_to_zz_speed(reg);
        ehci_zynq_set_phy_speed(zz_speed_to_usb(root_port_speed_zz));
        usleep(100);
        root_reset.observed_portsc = ehci_readl(portsc);
        diag_detail = root_reset.observed_portsc;
        put_be16(&cmd->speed, root_port_speed_zz);
        /* TRSTRCY runs from the PHY speed switch, as it did after the
         * blocking path's udelay(100). */
        zzusb_root_reset_arm(&root_reset, reset_now_us());
        return ZZUSB_STATUS_PENDING;
    case ZZUSB_ROOT_RESET_COMPLETE:
        memset(toggle_bits, 0, sizeof(toggle_bits));
        root_port_connected = 1;
        put_be16(&cmd->speed, root_port_speed_zz);
        result = ZZUSB_STATUS_OK;
        break;
    case ZZUSB_ROOT_RESET_TIMEOUT:
        diag_detail = root_reset.observed_portsc;
        if (state == ZZUSB_ROOT_RESET_WAIT_STALE_CLEAR) {
            result = ZZUSB_STATUS_HOSTERROR;
        } else {
            root_port_speed_zz =
                portsc_to_zz_speed(root_reset.observed_portsc);
            put_be16(&cmd->speed, root_port_speed_zz);
            result = root_port_speed_zz == ZZUSB_SPEED_LOW ?
                     ZZUSB_STATUS_OFFLINE : ZZUSB_STATUS_ERROR;
        }
        break;
    default:
        break;
    }

    if (result == ZZUSB_STATUS_OK)
        usb_proxy_iso_after_controller_reset();
    usb_proxy_advance_controller_epoch();
    if (is_v2) {
        put_be16(&ext->version, ZZUSB_PROTOCOL_VERSION);
        put_be16(&ext->header_size, ZZUSB_V2_HEADER_SIZE);
        put_be32(&ext->controller_epoch, controller_epoch);
        put_be32(&ext->capabilities, ZZUSB_CAP_BASE);
    }
    return diag_complete_command(cmd, ext, is_v2, result);
}

static uint16_t control_xfer_complete(volatile struct ZZUSBCommand *cmd,
                                      int result)
{
    struct usb_device *dev = &proxy_xfer.dev;
    const struct devrequest *setup = &proxy_xfer.setup;
    int dev_addr = proxy_xfer.dev_addr;

    save_toggle(dev_addr, dev);

    if (result >= 0 && dev->status == 0) {
        if (proxy_xfer.is_in && proxy_xfer.data_len > 0 && dev->act_len > 0) {
            Xil_DCacheInvalidateRange((u32)dma_buf, ALIGN(dev->act_len, 32));
            memcpy(proxy_xfer.data_buf, dma_buf, dev->act_len);
        }
        put_be32(&cmd->actual_length, dev->act_len);

        if (setup->requesttype == 0x00 && setup->request == 0x05) {
            int new_addr = setup->value;
            diag_detail = (uint32_t)new_addr;
            if (new_addr > 0 && new_addr < 128) {
                toggle_bits[new_addr][0] = 0;
                toggle_bits[new_addr][1] = 0;
                if (new_addr != dev_addr) {
                    toggle_bits[dev_addr][0] = 0;
                    toggle_bits[dev_addr][1] = 0;
                }
            }
            zzusb_set_address_recovery_arm(&set_address_recovery,
                                           proxy_now_ticks());
            return ZZUSB_STATUS_PENDING;
        }

        /*
         * CLEAR_FEATURE(ENDPOINT_HALT): bmRequestType=0x02 (standard,
         * endpoint recipient, host-to-device), bRequest=0x01,
         * wValue=0x0000 (ENDPOINT_HALT), wIndex encodes endpoint
         * number in bits 0-3 and direction (IN=1) in bit 7.
         *
         * The device resets its toggle to DATA0 on a successful
         * clear-halt. If we don't mirror that in our per-address
         * toggle cache, the next bulk/interrupt transfer on the
         * endpoint will use the stale toggle value — which looks
         * like a DATA0/DATA1 mismatch and fails. Mass-storage
         * STALL recovery stops working after the first halt.
         */
        if (setup->requesttype == 0x02 && setup->request == 0x01 &&
            setup->value == 0x0000 &&
            dev_addr >= 0 && dev_addr < 128) {
            int clr_ep  = setup->index & 0x0f;
            int clr_dir = (setup->index & 0x80) ? 1 : 0;
            toggle_bits[dev_addr][clr_dir] &= ~(1U << clr_ep);
        }
        return ZZUSB_STATUS_OK;
    }

    diag_detail = (uint32_t)dev->status;
    put_be32(&cmd->actual_length, 0);
    return usb_status_to_zz(dev->status, proxy_xfer.is_in);
}

static uint16_t bulk_xfer_complete(volatile struct ZZUSBCommand *cmd,
                                   int result)
{
    struct usb_device *dev = &proxy_xfer.dev;

    save_toggle(proxy_xfer.dev_addr, dev);

    if (result >= 0 && dev->status == 0) {
        if (proxy_xfer.is_in && proxy_xfer.data_len > 0 && dev->act_len > 0)
            Xil_DCacheInvalidateRange((u32)proxy_xfer.data_buf,
                                      ALIGN(dev->act_len, 32));
        put_be32(&cmd->actual_length, dev->act_len);
        return ZZUSB_STATUS_OK;
    }

    diag_detail = (uint32_t)dev->status;
    put_be32(&cmd->actual_length, 0);
    return usb_status_to_zz(dev->status, proxy_xfer.is_in);
}

static uint16_t clear_stall_complete(int result)
{
    struct usb_device *dev = &proxy_xfer.dev;
    unsigned long target = proxy_xfer.target_pipe;
    int cleared = result >= 0 && dev->status == 0;

    /* The device resets the endpoint's toggle to DATA0 on a clear; persist
     * that so the next bulk/int transfer starts with DATA0. */
    if (cleared)
        usb_settoggle(dev, usb_pipeendpoint(target), usb_pipeout(target), 0);
    save_toggle(proxy_xfer.dev_addr, dev);
    return cleared ? ZZUSB_STATUS_OK : ZZUSB_STATUS_ERROR;
}

static uint16_t proxy_xfer_complete(volatile struct ZZUSBCommand *cmd,
                                    int result)
{
    switch (proxy_xfer.kind) {
    case PROXY_XFER_CONTROL:
        return control_xfer_complete(cmd, result);
    case PROXY_XFER_BULK:
        return bulk_xfer_complete(cmd, result);
    case PROXY_XFER_CLEAR_STALL:
    default:
        return clear_stall_complete(result);
    }
}

/* Starts proxy_xfer on the async schedule; a start failure completes the
 * command at once. */
static uint16_t proxy_xfer_start(volatile struct ZZUSBCommand *cmd,
                                 unsigned long pipe, void *buf, int length,
                                 unsigned long timeout_ms, int idle_bulk_poll)
{
    int result = ehci_submit_async_begin(
        &proxy_xfer.dev, pipe, buf, length,
        proxy_xfer.kind == PROXY_XFER_BULK ? NULL : &proxy_xfer.setup,
        timeout_ms, idle_bulk_poll);

    if (result < 0)
        return proxy_xfer_complete(cmd, result);
    proxy_xfer.active = 1;
    proxy_xfer.diag_detail = diag_detail;
    return ZZUSB_STATUS_PENDING;
}

int usb_proxy_async_xfer_pending(void)
{
    return proxy_xfer.active;
}

static uint16_t handle_control_xfer(volatile struct ZZUSBCommand *cmd,
                                uint8_t *data_buf)
{
    struct ehci_ctrl *ctrl = get_ehci_ctrl();
    if (!ctrl)
        return ZZUSB_STATUS_ERROR;
    if (proxy_xfer.active)
        return ZZUSB_STATUS_HOSTERROR;

    int dev_addr = be32(&cmd->dev_addr);
    int endpoint = be16(&cmd->endpoint);
    int maxpkt = be16(&cmd->max_pkt_size);
    int speed_zz = be16(&cmd->speed);
    int speed_usb = zz_speed_to_usb(speed_zz);
    uint32_t portsc_now = 0;
    uint32_t data_len_u32;
    if (!read_data_length(cmd, &data_len_u32))
        return ZZUSB_STATUS_BADPARAM;
    int data_len = (int)data_len_u32;
    int split_hub_addr = 0;
    int split_hub_port = 0;
    struct usb_device *split_hub_ptr =
        read_split(cmd, &split_hub_addr, &split_hub_port) ?
        &proxy_xfer.split_hub : NULL;
    int direct_root_addr0 = 0;

    /* No per-control-xfer trace in the hot path: the UART is
     * polled-blocking and every line costs milliseconds of
     * firmware time, turning a 1-second enumeration into 10+
     * seconds and a mass-storage mount into minutes. The
     * [usb-proxy] ctrl fail / set_addr / reset prints below
     * still fire for rare events and are enough for triage. */

    if (dev_addr == 0 && endpoint == 0 && !split_hub_ptr) {
        int root_speed_zz;
        int root_pspd;

        portsc_now = ehci_readl(&ctrl->hcor->or_portsc[0]);
        root_pspd = PORTSC_PSPD(portsc_now);
        root_speed_zz = (root_pspd == PORTSC_PSPD_HS)
                        ? ZZUSB_SPEED_HIGH : root_port_speed_zz;
        if (root_pspd != PORTSC_PSPD_HS) {
            if (split_hub_ptr) {
                diag_detail = portsc_now;
                split_hub_ptr = NULL;
                split_hub_addr = 0;
                split_hub_port = 0;
            }
            direct_root_addr0 = 1;
        }
        if (root_speed_zz != speed_zz) {
            diag_detail = portsc_now;
            speed_zz = root_speed_zz;
            speed_usb = zz_speed_to_usb(speed_zz);
            put_be16(&cmd->speed, speed_zz);
        }
    }

    drop_direct_root_split(ctrl, "ctrl", speed_usb,
                           &split_hub_ptr,
                           &split_hub_addr, &split_hub_port);

    if (endpoint == 0 && speed_usb == USB_SPEED_HIGH && maxpkt < 64)
        maxpkt = 64;

    if (direct_root_addr0 &&
        speed_zz == ZZUSB_SPEED_LOW &&
        cmd->setup_bRequestType == 0x80 &&
        cmd->setup_bRequest == 0x06 &&
        le16(&cmd->setup_wValue) == 0x0100) {
        /*
         * The Zynq/ChipIdea EHCI root port does not retire direct
         * low-speed EP0 transactions here, even with the QH and ULPI
         * speed set correctly. Fail fast so the Amiga-side driver can
         * hide the unsupported direct-LS device instead of waiting out
         * every enumeration attempt. LS behind a high-speed hub still
         * uses the split path above.
         */
        diag_detail = portsc_now;
        put_be32(&cmd->actual_length, 0);
        return ZZUSB_STATUS_OFFLINE;
    }

    struct usb_device *dev = &proxy_xfer.dev;
    prep_dev(dev, dev_addr, speed_usb, maxpkt, endpoint,
             split_hub_ptr, split_hub_addr, split_hub_port);

    unsigned long pipe;
    if (cmd->setup_bRequestType & 0x80)
        pipe = usb_rcvctrlpipe(dev, endpoint);
    else
        pipe = usb_sndctrlpipe(dev, endpoint);

    struct devrequest *setup = &proxy_xfer.setup;
    setup->requesttype = cmd->setup_bRequestType;
    setup->request = cmd->setup_bRequest;
    setup->value = le16(&cmd->setup_wValue);
    setup->index = le16(&cmd->setup_wIndex);
    setup->length = le16(&cmd->setup_wLength);

    Xil_DCacheFlushRange((u32)setup, ALIGN(sizeof(*setup), 32));

    int is_in = (cmd->setup_bRequestType & 0x80) != 0;
    void *buf = NULL;

    if (data_len > 0) {
        if (!is_in) {
            memcpy(dma_buf, data_buf, data_len);
            Xil_DCacheFlushRange((u32)dma_buf, ALIGN(data_len, 32));
        } else {
            Xil_DCacheInvalidateRange((u32)dma_buf, ALIGN(data_len, 32));
        }
        buf = dma_buf;
    }

    unsigned long timeout_ms = read_timeout_ms(cmd);
    if (direct_root_addr0 &&
        speed_usb == USB_SPEED_LOW &&
        setup->requesttype == 0x80 &&
        setup->request == 0x06 &&
        setup->value == 0x0100) {
        timeout_ms = 25;
    }

    proxy_xfer.kind = PROXY_XFER_CONTROL;
    proxy_xfer.dev_addr = dev_addr;
    proxy_xfer.is_in = is_in;
    proxy_xfer.data_len = data_len;
    proxy_xfer.data_buf = data_buf;
    return proxy_xfer_start(cmd, pipe, buf, data_len, timeout_ms, 0);
}

static uint16_t handle_bulk_xfer(volatile struct ZZUSBCommand *cmd,
                             uint8_t *data_buf)
{
    struct ehci_ctrl *ctrl = get_ehci_ctrl();
    if (!ctrl)
        return ZZUSB_STATUS_ERROR;
    if (proxy_xfer.active)
        return ZZUSB_STATUS_HOSTERROR;

    int dev_addr = be32(&cmd->dev_addr);
    int endpoint = be16(&cmd->endpoint);
    int maxpkt = be16(&cmd->max_pkt_size);
    int speed_usb = zz_speed_to_usb(be16(&cmd->speed));
    uint32_t data_len_u32;
    if (!read_data_length(cmd, &data_len_u32))
        return ZZUSB_STATUS_BADPARAM;
    int data_len = (int)data_len_u32;
    int is_in = (be16(&cmd->direction) & 0x80);

    /* Per-transfer CBW decode disabled — uncomment for deep
     * mass-storage debugging. Uses volatile to avoid GCC -O2
     * coalescing byte reads into a misaligned LDR that traps. */

    int split_hub_addr = 0;
    int split_hub_port = 0;
    struct usb_device *dev = &proxy_xfer.dev;
    struct usb_device *split_hub_ptr =
        read_split(cmd, &split_hub_addr, &split_hub_port) ?
        &proxy_xfer.split_hub : NULL;
    drop_direct_root_split(ctrl, "bulk", speed_usb,
                           &split_hub_ptr,
                           &split_hub_addr, &split_hub_port);
    prep_dev(dev, dev_addr, speed_usb, maxpkt, endpoint,
             split_hub_ptr, split_hub_addr, split_hub_port);

    unsigned long pipe;
    if (is_in)
        pipe = usb_rcvbulkpipe(dev, endpoint);
    else
        pipe = usb_sndbulkpipe(dev, endpoint);

    /* DMA directly from/to the shared mailbox (USB_BLOCK_STORAGE_ADDRESS
     * + ZZUSB_DATA_OFFSET). The mailbox is 64-byte aligned in DDR and
     * reachable by the EHCI DMA engine, so the dma_buf bounce copy is
     * pure overhead. Skipping it saves one 8KB DDR→DDR memcpy plus one
     * cache op per bulk chunk. Fall back to dma_buf if the caller did
     * not provide a buffer (shouldn't happen for bulk, but defensive). */
    uint8_t *xfer_buf = data_buf ? data_buf : dma_buf;

    if (data_len > 0) {
        if (!is_in) {
            Xil_DCacheFlushRange((u32)xfer_buf, ALIGN(data_len, 32));
        } else {
            Xil_DCacheInvalidateRange((u32)xfer_buf, ALIGN(data_len, 32));
        }
    }

    proxy_xfer.kind = PROXY_XFER_BULK;
    proxy_xfer.dev_addr = dev_addr;
    proxy_xfer.is_in = is_in != 0;
    proxy_xfer.data_len = data_len;
    proxy_xfer.data_buf = xfer_buf;
    return proxy_xfer_start(cmd, pipe, data_len > 0 ? xfer_buf : NULL,
                            data_len, read_timeout_ms(cmd),
                            (be16(&cmd->flags) & ZZUSB_FLAG_BULK_IN_POLL) != 0);
}

static uint16_t handle_int_xfer(volatile struct ZZUSBCommand *cmd,
                            uint8_t *data_buf)
{
    struct ehci_ctrl *ctrl = get_ehci_ctrl();
    if (!ctrl)
        return ZZUSB_STATUS_ERROR;

    int dev_addr = be32(&cmd->dev_addr);
    int endpoint = be16(&cmd->endpoint);
    int maxpkt = be16(&cmd->max_pkt_size);
    int speed_usb = zz_speed_to_usb(be16(&cmd->speed));
    uint32_t data_len_u32;
    if (!read_data_length(cmd, &data_len_u32))
        return ZZUSB_STATUS_BADPARAM;
    int data_len = (int)data_len_u32;
    int is_in = (be16(&cmd->direction) & 0x80);
    uint16_t interval = be16(&cmd->interval);

    /*
     * Legacy INT_XFER carries the USB descriptor's high-speed bInterval
     * exponent. PERIODIC_ARM uses the v2 normalized microframe interval.
     */
    if (speed_usb == USB_SPEED_HIGH) {
        interval = ehci_periodic_normalize_hs_binterval(interval);
        if (!interval)
            return ZZUSB_STATUS_BADPARAM;
    }

    int split_hub_addr = 0;
    int split_hub_port = 0;
    struct usb_device split_hub;
    struct usb_device dev;
    struct usb_device *split_hub_ptr =
        read_split(cmd, &split_hub_addr, &split_hub_port) ? &split_hub : NULL;
    drop_direct_root_split(ctrl, "int", speed_usb,
                           &split_hub_ptr,
                           &split_hub_addr, &split_hub_port);
    prep_dev(&dev, dev_addr, speed_usb, maxpkt, endpoint,
             split_hub_ptr, split_hub_addr, split_hub_port);

    unsigned long pipe;
    if (is_in)
        pipe = usb_rcvintpipe(&dev, endpoint);
    else
        pipe = usb_sndintpipe(&dev, endpoint);

    if (!is_in && data_len > 0) {
        memcpy(dma_buf, data_buf, data_len);
        Xil_DCacheFlushRange((u32)dma_buf, ALIGN(data_len, 32));
    } else if (is_in && data_len > 0) {
        Xil_DCacheInvalidateRange((u32)dma_buf, ALIGN(data_len, 32));
    }

    int result = submit_int_msg(&dev, pipe, data_len > 0 ? dma_buf : NULL,
                                 data_len, interval);

    save_toggle(dev_addr, &dev);

    /*
     * Interrupt IN success path covers two sub-cases:
     *   - qTD retired with data: act_len > 0, copy buffer out.
     *   - poll window elapsed with no data (idle endpoint): act_len
     *     is 0 and status is 0. Reported as ZZUSB_STATUS_OK with
     *     actual_length=0, which Poseidon treats as "no report this
     *     cycle, keep polling" (NOT as endpoint timeout).
     * Only real device/transfer errors (stall, babble, data-buffer)
     * fall through to the error branch.
     */
    if (result >= 0 && dev.status == 0) {
        if (is_in && data_len > 0 && dev.act_len > 0) {
            Xil_DCacheInvalidateRange((u32)dma_buf, ALIGN(dev.act_len, 32));
            memcpy(data_buf, dma_buf, dev.act_len);
        }
        put_be32(&cmd->actual_length, dev.act_len);
        return ZZUSB_STATUS_OK;
    } else {
        put_be32(&cmd->actual_length, 0);
        diag_detail = (uint32_t)dev.status;
        return usb_status_to_zz(dev.status, is_in);
    }
}

static uint16_t handle_clear_stall(volatile struct ZZUSBCommand *cmd)
{
    struct ehci_ctrl *ctrl = get_ehci_ctrl();
    if (!ctrl)
        return ZZUSB_STATUS_ERROR;
    if (proxy_xfer.active)
        return ZZUSB_STATUS_HOSTERROR;

    int dev_addr = be32(&cmd->dev_addr);
    int endpoint = be16(&cmd->endpoint);
    int maxpkt = be16(&cmd->max_pkt_size);
    int speed_usb = zz_speed_to_usb(be16(&cmd->speed));

    int split_hub_addr = 0;
    int split_hub_port = 0;
    struct usb_device *dev = &proxy_xfer.dev;
    struct usb_device *split_hub_ptr =
        read_split(cmd, &split_hub_addr, &split_hub_port) ?
        &proxy_xfer.split_hub : NULL;
    drop_direct_root_split(ctrl, "clear-stall", speed_usb,
                           &split_hub_ptr,
                           &split_hub_addr, &split_hub_port);
    prep_dev(dev, dev_addr, speed_usb, maxpkt, endpoint,
             split_hub_ptr, split_hub_addr, split_hub_port);

    /* Direction convention matches the rest of the driver: bit 7 of
     * `direction` selects IN (0x80) vs OUT (0x00). */
    unsigned long target;
    if (be16(&cmd->direction) & 0x80)
        target = usb_rcvbulkpipe(dev, endpoint);
    else
        target = usb_sndbulkpipe(dev, endpoint);

    /* CLEAR_FEATURE(ENDPOINT_HALT), as usb_clear_halt() sends it. */
    struct devrequest *setup = &proxy_xfer.setup;
    setup->requesttype = USB_RECIP_ENDPOINT;
    setup->request = USB_REQ_CLEAR_FEATURE;
    setup->value = cpu_to_le16(0);
    setup->index = cpu_to_le16(usb_pipeendpoint(target) |
                               (usb_pipein(target) << 7));
    setup->length = cpu_to_le16(0);
    Xil_DCacheFlushRange((u32)setup, ALIGN(sizeof(*setup), 32));

    proxy_xfer.kind = PROXY_XFER_CLEAR_STALL;
    proxy_xfer.dev_addr = dev_addr;
    proxy_xfer.is_in = 0;
    proxy_xfer.data_len = 0;
    proxy_xfer.data_buf = NULL;
    proxy_xfer.target_pipe = target;
    return proxy_xfer_start(cmd, usb_sndctrlpipe(dev, 0), NULL, 0, 0, 0);
}

static uint16_t handle_enumerate(volatile struct ZZUSBCommand *cmd,
                             uint8_t *data_buf)
{
    (void)data_buf;
    put_be32(&cmd->actual_length, 0);
    return ZZUSB_STATUS_OK;
}

static int is_port_connected(void)
{
    struct ehci_ctrl *ctrl = get_ehci_ctrl();
    uint32_t portsc;
    int connected;

    if (!ctrl)
        return 0;
    portsc = ehci_readl(&ctrl->hcor->or_portsc[0]);
    connected = (portsc & EHCI_PS_CS) ? 1 : 0;
    if (!connected && root_port_connected) {
        memset(toggle_bits, 0, sizeof(toggle_bits));
        root_port_connected = 0;
        usb_proxy_advance_controller_epoch();
    } else if (connected && !root_port_connected) {
        memset(toggle_bits, 0, sizeof(toggle_bits));
        root_port_connected = 1;
    }
    return connected;
}

static uint16_t handle_retire_ep(volatile struct ZZUSBCommand *cmd)
{
    uint32_t address = be32(&cmd->dev_addr);
    uint16_t endpoint = be16(&cmd->endpoint);
    uint16_t direction = be16(&cmd->direction);

    if (address > 127 || endpoint > 15)
        return ZZUSB_STATUS_BADPARAM;
    toggle_bits[address][(direction & 0x80) ? 1 : 0] &=
        ~(1U << endpoint);
    return ZZUSB_STATUS_OK;
}

static uint16_t handle_check_port(volatile struct ZZUSBCommand *cmd)
{
    struct ehci_ctrl *ctrl = get_ehci_ctrl();
    if (!ctrl)
        return ZZUSB_STATUS_OFFLINE;

    uint32_t reg = ehci_readl(&ctrl->hcor->or_portsc[0]);

    if (!(reg & EHCI_PS_CS))
        return ZZUSB_STATUS_OFFLINE;

    int zz_speed = portsc_to_zz_speed(reg);
    root_port_speed_zz = zz_speed;

    put_be16(&cmd->speed, zz_speed);
    return ZZUSB_STATUS_OK;
}

static uint32_t diag_timestamp(void)
{
    XTime now;
    XTime_GetTime(&now);
    return (uint32_t)now;
}

static void diag_flush(volatile void *address, size_t length)
{
    Xil_DCacheFlushRange((UINTPTR)address, (u32)length);
}

static uint32_t diag_request_id(
    volatile struct ZZUSBProtocolExtension *ext, int is_v2)
{
    return is_v2 ? be32(&ext->request_id) : 0U;
}

static uint16_t diag_topology(volatile struct ZZUSBCommand *cmd)
{
    return (uint16_t)((be16(&cmd->split_hub_addr) << 8) |
                      (be16(&cmd->split_hub_port) & 0xffU));
}

static void diag_begin_command(
    volatile struct ZZUSBCommand *cmd,
    volatile struct ZZUSBProtocolExtension *ext, int is_v2)
{
    diag_detail = 0;
    zzusb_diag_count(ZZUSB_DIAG_COUNT_REQUEST);
    zzusb_diag_record(ZZUSB_DIAG_EVENT_REQUEST, ZZUSB_STATUS_PENDING,
                      diag_request_id(ext, is_v2), controller_epoch,
                      (uint16_t)be32(&cmd->dev_addr),
                      (uint8_t)be16(&cmd->endpoint),
                      (uint8_t)be16(&cmd->direction),
                      diag_topology(cmd), be16(&cmd->flags),
                      be16(&cmd->cmd), diag_timestamp());
}

static uint16_t diag_complete_command(
    volatile struct ZZUSBCommand *cmd,
    volatile struct ZZUSBProtocolExtension *ext, int is_v2,
    uint16_t result)
{
    uint16_t event_type = ZZUSB_DIAG_EVENT_COMPLETION;
    uint16_t command = be16(&cmd->cmd);

    zzusb_diag_count(ZZUSB_DIAG_COUNT_COMPLETION);
    if (command == ZZUSB_CMD_RESET_PORT) {
        zzusb_diag_count(ZZUSB_DIAG_COUNT_RESET);
        event_type = ZZUSB_DIAG_EVENT_RESET;
    } else if (result == ZZUSB_STATUS_TIMEOUT) {
        zzusb_diag_count(ZZUSB_DIAG_COUNT_TIMEOUT);
        event_type = ZZUSB_DIAG_EVENT_TIMEOUT;
    } else if (result == ZZUSB_STATUS_CANCELLED) {
        zzusb_diag_count(ZZUSB_DIAG_COUNT_CANCELLATION);
        event_type = ZZUSB_DIAG_EVENT_CANCELLATION;
    } else if (result == ZZUSB_STATUS_STALE) {
        zzusb_diag_count(ZZUSB_DIAG_COUNT_STALE);
        event_type = ZZUSB_DIAG_EVENT_STALE;
    } else if (result == ZZUSB_STATUS_HOSTERROR) {
        zzusb_diag_count(ZZUSB_DIAG_COUNT_EHCI_ERROR);
        event_type = ZZUSB_DIAG_EVENT_EHCI_ERROR;
    }

    zzusb_diag_record(event_type, result,
                      diag_request_id(ext, is_v2), controller_epoch,
                      (uint16_t)be32(&cmd->dev_addr),
                      (uint8_t)be16(&cmd->endpoint),
                      (uint8_t)be16(&cmd->direction),
                      diag_topology(cmd), be16(&cmd->flags),
                      diag_detail ? diag_detail : be32(&cmd->actual_length),
                      diag_timestamp());
    return result;
}

void usb_proxy_publish_diagnostics(volatile void *aperture,
                                   uint32_t queue_state)
{
    struct ehci_ctrl *ctrl = get_ehci_ctrl();
    uint32_t schedule_bits = 0;

    if (ctrl) {
        schedule_bits = ehci_readl(&ctrl->hcor->or_usbcmd) & 0x00000031U;
        schedule_bits |=
            (ehci_readl(&ctrl->hcor->or_usbsts) & 0x0000f03fU) << 8;
    }
    queue_state |= usb_proxy_periodic_queue_state();
    queue_state |= usb_proxy_iso_queue_state();
    schedule_bits |= usb_proxy_periodic_schedule_bits() << 16;
    zzusb_diag_publish((volatile uint8_t *)aperture + ZZUSB_DIAG_OFFSET,
                       ZZUSB_DIAG_SIZE, ZZUSB_CAP_BASE, controller_epoch,
                       last_request_id, queue_state, schedule_bits,
                       diag_flush);
}

void usb_proxy_note_late_completion(
    volatile struct ZZUSBCommand *cmd,
    volatile struct ZZUSBProtocolExtension *ext, int is_v2)
{
    zzusb_diag_count(ZZUSB_DIAG_COUNT_LATE_COMPLETION);
    zzusb_diag_record(ZZUSB_DIAG_EVENT_LATE_COMPLETION,
                      be16(&cmd->status), diag_request_id(ext, is_v2),
                      controller_epoch, (uint16_t)be32(&cmd->dev_addr),
                      (uint8_t)be16(&cmd->endpoint),
                      (uint8_t)be16(&cmd->direction),
                      diag_topology(cmd), be16(&cmd->flags),
                      be32(&cmd->actual_length), diag_timestamp());
}

static uint16_t complete_command(volatile struct ZZUSBCommand *cmd,
                                 volatile struct ZZUSBProtocolExtension *ext,
                                 int is_v2, uint16_t result)
{
    if (is_v2) {
        put_be16(&ext->version, ZZUSB_PROTOCOL_VERSION);
        put_be16(&ext->header_size, ZZUSB_V2_HEADER_SIZE);
        put_be32(&ext->controller_epoch, controller_epoch);
        put_be32(&ext->capabilities, ZZUSB_CAP_BASE);
    }
    return diag_complete_command(cmd, ext, is_v2, result);
}

uint16_t usb_proxy_handle_command(volatile struct ZZUSBCommand *cmd,
                                  volatile struct ZZUSBProtocolExtension *ext,
                                  uint8_t *data_buf, int is_v2)
{
    uint16_t command = be16(&cmd->cmd);
    uint16_t result;
    uint16_t validation;

    put_be32(&cmd->actual_length, 0);
    diag_begin_command(cmd, ext, is_v2);
    if (command != ZZUSB_CMD_QUERY_CAPS &&
        ehci_controller_needs_recovery()) {
        zzusb_diag_count(ZZUSB_DIAG_COUNT_RECOVERY);
        zzusb_diag_record(ZZUSB_DIAG_EVENT_RECOVERY, ZZUSB_STATUS_PENDING,
                          diag_request_id(ext, is_v2), controller_epoch,
                          0, 0, 0, 0, 0, command, diag_timestamp());
        periodic_stop_all();
        usb_proxy_iso_stop_all(EHCI_ISO_PACKET_CANCELLED);
        if (ehci_controller_recover() < 0)
            return diag_complete_command(cmd, ext, is_v2,
                                         ZZUSB_STATUS_HOSTERROR);
        usb_proxy_iso_after_controller_reset();
        periodic_after_controller_reset();
        memset(toggle_bits, 0, sizeof(toggle_bits));
        usb_proxy_advance_controller_epoch();
        if (is_v2) {
            put_be16(&ext->version, ZZUSB_PROTOCOL_VERSION);
            put_be16(&ext->header_size, ZZUSB_V2_HEADER_SIZE);
            put_be32(&ext->controller_epoch, controller_epoch);
            put_be32(&ext->capabilities, ZZUSB_CAP_BASE);
        }
        return diag_complete_command(
            cmd, ext, is_v2,
            is_v2 ? ZZUSB_STATUS_STALE : ZZUSB_STATUS_HOSTERROR);
    }

    validation = zzusb_validate_command(cmd, ext, is_v2, controller_epoch);
    if (validation != ZZUSB_STATUS_OK)
        return diag_complete_command(cmd, ext, is_v2, validation);

    if (is_v2) {
        uint32_t request_id = be32(&ext->request_id);

        if (command == ZZUSB_CMD_QUERY_CAPS) {
            last_request_id = request_id;
            put_be16(&ext->version, ZZUSB_PROTOCOL_VERSION);
            put_be16(&ext->header_size, ZZUSB_V2_HEADER_SIZE);
            put_be32(&ext->controller_epoch, controller_epoch);
            put_be32(&ext->capabilities, ZZUSB_CAP_BASE);
            return diag_complete_command(cmd, ext, is_v2,
                                         ZZUSB_STATUS_OK);
        }
        if (!request_id_after(request_id, last_request_id))
            return diag_complete_command(cmd, ext, is_v2,
                                         ZZUSB_STATUS_STALE);
        last_request_id = request_id;
    }

    if (command != ZZUSB_CMD_RESET_PORT &&
        command != ZZUSB_CMD_DIAG_SNAPSHOT &&
        !is_port_connected()) {
        result = ZZUSB_STATUS_OFFLINE;
    } else {
        switch (command) {
        case ZZUSB_CMD_CONTROL_XFER:
            result = handle_control_xfer(cmd, data_buf);
            break;
        case ZZUSB_CMD_BULK_XFER:
            result = handle_bulk_xfer(cmd, data_buf);
            break;
        case ZZUSB_CMD_INT_XFER:
            result = handle_int_xfer(cmd, data_buf);
            break;
        case ZZUSB_CMD_CLEAR_STALL:
            result = handle_clear_stall(cmd);
            break;
        case ZZUSB_CMD_RESET_PORT:
            result = handle_reset_port(cmd);
            if (result != ZZUSB_STATUS_PENDING) {
                if (result == ZZUSB_STATUS_OK)
                    usb_proxy_iso_after_controller_reset();
                usb_proxy_advance_controller_epoch();
            }
            break;
        case ZZUSB_CMD_CHECK_PORT:
            result = handle_check_port(cmd);
            break;
        case ZZUSB_CMD_ENUMERATE:
            result = handle_enumerate(cmd, data_buf);
            break;
        case ZZUSB_CMD_RETIRE_EP:
            result = handle_retire_ep(cmd);
            break;
        case ZZUSB_CMD_CANCEL_EP:
            result = handle_cancel_ep(cmd);
            break;
        case ZZUSB_CMD_PERIODIC_ARM:
            result = handle_periodic_arm(cmd, data_buf);
            break;
        case ZZUSB_CMD_PERIODIC_REAP:
            result = handle_periodic_reap(cmd, data_buf);
            break;
        case ZZUSB_CMD_PERIODIC_STOP:
            result = handle_periodic_stop(cmd);
            break;
        case ZZUSB_CMD_ISO_QUEUE:
            result = usb_proxy_iso_handle_queue(cmd, data_buf);
            break;
        case ZZUSB_CMD_ISO_REAP:
            result = usb_proxy_iso_handle_reap(cmd, data_buf);
            break;
        case ZZUSB_CMD_ISO_STOP:
            result = usb_proxy_iso_handle_stop(cmd);
            break;
        case ZZUSB_CMD_DIAG_SNAPSHOT:
            result = ZZUSB_STATUS_OK;
            break;
        case ZZUSB_CMD_RESUME_PORT:
        case ZZUSB_CMD_SUSPEND_PORT:
        case ZZUSB_CMD_QUERY_DEVICE:
        case ZZUSB_CMD_SET_ADDRESS:
        case ZZUSB_CMD_ISO_XFER:
            result = ZZUSB_STATUS_UNSUPPORTED;
            break;
        default:
            result = ZZUSB_STATUS_BADPARAM;
            break;
        }
    }
    if (result == ZZUSB_STATUS_PENDING)
        return result;
    return complete_command(cmd, ext, is_v2, result);
}

uint16_t usb_proxy_poll_async_xfer(volatile struct ZZUSBCommand *cmd,
                                   volatile struct ZZUSBProtocolExtension *ext,
                                   int is_v2)
{
    uint16_t status;
    int result;

    if (!proxy_xfer.active)
        return ZZUSB_STATUS_ERROR;
    if (!ehci_submit_async_poll(&result))
        return ZZUSB_STATUS_PENDING;
    proxy_xfer.active = 0;
    diag_detail = proxy_xfer.diag_detail;
    status = proxy_xfer_complete(cmd, result);
    /* A successful SET_ADDRESS continues as its recovery delay. */
    if (status == ZZUSB_STATUS_PENDING)
        return status;
    return complete_command(cmd, ext, is_v2, status);
}
