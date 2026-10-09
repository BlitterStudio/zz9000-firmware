# Building the ZZ9000 firmware

Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
SPDX-License-Identifier: GPL-3.0-or-later

Small composable scripts, each doing one thing, run from the repo root. All
outputs land in `bootimage_work/`. CI runs the same scripts.

| Script | What it builds | Needs |
|---|---|---|
| [`build_firmware.sh`](build_firmware.sh) | `ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf` | `arm-none-eabi-gcc` (Arm GNU Toolchain with newlib), host `cc` |
| [`build_libjpeg_turbo.sh`](build_libjpeg_turbo.sh) | `ZZ9000_proto.sdk/ZZ9000OS/build/deps/libjpeg-turbo/.../libjpeg.a` | Arm GNU Toolchain, `cmake`, `make`, `wget` |
| [`build_zlib.sh`](build_zlib.sh) | `ZZ9000_proto.sdk/ZZ9000OS/build/deps/zlib/.../libz.a` | Arm GNU Toolchain, `cmake`, `make`, `wget` |
| [`build_libpng.sh`](build_libpng.sh) | `ZZ9000_proto.sdk/ZZ9000OS/build/deps/libpng/.../liblibpng16_static.a` | Arm GNU Toolchain, `cmake`, `make`, `wget`; calls `build_zlib.sh` |
| [`build_libwebp.sh`](build_libwebp.sh) | Pinned libwebp decoder/demux archives under `ZZ9000OS/build/deps/libwebp/1.6.0/{arm,host}/lib/` | Arm GNU Toolchain (native compiler with `--host`), `cmake`, `make`, `curl` or `wget` |
| [`build_dvd_codecs.sh`](build_dvd_codecs.sh) | Pinned decoder-only libmpeg2 0.5.1 and liba52 0.7.4 archives under `ZZ9000OS/build/deps/{libmpeg2,a52dec}/.../{arm,host}/` (U11 DVD experiment; GPL-2.0-or-later, notices in `ZZ9000OS/src/dvd_codecs/`) | Arm GNU Toolchain (native compiler with `--host`), `make`, `curl` or `wget` |
| [`build_flac.sh`](build_flac.sh) | Pinned decoder-only libFLAC 1.5.0 archive `libflacdec.a` under `ZZ9000OS/build/deps/flac/1.5.0/{arm,host}/` (U7 native FLAC; BSD-3-Clause, notice in `ZZ9000OS/src/flac/COPYING.Xiph`) | Arm GNU Toolchain (native compiler with `--host`), `xz`, `curl` or `wget` |
| [`build_vorbis.sh`](build_vorbis.sh) | Pinned decoder-only libogg 1.3.6 + fixed-point Tremor (commit `820fb323`) archive `libvorbisdec.a` under `ZZ9000OS/build/deps/vorbis/ogg-1.3.6-tremor-820fb3237ea8/{arm,host}/` (U8 Ogg Vorbis; BSD-3-Clause, notices in `ZZ9000OS/src/vorbis/`) | Arm GNU Toolchain (native compiler with `--host`), `xz`, `gzip`, `curl` or `wget` |
| [`build_lzma_sdk.sh`](build_lzma_sdk.sh) | LZMA SDK decoder sources under `ZZ9000_proto.sdk/ZZ9000OS/build/deps/lzma-sdk/` (compiled into `ZZ9000OS.elf` by `build_firmware.sh`) | `wget`, `7z`/`7za`/`7zr` |
| [`build_bitstream.sh`](build_bitstream.sh) | `bootimage_work/zz9000_ps_wrapper.bit` | Vivado 2018.3 on Linux |
| [`build_bitstream.ps1`](build_bitstream.ps1) | `bootimage_work/zz9000_ps_wrapper.bit` | Vivado 2018.3 on Windows |
| [`build_variant_bitstreams.sh`](build_variant_bitstreams.sh) | all release variant `.bit` files | Vivado 2018.3 on Linux |
| [`build_bootimage.sh`](build_bootimage.sh) | `bootimage_work/BOOT.bin` | `bootgen` |
| [`build_release_assets.sh`](build_release_assets.sh) | release ZIPs under `release/` | `bootgen`, `zip` |

Firmware dependency builders run automatically. FPGA, BOOT-image, and release
packaging remain separate commands.

## Common flows

**ARM firmware change only** (most iteration loops). Uses the committed
bitstream. `build_firmware.sh` automatically downloads, verifies, and builds
the libjpeg-turbo, zlib, libpng, libwebp, libmpeg2, liba52, libFLAC, libogg/Tremor and LZMA SDK dependencies under
`ZZ9000OS/build/deps/` when needed:
```bash
./build_firmware.sh
./build_bootimage.sh
```

On Windows or on a machine without the Arm toolchain installed, use Docker.
This follows the CI toolchain path and writes a separate BOOT image by
default:
```powershell
.\build_firmware_docker.ps1
```
or from a POSIX shell:
```bash
./build_firmware_docker.sh
```
The Docker wrappers cache the official Arm GNU Toolchain and bootgen in Docker
volumes named `zz9000-arm-toolchain` and `zz9000-bootgen`.

The firmware linker keeps low DDR below the framebuffer at `0x00200000`.
Video/HDMI control code and constants use the existing high-DDR code region;
mutable state retains the normal data/BSS placement. Keep the
`firmware low sections overlap framebuffer memory` assertion intact: a
failure means the image layout must be repaired, not the framebuffer boundary
increased. Both normal and legacy-bitstream firmware targets depend on
`src/lscript.ld`, so linker-script changes trigger relinking.

**USB proxy change** — run the bounded host models before building the ARM
artifact:

```bash
make -C test/usb test
```

Then verify the mirrored driver contract from a sibling
`zz9000-drivers` checkout:

```bash
python3 ../zz9000-drivers/tools/check-usb-proxy-contract.py "$PWD"
```

ISO protocol changes must update firmware and driver constants together.
Physical Poseidon qualification is separate from these host checks; record it
in `zz9000-drivers/docs/usb-qualification-matrix.md`.

**HDL change (bitstream rebuild)** — requires a Linux box with Vivado:
```bash
./build_bitstream.sh       # regenerates project from zz9000_project.tcl
./build_firmware.sh        # in case firmware wasn't built yet
./build_bootimage.sh
```
On Windows with Vivado 2018.3:
```powershell
.\build_bitstream.ps1
```
Commit the updated `bootimage_work/zz9000_ps_wrapper.bit` so CI (which
does not run Vivado) picks up the change on the next pipeline.

For cold-boot diagnostics, `./build_bitstream.sh --no-autoboot` builds a
bitstream that does not advertise the Zorro autoboot ROM. The PowerShell
equivalent is `.\build_bitstream.ps1 -NoAutoboot`.

Legacy USB mass-storage block support is disabled in firmware by
default. SD HDF boot and the Poseidon USB proxy remain enabled. For an
old-driver regression test, rebuild firmware with
`EXTRA_CFLAGS=-DENABLE_LEGACY_USB_BLOCK_STORAGE=1`.

WebP, FLAC and Ogg Vorbis decoding are built into every firmware but stay
unadvertised until physical Zorro II/III qualification. A hardware
qualification build advertises their service flags so the shipped clients
use them: `EXTRA_CFLAGS=-DZZ9000_QUALIFY_UNADVERTISED ./build_firmware.sh`,
then `./build_release_assets.sh --firmware-flavor qual`. Never publish that
flavor as a release.

### Timing gates

`build_bitstream.sh` / `build_bitstream.ps1` source three gate scripts after
implementation (before `write_bitstream`): `verify_formatter_ooc_timing.tcl`,
`verify_vcap_cdc_timing.tcl`, and `verify_runtime_pixel_timing.tcl`. The
release timing gate fails the build on **any** negative setup or hold slack in
**any** clock group (a per-clock loop over `get_clocks`, including port
clocks such as `i2s_mclk` and `zorro_fcs` that launch input-delay paths), in
addition to the dedicated 150 MHz runtime pixel-clock checks. The final
`TIMING_GATE: PASS - ...` line is what CI-style greps key on; a failure
raises a Tcl error naming the violating group and slack.

The build runs implementation through the post-route `phys_opt_design` step
before the gates execute. That step is load-bearing for the ADAU1701 I2S
input capture (`tSODM` = 40 ns consumes the whole BCLK half period, so the
fast-corner margin is only BCLK insertion minus pin-to-register delay; the
trial build moved from −0.090 ns routed to +0.143 ns after post-route
phys_opt). Treat a routed-checkpoint WNS of a few hundred picoseconds
negative on `i2s_mclk` as "let phys_opt finish" rather than a real
regression; the gated (final) netlist is the authority.

Clock-domain crossings are bounded with `set_max_delay -datapath_only`
instead of blanket false paths so XPM/FIFO-generated CDC constraints keep
precedence (Vivado otherwise reports methodology TIMING-24 and leaves the
boundary unanalyzed). Every exception in `ZZ9000_proto.srcs/constrs_1/new/`
carries a comment stating its rationale; `check_timing` is expected to
report no unconstrained input or output ports.


**Native-PAL videocap default (issue #7)** — for setups that boot
without the host driver (no startup-sequence, floppy-only demo
sessions), the standard 60 Hz videocap default produces visible
stutter on PAL chipset output. The supported way to change this is the
`ZZ9000.CFG` file on the SD card (issue #33):
```ini
videocap_profile = filtered_pal_exact
```
The equivalent compile-time default still exists for manual builds
(`EXTRA_CFLAGS=-DDEFAULT_NS_VIDEOCAP=1 ./build_firmware.sh`; the config
file overrides it when present), but CI no longer packages a separate
`ns-pal` release flavor. Do not reintroduce a PAL firmware-flavor
release matrix just for this setting. **PAL Amiga only**: the genlock clock
defaults assume a PAL chipset master, so on an NTSC machine you'd see
clock-mismatch artifacts until the host driver loads and corrects it.
Not all HDMI sinks accept the non-standard timing either, which is why
the built-in default stays at 60 Hz.

**Release variant bitstreams** — on the Vivado box:
```bash
./build_variant_bitstreams.sh
```

These are hardware/autoconfig bitstream variants, not separate firmware
behavior flavors. The standard Zorro III build uses E7M capture; the
A4000-only build uses the video-slot C28 capture clock. Zorro III FastRAM
is a `fast_ram` `ZZ9000.CFG` setting, not a variant: the former
`zorro3-nofast` pair is gone and its behavior is the fail-closed default.
The script restores `mntzorro.v` and the canonical E7M bitstream afterward.
To rebuild selected variants on Linux, for example:
```bash
./build_variant_bitstreams.sh zorro2 zorro3-aga
```
On Windows with Git Bash, pass both builders when selecting a C28 variant:
```bash
BITSTREAM_BUILDER="powershell -NoProfile -ExecutionPolicy Bypass -File ./build_bitstream.ps1" \
C28_BITSTREAM_BUILDER="powershell -NoProfile -ExecutionPolicy Bypass -File ./build_bitstream.ps1 -CaptureC28 -OutputBitstream" \
  ./build_variant_bitstreams.sh zorro3-aga
```

**Clean rebuild** — no Vivado, uses the committed bitstream:
```bash
./build_firmware.sh clean
./build_firmware.sh
./build_bootimage.sh
```

## Tests

Host-side suites (any machine with a C compiler):
```bash
make -C test/rtg test        # RTG correctness regression
make -C test/video test      # VDMA, native modes, overlays
```

The Linux image-session suite runs the actual JPEG/PNG/WebP firmware decoder:
```bash
make -C test/image test
make -C test/image sanitize
make -C test/image fixtures smoke
test/image/build/webp_fixture_extract animation /tmp/preview.webp
test/image/build/webp_smoke /tmp/preview.webp /tmp/preview.ppm
```
It needs libjpeg, libpng and zlib development headers, CMake, and curl/wget.
The WebP dependency is fetched with a pinned SHA-256 and built decoder/demux-only,
without threads. `src/webp/{COPYING,PATENTS,AUTHORS}` under `ZZ9000OS` retains
the upstream notices; binary distributions must carry the applicable notices.
Tests cover stored reference pixels, fragmented input, bounded tile drains,
malformed containers, allocation failure, tracker exhaustion and reset reclaim.
The sanitizer target instruments the firmware host path and harness; the
downloaded libwebp archives retain their normal release build configuration.
The smoke command writes the first composited canvas of animated input, not
animation playback. Host memory figures and sanitizers do not qualify physical
Z2/Z3 cache behavior, supported image sizes, or throughput. WebP remains
unadvertised pending client integration and target qualification.

The native FLAC backend (`sdk_audio_flac.c`) has its own Linux suite, linked
against the pinned decoder-only libFLAC host archive:
```bash
make -C test/flac test
make -C test/flac sanitize
test/flac/gen_fixtures.sh   # regenerate fixtures; needs the reference flac tool
```
`build_flac.sh` downloads `flac-1.5.0.tar.xz` (release 2025-02-11) from the
xiph/flac GitHub release with a pinned SHA-256 and compiles only the stream
decoder translation units (no encoder, Ogg, metadata editing, programs,
examples or SIMD), integer-only, with every libFLAC allocation routed through
`src/sdk_flac_alloc.h` into the tracked core-1 decode heap. Fixtures are
encoded and reference-decoded by the `flac` tool recorded in
`test/flac/flac_fixtures.h`; tests compare decoded PCM byte-for-byte and
cover fragmented feeds, known/unknown totals, invalid metadata, CRC errors,
truncation, multichannel and Ogg-FLAC rejection, allocation-failure injection
and restart reclaim. Host figures are not physical Z2/Z3 measurements.

The Ogg Vorbis backend (`sdk_audio_vorbis.c`, arena `sdk_vorbis_alloc.c`)
has its own Linux suite, linked against the pinned libogg/Tremor host
archive:
```bash
make -C test/vorbis test
make -C test/vorbis sanitize
test/vorbis/gen_fixtures.sh   # regenerate fixtures; needs ffmpeg with libvorbis
```
`build_vorbis.sh` downloads `libogg-1.3.6.tar.xz` (release 2025-06-16) from
downloads.xiph.org and the Tremor master commit `820fb3237ea8` (2025-04-03)
archive from gitlab.xiph.org, each with a pinned SHA-256, and compiles only
libogg's framing/bitwise units and Tremor's synthesis core (no vorbisfile,
examples, ARM assembly or low-accuracy path; Tremor has no encoder), with
every allocation routed through `src/sdk_vorbis_alloc.h` into a per-stream
arena of tracked core-1 decode-heap regions. Fixtures under
`test/vorbis/fixtures/` are encoded by ffmpeg's libvorbis wrapper and
reference-decoded by ffmpeg's independent floating-point Vorbis decoder
(versions in `test/vorbis/vorbis_fixtures.h`); decoded PCM must match within
4 LSB per sample and 1 LSB RMS, and be byte-identical across every chunking,
ring size and drain pattern. Tests cover mono/stereo, three block-size
pairs, packets split across pages, 192 KiB comment headers, bad CRC,
missing/out-of-order headers, oversized setup headers and codebooks,
truncation, page gaps, chained/multiplexed/Opus/Ogg-FLAC/3-channel
rejection, allocation-failure injection and restart reclaim. Host figures
are not physical Z2/Z3 measurements.

For source-preserving Docker runs, mount this checkout read-only, copy the
firmware/test tree into container-local storage, and clean copied build caches
there before building. CMake caches contain absolute paths; copying an existing
dependency build does not make it relocatable.

Capture RTL simulations also run in CI with Verilator. They exercise the
production C28/E7M clock controller, filtered and full-width PAL-shaped
capture, the 28 MHz line origin, writeback layout, diagnostics, and the
video-slot/Denise/Zorro II RGB pin mappings:
```bash
bash test/video/run_videocap_clock_verilator.sh
bash test/video/run_videocap_verilator_sim.sh
```
The sampler CI suite uses a test-only XPM handshake model. It does not
replace vendor XPM, MMCM phase, IOB placement, routed timing, or physical
capture qualification. Run the full sampler matrix against Vivado's XPM
library before rebuilding FPGA bitstreams:
```bash
test/video/run_videocap_sim.sh
```

Functional simulation of the video formatter (needs Vivado 2018.3 for
xsim; run on the Vivado machine before committing `video_formatter.v`
changes):
```bash
test/video/run_formatter_sim.sh current   # working-tree RTL
test/video/run_formatter_sim.sh master    # committed baseline (sanity)
```
The testbench models the 64-bit VDMA stream (tkeep, tuser/tlast) and
compares every displayed pixel against expected framebuffer contents
across all color modes (8/15/16/32 bpp), scale_x/scale_y, odd-width
tkeep tails, the native videocap shape and 1920-wide 32 bpp lines. The
sweep fails if any configuration mismatches or fails to report.

Capture-clock phase regression (Vivado 2018.3 UNISIM; set `VIVADO_BIN` for a
non-default installation):
```bash
python3 test/video/run_videocap_phase_sim.py
```
This runs the production phase engine and MMCM, measuring both capture and
grid clock displacement for positive, negative, and restored-zero targets.
A completed phase request is not sufficient: enabling fine phase shift on
both feedback and outputs cancels the intended clock movement.

Variant elaboration (Vivado 2018.3 xvlog/xelab; set `VIVADO_BIN` for a
non-default installation): compiles `MNTZorro` once for each release
variant, using the `define blocks from `build_variant_bitstreams.sh`. Run it
after RTL changes, before an hours-long variant rebuild:
```bash
python3 test/video/run_variant_elaboration.py
```

## Flashing

Copy `bootimage_work/BOOT.bin` to the ZZ9000 SD card (rename if needed
depending on your QSPI/SD boot setup), power-cycle the Amiga.

## Toolchain locations

- **`arm-none-eabi-gcc`** (firmware):
  - macOS: `brew install --cask gcc-arm-embedded`
  - Linux: download from <https://developer.arm.com/downloads> — do **not**
    use Debian's `gcc-arm-none-eabi` package; it uses picolibc and is
    incompatible with the Xilinx BSP.
- **Host C compiler** (firmware helper): `build_firmware.sh` uses `cc`
  to generate an 8 KB boot ROM image from the checked-in diag/device
  arrays, then embeds it as an initialized ELF section. Override with
  `$CC_FOR_BUILD` if needed.
- **`bootgen`** (packaging):
  - Prebuilt: <https://github.com/Xilinx/bootgen> (clone + `make`). The
    script finds it via `$BOOTGEN`, then `$PATH`, then a Mac default of
    `/Users/midwan/Gitlab/bootgen/bootgen`.
- **Vivado 2018.3** (bitstream):
  - Linux: set `$VIVADO_DIR` if not at `/opt/Xilinx/Vivado/2018.3`.
  - Windows: `build_bitstream.ps1` checks `$env:VIVADO_BAT`,
    `$env:VIVADO_DIR`, `D:\Xilinx\Vivado\2018.3\bin\vivado.bat`, then
    `C:\Xilinx\Vivado\2018.3\bin\vivado.bat`.

## Xilinx Platform Cable setup

Vivado/JTAG workflows on Linux may need the legacy Xilinx USB firmware
loader rules. The repository includes the required firmware blobs and
udev rules under [`xilinx-xusb/`](xilinx-xusb/):

```bash
sudo apt install fxload
sudo mkdir -p /etc/xilinx-xusb
sudo cp xilinx-xusb/*.hex /etc/xilinx-xusb/
sudo cp xilinx-xusb/xusbdfwu.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

udev will run `fxload` when the platform cable is plugged in. The cable
LED should turn green after the firmware loads.

## CI

The GitHub Actions workflow
[`.github/workflows/build.yml`](.github/workflows/build.yml) runs on
every push and pull request: runs the host regression suites, including
`test/usb`; installs the Arm GNU Toolchain (cached); builds bootgen from
source (cached); then runs `./build_firmware.sh` +
`./build_bootimage.sh` against the committed bitstream. It does **not**
run Vivado — any HDL change must include an updated
`bootimage_work/zz9000_ps_wrapper.bit` for CI to pick up the new logic.
Release-style ZIP artifacts are uploaded per run.

### Cutting a release

Push a `v*` tag (e.g. `v2.2.0`, or `v2.2.0-rc1` for a pre-release) and
the workflow will build the firmware and publish a GitHub Release with
`zz9000-firmware-<tag>-<variant>.zip` archives attached. Each ZIP
contains a directory with the user-facing `BOOT.bin` and sample
`ZZ9000.CFG` file to copy to the ZZ9000 microSD card. The workflow
packages one standard firmware flavor; the former `ns-pal` behavior is
selected with `videocap_profile = filtered_pal_exact` in `ZZ9000.CFG`.

```bash
git tag -a v2.2.0 -m "Firmware 2.2.0"
git push origin v2.2.0
```

Tags containing `-` are marked as pre-releases.

CI cannot run Vivado, so it packages variants from committed bitstreams.
The default Zorro III E7M bitstream is
`bootimage_work/zz9000_ps_wrapper.bit`; the Zorro III no-RAM, A4000 C28,
Zorro II, A500, and 2MB variants live under `bootimage_work/variants/`;
see [`bootimage_work/variants/README.md`](bootimage_work/variants/README.md).
Build them with `./build_variant_bitstreams.sh` on a Vivado machine.
Tagged release builds require all listed bitstreams; branch/PR builds
package whatever is present. The deprecated no-USB-autoboot variant
is intentionally skipped.

## Why `bootimage_work/` is the canonical output dir

- `FSBL_exec.elf` lives there and is committed (saves having to rebuild
  the FSBL, which needs Xilinx SDK 2018.3 — an old, painful dependency
  we've chosen to avoid). **Warning:** the committed FSBL bakes in the
  PS configuration (`ps7_init`) from the hardware design it was built
  against. Changing any `PCW_*` parameter in `zz9000_project.tcl` does
  NOT update it — the 2026-07 HP0 32→64-bit widening shipped with a
  runtime guard in `video.c` (`video_hp0_bus_width_init()`) precisely
  because the committed FSBL still programmed the port for 32 bits.
  Clock, DDR or MIO `PCW_*` changes cannot be patched at runtime and
  would require rebuilding the FSBL against a fresh hardware export.
- `bootimage.bif` lives there, paths are repo-root-relative, same file
  used by humans and CI.
- The bitstream lives there because that's where `bootgen` reads it
  from per the BIF, and committing it makes CI work without Vivado.
- The firmware ELF contains an initialized 8 KB `.bootrom_image` segment
  at `0x3FCF0000`, so FSBL preloads the Zorro autoboot ROM before
  `main()` starts. The default BOOT image layout still stays
  `FSBL -> bitstream -> ZZ9000OS`.
