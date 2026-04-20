/*
 * ZZ9000 autoboot ROM — shared definitions.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BOOTROM_H_
#define BOOTROM_H_

/* Size of the boot ROM image populated by boot_rom_init() and served
 * by the Zorro read/write handlers. Must stay in sync with the FPGA
 * bitstream, which maps 0x6000..(0x6000 + BOOT_ROM_SIZE) to the
 * BOOT_ROM_ADDRESS DDR region. Reading past this window returns bytes
 * the firmware never populated (ethernet TX frame in the current
 * bitstream), so the software read path must clamp accordingly. */
#define BOOT_ROM_SIZE  0x2000u  /* 8 KB */

void boot_rom_init();

#endif
