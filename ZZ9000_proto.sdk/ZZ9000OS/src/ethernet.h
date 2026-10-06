/*
 * MNT ZZ9000 Amiga Graphics and Coprocessor Card Operating System (ZZ9000OS)
 *
 * Copyright (C) 2019-2026, Lucie L. Hartmann <lucie@mntre.com>
 *                          MNT Research GmbH, Berlin
 *                          https://mntre.com
 *
 * More Info: https://mntre.com/zz9000
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 *
*/

#ifndef ETHERNET_H_
#define ETHERNET_H_

#define ETH_CONFIG_CAP_MULTICAST_HASH 0x0001
/* Read-side: firmware reports Ethernet link readiness in bit 8. */
#define ETH_CONFIG_CAP_LINK_STATE     0x0002
/* Set once PHY auto-negotiation has completed and the EMAC is running. */
#define ETH_CONFIG_LINK_READY         0x0100
/* Read: ETH_CONFIG_RX_OFFSET2 below is understood. */
#define ETH_CONFIG_CAP_RX_OFFSET2     0x0004
#define ETH_CONFIG_HASH_SET            0x8000
#define ETH_CONFIG_HASH_CLEAR          0x4000
#define ETH_CONFIG_HASH_RESET          0x2000
#define ETH_CONFIG_HASH_INDEX          0x003f
/* Write 0x1000|on: place received frames 2 bytes further into the slot
 * (GEM RX buffer offset), so the payload behind the 14-byte Ethernet header
 * starts on a longword in the window.  Off after every Amiga reset; frames
 * written that way carry ETH_RX_LEN_OFFSET2 in their length word. */
#define ETH_CONFIG_RX_OFFSET2          0x1000
#define ETH_RX_LEN_OFFSET2             0x8000

enum {
	ETH_TASK_SETUP,
	ETH_TASK_NEGOTIATE,
	ETH_TASK_INIT,
	ETH_TASK_READY
};

extern int ethernet_task_state;
extern int ethernet_hw_ready;

int ethernet_init();
void ethernet_set_multicast_hash(u16 command);
void ethernet_set_rx_offset2(int on);
void ethernet_set_rx_offset2_quiet(int on);
u16 ethernet_get_multicast_config(void);
u32 ethernet_emac_base(void);
u32 ethernet_mac_lo_word(const uint8_t mac[6]);
u16 ethernet_zorro16(u32 word, u32 zaddr);
u16 ethernet_send_frame(u16 frame_size);
int ethernet_receive_frame(u16 acked_serial);
u32 get_frames_received();
uint8_t* ethernet_get_mac_address_ptr();
void ethernet_update_mac_address();
uint8_t* ethernet_current_receive_ptr();
int ethernet_get_backlog();
u16 ethernet_get_rx_status();
u16 ethernet_get_rx_stats();
void ethernet_task();
void ethernet_reset_for_amiga();

#define FRAME_MAX_BACKLOG 128

/*
 * Receive descriptors armed at once: the frames the GEM lands without the
 * ARM's help.  A sender on the same gigabit switch puts a whole TCP window
 * on the wire back to back, 12 us a frame, and every frame past the armed
 * count is lost in the GEM before anything here can count it: with 32, a
 * 47 KB window lost frames 33, 34 and 35 of every burst (A3000, 25
 * retransmissions in ten seconds).
 */
#define RXBD_CNT       64	/* Number of RxBDs to use */
#define TXBD_CNT       2	/* Number of TxBDs to use */

/*
 * REG_ZZ_ETH_RX_FRAMES: how many frames a sender may put on the wire at once.
 * The descriptors armed for the GEM bound a back-to-back burst (every frame
 * past them is lost in the GEM), and the frames the host may leave queued
 * before the firmware pauses the wire bound a sustained one; the smaller of
 * the two.  A stable build property, unlike the reservation counter in
 * REG_ZZ_ETH_RX_STATUS.  Older firmware reads 0: assume 32.
 */
#define ETH_RX_FRAMES_PRESENT 0x8000u
#define ETH_RX_FRAMES_COUNT   0x7fffu
u16 ethernet_get_rx_frames(void);

#endif
