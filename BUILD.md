# Building the ZZ9000 firmware

Three small scripts, each doing one thing, run from the repo root. All
outputs land in `bootimage_work/`. CI runs the same scripts.

| Script | What it builds | Needs |
|---|---|---|
| [`build_firmware.sh`](build_firmware.sh) | `ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf` | `arm-none-eabi-gcc` (Arm GNU Toolchain with newlib) |
| [`build_bitstream.sh`](build_bitstream.sh) | `bootimage_work/zz9000_ps_wrapper.bit` | Vivado 2018.3 on Linux |
| [`build_variant_bitstreams.sh`](build_variant_bitstreams.sh) | all release variant `.bit` files | Vivado 2018.3 on Linux |
| [`build_bootimage.sh`](build_bootimage.sh) | `bootimage_work/BOOT.bin` | `bootgen` |
| [`build_release_assets.sh`](build_release_assets.sh) | old-style ZIPs under `release/` | `bootgen`, `zip` |

The scripts are composable — nothing calls anything else implicitly.

## Common flows

**ARM firmware change only** (most iteration loops). Uses the committed
bitstream:
```bash
./build_firmware.sh
./build_bootimage.sh
```

**HDL change (bitstream rebuild)** — requires a Linux box with Vivado:
```bash
./build_bitstream.sh       # regenerates project from zz9000_project.tcl
./build_firmware.sh        # in case firmware wasn't built yet
./build_bootimage.sh
```
Commit the updated `bootimage_work/zz9000_ps_wrapper.bit` so CI (which
does not run Vivado) picks up the change on the next pipeline.

**Release variant bitstreams** — on the Vivado box:
```bash
./build_variant_bitstreams.sh
```
This builds the default Zorro III bitstream and the Zorro II / A500 /
2MB variants, copies them to the release paths under `bootimage_work/`,
and restores `mntzorro.v` afterward. You can also pass one or more
variant names, for example:
```bash
./build_variant_bitstreams.sh zorro2 zorro2-2mb a500plus
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
- **`bootgen`** (packaging):
  - Prebuilt: <https://github.com/Xilinx/bootgen> (clone + `make`). The
    script finds it via `$BOOTGEN`, then `$PATH`, then a Mac default of
    `/Users/midwan/Gitlab/bootgen/bootgen`.
- **Vivado 2018.3** (bitstream): set `$VIVADO_DIR` if not at
  `/opt/Xilinx/Vivado/2018.3`.

## CI

The GitHub Actions workflow
[`.github/workflows/build.yml`](.github/workflows/build.yml) runs on
every push and pull request: installs the Arm GNU Toolchain (cached),
builds bootgen from source (cached), then runs `./build_firmware.sh`
+ `./build_bootimage.sh` against the committed bitstream. It does **not**
run Vivado — any HDL change must include an updated
`bootimage_work/zz9000_ps_wrapper.bit` for CI to pick up the new logic.
Build artifacts (`ZZ9000OS.elf`, `BOOT.bin`) are uploaded per run.

### Cutting a release

Push a `v*` tag (e.g. `v1.14`, or `v1.15-rc1` for a pre-release) and
the workflow will build the firmware and publish a GitHub Release with
`BOOT-<tag>.bin`, `ZZ9000OS-<tag>.elf`, and old-style
`zz9000-firmware-<tag>-<variant>.zip` archives attached. Each ZIP
contains a directory with the user-facing `BOOT.bin` file to copy to the
ZZ9000 microSD card.

```bash
git tag -a v1.14 -m "Firmware 1.14"
git push origin v1.14
```

Tags containing `-` are marked as pre-releases.

CI cannot run Vivado, so it packages variants from committed bitstreams.
The default Zorro III bitstream is `bootimage_work/zz9000_ps_wrapper.bit`.
Zorro II / A500 / 2MB variant bitstreams live under
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
