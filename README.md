[![CI](https://github.com/BlitterStudio/zz9000-firmware/actions/workflows/build.yml/badge.svg)](https://github.com/BlitterStudio/zz9000-firmware/actions/workflows/build.yml)

# ZZ9000 Firmware

FPGA logic and bare-metal ARM firmware for the MNT ZZ9000 Zorro II/III
graphics and coprocessor card.

This repository contains the Zorro bus interface, video formatter,
scanline generator, AXI plumbing, boot image layout, and `ZZ9000OS`
firmware that runs on the Zynq-7020 ARM core. The matching Amiga-side
drivers and tools live in
[BlitterStudio/zz9000-drivers](https://github.com/BlitterStudio/zz9000-drivers).

> **Fork notice:** this is the BlitterStudio firmware fork, maintained by
> Dimitris Panokostas / midwan. It continues the original MNT ZZ9000
> firmware sources, but is not affiliated with, endorsed by, or supported
> by MNT Research GmbH. Hardware support questions belong with MNT;
> firmware issues for this fork belong in this repository.
>
> Original upstream: <https://source.mnt.re/amiga/zz9000-firmware>

## Features

- Zorro II and Zorro III bus interface for RTG, memory windows, registers,
  interrupts, boot ROM, and DMA-style firmware services.
- RTG acceleration paths in `ZZ9000OS` for fills, blits, pattern drawing,
  planar conversion, palette updates, and sprite/video state.
- Scanlines V2 with classic, soft, and gradient patterns plus parity
  control, gated to RTG modes below 350 lines and AGA scandoubled modes.
- SD HDF boot support from the ZZ9000 microSD card.
- USB 2.0 host support through the ARM EHCI stack and Amiga-side USB
  command proxy.
- Gigabit Ethernet support through the Zynq GEM and ZZ9000 shared
  register/mailbox protocol.
- Reproducible firmware and release ZIP builds without Xilinx SDK; Vivado
  2018.3 is only needed when rebuilding FPGA bitstreams.

## Installing Firmware

Tagged releases attach old-style ZIPs that each contain a user-facing
`BOOT.bin`. Release notes and change history live on the
[GitHub Releases](https://github.com/BlitterStudio/zz9000-firmware/releases)
page.

The direct update path is:

1. Download or build the correct release ZIP.
2. Extract its `BOOT.bin`.
3. Copy `BOOT.bin` to the ZZ9000 FAT32 microSD card, using the filename
   required by the card's QSPI/SD boot setup.
4. Power-cycle the Amiga.

Firmware builds with FWUP support can also receive a new `BOOT.bin` from
AmigaOS using `ZZFwUpdate` from
[zz9000-drivers](https://github.com/BlitterStudio/zz9000-drivers). FWUP
writes through the ZZ9000 register window to the mounted FAT32 microSD
card, stages the upload through a temporary file, then replaces the
target on close. If an existing file is replaced, it is kept as a
same-name `.bak` backup, such as `BOOT.bak`.

FWUP accepts flat root-level filenames only: up to 64 characters, simple
ASCII letters/digits plus `.`, `_`, and `-`, with no path separators.
Power-cycle the Amiga after replacing `BOOT.bin`.

## Firmware Variants

Use the ZIP whose variant matches the target machine:

| Variant | Use for |
|---|---|
| `zorro3` | A3000/A4000 with Zorro III FastRAM enabled |
| `zorro3-nofast` | A3000/A4000 without the extra Zorro RAM advertisement |
| `zorro2` | A2000, Zorro II, 4 MB window |
| `zorro2-2mb` | A2000, Zorro II, 2 MB window |
| `a500` | A500 with ZZ9500CX Denise adapter, 4 MB window |
| `a500-2mb` | A500 with ZZ9500CX Denise adapter, 2 MB window |
| `a500plus` | A500+ or Super Denise with ZZ9500CX Denise adapter |

Most users should install the standard flavor. The `ns-pal` flavor is
for PAL Amiga setups that boot without the host driver and need native
~49.92 Hz videocap timing from cold boot. Use the standard flavor on
NTSC machines or when the display does not accept the non-standard PAL
timing.

## Building

See [BUILD.md](BUILD.md) for the complete build flow and toolchain
details. On this repo, the normal firmware/BOOT image loop uses the
committed bitstream in `bootimage_work/` and does not require Vivado:

```bash
export PATH="$PWD/.toolchain/arm-gnu-toolchain/bin:$PATH"
./build_firmware.sh clean
./build_firmware.sh
BOOTGEN="$PWD/.toolchain/bootgen/bootgen" ./build_bootimage.sh
```

The build scripts are intentionally composable:

| Script | Output |
|---|---|
| [`build_firmware.sh`](build_firmware.sh) | `ZZ9000OS.elf` |
| [`build_bootimage.sh`](build_bootimage.sh) | `BOOT.bin` |
| [`build_release_assets.sh`](build_release_assets.sh) | release ZIPs |
| [`build_bitstream.sh`](build_bitstream.sh) | default FPGA bitstream |
| [`build_variant_bitstreams.sh`](build_variant_bitstreams.sh) | release variant bitstreams |

Vivado 2018.3 is required only for bitstream rebuilds. CI does not run
Vivado, so HDL changes must commit the rebuilt bitstream files under
`bootimage_work/`.

## Testing

Firmware builds are covered by the GitHub Actions workflow in
[`.github/workflows/build.yml`](.github/workflows/build.yml). The host
RTG regression harness can be run locally:

```bash
make -C test/rtg test
make -C test/rtg bench
```

Hardware validation is still required before treating performance or bus
timing changes as proven.

## Release Process

Release CI is tag-driven. Push a `v*` tag and the workflow builds the
standard and `ns-pal` firmware flavors, packages every committed variant
bitstream, and publishes a GitHub Release. Tags containing `-`, such as
`v2.1.0-rc1`, are marked as pre-releases.

```bash
git tag -a v2.1.0 -m "Firmware 2.1.0"
git push origin v2.1.0
```

## Repository Layout

| Path | Purpose |
|---|---|
| [`mntzorro.v`](mntzorro.v) | Zorro II/III bus interface, register window, video capture engine, and AXI bridge |
| [`video_formatter.v`](video_formatter.v) | AXI-Stream video formatter and 24-bit RGB output path |
| [`ZZ9000_proto.sdk/ZZ9000OS/src/`](ZZ9000_proto.sdk/ZZ9000OS/src/) | Bare-metal ARM firmware sources |
| [`ZZ9000_proto.sdk/ZZ9000FSBL/src/`](ZZ9000_proto.sdk/ZZ9000FSBL/src/) | First-stage bootloader sources |
| [`ZZ9000_proto.srcs/constrs_1/new/zz9000.xdc`](ZZ9000_proto.srcs/constrs_1/new/zz9000.xdc) | FPGA pin mapping and timing constraints |
| [`zz9000_project.tcl`](zz9000_project.tcl) | Exported Vivado project/block design source |
| [`bootimage_work/`](bootimage_work/) | Committed FSBL and bitstream inputs used by local and CI boot image builds |
| [`test/rtg/`](test/rtg/) | Host-side RTG correctness and benchmark harness |

![ZZ9000 block design](gfx/zz9000-bd.png?raw=true)

## Amiga MMU and Cache Notes

On 68040/68060 systems, configure any pure ZZ9000 RAM window in the
Amiga-side MMU tool. For the optional Zorro III FastRAM range,
`Writethrough` has shown the best tested performance because CPU reads
can still benefit from cache while writes reach the board immediately.
In MuLibs/MMULib terms, this is typically:

```text
For 28014 5 SetCacheMode {base} {size} Valid Writethrough
```

Avoid `CopyBack` for ZZ9000 RAM unless the driver and workload are known
to be cache-coherent. Keep MMIO, register, boot, USB, Ethernet, and other
FPGA/ARM shared windows cache inhibited or data no-cache. If a machine is
unstable with `Writethrough`, fall back to `Data NoCache` /
`CacheInhibit` for the configured Zorro RAM range. Leave instruction
cache enabled. 68030 systems do not need this workaround.

If a 68040/68060 machine remains unstable with Zorro III FastRAM enabled,
use the `zorro3-nofast` firmware variant.

## Hardware

The ZZ9000 is built around a Xilinx Zynq-7020 with FPGA fabric, dual
Cortex-A9 cores, and 1 GB DDR3. Main board interfaces:

- DVI output through the Silicon Image 9022 encoder
- Gigabit Ethernet through the Micrel KSZ9031 PHY
- FAT32 microSD card for firmware images and SD boot
- USB 2.0 host port

The hardware manual and schematics are available from MNT:
<https://mntre.com/media/ZZ9000_info_md/zz9000-manual.pdf>

## Credits

- Original MNT ZZ9000 firmware sources: MNT Research GmbH and upstream
  contributors.
- Scanlines V1/V2: Xanxi, adapted for this fork by Dimitris Panokostas.
- BlitterStudio fork features, including RTG performance work, USB host
  stack integration, SD boot, FWUP, videocap fixes, GCC build scripts,
  CI packaging, and release infrastructure: Dimitris Panokostas.

Per-file copyright notices are preserved in the source tree.

## License

SPDX-License-Identifier: `GPL-3.0-or-later`

See [LICENSE](LICENSE).
