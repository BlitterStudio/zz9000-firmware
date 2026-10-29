# RasterIX XSim scenes

`rix_xsim_gen.c` emits RasterIX EF command streams for
`tb_zz9k_rasterix.sv` and `run_xsim.bat`, renders an analytic reference frame
per scene with the exact `Rasterizer.v` edge rules, and gates replay through
`compare_frames.py` full-image comparison via `run_scenes.sh` (builds the
generator in gcc:13, replays every scene below over MMIO, `SHARED=1` and
`DMA_BURST=1`, replays `ef-stress` with three randomized seeds, then replays
every scene through the real FIFO command path (see below), and fails on any
mismatch).  `run_scenes.sh OUT LATENCY stream` runs only the stream-level
matrix, `run_scenes.sh OUT LATENCY fifo` (or `run_fifo_path.sh [OUT]
[LATENCY]`) only the FIFO path.

| Scene | Workload | TMU0 state |
| --- | --- | --- |
| `ef-s1` | Full color/depth clear | Enabled with an 8x8 white texture so the renderer configuration is consistent with draw scenes. |
| `ef-probe` | Asymmetric direct-DDR probe: right triangles in opposing corners and a one-pixel vertical line. | Enabled with the 8x8 white texture; texenv uses primary colour. |
| `ef-s2` | Flat-shaded green triangle | Enabled with the 8x8 white texture; texenv replaces with primary colour. |
| `ef-s3` | Textured triangle | Enabled with deterministic 8x8 white texture. |
| `ef-s4` | 48-triangle radial stress scene | Enabled with deterministic 8x8 white texture. |
| `ef-lm` | Two-pass lightmap: white textured base, then `ZERO,SRC_COLOR` with the uniform `0x8410` lightmap page | 8x8 white page, then the 8x8 lightmap page. |
| `ef-stress` | Read-channel stress: 40 large overlapping triangles in shuffled depth order (`LESS`, depth writes), then six disjoint `ZERO,SRC_COLOR` lightmap triangles with the depth test still on | 256x256 RGB565 texture of uniform 64x64 blocks (zero/full channels, UVs 4 texels inside each block) so the texture cache keeps missing; then the 8x8 lightmap page. |

Each scene sets the 640×480 RGB565 color target and Z16 depth target, then
performs a single full-target color/depth `MEMSET` and renders directly to
DDR.  No scene emits IF bands, offsets, `READ`, segment `MEMSET`, or `COMMIT`.
The testbench submits each DSE packet through the FIFO TX stream model; the
`DMA_BURST` plusarg uses the same committed packet boundaries as a fixed-
destination PL330 producer.

The default DDR model accepts one render read or write transaction at a time.
It deasserts read-address readiness every fifth cycle, write-address
readiness every sixth, and write-data readiness every fourth; each read beat
has the selected `LATENCY` idle cycles before becoming valid.  Cycle reports
are at the 100 MHz renderer clock, so `LATENCY=2` is the nominal model and a
larger latency is a shared-DDR sensitivity bound, not a board bandwidth
measurement.

`SHARED=1` replaces the read model with the PS HP-port shape: up to 27
accepted read addresses queue in front of one response stream that never
interleaves bursts, so a crossbar port that refuses a beat blocks every
response behind it.  `RAND_SEED=n` (nonzero) adds 0..`RAND_LATENCY` (default
16) extra cycles before every beat and serves next the oldest queued request
with the ID of a randomly chosen one, reordering responses across IDs as AXI
allows.  The EF wrapper has no command-read master; the testbench ties the
legacy model's master side idle and fails on any unknown renderer AXI
handshake.  When the renderer is busy and no memory or command handshake
happens for `DEADLOCK_LIMIT` cycles (default 200000; 0 disables), the run
prints the presented read beat's owning crossbar port, the queued responses,
the last 48 accepted read addresses and the per-port and fragment-pipeline
handshakes, then fails; the same dump accompanies a completion timeout.
`TRACE_AXI=1` adds a 250000-cycle counter heartbeat and `TRACE_AXI=2` prints
every accepted read address.  All of these are `run_xsim.bat` environment
variables.

TMU0 MUST remain enabled for every scene that reaches the pixel pipeline.
`TextureMappingUnit.v:275-339` always sends and joins the texture-read
handshake before final colour selection, while
`RasterIXRenderCore.v:546-571` only enables the external `TextureReader` when
TMU0 and a texture source are configured. A MiniGL state mapper therefore must
provide a valid texture state/page and retain a texture texenv source even for
flat-primary-colour rendering.

## Real FIFO command path

The stream-level harness feeds the RasterIX command stream directly.  With
`FIFO_PATH` defined, the testbench instead drives the real EF command ingress
from an AXI3 GP1 master: `ps7_0_axi_periph` (AXI3 to AXI4-Lite protocol
converter, crossbar, and the clock converters to the 100 MHz RasterIX clock on
M07/M08) and `gpu_cmd_fifo` (`axi_fifo_mm_s`), whose TX stream feeds the
wrapper.  GP1 runs at 25 MHz.  `fifo_path/fifo_path.tcl` builds that block
design from `zz9000_project.tcl --rasterix --rasterix-variant ef`, copying both
cells from the real `zz9000_ps` design; it fails unless the copied
interconnect matches the real one cell for cell and configuration for
configuration.  CSR accesses go through M07 at the real `0x83c60000` window,
FIFO accesses through M08 at `0x83c70000`.

The generator also writes `<scene>.fifo`: the exact FIFO packets
`rix_drv.c` commits for the scene (`P <words> <dma-first> <dma-words>` plus
the TDFD words), including which payload the firmware hands to its PL330
callback.  The testbench waits for `TDFV`, stages each packet, commits it with
`TLF`, and fails if `ISR` then reports `TSE` or `TPOE`; it also checks every
accepted stream beat and its `TLAST` against the committed packets and that
every committed word streamed.  `FIFO_TRANSPORT` selects how the payload
reaches `TDFD`:

| Transport | GP1 traffic |
| --- | --- |
| `mmio` | One single-beat INCR store per word, each waiting for its response (strongly ordered CPU stores). |
| `pl330` | The firmware's PL330 program: single-beat FIXED stores, posted back to back, then the DMAWMB wait before `TLF`. |
| `burst16` | 16-beat FIXED bursts (shorter tail), posted, then the wait before `TLF`. |
| `pl330-early` | The pre-fix completion race: `TLF` written after the header words, before the DMA payload.  The run passes only if `ISR.TSE` is set and nothing streamed, the failure mode observed on hardware. |

`run_scenes.sh` (or `run_fifo_path.sh`) builds the design once with
`fifo_path\run_fifo_path.bat build ROOT WORK` (Vivado 2018.3 batch, about
two minutes) into `OUT/fifo_path_work`, then runs `run_fifo_path.bat run
WORK OUT SCENE LATENCY TRANSPORT` for every scene and transport.  Frames land
in `OUT/<scene>_fifo-<transport>.ppm`; each must pass `compare_frames.py`
against the analytic reference and equal the `fifo-mmio` frame (and, in the
full matrix, the stream-level MMIO frame) byte for byte.

## Captured MiniGL scenes

Build real wire-v9 captures with `make -C test/minigl_v9 rix_v9_capture`, then
run `test/minigl_v9/rix_v9_capture OUT`. It emits `blended_quad`, `gears`,
`textured_lightmap`, `plain_textured`, and `glquake_like` directories
containing `commands.hex`, `ddr.hex`, and `cpu.ppm`; replay each directory
with `run_xsim.bat ROOT OUT SCENE LATENCY`.

`plain_textured` is one unit-0 texture draw. `textured_lightmap` is the
two-pass lightmap mapping (unit 0, then `GL_EQUAL` with `DST_COLOR,ZERO`).
Both use uniform non-white texels: RasterIX's fixed-point texture
interpolation differs from the float CPU reference by up to 0.083 texel, so
patterned nearest-filtered texels flip only in pixels whose centres lie that
close to a texel edge.  Uniform texels measure fetch, blend and coverage
without that edge band.  RasterIX stores textures as RGB565, so modulated
results can differ from the RGBA8888 CPU reference by one LSB.

`glquake_like` is one frame: an accelerated two-pass lightmapped world, then
an opaque alias model with polygon offset, alpha-blended particles with
depth writes off, and an alpha-tested HUD.  The alias fallback holds the rest
of the frame on the CPU.  The capture asserts one RasterIX submission, one
hardware and three software draws, with polygon offset as the first reason.
The EF replay ends at the CPU hand-off, so compare `frame.ppm` with
`cpu_world.ppm`.  Then run `rix_v9_capture --compose OUT/glquake_like`: it
loads the replayed `frame.ppm` into the live RGB565 target and finishes the
frame through the production drain/convert and CPU fallback path into
`composite.ppm`, which is compared with `cpu.ppm`.

`run_xsim.bat ROOT OUT SCENE LATENCY` automatically selects capture mode when
`OUT\SCENE\commands.hex` (or the singular `command.hex`) and `ddr.hex` exist.
It passes `CMDS`, `DDR`, `PPM`, and `SCENE` to the testbench; the output is
`OUT\SCENE\frame.ppm`. For example:

```bat
run_xsim.bat D:\Github\zz9000-rasterix-fit D:\captures cleared 2
```

`commands.hex` contains one little-endian 32-bit DSE word per line. It may
concatenate submissions: every buffer starts with
`90000000 | parser_byte_count`, followed by its zero address word and parser
words. The harness loads the words contiguously from `0x00100000` and submits
each buffer in order after the prior completion.

`ddr.hex` is a sparse, `$readmemh`-style byte file. Use `@` plus an eight-digit
physical byte address on its own line, then one two-digit byte value per line;
the address advances after each value. Unspecified bytes are zero:

```text
@00380000
ff
00
```

The host capture serialiser rewrites only RasterIX color/depth register
operands from host heap pointers to the harness's `0x07820000` color and
`0x07b20000` depth aliases, retaining the backend's commands and per-band
offsets unchanged. The harness maps those targets to compact backing arrays
and maps the capture's 24 MiB texture pool from `0x00380000`; the sparse
preload represents omitted bytes as zero. It writes the PPM from the physical
color location. Direct single-buffer `CMD`/`TEXTURE` operation remains the
S1–S4 path.
For a mixed CPU/FPGA frame with RasterIX work after a CPU hand-off, an
optional `handoff.ddr` is a sparse target snapshot written by the CPU
fallback. The launcher applies it after captured submission 1 and before
submission 2, preserving the real target hand-off.
`compare_frames.py cpu.ppm frame.ppm diff.png comparison.json` quantizes the
CPU RGBA reference to RGB565 before comparing it. Its pass limits are a
one-LSB channel maximum, mean at most 0.02 LSB, and at most 0.25% of pixels
either beyond one LSB or coverage mismatches. RGB565 quantization itself is
lossless under this comparison; the nonzero aggregate allowance is reserved
for a one-pixel raster edge band, not broad state, texture, or byte-order
errors.



## Texture-reader and conversion regressions

The captured MiniGL scenes exercise three RTL boundary cases that the original
synthetic scenes did not. `TextureReaderContext` emits a four-texel sample
even when nearest filtering loaded only texel 00; all lanes are reset so
unused neighbours cannot become unknown pipeline data. `TexEnv` advances its
operand registers only on a valid input transfer, preserving data alignment
through sparse texture-cache response streams. `FloatToInt` treats a zero
right shift as having no discarded rounding bit rather than indexing bit `-1`;
this occurs when converting normalized colour values in the `[0.5, 1.0)`
range. The full-frame capture mode rejects any framebuffer unknowns, so all
three regressions are covered by the real service streams.
