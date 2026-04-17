#include "bootrom.h"
#include "xil_types.h"
#include "memorymap.h"
#include <stdio.h>

#include "zzsd-device.h"
#include "diag-code.h"

// Standard AmigaOS DiagArea header layout:
//   0x00: da_Config (UBYTE)  = DAC_CONFIGTIME|DAC_WORDWIDE
//   0x01: da_Size   (UBYTE)  = size in 512-byte blocks (8KB = 16)
//   0x02: da_DiagPoint (UWORD) = offset to DiagEntry
//   0x04: da_BootPoint (UWORD) = offset to BootEntry
//   0x06: da_Name      (UWORD) = offset to device name string
//   0x08: reserved
//   0x0E: device name string ("zzsd.device\0")
//
// The diag_code[] binary (from boot.S) is placed at offset 0x20:
//   0x20: RomTagCopy (filled by DiagEntry at runtime)
//   0x3C: BootEntry (finds dos.library, inits DOS)
//   0x4E: DiagEntry (relocates driver, copies romtag)
//
// The driver binary (zzsd_device[]) is placed at offset 0x400.

const u16 BOOT_ROM[] = {
  0x9010, 0x004e, 0x0000,
  0x000e,       // da_Name=0x000E
  0x0000, 0x0000, 0x0000,
  // Device name at offset 0x0E: "zzsd.device\0"
  0x7a7a,0x7364,
  0x2e64,0x6576,
  0x6963,0x6500,
};

void boot_rom_init() {
  u8* romp = (u8*)BOOT_ROM;
  u16* ramp = (u16*)BOOT_ROM_ADDRESS;
  u8* ramp8 = (u8*)BOOT_ROM_ADDRESS;

  for (int i=0; i<sizeof(BOOT_ROM); i++) {
    ramp[i] = (romp[i*2]<<8) | (romp[i*2+1]);
  }

  for (int i=0; i<diag_code_len; i++) {
    ramp8[0x20 + i] = diag_code[i];
  }

  for (int i=0; i<zzsd_device_len; i++) {
    ramp8[0x400 + i] = zzsd_device[i];
  }

  printf("boot_rom_init() done.\r\n");
}
