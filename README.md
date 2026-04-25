[![CI](https://github.com/BlitterStudio/zz9000-firmware/actions/workflows/build.yml/badge.svg)](https://github.com/BlitterStudio/zz9000-firmware/actions/workflows/build.yml)

# ZZ9000 Firmware (FPGA + ZZ9000OS) — BlitterStudio fork

> **Fork notice.** This repository is an independent fork and continued
> development of the original MNT ZZ9000 FPGA/firmware sources. It is
> maintained by Dimitris Panokostas / **BlitterStudio** and is **not
> affiliated with, endorsed by, or supported by MNT Research GmbH**.
> The ZZ9000 hardware itself is designed and manufactured by MNT
> Research GmbH — hardware questions belong with them; firmware issues
> and fork-specific discussion belong in this repo's
> [Issues](https://github.com/BlitterStudio/zz9000-firmware/issues).
>
> Upstream (pre-fork): https://source.mnt.re/amiga/zz9000-firmware

FPGA logic and bare-metal ARM firmware for the MNT ZZ9000 Zorro II/III
graphics and coprocessor card. Built around a Xilinx Zynq-7020 (7-series
FPGA fabric + dual Cortex-A9 at 666 MHz, 1 GB DDR3). This repository
holds the Zorro bus interface, video formatter, scanline generator, AXI
plumbing, and the small `ZZ9000OS` firmware that runs on the ARM and
drives it. Companion AmigaOS drivers live in
[zz9000-drivers](https://github.com/BlitterStudio/zz9000-drivers).

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
- **CI + releases** — GitHub Actions pipeline builds firmware + BOOT.bin
  on every push/PR and publishes tagged GitHub Releases with old-style
  firmware ZIPs containing the user-facing `BOOT.bin`
  ([.github/workflows/build.yml](.github/workflows/build.yml)).

## Repository layout

| Path | What it is |
|------|------------|
| [`mntzorro.v`](mntzorro.v) | Zorro II/III bus interface, 24-bit video capture engine, AXI4-Lite master into the rest of the design. |
| [`video_formatter.v`](video_formatter.v) | AXI-Stream formatter: reinterprets 32-bit word stream as 8-bit palette / RGB565 / RGBX and emits 24-bit parallel RGB with H/V sync. |
| [`ZZ9000_proto.sdk/ZZ9000OS/src/`](ZZ9000_proto.sdk/ZZ9000OS/src/) | ZZ9000OS firmware (Cortex-A9 core 0) — boot, RTG accel, Ethernet, USB host, SD boot, audio. |
| [`ZZ9000_proto.srcs/constrs_1/new/zz9000.xdc`](ZZ9000_proto.srcs/constrs_1/new/zz9000.xdc) | Pin/ball mapping and timing constraints. |
| [`zz9000_project.tcl`](zz9000_project.tcl) | Exported Vivado block design — source of truth for the project. |
| [`bootimage_work/`](bootimage_work/) | Canonical output directory. Holds committed `FSBL_exec.elf`, bitstream, and generated `BOOT.bin`. |

![ZZ9000 Block Design](gfx/zz9000-bd.png?raw=true "ZZ9000 Block Design")

## Building

See **[BUILD.md](BUILD.md)** for the full build flow. Three composable
scripts, same path CI takes:

| Script | Builds | Needs |
|---|---|---|
| [`build_firmware.sh`](build_firmware.sh)   | `ZZ9000OS.elf`                  | `arm-none-eabi-gcc` (Arm GNU Toolchain with newlib), host `cc` |
| [`build_bitstream.sh`](build_bitstream.sh) | `zz9000_ps_wrapper.bit`         | Vivado 2018.3 (Linux)                               |
| [`build_variant_bitstreams.sh`](build_variant_bitstreams.sh) | release variant `.bit` files | Vivado 2018.3 (Linux)                               |
| [`build_bootimage.sh`](build_bootimage.sh) | `BOOT.bin`                      | `bootgen`                                           |

Firmware-only iteration needs `arm-none-eabi-gcc`, a host C compiler,
and `bootgen` — no Vivado, no Xilinx SDK. The committed bitstream in
`bootimage_work/` is CI's source of truth; commit a new one alongside
any HDL change.

## Releases

Pushing a tag matching `v*` (e.g. `v2.0.0`, `v2026.04`) triggers the CI
build and then publishes a GitHub Release with `BOOT-<tag>.bin`,
`ZZ9000OS-<tag>.elf`, and `zz9000-firmware-<tag>-<variant>.zip`
archives attached. Each ZIP contains a `BOOT.bin`, matching the old
release format users copy to the microSD card. Tags containing a `-`
(e.g. `v1.15-rc1`) are marked as pre-releases. Release notes are
generated automatically from commits and merged PRs since the previous
tag.

```bash
git tag -a v2.0.0 -m "Firmware 2.0.0"
git push origin v2.0.0
```

## Flashing

Copy `bootimage_work/BOOT.bin` (or the tagged `BOOT-<tag>.bin` from a
release) to the ZZ9000's microSD — rename per your QSPI/SD boot setup —
reseat, and power-cycle the Amiga.

For release downloads, use the ZIP variant for the machine: `zorro3` for
A3000/A4000, `zorro3-nofast` for A3000/A4000 without Zorro RAM,
`zorro2` or `zorro2-2mb` for A2000, `a500` or `a500-2mb` for A500 with
the ZZ9500CX Denise adapter, and `a500plus` for A500+ / Super Denise.
Build those bitstreams on a Vivado machine with
`./build_variant_bitstreams.sh`, then commit the resulting files under
`bootimage_work/`. Deprecated no-USB-autoboot builds are not published.

## Amiga-side MMU/cache setup

On 68040/68060 systems, configure any pure ZZ9000 RAM window in your
Amiga-side MMU tool. For the optional Zorro III FastRAM range,
`Writethrough` has shown the best performance on tested systems because
CPU reads can still benefit from cache while writes reach the board
immediately. In MuLibs/MMULib terms, this is typically:

```
For 28014 5 SetCacheMode {base} {size} Valid Writethrough
```

Avoid `CopyBack` for ZZ9000 RAM unless you know the driver and workload
are cache-coherent; dirty cache lines can otherwise remain on the CPU
instead of reaching the board when expected. Keep MMIO, register, boot,
USB, Ethernet, and other FPGA/ARM shared windows cache inhibited or data
no-cache. On systems that become unstable with `Writethrough`, fall back
to a `Data NoCache` / `CacheInhibit` mapping for the configured Zorro RAM
range. Leave the instruction cache enabled. 68030 systems do not need
this workaround.

If a 68040/68060 machine remains unstable with Zorro III FastRAM enabled,
use the `zorro3-nofast` firmware variant to disable the extra Zorro RAM
advertisement.

## Hardware connectivity

Schematics are in the manual (PDF):
<https://mntre.com/media/ZZ9000_info_md/zz9000-manual.pdf>

- DVI (via non-HDMI-compliant HDMI connector), SiliconImage 9022 encoder
- Gigabit Ethernet, Micrel KSZ9031 PHY
- microSD slot (firmware + SD boot)
- USB 2.0 host port (wired up by this fork)

## Credits

- **Scanlines** — V1 intensity scanlines and V2 multi-mode scanlines
  (classic / soft / gradient with parity control) by Xanxi, adapted for
  firmware 2.0.0 by Dimitris Panokostas (midwan). V2 leaves the
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
- **SD boot** (HDF-on-FAT storage backend, autoboot ROM), videocap /
  sprite fixes, GCC 15 build, and CI pipeline — Dimitris Panokostas.
- **Upstream firmware sources** (pre-fork) — see the fork notice above.

Per-file copyright notices are preserved in each source file.

## License

SPDX-License-Identifier: **GPL-3.0-or-later**
<https://spdx.org/licenses/GPL-3.0-or-later.html>

## Xilinx Platform Cable setup (Linux)

```bash
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
