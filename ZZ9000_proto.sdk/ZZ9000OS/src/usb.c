/*
 * MNT ZZ9000 Amiga Graphics and Coprocessor Card Operating System (ZZ9000OS)
 *
 * Copyright (C) 2019, Lukas F. Hartmann <lukas@mntre.com>
 *                     MNT Research GmbH, Berlin
 *                     https://mntre.com
 *
 * More Info: https://mntre.com/zz9000
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 *
 * Some portions (from example code, weird custom license) Copyright (C) 2010 - 2015 Xilinx, Inc.
 *
*/

#include <stdio.h>
#include "platform.h"
#include <xil_printf.h>
#include <xil_cache.h>
#include <xil_mmu.h>
#include "sleep.h"
#include "xparameters.h"
#include <xscugic.h>
#include "usb/ehci.h"
#include "usb/usb.h"
#include "ethernet.h"

struct zynq_ehci_priv _zynq_ehci;

/*
 * https://www.cypress.com/file/134171/download
 * https://elixir.bootlin.com/u-boot/latest/source/common/usb_storage.c#L653
 *
 * "Enumeration directly follows the device detection, and is the process
of assigning a unique address to a newly attached device. Configuration is the process of determining a device‘s
capabilities by an exchange of device requests. The requests that the host uses to learn about a device are called
standard requests and must support these requests on all USB devices."
 */

// returns 1 if storage device available
int zz_usb_init() {
	printf("[USB] trying to probe zynq ehci...\n");
	ehci_zynq_probe(&_zynq_ehci);
	printf("[USB] probed!\n");
	usb_init();
	printf("[USB] initialized!\n");

	if (!usb_stor_scan(1)) {
		return 1;
	}
	return 0;
}

unsigned long zz_usb_read_blocks(int dev_index, unsigned long blknr, unsigned long blkcnt, void *buffer) {
	int res = usb_stor_read_direct(dev_index, blknr, blkcnt, buffer);
	//printf("[USB] RD %lu# at %lu: [%08lx %08lx]\n",blkcnt,blknr,((uint32_t*)buffer)[0],((uint32_t*)buffer)[1]);
	return res;
}

unsigned long zz_usb_write_blocks(int dev_index, unsigned long blknr, unsigned long blkcnt, void *buffer) {
	int res = usb_stor_write_direct(dev_index, blknr, blkcnt, buffer);
	//printf("[USB] WR %lu# at %lu: [%08lx %08lx]\n",blkcnt,blknr,((uint32_t*)buffer)[0],((uint32_t*)buffer)[1]);
	return res;
}

unsigned long zz_usb_storage_capacity(int dev_index) {
	unsigned long cap = usb_stor_get_capacity(dev_index);
	printf("[USB] get capacity: %lx\n",cap);
	return cap;
}
