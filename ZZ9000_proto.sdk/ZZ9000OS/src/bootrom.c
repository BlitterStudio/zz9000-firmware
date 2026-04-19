/*
 * ZZ9000 autoboot DiagArea initializer — lays down the Amiga DiagArea
 * header, the m68k diag-code thunk, and the packed zzsd.device image
 * into BOOT_ROM_ADDRESS so Kickstart's autoconfig pass finds a valid
 * resident on the board.
 *
 * Copyright (C) 2026, MNT Research GmbH
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "bootrom.h"
#include "xil_types.h"
#include "xil_cache.h"
#include "memorymap.h"
#include <stdio.h>
#include <string.h>

#include "zzsd-device.h"
#include "diag-code.h"

// AmigaOS DiagArea header layout (from Amiga Hardware Reference Manual):
//   0x00 UBYTE  da_Config     = DAC_WORDWIDE | DAC_CONFIGTIME  (0x90)
//   0x01 UBYTE  da_Flags      = 0
//   0x02 UWORD  da_Size       = ROM size in bus-width units (words for WORDWIDE)
//   0x04 UWORD  da_DiagPoint  = offset to DiagEntry
//   0x06 UWORD  da_BootPoint  = offset to BootEntry
//   0x08 UWORD  da_Name       = offset to device name string
//   0x0A UWORD  da_Reserved01
//   0x0C UWORD  da_Reserved02
//   0x0E       device name string ("zzsd.device\0")
//
// The diag_code[] binary (assembled from boot.S) is placed at offset 0x20:
//   0x20 RomTagCopy   (patched by DiagEntry at runtime to look like a resident
//                      module pointing at the relocated driver's romtag)
//   0x3C BootEntry    (FindResident("dos.library") + call RT_INIT)
//   0x4E DiagEntry    (relocate driver hunks, patch RomTagCopy)
//
// The driver binary (zzsd_device[]) is placed at offset 0x600.
// (boot.S's `add.l #0x600, a2` and `add.l #0x6600, a0` must match.)
//
// ROM window is 8 KB (boardaddr + 0x6000..0x7FFF). The FPGA bitstream
// maps 0x6000..0x8000 to BOOT_ROM via AXI DMA; 0x8000..0xA000 goes to
// TX_FRAME_ADDRESS (ethernet TX frame), so we cannot widen the ROM
// without re-synthesising the bitstream. The driver+header+diag_code
// must therefore all fit in 8 KB.

/* ROM size is the single source of truth in bootrom.h so the Zorro
 * read/write handlers in main.c and this initializer cannot drift. */
#define ZZSD_ROM_SIZE_BYTES  BOOT_ROM_SIZE
#define ZZSD_DRIVER_OFFSET   0x0300u  // after trimmed diag_code (692 B @ 0x20 ends ~0x2D4)
#define ZZSD_DA_DIAG_POINT   0x004Eu
#define ZZSD_DA_BOOT_POINT   0x003Cu
#define ZZSD_DA_NAME         0x000Eu

static const u8 BOOT_ROM[] = {
  0x90, 0x00,                    // 0x00: da_Config, da_Flags
  (ZZSD_ROM_SIZE_BYTES >> 8) & 0xff, ZZSD_ROM_SIZE_BYTES & 0xff,  // 0x02: da_Size
  (ZZSD_DA_DIAG_POINT >> 8) & 0xff, ZZSD_DA_DIAG_POINT & 0xff,    // 0x04: da_DiagPoint
  (ZZSD_DA_BOOT_POINT >> 8) & 0xff, ZZSD_DA_BOOT_POINT & 0xff,    // 0x06: da_BootPoint
  (ZZSD_DA_NAME >> 8) & 0xff, ZZSD_DA_NAME & 0xff,                // 0x08: da_Name
  0x00, 0x00,                    // 0x0A: reserved
  0x00, 0x00,                    // 0x0C: reserved
  'z','z','s','d','.','d','e','v','i','c','e','\0',  // 0x0E: name
};

void boot_rom_init() {
  u8* ramp8 = (u8*)BOOT_ROM_ADDRESS;

  /* Zero the full ROM window so any bytes we don't explicitly fill
   * (padding between header/diag_code/driver) are deterministic. */
  memset(ramp8, 0, ZZSD_ROM_SIZE_BYTES);

  memcpy(ramp8, BOOT_ROM, sizeof(BOOT_ROM));
  memcpy(ramp8 + 0x20, diag_code, diag_code_len);
  memcpy(ramp8 + ZZSD_DRIVER_OFFSET, zzsd_device, zzsd_device_len);

  /* Push ARM D-cache to DDR so Zorro-bus reads (which bypass the ARM
   * cache) see the freshly copied driver/header bytes. Without this,
   * the FPGA sees stale zeros where the driver should be. */
  Xil_DCacheFlushRange((UINTPTR)ramp8, ZZSD_ROM_SIZE_BYTES);

  printf("boot_rom_init() done. diag_code=%u, driver=%u bytes.\r\n"
         "[zzsd] build tag: z3660-style Zorro driver read + 16-bit cmd mask\r\n",
         diag_code_len, zzsd_device_len);
}
