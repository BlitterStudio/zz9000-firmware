# ZZ9000 Firmware — Agent Instructions

## What This Is

FPGA logic (Verilog) + bare-metal ARM firmware for the MNT ZZ9000 Zorro II/III graphics and coprocessor card. Xilinx Zynq-7020 SoC (dual Cortex-A9 + FPGA fabric). GPL-3.0-or-later.

## Build Commands (Most Important)

Scripts are composable — nothing calls anything else implicitly. All run from repo root.

**Firmware-only iteration (most common, no Vivado):**
```bash
./build_firmware.sh clean          # optional: clean first
./build_firmware.sh                # → ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf
BOOTGEN=/path/to/bootgen ./build_bootimage.sh   # → bootimage_work/BOOT.bin
```

**On Windows or without local toolchain (Docker):**
```powershell
.\build_firmware_docker.ps1       # outputs BOOT-sdk-docker.bin by default
```

**HDL change (requires Vivado 2018.3 on Linux, or PS1 on Windows):**
```bash
./build_bitstream.sh              # → bootimage_work/zz9000_ps_wrapper.bit
# Then commit the .bit file — CI cannot run Vivado!
```

**Release packaging:**
```bash
./build_release_assets.sh --tag v2.1.0   # → release/*.zip (one per variant)
```

## Toolchain Gotchas

- **`arm-none-eabi-gcc`**: Must be the official Arm GNU Toolchain with **newlib**. Debian's `gcc-arm-none-eabi` uses picolibc and is **incompatible** with the Xilinx BSP. CI uses version `13.2.rel1`.
- **`bootgen`**: Not packaged by any distro. Build from <https://github.com/Xilinx/bootgen>. Script discovers via `$BOOTGEN`, then `$PATH`, then in-tree `bootgen/`.
- **Host C compiler** (`cc`): Needed to build the boot ROM image generator. Override with `$CC_FOR_BUILD`.

## CI & Releases

- GitHub Actions (`.github/workflows/build.yml`) builds firmware + packages release ZIPs on every push/PR.
- **CI does NOT run Vivado.** HDL changes must commit updated `.bit` files under `bootimage_work/`.
- Tag a release: `git tag -a v2.1.0 && git push origin v2.1.0`. Tags with `-` (e.g., `v2.1.0-rc1`) become pre-releases.
- CI builds two firmware flavors: standard and `ns-pal` (PAL Amiga ~49.92 Hz videocap default).

## Testing

```bash
make -C test/rtg test              # RTG correctness regression
make -C test/rtg bench             # Host micro-benchmarks (comparative only)
```

Hardware validation is still required for performance/bus timing changes.

## Architecture Overview

| Path | Purpose |
|---|---|
| `mntzorro.v` | Zorro bus interface, register window, video capture, AXI bridge |
| `video_formatter.v` | AXI-Stream video formatter, 24-bit RGB output |
| `ZZ9000_proto.sdk/ZZ9000OS/src/` | Bare-metal ARM firmware (C) |
| `ZZ9000_proto.sdk/ZZ9000FSBL/src/` | First-stage bootloader |
| `zz9000_project.tcl` | Vivado project/block design source |
| `bootimage_work/` | Committed FSBL, bitstreams, BIF — canonical output dir |

## Firmware Variants

7 variants controlled by Verilog `` `define `` blocks in `mntzorro.v`:
`zorro3`, `zorro3-nofast`, `zorro2`, `zorro2-2mb`, `a500`, `a500-2mb`, `a500plus`.

Build with `./build_variant_bitstreams.sh` (Vivado machine only). The script rewrites the define block in `mntzorro.v`, builds, copies the `.bit`, then restores the source. Bitstream outputs live under `bootimage_work/variants/`.

## Build Artifacts & Ignored Paths

- `ZZ9000_proto/` (Vivado project dir) — generated, gitignored
- `ZZ9000_proto.sdk/ZZ9000OS/build/` — firmware build output, gitignored
- `bootimage_work/BOOT.bin` — generated boot image, gitignored
- `release/` — release ZIPs, gitignored
- `bootimage_work/*.bit` and `bootimage_work/variants/*.bit` — **committed** (CI needs them)

## Special Build Flags

- `EXTRA_CFLAGS=-DDEFAULT_NS_VIDEOCAP=1 ./build_firmware.sh` — PAL Amiga native videocap timing
- `./build_bitstream.sh --no-autoboot` — diagnostic bitstream without Zorro autoboot ROM
- `EXTRA_CFLAGS=-DENABLE_LEGACY_USB_BLOCK_STORAGE=1` — re-enable legacy USB mass-storage (disabled by default)

## Key Dependencies

Firmware auto-builds these static libs under `ZZ9000OS/build/deps/`: libjpeg-turbo, zlib, libpng, lzma-sdk. Scripts download sources on first build.
