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

## What Changed Since the Original MNT Firmware

The original MNT project established the ZZ9000 hardware and its core Amiga
support. This independent BlitterStudio fork continues from those pre-fork
sources and turns the card into a broader, easier-to-use graphics and
coprocessor platform. These are the highest-value differences for an owner:

| Improvement | What it means in everyday use |
|---|---|
| **Sharper and more compatible native Amiga video** | Full-rate variants preserve the complete 28 MHz Amiga pixel stream, including SuperHires detail. Native video can be shown pixel-exact at 1280x1024 or centered unchanged inside a monitor-friendly 1920x1080 picture with clean black borders. |
| **Live picture positioning** | Current ZZTop and matched firmware can open a calibration screen and move the captured Amiga picture with the cursor keys. You no longer need a long edit, reboot, inspect, and repeat cycle just to center the image. |
| **Faster, more capable RTG output** | The fork adds substantial Picasso96 acceleration and fixes, a 64-bit display path, high-resolution Zorro III modes including 1920x1080x32, monitor power management, improved scanlines, and a hardware-scaled video window. |
| **Video and audio playback on the card** | The two ARM cores can decode MPEG-1 video and MPEG audio for the SDK's **ZZPlay** application, keeping the classic Amiga CPU free for the interface and other work. Playback can use a window on Workbench or a dedicated screen. |
| **Hardware-assisted images, archives, audio and secure networking** | Firmware services accelerate JPEG/PNG work, MP3/audio streaming, LHA/LZH decompression, and selected cryptography used by the accelerated AmiSSL build. Applications fall back safely when a service is unavailable. |
| **Settings without special firmware builds** | One readable `ZZ9000.CFG` file on the microSD card now controls native-video profiles, framing, scanlines, INT2, MAC address, PIP/off-screen features, and the boot HDF. The old separate `ns-pal` firmware flavor is no longer needed. |
| **Updates and recovery from AmigaOS** | `ZZFwUpdate` can install firmware files without removing the microSD card, keeps a `.bak` copy when replacing a file, and can restore that backup if an update boots but misbehaves. |
| **More supported machines and card configurations** | Releases maintain seven FPGA images covering Zorro III, Zorro II, A500/ZZ9500CX, 2 MB, no-Fast-RAM, and Super Denise configurations. |

The less visible work matters too: USB 2.0/Poseidon support, gigabit Ethernet,
SD-card HDF boot, on-board audio improvements, safer Zorro II memory sharing,
and a reproducible GCC/Docker/CI build and release process. FPGA releases are
built and timing-checked for every supported hardware variant.

The firmware, AmigaOS drivers, and SDK applications are designed as one
matched release. This is especially important on Zorro II, where the current
stack safely agrees which parts of the smaller 2 MB or 4 MB address window may
be used. Both shipped Zorro II profiles support compact image, archive, audio,
and secure-network services; the 4 MB profile also has room for one bounded
ZZPlay picture-in-picture source. See the SDK's
[plain-language Zorro II service matrix](https://github.com/BlitterStudio/zz9000-sdk/blob/master/docs/zz9k-zorro2-services.md)
for the exact limits.

## Installing Firmware

Tagged releases attach ZIPs that each contain a user-facing `BOOT.bin`
and sample `ZZ9000.CFG`. Release notes and change history live on the
[GitHub Releases](https://github.com/BlitterStudio/zz9000-firmware/releases)
page.

The direct update path is:

1. Download or build the correct release ZIP for the target hardware.
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

## Board / Bitstream Variants

Use the ZIP whose board/bitstream variant matches the target machine:

| Variant | Use for |
|---|---|
| `zorro3` | A3000/A4000 with Zorro III FastRAM enabled |
| `zorro3-nofast` | A3000/A4000 without the extra Zorro RAM advertisement |
| `zorro2` | A2000, Zorro II, 4 MB window |
| `zorro2-2mb` | A2000, Zorro II, 2 MB window |
| `a500` | A500 with ZZ9500CX Denise adapter, 4 MB window |
| `a500-2mb` | A500 with ZZ9500CX Denise adapter, 2 MB window |
| `a500plus` | A500+ or Super Denise with ZZ9500CX Denise adapter |

These are hardware/autoconfig bitstream variants. Current releases use
one firmware flavor across all of them; settings that used to require a
separate firmware flavor or ENV: variables now live in the optional
[`ZZ9000.CFG`](ZZ9000.CFG) config file (see below). Older releases also
shipped an `ns-pal` firmware flavor; its behavior is now the
`filtered_pal_exact` native-video profile in `ZZ9000.CFG`.

## Configuration File (ZZ9000.CFG)

The firmware reads an optional `ZZ9000.CFG` INI-style text file from
the root of the FAT32 microSD card — next to `BOOT.bin` — once at cold
boot (power-on). Options apply immediately, before the Amiga even
starts booting, so they work without any driver, ENV: variable, or
startup-sequence. An Amiga soft reset does not re-read the file;
power-cycle after editing. Release ZIPs include a fully commented
sample; the copy at the repo root documents every option:

| Key | Values | Replaces |
|---|---|---|
| `videocap_profile` | `full_60` (default), `full_exact`, `filtered_60`, `filtered_pal`, `filtered_pal_exact`, `filtered_ntsc_exact`, `centered_1080p_60` | atomic native-video output policy |
| `videocap_sample` | `average` (default), `even`, `odd` | native-video capture sample selection |
| `videocap_crop_h` | `0`–`4095` (missing = Automatic) | horizontal capture origin in 28 MHz samples |
| `videocap_crop_v` | `0`–`4095` (missing = Automatic) | vertical capture origin in lines |
| `scanline_mode` | `0` (off, default) – `3` | ZZTop/ZZScanlines (still work at runtime) |
| `scanline_parity` | `0` (default), `1` | ZZTop/ZZScanlines |
| `int2` | `off` (default), `on` | `ENV:ZZ9K_INT2` (drivers query it) |
| `offscreen_bitmaps` | `on` (default), `off` | `ENV:ZZ9000-NO-OFFSCREEN`, `OFFSCREENBITMAPS` tooltype |
| `video_overlay` | `on` (default), `off` | `ENV:ZZ9000-NO-PIP`, `VIDEOPIP` tooltype |
| `mac` | `aa:bb:cc:dd:ee:ff` | `ENV:ZZ9K_MAC` |
| `hdf` | root-level image filename | fixed `zz9000.hdf` path |

Unknown keys and invalid values are logged on the debug UART and
skipped. Runtime mechanisms (driver register writes, ZZTop) still
override config values until the next power cycle. For the
driver-consumed options (`int2`, `mac`, `videocap_mode`,
`nonstandard_vsync`, `offscreen_bitmaps`, `video_overlay`), an existing
`ENV:` variable or RTG tooltype takes precedence over the config file —
remove those when migrating.

Automatic framing is resolved independently for each crop axis by the FPGA
capture path. A full-rate bitstream using a full-width native-video profile
uses `280/40`; filtered profiles and Denise-adapter/Super Denise bitstreams use
the historical `188/26`. An explicit numeric key is always a literal Custom
override, including `188`, `26`, `0`, and `4095`.

`centered_1080p_60` preserves the existing 1280x1024 captured content and
places it one-to-one at physical coordinate `(320,28)` in a 1920x1080 active
raster. The content occupies `[320,1600) x [28,1052)`; every pixel outside
that rectangle remains black, including after PIP, sprite, and scanline
composition. This is native Amiga chipset video and is separate from the
Picasso96 1920x1080x32 RTG screen mode.

The centered profile keeps the existing 2200x1125 timing at a 150 MHz pixel
clock. Its `60` name is nominal: the resulting refresh is approximately
60.60606 Hz. It requires a matching viewport-capable, full-rate bitstream,
firmware advertising capability bit 3, and current `ZZ9000.card`/ZZTop. The
full-rate `zorro3`, `zorro3-nofast`, `zorro2`, and `zorro2-2mb` variants are
eligible; filtered Denise/Super Denise variants are not. An old or mixed
firmware/bitstream/driver/tool stack safely resolves the request to
`full_60`/native mode 1 and does not preserve the centered identity.
`full_60` remains the built-in default.

The normal configuration lifecycle is unchanged: the SD-card file is read at
cold boot, ZZTop writes staged settings only when **Save** is selected, and a
power cycle applies that file. Valid runtime native-mode changes are applied
at the next stable vblank. The centered profile does not add a monitor
hot-plug or HDMI mode-switch guarantee.

Firmware 2.8 with the live-control capability and the matching protocol-1
bitstream also expose acknowledged live framing control for ZZTop 2.8. The
firmware capability bitmap is returned at register `0xE6`; older 2.8 firmware
returns zero, keeping mixed RC1/current installations safely disabled. The
FPGA publishes the exact applied raw
Automatic/Custom state, resolved effective H/V values, capture-path signature,
and detected PAL/NTSC standard. A valid live change crosses clock domains as
one coherent word and becomes active only at a capture-frame boundary; the
host does not treat it as applied until the matching sequence is acknowledged.
The rejected status flag is sticky arbitration history, not request identity:
ZZTop also verifies that the coherently read applied raw word exactly matches
its request, and reports a competing-writer conflict instead of false success.
Both the firmware revision and exact bitstream capability are required, so an
old/new mixed installation fails closed and retains ordinary Automatic/manual
Custom editing without offering live calibration.

In ZZTop, **Calibrate** opens a native chipset PAL/NTSC test screen. Arrows move
the visible picture by one unit, Shift+Arrows by 16, Enter returns an explicit
Custom pair to Advanced Video, and Escape restores the exact entry state.
Enter does not write the card: Advanced **Done** stages the pair and the main
Settings **Save** action persists `videocap_crop_h` and `videocap_crop_v`.
Changing to a different applied capture path requires Automatic + Save + cold
boot before calibrating that path. If live acknowledgement stops because
capture frames disappear, ZZTop keeps the calibration/rollback UI open rather
than claiming success; cold boot remains the authoritative recovery to the
last persisted CFG.

ZZTop can edit this file in place from AmigaOS (Project menu →
Settings), writing it back over the FWUP path with a `ZZ9000.bak`
backup. It rewrites the whole file from the keys it knows, so use
**ZZTop 2.7 or newer** for schema-safe editing, and ZZTop 2.8 for live
calibration. Version 2.6 exposes the older independent full-width and
refresh controls, 2.5 predates the full-width and crop controls,
2.4 predates `videocap_sample`, and 2.3 also predates
`offscreen_bitmaps` and `video_overlay`; those older versions would drop
the newer lines. The drivers repo's
`tools/check-cfg-keys.sh` fails if the parser, this table, the sample
`ZZ9000.CFG` and ZZTop's editor ever disagree on either keys or the ordered
`videocap_profile` values.

The older `videocap_mode`, `videocap_shres`, and `nonstandard_vsync` keys
remain accepted for existing cards and hand-written files, but new files should
use one `videocap_profile` so that width, resolution, and refresh cannot
contradict one another.

`yuv_rect` was accepted by firmware 2.4–2.7 but never consumed by
anything, and was removed in 2.8. Its key slot stays reserved so the
numeric ids the drivers query do not shift. A config file that still
contains the line is harmless — it is logged as an unknown key and
skipped.

Amiga-side software can query parsed values through the
`REG_ZZ_CONFIG_KEY` register (`0xE8`): write a key id from
`zz_config.h`, then read back value (`0xE8`) and present flag (`0xEA`).
The raw file contents can be fetched through `REG_ZZ_CONFIG_FILE`
(`0xEC`): write `0` (reset handshake) then `1`, poll status (`0xEC`)
until it leaves `0xFFFF`, then read length (`0xEE`) and the bytes from
the shared buffer at card base + `0xA000`. Writing the file back is the
existing FWUP path (`ZZ9000.CFG` is a flat root-level filename), which
also leaves a `.bak` of the previous version.

Cold-boot scanlines need a bitstream that decodes the
`MNTVF_OP_SCANLINES` video-control op. The committed default and
non-default release variant bitstreams in this repo have been rebuilt
for that support; older bitstreams simply ignore the option.

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
[`.github/workflows/build.yml`](.github/workflows/build.yml), which also
runs every host suite below and checks the mailbox ABI against the SDK.
All of them run locally without hardware:

```bash
make -C test/rtg test          # RTG correctness
make -C test/rtg bench         # host micro-benchmarks (comparative only)
make -C test/video test        # VDMA math + video_formatter invariants
make -C test/video_codec test  # pl_mpeg fixture + exact YUY2 output
make -C test/media test        # media session state and timing
make -C test/palette test      # primary-CLUT shadow + query packing
make -C test/audio test        # audio capture
make -C test/config test       # ZZ9000.CFG parser/loader
make -C test/scheduler test    # dual-core queue, routing, reclaim
make -C test/allocator test    # surface allocator
make -C test/aperture test     # Z2 aperture layout/ack contract
make -C test/fwupdate test     # firmware-file restore
make -C test/sd_activity_led test
```

The video pipeline has its own harness under [`test/video/`](test/video/):
host-side checks (`make -C test/video test`) plus a functional Vivado xsim
testbench that drives `video_formatter.v` with a modeled VDMA stream and
compares every displayed pixel across all color modes, scaling modes and
line widths:

```bash
test/video/run_formatter_sim.sh current   # working-tree formatter
test/video/run_formatter_sim.sh master    # committed baseline
```

Hardware validation is still required before treating performance or bus
timing changes as proven.

## Release Process

Release CI is tag-driven. Push a `v*` tag and the workflow builds the
single standard firmware flavor, packages every committed hardware
variant bitstream (each ZIP includes the sample `ZZ9000.CFG`), and
publishes a GitHub Release. Tags containing `-`, such as
`v2.3.0-rc1`, are marked as pre-releases. Do not add an extra
`ns-pal` release flavor; use `ZZ9000.CFG` for PAL/native-video
defaults and related boot-time settings.

```bash
git tag -a v2.3.0 -m "Firmware 2.3.0"
git push origin v2.3.0
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
| [`test/video/`](test/video/) | Video pipeline tests: host checks + xsim functional testbench for `video_formatter.v` |

![ZZ9000 block design](gfx/zz9000-bd.png?raw=true)

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
- BlitterStudio fork features, including RTG performance work, 64-bit
  scanout, USB host stack integration, SD boot/HDF work, FWUP/RESTORE,
  `ZZ9000.CFG`, videocap fixes, SDK services, GCC build scripts, CI
  packaging, and release infrastructure: Dimitris Panokostas.

Per-file copyright notices are preserved in the source tree.

## License

SPDX-License-Identifier: `GPL-3.0-or-later`

See [LICENSE](LICENSE).
