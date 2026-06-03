# Building the ZZ9000 firmware

Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
SPDX-License-Identifier: GPL-3.0-or-later

Three small scripts, each doing one thing, run from the repo root. All
outputs land in `bootimage_work/`. CI runs the same scripts.

| Script | What it builds | Needs |
|---|---|---|
| [`build_firmware.sh`](build_firmware.sh) | `ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf` | `arm-none-eabi-gcc` (Arm GNU Toolchain with newlib), host `cc` |
| [`build_libjpeg_turbo.sh`](build_libjpeg_turbo.sh) | `ZZ9000_proto.sdk/ZZ9000OS/build/deps/libjpeg-turbo/.../libjpeg.a` | Arm GNU Toolchain, `cmake`, `make`, `wget` |
| [`build_zlib.sh`](build_zlib.sh) | `ZZ9000_proto.sdk/ZZ9000OS/build/deps/zlib/.../libz.a` | Arm GNU Toolchain, `cmake`, `make`, `wget` |
| [`build_libpng.sh`](build_libpng.sh) | `ZZ9000_proto.sdk/ZZ9000OS/build/deps/libpng/.../liblibpng16_static.a` | Arm GNU Toolchain, `cmake`, `make`, `wget`; calls `build_zlib.sh` |
| [`build_bitstream.sh`](build_bitstream.sh) | `bootimage_work/zz9000_ps_wrapper.bit` | Vivado 2018.3 on Linux |
| [`build_bitstream.ps1`](build_bitstream.ps1) | `bootimage_work/zz9000_ps_wrapper.bit` | Vivado 2018.3 on Windows |
| [`build_variant_bitstreams.sh`](build_variant_bitstreams.sh) | all release variant `.bit` files | Vivado 2018.3 on Linux |
| [`build_bootimage.sh`](build_bootimage.sh) | `bootimage_work/BOOT.bin` | `bootgen` |
| [`build_release_assets.sh`](build_release_assets.sh) | old-style ZIPs under `release/` | `bootgen`, `zip` |

The scripts are composable — nothing calls anything else implicitly.

## Common flows

**ARM firmware change only** (most iteration loops). Uses the committed
bitstream. `build_firmware.sh` automatically downloads, verifies, and builds
the libjpeg-turbo, zlib, and libpng static dependencies under
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

**Native-PAL videocap default (issue #7)** — for setups that boot
without the host driver (no startup-sequence, floppy-only demo
sessions), the standard 60 Hz videocap default produces visible
stutter on PAL chipset output. Build with:
```bash
EXTRA_CFLAGS=-DDEFAULT_NS_VIDEOCAP=1 ./build_firmware.sh
```
This flips `video_init()` to default to 720x576 at the Amiga's native
~49.92 Hz and pre-enables `CARD_FEATURE_NONSTANDARD_VSYNC`, so videocap
locks to the chipset rate from cold boot. CI also packages this as the
`ns-pal` release flavor (e.g. `zz9000-firmware-<tag>-zorro3-ns-pal.zip`)
alongside the standard archives. **PAL Amiga only**: the genlock clock
defaults assume a PAL chipset master, so on an NTSC machine you'd see
clock-mismatch artifacts until the host driver loads and corrects it
— use the standard build there instead. Not all HDMI sinks accept the
non-standard timing either, which is why the standard build keeps the
60 Hz default.

**Release variant bitstreams** — on the Vivado box:
```bash
./build_variant_bitstreams.sh
```
This builds the default Zorro III bitstream and the Zorro III no-RAM /
Zorro II / A500 / 2MB variants, copies them to the release paths under
`bootimage_work/`, and restores `mntzorro.v` afterward. You can also
pass one or more variant names, for example:
```bash
./build_variant_bitstreams.sh zorro3-nofast zorro2 zorro2-2mb a500plus
```

**Clean rebuild** — no Vivado, uses the committed bitstream:
```bash
./build_firmware.sh clean
./build_firmware.sh
./build_bootimage.sh
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
every push and pull request: installs the Arm GNU Toolchain (cached),
builds bootgen from source (cached), then runs `./build_firmware.sh`
+ `./build_bootimage.sh` against the committed bitstream. It does **not**
run Vivado — any HDL change must include an updated
`bootimage_work/zz9000_ps_wrapper.bit` for CI to pick up the new logic.
Release-style ZIP artifacts are uploaded per run.

### Cutting a release

Push a `v*` tag (e.g. `v2.1.0`, or `v2.1.0-rc1` for a pre-release) and
the workflow will build the firmware and publish a GitHub Release with
old-style `zz9000-firmware-<tag>-<variant>.zip` archives attached. Each
ZIP contains a directory with the user-facing `BOOT.bin` file to copy to
the ZZ9000 microSD card. The workflow packages both the standard firmware
flavor and the `ns-pal` flavor.

```bash
git tag -a v2.1.0 -m "Firmware 2.1.0"
git push origin v2.1.0
```

Tags containing `-` are marked as pre-releases.

CI cannot run Vivado, so it packages variants from committed bitstreams.
The default Zorro III bitstream is `bootimage_work/zz9000_ps_wrapper.bit`.
The Zorro III no-RAM, Zorro II, A500, and 2MB variant bitstreams live under
`bootimage_work/variants/`; see
[`bootimage_work/variants/README.md`](bootimage_work/variants/README.md).
Build them with `./build_variant_bitstreams.sh` on a Vivado machine.
Tagged release builds require all listed variant bitstreams, while
branch/PR builds package whatever is present. The deprecated
no-USB-autoboot variant is intentionally skipped.

## Why `bootimage_work/` is the canonical output dir

- `FSBL_exec.elf` lives there and is committed (saves having to rebuild
  the FSBL, which needs Xilinx SDK 2018.3 — an old, painful dependency
  we've chosen to avoid).
- `bootimage.bif` lives there, paths are repo-root-relative, same file
  used by humans and CI.
- The bitstream lives there because that's where `bootgen` reads it
  from per the BIF, and committing it makes CI work without Vivado.
- The firmware ELF contains an initialized 8 KB `.bootrom_image` segment
  at `0x3FCF0000`, so FSBL preloads the Zorro autoboot ROM before
  `main()` starts. The default BOOT image layout still stays
  `FSBL -> bitstream -> ZZ9000OS`.
