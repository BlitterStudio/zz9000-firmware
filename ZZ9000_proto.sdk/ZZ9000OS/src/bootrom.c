#include "bootrom.h"
#include "xil_types.h"
#include "ethernet.h"
#include <stdio.h>

// zzusb.device converted to an array
#include "zzusb-device.h"
// boot.rom converted to an array
#include "zzbootrom.h"

const u16 BOOT_ROM[] = {
  0x9000, 0x2000, // WORDWIDE+CONFIGTIME  DAsize
  0x004e, 0x003c, // DiagPt: 0x4e         BootPt: 0x3c
  0x000e, 0x0000, // DevName pointer      Resvd 1
  0x0000,

  // DevName
  0x7a7a,0x7573, // zzusb.device
  0x622e,0x6465,
  0x7669,0x6365,
  0x0000,

  // Boot@0x20
  // Diag@0x20+0x12 = 0x32
};

void boot_rom_init() {
  u8* romp = (u8*)BOOT_ROM;
  u16* ramp = (u16*)BOOT_ROM_ADDRESS;
  u8* ramp8 = (u8*)BOOT_ROM_ADDRESS;

  for (int i=0; i<sizeof(BOOT_ROM); i++) {
    ramp[i] = (romp[i*2]<<8) | (romp[i*2+1]);
  }

  for (int i=0; i<boot_rom_len; i++) {
    // code starts at 0x20
    ramp8[0x20 + i] = boot_rom[i+0x20];
  }

  for (int i=0; i<zzusb_device_len; i++) {
    ramp8[0x400 + i] = zzusb_device[i];
  }

  printf("boot_rom_init() done.\n");
}
