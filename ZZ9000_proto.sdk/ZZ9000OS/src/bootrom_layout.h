/*
 * ZZ9000 autoboot ROM layout shared by the ARM firmware and host-side
 * BOOT.bin packaging tools.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BOOTROM_LAYOUT_H_
#define BOOTROM_LAYOUT_H_

#include "bootrom.h"

#define ZZSD_ROM_SIZE_BYTES  BOOT_ROM_SIZE
#define ZZSD_DRIVER_OFFSET   0x0300u
#define ZZSD_DA_DIAG_POINT   0x0056u
#define ZZSD_DA_BOOT_POINT   0x003Cu
#define ZZSD_DA_NAME         0x000Eu

static const unsigned char ZZSD_BOOT_ROM_HEADER[] = {
	0x90, 0x00,                    // 0x00: da_Config, da_Flags
	(ZZSD_ROM_SIZE_BYTES >> 8) & 0xff, ZZSD_ROM_SIZE_BYTES & 0xff,
	(ZZSD_DA_DIAG_POINT >> 8) & 0xff, ZZSD_DA_DIAG_POINT & 0xff,
	(ZZSD_DA_BOOT_POINT >> 8) & 0xff, ZZSD_DA_BOOT_POINT & 0xff,
	(ZZSD_DA_NAME >> 8) & 0xff, ZZSD_DA_NAME & 0xff,
	0x00, 0x00,                    // 0x0A: reserved
	0x00, 0x00,                    // 0x0C: reserved
	'z','z','s','d','.','d','e','v','i','c','e','\0',
};

#endif
