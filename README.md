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

`ZZ9000.CFG` is an optional text file stored beside `BOOT.bin` in the root of
the FAT32 microSD card. Firmware reads it once at power-on; a soft reset does
not reload it. The easiest way to manage it is **ZZTop → Project → Settings**.
Release ZIPs also include a fully commented [sample file](ZZ9000.CFG) for
manual editing.

The file controls these boot-time defaults:

| Key | Purpose |
|---|---|
| `videocap_profile` | Native output: `full_60`, `full_exact`, `filtered_60` (default), `filtered_pal`, `filtered_pal_exact`, `filtered_ntsc_exact`, `centered_1080p_60` |
| `videocap_sample` | Native-video capture sampling |
| `videocap_crop_h` | Horizontal picture position; omit for Automatic |
| `videocap_crop_v` | Vertical picture position; omit for Automatic |
| `scanline_mode` | Scanline style, or off |
| `scanline_parity` | Which line is darkened |
| `int2` | Use INT2 instead of INT6 |
| `offscreen_bitmaps` | Enable or disable Picasso96 off-screen bitmaps |
| `video_overlay` | Enable or disable the Picasso96 video window |
| `mac` | Ethernet MAC-address override |
| `hdf` | Root-level HDF image used for SD-card boot |

The audio control plane (ZZ9000AX) adds one group of keys per scene,
the active selection, operator baseline and per-card clean ceilings.
Values are plain decimals; band pairs and prefactor/volume pack two
0-100 fields as `hi*128+lo`, and baseline packs mixer legs as
`paula*256+ax` (0-255 each, 127 = 0 dB). The ceiling keys are measured
clean combined levels (1-4095): firmware weights Paula by
`audio_ceiling_ax/audio_ceiling_paula` and enforces 3/4 of the AX
ceiling. Name keys pack two ASCII characters as `c1*256+c2`.

| Key | Purpose |
|---|---|
| `audio_active` | Audio scene slot (0-7) applied at boot and after every warm reset |
| `audio_baseline` | Operator Paula/AX balance packed paula*256+ax, replacing the old `ZZ9K_MIX_LEVELS` env var |
| `audio_ceiling_paula` | Measured clean Paula-only ceiling (1-4095); pair with `audio_ceiling_ax` |
| `audio_ceiling_ax` | Measured clean AX-only ceiling (1-4095); 3/4 of this is the AX-equivalent enforced boundary |
| `audio_scene0_lpf` | Scene 1 low-pass cutoff in Hz (1-23900; 23900 = DSP default) |
| `audio_scene0_eq01` | Scene 1 EQ bands 0+1 packed b0*128+b1 (each 0-100, 50 = 0 dB) |
| `audio_scene0_eq23` | Scene 1 EQ bands 2+3 packed b2*128+b3 |
| `audio_scene0_eq45` | Scene 1 EQ bands 4+5 packed b4*128+b5 |
| `audio_scene0_eq67` | Scene 1 EQ bands 6+7 packed b6*128+b7 |
| `audio_scene0_eq89` | Scene 1 EQ bands 8+9 packed b8*128+b9 |
| `audio_scene0_out` | Scene 1 prefactor and volume packed pref*128+vol |
| `audio_scene0_pan` | Scene 1 stereo pan (0 = left, 50 = center, 100 = right) |
| `audio_scene0_nm1` | Scene 1 name chunk 1: two ASCII characters packed c1*256+c2 (nm1..nm8 hold up to 16 characters; a zero chunk terminates, trailing chunks may be omitted) |
| `audio_scene0_nm2` | Scene 1 name chunk 2 |
| `audio_scene0_nm3` | Scene 1 name chunk 3 |
| `audio_scene0_nm4` | Scene 1 name chunk 4 |
| `audio_scene0_nm5` | Scene 1 name chunk 5 |
| `audio_scene0_nm6` | Scene 1 name chunk 6 |
| `audio_scene0_nm7` | Scene 1 name chunk 7 |
| `audio_scene0_nm8` | Scene 1 name chunk 8 |
| `audio_scene1_lpf` | Scene 2 low-pass cutoff in Hz (1-23900) |
| `audio_scene1_eq01` | Scene 2 EQ bands 0+1 packed b0*128+b1 |
| `audio_scene1_eq23` | Scene 2 EQ bands 2+3 packed b2*128+b3 |
| `audio_scene1_eq45` | Scene 2 EQ bands 4+5 packed b4*128+b5 |
| `audio_scene1_eq67` | Scene 2 EQ bands 6+7 packed b6*128+b7 |
| `audio_scene1_eq89` | Scene 2 EQ bands 8+9 packed b8*128+b9 |
| `audio_scene1_out` | Scene 2 prefactor and volume packed pref*128+vol |
| `audio_scene1_pan` | Scene 2 stereo pan |
| `audio_scene1_nm1` | Scene 2 name chunk 1: two ASCII characters packed c1*256+c2 |
| `audio_scene1_nm2` | Scene 2 name chunk 2 |
| `audio_scene1_nm3` | Scene 2 name chunk 3 |
| `audio_scene1_nm4` | Scene 2 name chunk 4 |
| `audio_scene1_nm5` | Scene 2 name chunk 5 |
| `audio_scene1_nm6` | Scene 2 name chunk 6 |
| `audio_scene1_nm7` | Scene 2 name chunk 7 |
| `audio_scene1_nm8` | Scene 2 name chunk 8 |
| `audio_scene2_lpf` | Scene 3 low-pass cutoff in Hz (1-23900) |
| `audio_scene2_eq01` | Scene 3 EQ bands 0+1 packed b0*128+b1 |
| `audio_scene2_eq23` | Scene 3 EQ bands 2+3 packed b2*128+b3 |
| `audio_scene2_eq45` | Scene 3 EQ bands 4+5 packed b4*128+b5 |
| `audio_scene2_eq67` | Scene 3 EQ bands 6+7 packed b6*128+b7 |
| `audio_scene2_eq89` | Scene 3 EQ bands 8+9 packed b8*128+b9 |
| `audio_scene2_out` | Scene 3 prefactor and volume packed pref*128+vol |
| `audio_scene2_pan` | Scene 3 stereo pan |
| `audio_scene2_nm1` | Scene 3 name chunk 1: two ASCII characters packed c1*256+c2 |
| `audio_scene2_nm2` | Scene 3 name chunk 2 |
| `audio_scene2_nm3` | Scene 3 name chunk 3 |
| `audio_scene2_nm4` | Scene 3 name chunk 4 |
| `audio_scene2_nm5` | Scene 3 name chunk 5 |
| `audio_scene2_nm6` | Scene 3 name chunk 6 |
| `audio_scene2_nm7` | Scene 3 name chunk 7 |
| `audio_scene2_nm8` | Scene 3 name chunk 8 |
| `audio_scene3_lpf` | Scene 4 low-pass cutoff in Hz (1-23900) |
| `audio_scene3_eq01` | Scene 4 EQ bands 0+1 packed b0*128+b1 |
| `audio_scene3_eq23` | Scene 4 EQ bands 2+3 packed b2*128+b3 |
| `audio_scene3_eq45` | Scene 4 EQ bands 4+5 packed b4*128+b5 |
| `audio_scene3_eq67` | Scene 4 EQ bands 6+7 packed b6*128+b7 |
| `audio_scene3_eq89` | Scene 4 EQ bands 8+9 packed b8*128+b9 |
| `audio_scene3_out` | Scene 4 prefactor and volume packed pref*128+vol |
| `audio_scene3_pan` | Scene 4 stereo pan |
| `audio_scene3_nm1` | Scene 4 name chunk 1: two ASCII characters packed c1*256+c2 |
| `audio_scene3_nm2` | Scene 4 name chunk 2 |
| `audio_scene3_nm3` | Scene 4 name chunk 3 |
| `audio_scene3_nm4` | Scene 4 name chunk 4 |
| `audio_scene3_nm5` | Scene 4 name chunk 5 |
| `audio_scene3_nm6` | Scene 4 name chunk 6 |
| `audio_scene3_nm7` | Scene 4 name chunk 7 |
| `audio_scene3_nm8` | Scene 4 name chunk 8 |
| `audio_scene4_lpf` | Scene 5 low-pass cutoff in Hz (1-23900) |
| `audio_scene4_eq01` | Scene 5 EQ bands 0+1 packed b0*128+b1 |
| `audio_scene4_eq23` | Scene 5 EQ bands 2+3 packed b2*128+b3 |
| `audio_scene4_eq45` | Scene 5 EQ bands 4+5 packed b4*128+b5 |
| `audio_scene4_eq67` | Scene 5 EQ bands 6+7 packed b6*128+b7 |
| `audio_scene4_eq89` | Scene 5 EQ bands 8+9 packed b8*128+b9 |
| `audio_scene4_out` | Scene 5 prefactor and volume packed pref*128+vol |
| `audio_scene4_pan` | Scene 5 stereo pan |
| `audio_scene4_nm1` | Scene 5 name chunk 1: two ASCII characters packed c1*256+c2 |
| `audio_scene4_nm2` | Scene 5 name chunk 2 |
| `audio_scene4_nm3` | Scene 5 name chunk 3 |
| `audio_scene4_nm4` | Scene 5 name chunk 4 |
| `audio_scene4_nm5` | Scene 5 name chunk 5 |
| `audio_scene4_nm6` | Scene 5 name chunk 6 |
| `audio_scene4_nm7` | Scene 5 name chunk 7 |
| `audio_scene4_nm8` | Scene 5 name chunk 8 |
| `audio_scene5_lpf` | Scene 6 low-pass cutoff in Hz (1-23900) |
| `audio_scene5_eq01` | Scene 6 EQ bands 0+1 packed b0*128+b1 |
| `audio_scene5_eq23` | Scene 6 EQ bands 2+3 packed b2*128+b3 |
| `audio_scene5_eq45` | Scene 6 EQ bands 4+5 packed b4*128+b5 |
| `audio_scene5_eq67` | Scene 6 EQ bands 6+7 packed b6*128+b7 |
| `audio_scene5_eq89` | Scene 6 EQ bands 8+9 packed b8*128+b9 |
| `audio_scene5_out` | Scene 6 prefactor and volume packed pref*128+vol |
| `audio_scene5_pan` | Scene 6 stereo pan |
| `audio_scene5_nm1` | Scene 6 name chunk 1: two ASCII characters packed c1*256+c2 |
| `audio_scene5_nm2` | Scene 6 name chunk 2 |
| `audio_scene5_nm3` | Scene 6 name chunk 3 |
| `audio_scene5_nm4` | Scene 6 name chunk 4 |
| `audio_scene5_nm5` | Scene 6 name chunk 5 |
| `audio_scene5_nm6` | Scene 6 name chunk 6 |
| `audio_scene5_nm7` | Scene 6 name chunk 7 |
| `audio_scene5_nm8` | Scene 6 name chunk 8 |
| `audio_scene6_lpf` | Scene 7 low-pass cutoff in Hz (1-23900) |
| `audio_scene6_eq01` | Scene 7 EQ bands 0+1 packed b0*128+b1 |
| `audio_scene6_eq23` | Scene 7 EQ bands 2+3 packed b2*128+b3 |
| `audio_scene6_eq45` | Scene 7 EQ bands 4+5 packed b4*128+b5 |
| `audio_scene6_eq67` | Scene 7 EQ bands 6+7 packed b6*128+b7 |
| `audio_scene6_eq89` | Scene 7 EQ bands 8+9 packed b8*128+b9 |
| `audio_scene6_out` | Scene 7 prefactor and volume packed pref*128+vol |
| `audio_scene6_pan` | Scene 7 stereo pan |
| `audio_scene6_nm1` | Scene 7 name chunk 1: two ASCII characters packed c1*256+c2 |
| `audio_scene6_nm2` | Scene 7 name chunk 2 |
| `audio_scene6_nm3` | Scene 7 name chunk 3 |
| `audio_scene6_nm4` | Scene 7 name chunk 4 |
| `audio_scene6_nm5` | Scene 7 name chunk 5 |
| `audio_scene6_nm6` | Scene 7 name chunk 6 |
| `audio_scene6_nm7` | Scene 7 name chunk 7 |
| `audio_scene6_nm8` | Scene 7 name chunk 8 |
| `audio_scene7_lpf` | Scene 8 low-pass cutoff in Hz (1-23900) |
| `audio_scene7_eq01` | Scene 8 EQ bands 0+1 packed b0*128+b1 |
| `audio_scene7_eq23` | Scene 8 EQ bands 2+3 packed b2*128+b3 |
| `audio_scene7_eq45` | Scene 8 EQ bands 4+5 packed b4*128+b5 |
| `audio_scene7_eq67` | Scene 8 EQ bands 6+7 packed b6*128+b7 |
| `audio_scene7_eq89` | Scene 8 EQ bands 8+9 packed b8*128+b9 |
| `audio_scene7_out` | Scene 8 prefactor and volume packed pref*128+vol |
| `audio_scene7_pan` | Scene 8 stereo pan |
| `audio_scene7_nm1` | Scene 8 name chunk 1: two ASCII characters packed c1*256+c2 |
| `audio_scene7_nm2` | Scene 8 name chunk 2 |
| `audio_scene7_nm3` | Scene 8 name chunk 3 |
| `audio_scene7_nm4` | Scene 8 name chunk 4 |
| `audio_scene7_nm5` | Scene 8 name chunk 5 |
| `audio_scene7_nm6` | Scene 8 name chunk 6 |
| `audio_scene7_nm7` | Scene 8 name chunk 7 |
| `audio_scene7_nm8` | Scene 8 name chunk 8 |

Absent audio keys keep the firmware defaults. The firmware applies the
saved active scene at cold boot and again after every Amiga warm
reset, before any application can allocate the audio device.

Note that both save paths — ZZTop's Settings **Save** and the
firmware's audio **Save** — regenerate the file from the settings they
know and keep the previous copy as `ZZ9000.bak`: hand-written comments
are not preserved. The file parser reads at most 4 KiB; a larger file
has its tail ignored, which the drivers can observe through the
config-query key `ZZ_CONFIG_KEY_AUDIO_TRUNCATED` (the audio keys
serialize last, so they are the first casualty of an oversized file).

For most systems, leave missing options at their defaults. A minimal custom
file might look like this:

```ini
videocap_profile = filtered_60
scanline_mode = 2
scanline_parity = 0
```

With no valid profile, `filtered_60` provides the monitor-safe 800x600 60 Hz
output. Explicit `full_60` and `full_exact` selections preserve full SuperHires
detail in a 1280x1024 output. On supported full-rate variants,
`centered_1080p_60` places the unchanged 1280x1024 native picture in a
1920x1080 signal with black borders at approximately 60.61 Hz. It is separate
from the Picasso96 1920x1080 RTG mode. An explicitly selected centered profile
falls back to `full_60` on older or unsupported stacks.

ZZTop 2.8 can also preview and calibrate native-picture positioning. Its
**Save** action writes the file and keeps the previous copy as `ZZ9000.bak`;
power-cycle afterwards to apply the saved settings at boot.

Keep these rules in mind:

- Existing `ENV:` variables and ZZ9000.card tooltypes take precedence over
  equivalent file settings. Remove old overrides when migrating.
- Use `videocap_profile` for new files. The older independent video keys remain
  accepted only for compatibility.
- Invalid or unknown entries are skipped rather than preventing boot.
- Some options require matching current firmware, bitstream, and drivers.

See the commented [ZZ9000.CFG sample](ZZ9000.CFG) for every accepted value and
additional notes.

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
