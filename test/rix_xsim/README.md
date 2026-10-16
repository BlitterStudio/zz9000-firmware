# RasterIX XSim scenes

`rix_xsim_gen.c` emits IF and EF real ZZ9000 RasterIX command streams for
`tb_zz9k_rasterix.sv` and `run_xsim.bat`, renders an analytic reference frame
per scene with the exact `Rasterizer.v` edge rules, and gates replay through
`compare_frames.py` full-image comparison via `run_scenes.sh` (builds the
generator in gcc:13, replays `probe s1 s2 s3 s4`, fails on any mismatch).

| Scene | Workload | TMU0 state |
| --- | --- | --- |
| `s1` | Clear and per-band colour commit | Enabled with an 8x8 white texture so the renderer configuration is consistent with draw scenes. |
| `probe` | Asymmetric band-protocol probe: right triangles top-left/bottom-right and a 1-px vertical line crossing every band boundary. | Enabled with the 8x8 white texture; texenv uses the primary colour. |
| `s2` | Flat-shaded green triangle | Enabled with the 8x8 white texture; texenv replaces with primary colour. |
| `s3` | Textured triangle | Enabled with the deterministic 8x8 white texture. |
| `s4` | 48-triangle radial stress scene | Enabled with the deterministic 8x8 white texture. |

The `ef-s1`–`ef-s4` counterparts set the same 640×480 RGB565 color target and
16-bit depth target, but execute a single full-target color/depth `MEMSET` and
draw directly to DDR.  The frame ends with an upstream-style `COMMIT` flush
(`Renderer::addCommitFramebufferCommand`); in EF that command only drains the
write pipeline and acts as a no-op frame boundary.  The scenes emit no band
offsets or `READ`.  Select the implementation with `run_xsim.bat ROOT OUT SCENE LATENCY ef`.

The DDR model accepts one render read or write transaction at a time.  It
deasserts read-address readiness every fifth cycle, write-address readiness
every sixth, and write-data readiness every fourth; each read beat has the
selected `LATENCY` idle cycles before becoming valid.  Cycle reports are at
the 100 MHz renderer clock, so `LATENCY=2` is the nominal model and a larger
latency is a shared-DDR sensitivity bound, not a board bandwidth measurement.

TMU0 MUST remain enabled for every scene that reaches the pixel pipeline.
`TextureMappingUnit.v:275-339` always sends and joins the texture-read
handshake before final colour selection, while
`RasterIXRenderCore.v:546-571` only enables the external `TextureReader` when
TMU0 and a texture source are configured. A MiniGL state mapper therefore must
provide a valid texture state/page and retain a texture texenv source even for
flat-primary-colour rendering.

## Captured MiniGL scenes

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

The harness maps MiniGL's `0x07820000` color and `0x07b20000` depth buffers
to compact backing arrays, and maps the capture's 24 MiB texture pool from
`0x00380000`; the sparse preload represents omitted bytes as zero. It writes
the PPM from the physical color location. Direct single-buffer `CMD`/`TEXTURE`
operation remains the S1–S4 path.
For a mixed CPU/FPGA frame, an optional `handoff.ddr` is a sparse target
snapshot written by the CPU fallback. The launcher applies it after captured
submission 1 and before submission 2, preserving the real target hand-off.
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
