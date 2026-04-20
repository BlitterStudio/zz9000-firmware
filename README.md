# ZZ9000 FPGA + ZZ9000OS — BlitterStudio fork

> **Fork notice.** This repository is an independent fork and continued
> development of the original MNT ZZ9000 FPGA/firmware sources. It is
> maintained by Dimitris Panokostas / **BlitterStudio** and is **not
> affiliated with, endorsed by, or supported by MNT Research GmbH**.
> The ZZ9000 hardware itself is designed and manufactured by MNT
> Research GmbH — hardware questions belong with them; firmware issues
> and fork-specific discussion belong here.
>
> Upstream (pre-fork): https://source.mnt.re/amiga/zz9000-firmware

The ZZ9000 is a graphics and ARM coprocessor card for Amiga computers
with Zorro II/III slots. It is built around a Xilinx Zynq-7020: 7-series
FPGA fabric next to a dual-core ARM Cortex-A9 at 666 MHz, with 1 GB of
DDR3. This repository holds the FPGA logic (Zorro bus, video formatter,
scanline generator, AXI plumbing) and the small bare-metal firmware
(ZZ9000OS) that runs on the ARM and drives it.

Current firmware revision: **1.14**.

## What this fork adds on top of upstream

- **Scanlines V2** — three patterns (classic / soft / gradient) with
  odd/even parity, gated to AGA scandoubled modes and RTG resolutions
  below 350 lines. V1 intensity scanlines still work. Original patches
  by Xanxi, adapted by midwan.
- **RTG performance** — NEON intrinsics for pixel fill/blit paths,
  batch palette transfer, operation fusion, and tuned compiler flags.
- **USB 2.0 host stack (ARM side)** — EHCI driver plus a USB command
  proxy, with Zynq AXI stability fixes (USBMODE_SDIS, ULPI dynamic XCVR
  switching, BURSTSIZE, TXFIFO threshold) and direct-DMA bulk transfers
  straight from the shared mailbox. Previously unused on the card.
- **SD boot** — HDF-on-FAT storage backend and an autoboot ROM path, so
  the Amiga can boot from an image file on the ZZ9000's microSD.
- **Videocap fixes** — NTSC black-screen on RTG→capture switch, interlace
  detection hardening, big-sprite 2×2 doubling, `double_sprite` /
  `hires_sprite` flag propagation through the Z2 and Z3 sprite paths.
- **GCC 15 toolchain** — standalone Makefile build that no longer needs
  Xilinx's ancient Eclipse SDK. See [BUILD.md](BUILD.md).
- **CI** — GitHub Actions workflow that builds firmware + BOOT.bin on
  every push/PR using the committed bitstream, and publishes a GitHub
  Release with `BOOT.bin` + `ZZ9000OS.elf` attached when a `v*` tag is
  pushed ([.github/workflows/build.yml](.github/workflows/build.yml)).

## Building

See **[BUILD.md](BUILD.md)** for the current, supported build flow
(three composable scripts, same path CI takes):

- [`build_firmware.sh`](build_firmware.sh) — ARM firmware ELF
- [`build_bitstream.sh`](build_bitstream.sh) — FPGA bitstream (needs Vivado 2018.3)
- [`build_bootimage.sh`](build_bootimage.sh) — packages everything into `BOOT.bin`

Firmware-only iteration only needs `arm-none-eabi-gcc` + `bootgen` — no
Vivado, no Xilinx SDK. The committed bitstream in `bootimage_work/` is
the CI source of truth; commit a new one alongside any HDL change.

To (re)generate the Vivado project from scratch:
```
source /path/to/Xilinx/Vivado/2018.3/settings64.sh
cd zz9000-firmware
vivado -mode tcl -source zz9000_project.tcl
```

## Repository layout

The interesting bits:

- [`mntzorro.v`](mntzorro.v) — Zorro II/III bus interface, 24-bit video
  capture engine, AXI4-Lite master into the rest of the design.
- [`video_formatter.v`](video_formatter.v) — AXI-Stream formatter that
  reinterprets a 32-bit word stream as 8-bit palette, 16-bit RGB565, or
  24-bit RGBX, and emits a 24-bit parallel RGB stream with H/V sync.
- [`ZZ9000_proto.sdk/ZZ9000OS/src/`](ZZ9000_proto.sdk/ZZ9000OS/src/) — ZZ9000OS firmware (bare-metal, runs on Cortex-A9 core 0):
  - `main.c` — entrypoint, register dispatch, IRQ wiring
  - `gfx.c`, `dma_rtg.c` — RTG accel (rect, blit, pan, sprites)
  - `ethernet.c` — KSZ9031 driver / framer
  - `usb.c`, `usb_proxy.c` — EHCI host + Amiga-side proxy
  - `sd_boot.c` — HDF-on-FAT SD boot path
- [`ZZ9000_proto.srcs/constrs_1/new/zz9000.xdc`](ZZ9000_proto.srcs/constrs_1/new/zz9000.xdc) — pin/ball mapping and timing constraints.
- [`zz9000_project.tcl`](zz9000_project.tcl) — exported Vivado block design (source of truth for the project).
- [`bootimage_work/`](bootimage_work/) — canonical output directory; holds the committed `FSBL_exec.elf`, bitstream, and generated `BOOT.bin`.

![ZZ9000 Block Design](gfx/zz9000-bd.png?raw=true "ZZ9000 Block Design")

## Flashing

Copy `bootimage_work/BOOT.bin` to the ZZ9000's microSD (rename per your
QSPI/SD boot setup), reseat, power-cycle the Amiga.

## Hardware connectivity

Schematics are in the manual (PDF):
<https://mntre.com/media/ZZ9000_info_md/zz9000-manual.pdf>

- DVI (via non-HDMI-compliant HDMI connector), SiliconImage 9022 encoder
- Gigabit Ethernet, Micrel KSZ9031 PHY
- microSD slot (firmware + SD boot)
- USB 2.0 host port (wired up by this fork)

## License

SPDX-License-Identifier: **GPL-3.0-or-later** —
<https://spdx.org/licenses/GPL-3.0-or-later.html>

Per-file copyright headers are authoritative. Pre-fork copyrights on
the original sources are preserved in-tree and belong to their original
authors — see the [upstream repository][upstream] for the canonical
pre-fork notices. Fork-specific changes are copyright their respective
contributors under the same GPL-3.0-or-later terms.

[upstream]: https://source.mnt.re/amiga/zz9000-firmware

### Fork contributions

- **Scanlines** — V1 intensity scanlines and V2 multi-mode scanlines
  (classic / soft / gradient with parity control) by Xanxi, adapted for
  firmware 1.13+ / 1.14 by Dimitris Panokostas (midwan). V2 leaves the
  V1 intensity registers decoded but unused.
- **Scanlines V2 block-design integration** (TCL wiring between
  mntzorro's scanline outputs and video_formatter) and the split
  firmware / bitstream / bootimage build scripts — Dimitris Panokostas.
- **RTG performance** — NEON intrinsics, compiler flags, batch palette
  transfer, operation fusion — Dimitris Panokostas.
- **USB 2.0 host stack on the ARM side** — EHCI + USB command proxy,
  Zynq AXI stability fixes (USBMODE_SDIS, ULPI dynamic XCVR switching,
  BURSTSIZE, TXFIFO threshold, direct-DMA bulk transfers from the shared
  mailbox) — Dimitris Panokostas.
- **SD boot** (HDF-on-FAT storage backend, autoboot ROM),
  videocap / sprite fixes, GCC 15 build, and CI pipeline —
  Dimitris Panokostas.

## Making the Xilinx Platform Cable work (Linux)

```
sudo apt install fxload
sudo cp -r xilinx-xusb /etc/xilinx-xusb
```

Create `/etc/udev/rules.d/xusbdfwu.rules` with:

```
# version 0003
ATTRS{idVendor}=="03fd", ATTRS{idProduct}=="0008", MODE="666"
SUBSYSTEM=="usb", ACTION=="add", ATTRS{idVendor}=="03fd", ATTRS{idProduct}=="0007", RUN+="/sbin/fxload -v -t fx2 -I /etc/xilinx-xusb/xusbdfwu.hex -D $tempnode"
SUBSYSTEM=="usb", ACTION=="add", ATTRS{idVendor}=="03fd", ATTRS{idProduct}=="0009", RUN+="/sbin/fxload -v -t fx2 -I /etc/xilinx-xusb/xusb_xup.hex -D $tempnode"
SUBSYSTEM=="usb", ACTION=="add", ATTRS{idVendor}=="03fd", ATTRS{idProduct}=="000d", RUN+="/sbin/fxload -v -t fx2 -I /etc/xilinx-xusb/xusb_emb.hex -D $tempnode"
SUBSYSTEM=="usb", ACTION=="add", ATTRS{idVendor}=="03fd", ATTRS{idProduct}=="000f", RUN+="/sbin/fxload -v -t fx2 -I /etc/xilinx-xusb/xusb_xlp.hex -D $tempnode"
SUBSYSTEM=="usb", ACTION=="add", ATTRS{idVendor}=="03fd", ATTRS{idProduct}=="0013", RUN+="/sbin/fxload -v -t fx2 -I /etc/xilinx-xusb/xusb_xp2.hex -D $tempnode"
SUBSYSTEM=="usb", ACTION=="add", ATTRS{idVendor}=="03fd", ATTRS{idProduct}=="0015", RUN+="/sbin/fxload -v -t fx2 -I /etc/xilinx-xusb/xusb_xse.hex -D $tempnode"
```

udev will launch fxload whenever the platform cable is plugged in,
loading those hex firmwares onto it. The cable LED should turn green.
