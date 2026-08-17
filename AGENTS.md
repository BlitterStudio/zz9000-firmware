# ZZ9000 Firmware — Agent Instructions

## What This Is

FPGA logic (Verilog) + bare-metal ARM firmware for the MNT ZZ9000 Zorro II/III graphics and coprocessor card. Xilinx Zynq-7020 SoC (dual Cortex-A9 + FPGA fabric). GPL-3.0-or-later.

## Matched Release Contract

Firmware, committed bitstreams, `zz9000-drivers`, and the SDK payload pinned by
the drivers form one release set. Coordinate changes to registers, capability
bits, mailbox services, Zorro II layout descriptors, or video-mode identities
across those repositories. Preserve capability-gated fallbacks for mixed
versions, but do not advertise a feature until the matched stack and applicable
hardware variants have been built and tested.

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
```

```powershell
.\build_bitstream.ps1             # clean-regenerates the Vivado project first
```

Then commit the resulting `.bit` file — CI cannot run Vivado. Never synthesize
from an old generated `ZZ9000_proto/` project.

**Release packaging:**
```bash
./build_release_assets.sh --tag vX.Y.Z   # → release/*.zip (one per hardware variant, one firmware flavor)
```

## Toolchain Gotchas

- **`arm-none-eabi-gcc`**: Must be the official Arm GNU Toolchain with **newlib**. Debian's `gcc-arm-none-eabi` uses picolibc and is **incompatible** with the Xilinx BSP. CI uses version `13.2.rel1`.
- **`bootgen`**: Not packaged by any distro. Build from <https://github.com/Xilinx/bootgen>. Script discovers via `$BOOTGEN`, then `$PATH`, then in-tree `bootgen/`.
- **Host C compiler** (`cc`): Needed to build the boot ROM image generator. Override with `$CC_FOR_BUILD`.

## CI & Releases

- GitHub Actions (`.github/workflows/build.yml`) builds firmware + packages release ZIPs on every push/PR.
- **CI does NOT run Vivado.** HDL changes must commit updated `.bit` files under `bootimage_work/`.
- Tag a release: `git tag -a vX.Y.Z && git push origin vX.Y.Z`. Tags with `-` (for example `vX.Y.Z-rc1`) become pre-releases.
- CI builds one firmware flavor. The former `ns-pal` flavor is replaced by the atomic `videocap_profile = filtered_pal_exact` setting in `ZZ9000.CFG`; the legacy `videocap_mode`, `videocap_shres`, and `nonstandard_vsync` keys remain parser compatibility only. The `DEFAULT_NS_VIDEOCAP` flag still works for manual builds.
- Do not reintroduce firmware-flavor release variants for PAL/native video, scanlines, INT2, MAC, or HDF selection; those are `ZZ9000.CFG` settings.

### `ZZ9000.CFG` contract

- Configuration is loaded from the SD card at cold boot; it is not a live
  settings channel. A changed file requires a full power cycle to take effect.
- `zz_config.c` is the source of truth for accepted keys. Keep its canonical
  key set and ordered `videocap_profile` values aligned with `ZZ9000.CFG`, this
  repo's README, and `zz9000-drivers/common/zzcfg_amiga.c`. Run the drivers
  repo's `tools/check-cfg-keys.sh` after changing any of them.
- Profile identities are append-only. Invalid values must leave the previous
  complete setting untouched; do not recreate independently mutable video-mode,
  resolution, and refresh controls.

## Testing

```bash
make -C test/rtg test              # RTG correctness regression
make -C test/rtg bench             # Host micro-benchmarks (comparative only)
make -C test/video test            # VDMA math + video_formatter source invariants
make -C test/config test           # ZZ9000.CFG parser/loader unit tests
make -C test/scheduler test        # dual-core queue, routing, and reclaim tests
make -C test/video_codec test      # pl_mpeg streaming fixture + exact YUY2 output
make -C test/palette test          # primary-CLUT shadow + big-endian query packing
test/video/run_formatter_sim.sh current   # xsim functional sim (Vivado machine)
```

Also run the affected suites under `test/allocator`, `test/aperture`,
`test/audio`, `test/fwupdate`, `test/media`, and `test/sd_activity_led` when
their contracts are touched. Every one of those directories has a `make test`
target.

On the Windows Vivado machine, Docker Desktop can block xsim's localhost
`PrivateChannel` handshake. If the sweep reports that error, stop Docker
Desktop, terminate stale `xsim.exe`/`xsimk.exe` processes, and rerun.

Any `video_formatter.v` or `video_overlay_linebuffer.v` change MUST pass the
full xsim sweep (currently 25 pixel-exact configurations, including centered
viewport transitions, all color modes/scales, and the pre-64-bit calibration)
before a bitstream is built from it. Hardware validation is still required for
performance, bus-timing, or HDMI-output changes.

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
- **Centered viewport is an atomic cross-domain contract:** control-plane Ops 28/29
  stage the origin and commit the complete rectangle; `OP_DIMENSIONS` bit 15 means
  canvas-only. Do not publish partially updated geometry, repurpose that bit, or bypass
  the frame-boundary handshake. Borders must remain black until a complete rectangle
  is installed in the pixel domain.
- **150 MHz is a release gate, not an estimate:** centered 1080p runs the formatter at
  150 MHz (6.667 ns). Keep the OOC constraint/hooks and the routed
  `verify_runtime_pixel_timing.tcl` gate intact; do not mask failing intra-clock paths
  with timing exceptions. A formatter build is acceptable only when both setup and
  hold pass before `write_bitstream`.
- **Centered output is capability-gated:** the exact 1280×1024-in-1080p profile is
  supported by the full-rate `zorro3`, `zorro3-nofast`, `zorro2`, and `zorro2-2mb`
  variants. Mixed or older components must fall back to the established full-frame
  60 Hz mode rather than leaving a centered request partially applied.

### Zorro II aperture contract (read before touching shared memory)

- Current Zorro II firmware, bitstreams, drivers, and SDK negotiate a
  generation-1 aperture-relative layout. The FPGA publishes the exact 2 MB or
  4 MB aperture, the RTG driver validates it against AutoConfig and reserves
  every region, and only then writes the ACK token. Firmware must keep the
  framebuffer limit, host heap, relocated `GFXData`, and PIP capability gated
  until that ACK. Invalid or unacknowledged generation-1 descriptors fail
  closed. Descriptor-absent legacy 4 MiB cards intentionally retain the
  historical fixed 64 KiB `HOST_WINDOW` compatibility path, but have no
  negotiated layout or PIP; legacy 2 MiB and unknown aperture sizes reject it.
- Do not restore fixed `board + size - ...` service addresses for
  generation-1 layouts or expose their historical shared heap through Zorro II.
  CPU-visible SDK clients use the negotiated host window; card-only rings stay
  outside the aperture. The 64 KiB template scratch is deliberately time-shared
  with Z2 `GFXData`, relying on the synchronous Zorro register command/ACK path.
- The shipped 2 MB profile has no PIP pool. The shipped 4 MB profile has one
  224 KiB fixed PIP pool and does not enable arbitrary P96 offscreen
  allocations. An 8 MB software profile exists, but there is no verified 8 MB
  AutoConfig define/build target or release bitstream; do not advertise it as
  a hardware variant.
- For the production-client matrix, shared-window limits, fallbacks, and
  hardware-qualification status, also read
  `zz9000-sdk/docs/zz9k-zorro2-services.md` in the matching SDK checkout.

### SD HDF storage contract

- `sd_storage.c` uses FatFs to open the configured HDF and resolve its cluster
  chain once, then serves normal Amiga block I/O as direct multi-sector reads
  and writes to the recorded physical extents. Do not replace that hot path with
  per-request `f_lseek`/`f_read`/`f_write`; fragmented multi-gigabyte images make
  it pathologically slow.
- The HDF must not be moved, resized, or reallocated while its extent map is in
  use. Corrupt/unsupported chains and maps exceeding the fixed extent table
  deliberately fall back to the legacy FatFs path.
- FatFs work triggered by register/interrupt paths must be deferred to the main
  loop. Firmware update and HDF access share the mounted filesystem; do not add
  concurrent filesystem calls from an ISR.

## Board / Bitstream Variants

7 hardware/autoconfig variants controlled by Verilog `` `define `` blocks in `mntzorro.v`:
`zorro3`, `zorro3-nofast`, `zorro2`, `zorro2-2mb`, `a500`, `a500-2mb`, `a500plus`.

Build with `./build_variant_bitstreams.sh` (Vivado machine only). On Windows,
run it from Git Bash with:

```bash
BITSTREAM_BUILDER="powershell -NoProfile -ExecutionPolicy Bypass -File ./build_bitstream.ps1" ./build_variant_bitstreams.sh
```

The script rewrites the define block in `mntzorro.v`, builds, copies the `.bit`,
then restores the source. Bitstream outputs live under
`bootimage_work/variants/`. All seven remain required for releases; the removed
matrix is the extra firmware-flavor matrix, not the board-bitstream matrix.

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
