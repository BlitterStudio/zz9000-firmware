<!--
  Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
  SPDX-License-Identifier: GPL-3.0-or-later
-->

# SD-card HDF boot (firmware side)

Companion documentation for the m68k `zzsd.device` driver. See
`zz9000-drivers/sd-boot/README.md` for the Amiga-side picture.

## Role of the firmware

The firmware (on the Zynq ARM core) is the storage backend. It:

1. Mounts the SD card as FAT32 via FatFs (`xilffs`), opens a fixed
   file `/zz9000.hdf`, and treats the file as a flat block device.
   (`sd_storage_init` in `src/sd_storage.c`.)
2. Scans the first 16 blocks of the HDF for an Amiga `RDSK` block,
   walks the partition and filesystem-header lists, caches the
   filesystem segment-list binaries into an internal `fs_buf`, and
   exposes the whole thing as an `sd_boot_info` struct for the Amiga
   driver to consume. (`src/sd_boot.c`.)
3. Serves the m68k driver's register protocol: boot metadata
   (`GETINFO`, `LOADFS`), block I/O (`SDBLK_TX*`/`SDBLK_RX*`/
   `SD_STATUS`). All payload moves through a shared DDR buffer at
   `USB_BLOCK_STORAGE_ADDRESS` that the FPGA maps into the Zorro
   address space at `cardbase+0xA000..0x10000` (24 KB visible).
4. Populates the DiagArea at `BOOT_ROM_ADDRESS` with: the Amiga
   DiagArea header, the m68k "diag code" thunk, and the
   relocatable `zzsd.device` image. The FPGA maps this region into
   the Zorro autoboot window `cardbase+0x6000..0x8000` (8 KB).

## Files touched by the SD-boot feature

```
ZZ9000_proto.sdk/ZZ9000OS/src/
├── bootrom.c        ← DiagArea layout, driver embed, cache flush
├── bootrom.h        ← single source of truth for BOOT_ROM_SIZE
├── diag-code.h      ← m68k boot thunk, generated from boot.S
├── zzsd-device.h    ← m68k driver binary, generated from zzsd.device
├── sd_boot.c        ← RDB/FSHD/LSEG parser, GETINFO, LOADFS
├── sd_boot.h        ← sd_boot_info wire format, dispatched from main.c
├── sd_storage.c     ← FatFs-backed read/write blocks
└── main.c           ← register-write handlers, cache flushes, main loop
```

## Constraints the firmware has to respect

* The FPGA bitstream decodes the autoboot ROM window at
  `0x6000..0x8000` only. `BOOT_ROM_SIZE` in `bootrom.h` must never
  exceed that. Reads past `0x8000` hit `TX_FRAME_ADDRESS` (ethernet
  TX frame) and return zeros — if the driver image spills over, the
  m68k relocator sees garbage at the tail of the HUNK stream.
* The shared buffer at `0xA000..0x10000` is 24 KB. Any chunk the
  firmware loads for the Amiga side has to fit in 24 KB; the driver
  currently uses up to 48-block (24 KB) transfers, filling the
  window exactly.
* The ARM D-cache is not coherent with the FPGA's AXI-HP path. Every
  time firmware writes data that the Amiga will read, we must call
  `Xil_DCacheFlushRange()` over the written span before signalling
  completion. (Done in `sd_storage_read_blocks`, `sd_boot_get_info`,
  `sd_boot_load_fs_chunk`, and `boot_rom_init`.)

## Protocol summary

The register protocol is defined from the driver's perspective in
`zz9000-drivers/sd-boot/zzsd_cmd.h`. Notable conventions:

* `REG_ZZ_SD_BOOT_CMD` (0xC2) is 16-bit packed:
  - bits `[3:0]` = command: `1`=GETINFO, `2..9`=LOADFS for FS index
    `0..7`.
  - bits `[15:4]` = 12-bit chunk index, used for LOADFS (each chunk
    is 16 KB of filesystem binary).
* All register writes produce synchronous ARM work and are acked to
  the Zorro bus immediately. Long operations (`f_read`, `f_write`)
  run in the firmware main loop; the register write just sets a
  `*_pending` flag and stamps `sd_status = SD_STATUS_BUSY`. The
  driver polls `SD_STATUS` until it transitions.
* `f_sync` failures close the HDF (`hdf_open = 0`) so subsequent
  I/O returns `0xFF` and the guest observes a clean error path
  instead of silent corruption.

## Not yet addressed

See `notes/FPGA_PLAN.md` for the roadmap. Highlights:

* **No completion interrupt.** The Amiga busy-waits on `SD_STATUS`
  during every block I/O, which pegs the 68060 and makes the whole
  system feel sluggish during disk activity. Fix requires an FPGA
  change to route a Zorro interrupt line.
* **HDF path is hard-coded.** A config file on the SD card could let
  users pick an alternate image or mount several.
* **Availability flag is latched at boot.** If the SD card is yanked
  post-boot, the firmware still reports the device as present and
  I/O just returns errors; no hot-remount.
