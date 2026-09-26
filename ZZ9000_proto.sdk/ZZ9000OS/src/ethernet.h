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
#define ETH_CONFIG_HASH_SET            0x8000
#define ETH_CONFIG_HASH_CLEAR          0x4000
#define ETH_CONFIG_HASH_RESET          0x2000
#define ETH_CONFIG_HASH_INDEX          0x003f

enum {
	ETH_TASK_SETUP,
	ETH_TASK_NEGOTIATE,
	ETH_TASK_INIT,
	ETH_TASK_READY
};

extern int ethernet_task_state;

int ethernet_init();
void ethernet_set_multicast_hash(u16 command);
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

#define RXBD_CNT       32	/* Number of RxBDs to use */
#define TXBD_CNT       2	/* Number of TxBDs to use */

#endif
