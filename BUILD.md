# Building the ZZ9000 firmware

Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
SPDX-License-Identifier: GPL-3.0-or-later

The normal build produces an ARM firmware ELF and packages it with the
committed FPGA bitstream and FSBL into a BOOT image for the ZZ9000 SD card.
Run every command in this guide from the repository root.

## Build a BOOT image

Install the official Arm GNU Toolchain with newlib so that
`arm-none-eabi-gcc` and `arm-none-eabi-objcopy` are on `PATH`. Install a host
C compiler as `cc`, plus `cmake`, `make`, `wget`, and `7z`, `7za`, or `7zr`.
The firmware build fetches and builds its pinned codec dependencies when they
are absent.

```bash
./build_firmware.sh
./build_bootimage.sh
```

The commands create these files:

| File | Purpose |
|---|---|
| `ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf` | Firmware for the default bitstream |
| `ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS-legacy-bitstream.elf` | Firmware for legacy bitstreams |
| `bootimage_work/BOOT.bin` | Image to copy to the ZZ9000 SD card |
| `bootimage_work/bootrom.bin` | Generated 8 KiB autoboot ROM data partition |

`build_firmware.sh` runs the dependency builders for libjpeg-turbo, libpng
(and zlib), the LZMA SDK, libwebp, libmpeg2/liba52, libFLAC, Tremor/libogg,
libvpx, and libopus before invoking `make -C ZZ9000_proto.sdk/ZZ9000OS`.
Their sources and outputs are cached below
`ZZ9000_proto.sdk/ZZ9000OS/build/deps/`.

To remove the firmware build tree before rebuilding, run:

```bash
./build_firmware.sh clean
./build_firmware.sh
./build_bootimage.sh
```

`build_bootimage.sh` packages `bootimage_work/FSBL_exec.elf`,
`bootimage_work/zz9000_ps_wrapper.bit`, and the default firmware ELF. Override
an input or output only when preparing a separate image:

```bash
./build_bootimage.sh --bitstream path/to/zz9000_ps_wrapper.bit \
  --firmware path/to/ZZ9000OS.elf --output path/to/BOOT.bin
```

The boot ROM data partition is included by default. Use
`--no-preload-bootrom` only for diagnostics.

Copy `bootimage_work/BOOT.bin` to the ZZ9000 SD card, then power-cycle the
Amiga. Put `ZZ9000.CFG` beside `BOOT.bin` on the card to select supported
runtime settings.

## Build with Docker

Use the Docker wrappers when the Arm toolchain is unavailable locally. Both
use `debian:bookworm-slim` by default, cache the Arm GNU Toolchain and bootgen
in the `zz9000-arm-toolchain` and `zz9000-bootgen` Docker volumes, and write
`bootimage_work/BOOT-sdk-docker.bin` by default.

From a POSIX shell:

```bash
./build_firmware_docker.sh
```

From PowerShell:

```powershell
.\build_firmware_docker.ps1
```

The POSIX wrapper accepts `--no-clean`, `--no-bootimage`, `--fsbl`, and
`--output PATH`. The PowerShell wrapper accepts `-NoClean`, `-NoBootimage`,
`-Image`, `-Output`, and `-ExtraCFlags`. `--fsbl` rebuilds the FSBL in the
container but does not replace `bootimage_work/FSBL_exec.elf`.

## Rebuild the FPGA bitstream

Rebuild the bitstream after RTL, constraint, IP, or Vivado-project changes.
The Linux and Windows builders recreate `ZZ9000_proto` from
`zz9000_project.tcl`, run synthesis, implementation, and `write_bitstream`,
then copy the generated bitstream into `bootimage_work/`.

Use Vivado 2018.3. On Linux, the default installation is
`/opt/Xilinx/Vivado/2018.3`; set `VIVADO_DIR` to another Vivado 2018.3
installation. On Windows, pass `-VivadoBat` or set `VIVADO_BAT` or
`VIVADO_DIR` if Vivado is not at one of the script's default locations.

On Linux:

```bash
./build_bitstream.sh
./build_firmware.sh
./build_bootimage.sh
```

On Windows:

```powershell
.\build_bitstream.ps1
```

The default output is `bootimage_work/zz9000_ps_wrapper.bit`. The generated
project targets `xc7z020clg400-1`. Commit the updated default bitstream with
an RTL change because CI packages committed bitstreams and does not run
Vivado.

Build a diagnostic bitstream without the Zorro autoboot ROM advertisement:

```bash
./build_bitstream.sh --no-autoboot
```

```powershell
.\build_bitstream.ps1 -NoAutoboot
```

Build the A4000 C28 capture candidate separately. Its default output is
`bootimage_work/capture-c28/zz9000_ps_wrapper.bit`; it must not replace the
default E7M bitstream.

```bash
./build_bitstream.sh --capture-c28
```

```powershell
.\build_bitstream.ps1 -CaptureC28
```

The synthesis flow runs `verify_formatter_ooc_timing.tcl`,
`verify_vcap_cdc_timing.tcl`, and `verify_runtime_pixel_timing.tcl` before
`write_bitstream`. A failure stops the build. The final bitstream is written
under `ZZ9000_proto/ZZ9000_proto.runs/impl_1/` before the builder copies it to
its requested output.
A recorded implementation trial improved the ADAU1701 I2S input routed slack
from -0.090 ns to +0.143 ns after post-route `phys_opt_design`. The final
gated implementation result, rather than an intermediate routed checkpoint,
determines whether the bitstream is written.

## Build hardware variants

`build_variant_bitstreams.sh` changes the release-variant define block in
`mntzorro.v` for each build, restores that file afterward, and writes the
bitstreams below `bootimage_work/`.

Build every release variant on the Linux Vivado host:

```bash
./build_variant_bitstreams.sh
```

Build selected variants:

```bash
./build_variant_bitstreams.sh zorro2 zorro3-aga
```

The supported variant names are `zorro3`, `zorro3-aga`, `zorro2`,
`zorro2-2mb`, `a500`, `a500-2mb`, and `a500plus`. The standard `zorro3` build
writes `bootimage_work/zz9000_ps_wrapper.bit`; the other outputs are under
`bootimage_work/variants/`.

From Git Bash on Windows, use the PowerShell builder. A selected C28 variant
requires a C28 builder that ends with its output-path option:

```bash
BITSTREAM_BUILDER="powershell -NoProfile -ExecutionPolicy Bypass -File ./build_bitstream.ps1" \
C28_BITSTREAM_BUILDER="powershell -NoProfile -ExecutionPolicy Bypass -File ./build_bitstream.ps1 -CaptureC28 -OutputBitstream" \
  ./build_variant_bitstreams.sh zorro3-aga
```

## Package release assets

Build the firmware and the required variant bitstreams before packaging a
release. `build_release_assets.sh` writes ZIP files to `release/` by default.
Each archive contains `BOOT.bin` and the sample `ZZ9000.CFG` in a directory
named `zz9000-firmware-<tag>-<variant>`.

```bash
./build_release_assets.sh --tag v2.2.0 --require-all
```

Use `--output DIR` to choose another output directory. Use
`--firmware-flavor SUFFIX` only when packaging an alternate firmware ELF; the
standard Zorro III image remains `bootimage_work/BOOT.bin`.

## Rebuild the FSBL

The committed `bootimage_work/FSBL_exec.elf` is the default FSBL input for
`build_bootimage.sh`. Rebuild it only after a Vivado build when the PS
configuration has changed. `ps7_init_gpl.c` and `ps7_init_gpl.h` must match
the `zz9000_ps_wrapper.sysdef` generated below
`ZZ9000_proto/ZZ9000_proto.runs/impl_1/`.

```bash
util/refresh_ps7_init.sh
./build_fsbl.sh
```

The FSBL ELF is written to
`ZZ9000_proto.sdk/ZZ9000FSBL/build/ZZ9000FSBL.elf`. After validating the
result and preparing recovery media, replace the boot-image input explicitly:

```bash
./build_fsbl.sh --install
```

`--install` copies the rebuilt ELF to `bootimage_work/FSBL_exec.elf`. A stale
FSBL can program PS registers for a different bitstream configuration.

## Targeted checks

Run the host checks for the area you changed before building the BOOT image.

| Change area | Command |
|---|---|
| USB proxy | `make -C test/usb test` |
| RTG | `make -C test/rtg test` |
| Video firmware | `make -C test/video test` |
| JPEG, PNG, or WebP decoder | `make -C test/image test` |
| FLAC decoder | `make -C test/flac test` |
| Ogg Vorbis decoder | `make -C test/vorbis test` |

The image, FLAC, and Vorbis suites also provide a `sanitize` target:

```bash
make -C test/image sanitize
make -C test/flac sanitize
make -C test/vorbis sanitize
```
For USB proxy ISO protocol changes, also check the matching driver checkout:

```bash
python3 ../zz9000-drivers/tools/check-usb-proxy-contract.py "$PWD"
```

Keep firmware and driver constants synchronized. Physical Poseidon
qualification remains separate from the host checks; record it in
`zz9000-drivers/docs/usb-qualification-matrix.md`.

For FPGA changes, run the relevant simulation on the Vivado host. The
formatter simulation accepts the working tree or the committed baseline; the
overlay scaling simulation covers the PIP line fetcher and scaler:

```bash
test/video/run_formatter_sim.sh current
test/video/run_formatter_sim.sh master
test/video/run_overlay_scale_sim.sh
python3 test/video/run_videocap_phase_sim.py
python3 test/video/run_variant_elaboration.py
```

## Tool lookup and CI

`build_bootimage.sh` locates bootgen in this order: `BOOTGEN`, `bootgen` on
`PATH`, `bootgen/bootgen`, `bootgen/build/bin/bootgen`, then
`/Users/midwan/Gitlab/bootgen/bootgen`. Build bootgen from
<https://github.com/Xilinx/bootgen> if none of those locations contains it.

GitHub Actions builds `ZZ9000OS.elf` and packages release assets from the
committed bitstreams. It does not run Vivado. Tagged builds require every
listed variant bitstream; branch and pull-request builds package the variants
that are present.


## Additional host checks

The workflow also runs these host suites. Run the suites that cover the code
you changed.

| Area | Command |
|---|---|
| Video codecs | `make -C test/video_codec test` |
| RTG comparative micro-benchmarks | `make -C test/rtg bench` |
| Media sessions | `make -C test/media test` |
| Palette shadow state | `make -C test/palette test` |
| Audio capture | `make -C test/audio test` |
| `ZZ9000.CFG` parser and loader | `make -C test/config test` |
| Dual-core scheduler | `make -C test/scheduler test` |
| Surface allocator | `make -C test/allocator test` |
| Zorro II aperture layout | `make -C test/aperture test` |
| Firmware-file restore | `make -C test/fwupdate test` |
| SD activity LED | `make -C test/sd_activity_led test` |

Hardware validation remains necessary before treating bus-timing or performance
changes as proven.

## Publish a release

Push an annotated `v*` tag after the firmware and every release bitstream have
been committed. GitHub Actions builds the standard firmware image, packages
every required variant with the sample `ZZ9000.CFG`, then creates a GitHub
Release from the ZIP files. A tag containing `-`, such as `v2.3.0-rc1`,
becomes a pre-release.

```bash
git tag -a v2.3.0 -m "Firmware 2.3.0"
git push origin v2.3.0
```

Use `ZZ9000.CFG` to select PAL or native-video defaults. Do not publish a
separate `ns-pal` firmware flavor.