[![CI](https://github.com/BlitterStudio/zz9000-firmware/actions/workflows/build.yml/badge.svg)](https://github.com/BlitterStudio/zz9000-firmware/actions/workflows/build.yml)

# ZZ9000 Firmware

ZZ9000 Firmware is the FPGA logic and bare-metal ARM software for the MNT
ZZ9000 Zorro II/III graphics and coprocessor card. It builds the boot image
for the Zynq-7020 and implements the card's Zorro interface, graphics output,
native-video capture, and firmware-side services.

Use matching Amiga-side drivers and tools from
[BlitterStudio/zz9000-drivers](https://github.com/BlitterStudio/zz9000-drivers).
The [`sdk/`](sdk/) directory contains the SDK, its API documentation, client
sources, and host tests.

## What's different from the original MNT firmware

- Full-rate native capture and centered 1080p output profiles, with matching-tool calibration and live geometry support.
- Picasso96 video overlay support and custom progressive modelines.
- ZZ9000AX boot-time audio scenes, mixer baseline settings, and clean-level ceilings.
- USB proxy protocol v2 with control, bulk, persistent-interrupt, and selected ISO transport paths.
- Firmware-file updates from AmigaOS through the matching driver tools.
- SDK media, image, archive, audio, and AmiSSL-provider components.
- Release packages for Zorro III, AGA video-slot, Zorro II, and ZZ9500CX adapter variants.

## Install firmware

Download a release ZIP from [GitHub Releases](https://github.com/BlitterStudio/zz9000-firmware/releases)
that matches the target board variant. Each package contains `BOOT.bin` and a
commented [`ZZ9000.CFG`](ZZ9000.CFG) sample.

| Variant | Target |
|---|---|
| `zorro3` | Zorro III A3000/A4000 systems using E7M capture |
| `zorro3-aga` | A4000/A4000T AGA systems using video-slot capture |
| `zorro2` | A2000 or other Zorro II system with a 4 MB window |
| `zorro2-2mb` | A2000 or other Zorro II system with a 2 MB window |
| `a500` | A500 with a ZZ9500CX Denise adapter and a 4 MB window |
| `a500-2mb` | A500 with a ZZ9500CX Denise adapter and a 2 MB window |
| `a500plus` | A500+ or Super Denise with a ZZ9500CX Denise adapter |

1. Copy the package's `BOOT.bin` to the FAT32 microSD card as required by the
   card's boot setup.
2. To change boot settings, copy and edit `ZZ9000.CFG` in the same directory.
3. Power-cycle the Amiga.

The release packages use one `ZZ9000OS` ARM image with the bitstream selected
for the board variant.

## Configure the card

`ZZ9000.CFG` is optional. The firmware reads it from the microSD root beside
`BOOT.bin`. Read the [configuration and compatibility guide](docs/configuration.md)
for every accepted key, its values and limits, native-video profiles, P96
options, audio scenes, USB behavior, and Amiga MMU/cache settings.

`fast_ram` applies only when the selected hardware supports Zorro III Fast
RAM. Its decision is fail-closed: a missing, malformed, unreadable, or `off`
configuration does not advertise Fast RAM.

Other focused implementation and protocol documents are in [`docs/`](docs/).
The SDK material in [`sdk/docs/`](sdk/docs/) covers SDK-facing formats,
services, and AmigaOS packaging.

## Build and contribute

Read [BUILD.md](BUILD.md) before building. It lists the supported toolchains,
the firmware-only and HDL flows, dependency builders, release packaging, and
commands for the relevant test suites.

Firmware-only work uses the committed bitstream inputs in `bootimage_work/`.
HDL work requires Vivado 2018.3 and an updated bitstream; GitHub Actions does
not run Vivado. The workflow in [`.github/workflows/build.yml`](.github/workflows/build.yml)
builds the firmware, checks the firmware/SDK mailbox ABI, packages releases,
and runs host regression suites under [`test/`](test/).

| Area | Start here |
|---|---|
| ARM firmware | [`ZZ9000_proto.sdk/ZZ9000OS/src/`](ZZ9000_proto.sdk/ZZ9000OS/src/) |
| First-stage bootloader | [`ZZ9000_proto.sdk/ZZ9000FSBL/src/`](ZZ9000_proto.sdk/ZZ9000FSBL/src/) |
| Zorro interface and capture RTL | [`mntzorro.v`](mntzorro.v) |
| Display formatter RTL | [`video_formatter.v`](video_formatter.v) |
| FPGA constraints | [`ZZ9000_proto.srcs/constrs_1/new/zz9000.xdc`](ZZ9000_proto.srcs/constrs_1/new/zz9000.xdc) |
| Vivado project source | [`zz9000_project.tcl`](zz9000_project.tcl) |
| SDK | [`sdk/`](sdk/) |
| Tests | [`test/`](test/) |

## Hardware and status

The board uses a Xilinx Zynq-7020 with FPGA fabric, dual Cortex-A9 cores, and
1 GB DDR3. It provides DVI through a Silicon Image 9022 encoder, gigabit
Ethernet through a Micrel KSZ9031 PHY, a FAT32 microSD interface, and USB 2.0
host. MNT publishes the [hardware manual and schematics](https://mntre.com/media/ZZ9000_info_md/zz9000-manual.pdf).

`centered_1080p_match` remains experimental. Native-pixel testing on an A4000
with a default-Z3 ZZ9000 reported correct display for all tested interlace
modes and no visual anomalies. That observation does not establish monitor
compatibility or tear-free operation on other systems.

## History and license

This independent BlitterStudio fork continues the original MNT ZZ9000 firmware
sources. It is not affiliated with, endorsed by, or supported by MNT Research
GmbH. Hardware support questions belong with MNT; firmware issues for this
fork belong in this repository. Original upstream:
<https://source.mnt.re/amiga/zz9000-firmware>.

Original firmware sources are credited to MNT Research GmbH and contributors.
Scanlines V1/V2 are by Xanxi and were adapted for this fork by Dimitris
Panokostas. Per-file copyright notices remain in the source tree.

SPDX-License-Identifier: `GPL-3.0-or-later`. See [LICENSE](LICENSE).
