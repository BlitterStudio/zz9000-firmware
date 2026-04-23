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

int ethernet_init();
u16 ethernet_send_frame(u16 frame_size);
int ethernet_receive_frame();
u32 get_frames_received();
uint8_t* ethernet_get_mac_address_ptr();
void ethernet_update_mac_address();
uint8_t* ethernet_current_receive_ptr();
int ethernet_get_backlog();
void ethernet_task();

#define FRAME_MAX_BACKLOG 128

#define RXBD_CNT       1	/* Number of RxBDs to use */
#define TXBD_CNT       2	/* Number of TxBDs to use */

#endif
