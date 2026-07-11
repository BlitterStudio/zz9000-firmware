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
./build_fsbl.sh                    # FSBL from source → ZZ9000_proto.sdk/ZZ9000FSBL/build/ZZ9000FSBL.elf
                                   # (--install only after bench validation, see ZZ9000FSBL/README.md)
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
./build_release_assets.sh --tag v2.3.0   # → release/*.zip (one per hardware variant, one firmware flavor)
```

## Toolchain Gotchas

- **`arm-none-eabi-gcc`**: Must be the official Arm GNU Toolchain with **newlib**. Debian's `gcc-arm-none-eabi` uses picolibc and is **incompatible** with the Xilinx BSP. CI uses version `13.2.rel1`.
- **`bootgen`**: Not packaged by any distro. Build from <https://github.com/Xilinx/bootgen>. Script discovers via `$BOOTGEN`, then `$PATH`, then in-tree `bootgen/`.
- **Host C compiler** (`cc`): Needed to build the boot ROM image generator. Override with `$CC_FOR_BUILD`.

## CI & Releases

- GitHub Actions (`.github/workflows/build.yml`) builds firmware + packages release ZIPs on every push/PR.
- **CI does NOT run Vivado.** HDL changes must commit updated `.bit` files under `bootimage_work/`.
- Tag a release: `git tag -a v2.3.0 && git push origin v2.3.0`. Tags with `-` (e.g., `v2.3.0-rc1`) become pre-releases.
- CI builds one firmware flavor. The former `ns-pal` flavor is replaced by `ZZ9000.CFG` on the SD card (`videocap_mode = pal` + `nonstandard_vsync = pal`); the `DEFAULT_NS_VIDEOCAP` flag still works for manual builds.
- Do not reintroduce firmware-flavor release variants for PAL/native video, scanlines, INT2, MAC, or HDF selection; those are `ZZ9000.CFG` settings.

## Testing

```bash
make -C test/rtg test              # RTG correctness regression
make -C test/rtg bench             # Host micro-benchmarks (comparative only)
make -C test/video test            # VDMA math + video_formatter source invariants
make -C test/config test           # ZZ9000.CFG parser/loader unit tests
make -C test/scheduler test        # dual-core queue, routing, and reclaim tests
make -C test/video_codec test      # pl_mpeg streaming fixture + exact YUY2 output
test/video/run_formatter_sim.sh current   # xsim functional sim (Vivado machine)
```

On the Windows Vivado machine, Docker Desktop can block xsim's localhost
`PrivateChannel` handshake. If the sweep reports that error, stop Docker
Desktop, terminate stale `xsim.exe`/`xsimk.exe` processes, and rerun.

Any `video_formatter.v` change MUST pass the xsim sweep (pixel-exact, all
color modes/scales, calibrated against the pre-64-bit formatter) before a
bitstream is built from it. Hardware validation is still required for
performance/bus timing changes.

## Architecture Overview

| Path | Purpose |
|---|---|
| `mntzorro.v` | Zorro bus interface, register window, video capture, AXI bridge |
| `video_formatter.v` | AXI-Stream video formatter (64-bit VDMA stream in, 24-bit RGB out) |
| `ZZ9000_proto.sdk/ZZ9000OS/src/` | Bare-metal ARM firmware (C) |
| `ZZ9000_proto.sdk/ZZ9000OS/src/sdk_video_*` | Generic core-1 video sessions, decoder backends, packed-YUV staging |
| `ZZ9000_proto.sdk/ZZ9000FSBL/src/` | First-stage bootloader |
| `zz9000_project.tcl` | Vivado project/block design source |
| `bootimage_work/` | Committed FSBL (rebuildable: `./build_fsbl.sh`, see `ZZ9000_proto.sdk/ZZ9000FSBL/README.md`), bitstreams, BIF — canonical output dir |

### Cache-coherency gotcha (read before touching cache code)

The Zorro bridge's main AXI master enters the PS through **S_AXI_ACP** (cache-coherent,
snoops/allocates L1/L2), while the GEM ethernet, VDMA, and audio formatter DMAs use
non-coherent HP/dedicated ports straight to DDR. The **full L1+L2 flush in the video
ISR** (`video.c`, `isr_video`) is what keeps these two worlds coherent — it is NOT just
for ARM-written framebuffer data. Making it conditional broke ethernet RX (the Amiga
read stale L2 lines instead of fresh GEM-written frames; June 2026). The fact that a
buffer is mapped uncached for the ARM does **not** protect it: the FPGA's ACP
transactions hit L2 regardless of the ARM's MMU attributes.

### Video pipeline / PS-config gotchas (read before touching the BD or formatter)

- **Committed FSBL vs `PCW_*` config:** `bootimage_work/FSBL_exec.elf` bakes in the
  ps7_init register writes from the design it was built against. Editing `PCW_*`
  parameters in `zz9000_project.tcl` does NOT change what the FSBL programs at boot.
  The HP0 32→64-bit widening (2026-07) corrupted half of every 64-bit VDMA beat this
  way ("every other pixel column garbage, all modes") until `video_hp0_bus_width_init()`
  in `video.c` started forcing the AFI0 width at runtime. Clock/DDR/MIO `PCW_*` changes
  cannot be fixed like that — they need an FSBL rebuild: `./build_fsbl.sh`, with
  `util/refresh_ps7_init.sh` + table validation via `util/compare_ps7_tables.py`
  (full procedure: `ZZ9000_proto.sdk/ZZ9000FSBL/README.md`).
- **Z3 fast-RAM owns DDR 0x20000000–0x30000000:** VARIANT_Z3_FASTRAM bitstreams map the
  256 MB fast-RAM PIC to that fixed range (`Z3_FASTRAM_ARM_BASE` in `mntzorro.v`,
  asserts in `memorymap.h`). Never carve ARM-side regions there. Bitstreams older than
  2026-07 mapped fast RAM at an autoconfig-placement-dependent offset instead — with the
  canonical A3000/A4000 layout it landed on 0x101f0000–0x201f0000, silently overlapping
  the task queue and core-1 stack.
- **Stale Vivado projects build stale RTL:** the project imports copies of the Verilog
  sources; running synthesis against an existing `ZZ9000_proto/` silently builds the
  code from when the project was generated. Always build via `build_bitstream.sh` /
  `build_bitstream.ps1` (they delete and regenerate the project first). To confirm what
  actually got synthesized, check the module's OOC run log, e.g.
  `ZZ9000_proto/ZZ9000_proto.runs/zz9000_ps_video_formatter_0_0_synth_1/runme.log`.
- **Formatter read-latency phase lock:** in `video_formatter.v`, `pixout32` must remain
  a direct wire from the line-buffer BRAM output (`READ_LATENCY_B(1)`). Adding a register
  stage shifts the `counter_subpixel` byte/halfword unpack phase and swaps/duplicates
  pixel columns in the 8/15/16 bpp modes even though 32 bpp still looks fine. The xsim
  sweep catches this; run it.

## Board / Bitstream Variants

7 hardware/autoconfig variants controlled by Verilog `` `define `` blocks in `mntzorro.v`:
`zorro3`, `zorro3-nofast`, `zorro2`, `zorro2-2mb`, `a500`, `a500-2mb`, `a500plus`.

Build with `./build_variant_bitstreams.sh` (Vivado machine only). The script rewrites the define block in `mntzorro.v`, builds, copies the `.bit`, then restores the source. Bitstream outputs live under `bootimage_work/variants/`. These are still required for releases; the removed matrix is the extra firmware flavor matrix, not the board bitstream matrix.

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
