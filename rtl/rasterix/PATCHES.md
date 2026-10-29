# Local RasterIX patches

The eight files below intentionally differ from those blobs; every other file in `RasterIX`, `Float`, and `3rdParty` remains byte-identical and is checked by `test/rasterix/run.sh`.

## FloatToInt zero-shift rounding

- File: `Float/rtl/float/FloatToInt.v`
- Upstream blob: `f9cb6366`
- Local blob: `523b3b5f`
- Patch: `patches/0001-floattoint-zero-shift-rounding.patch`
- Evidence: real MiniGL gears capture had known fractional primary colours, but the `attr_color` signal was unknown. `shiftSize == 0` selected `number[-1]`; the xsim run then propagated X through pixel output. The patched run produced known primary colour values.
- Synthesis safety: this is a combinational condition within the existing clocked assignment and preserves all `shiftSize > 0` behavior. A no-shift conversion has no discarded rounding bit, so zero is the defined rounding input.

## TexEnv only accepts valid input

- File: `RasterIX/TexEnv.v`
- Upstream blob: `20ee2a24`
- Local blob: `46f98da3`
- Patch: `patches/0002-texenv-gate-select-on-valid.patch`
- Evidence: after the FloatToInt correction, sparse texture-cache response bubbles overwrote TexEnv operand registers with unknown values, even though `m_valid` was delayed from `s_valid`. Real gears capture then failed before framebuffer completion.
- Pipeline semantics: `ce` is `m_ready`, `s_ready` is also `m_ready`, and `m_valid` is `s_valid` delayed four enabled cycles. The SelectColor registers are the stage-0 payload for that valid token. A non-valid transfer has no output token and no payload that may legitimately replace them; gating only prevents payload mutation on bubbles and leaves every valid-cycle datapath unchanged.
- Synthesis safety: this adds `s_valid` to the existing clock enable; it does not change latency, ready propagation, or any valid-cycle expression.

## Texture-reader reset initialization

- File: `RasterIX/TextureReaderContext.v`
- Upstream blob: `3b40bd0e`
- Local blob: `0a0442a8`
- Patch: `patches/0003-texture-reader-reset-all-lanes.patch`
- Evidence: nearest filtering populates texel 00 only while the registered sample has four lanes. Before a linear request, uninitialized neighbour lanes propagated X through the real MiniGL capture path.
- Synthesis safety: adds reset values only; normal request, cache, and sampling behavior is unchanged.

## Disabled depth/stencil ports are idle

- File: `RasterIX/RasterIXCoreIF.v`
- Patch: `patches/0004-idle-disabled-depth-stencil-ports.patch`
- Evidence: the real gears capture stalled with crossbar port 2 selected and
  its adapter streaming forever at 8,192 beats. With
  `ENABLE_STENCIL_BUFFER=0`, the upstream else branch assigned undeclared
  `m_stencil_*` names, leaving the actual `m_stencilbuffer_*` port-2 signals
  undriven. The disabled depth branch had the equivalent error.
- Synthesis safety: every external disabled-buffer port is now tied to its
  defined idle value; enabled-buffer generate branches are unchanged.

## Internal framebuffer stream length is clamped

- File: `RasterIX/InternalFramebufferCommandHandler.v`
- Patch: `patches/0005-clamp-internal-framebuffer-streams.patch`
- Evidence: with 16,384 pixels of internal color memory and two pixels per
  stream beat, a 307,200-pixel COMMIT advertised 153,600 beats to the
  `AxisToAxiAdapter` but the truncated handler limit stopped at 6,144 beats.
  The adapter then held the command open indefinitely. The same mismatch
  occurs whenever the requested length does not fit the internal address
  width.
- Stream semantics: the command size is converted once to beats, clamped to
  the 8,192-beat internal memory capacity, and used both for the external
  address transfer (COMMIT and READ) and the internal COMMIT/MEMSET terminal
  condition. The one-bit-wider terminal count explicitly represents a full
  band, rather than overloading zero after address wrap.
- Synthesis safety: this changes only command-length bookkeeping and terminal
  comparison widths; the framebuffer datapath, RAM addressing, and valid/ready
  timing are unchanged.

## Command-parser reset initialization

- File: `RasterIX/CommandParser.v`
- Patch: `patches/0006-command-parser-reset-initialization.patch`
- Evidence: `colorBufferApply`, `depthBufferApply`, `stencilBufferApply`, and
  related command registers were uninitialized on reset. In simulation, their
  undetermined `1'bx` state propagated through `colorBufferApplied` and
  `framebufferFinished`, stalling the parser in `WAIT_FOR_IDLE` with
  `s_cmd_axis_tready = 0` and deadlocking the initial DMA stream.
- Synthesis safety: adds defined reset values (0) to output registers; command
  decoding and execution cycles are unchanged.

## EF framebuffer addresses are zero-extended

- File: `RasterIX/RasterIXCoreEF.v`
- Patch: `patches/0007-ef-zero-extend-framebuffer-addresses.patch`
- Evidence: `m_color_araddr`/`m_color_waddr` (and depth/stencil equivalents)
  are `FRAMEBUFFER_SIZE_IN_PIXEL_LG` bits wide while the `StreamFramebuffer`
  fragment ports are `ADDR_WIDTH` bits. Passing the narrow vector undriven the
  upper bits, which simulated as `Z`; `FramebufferMMU`'s
  `(addr << PIXEL_WIDTH_LG) + confAddr` then produced `X` addresses, so every
  rasterized fragment write was dropped and the EF triangle scenes rendered an
  all-clear framebuffer. Zero-extending the pixel index first makes the same
  scenes pass pixel checks.
- Synthesis safety: only the constant-zero upper address bits change; the
  pixel index and byte address arithmetic are unchanged.

## StreamConcatFifo holds drained output data

- File: `RasterIX/StreamConcatFifo.v`
- Upstream blob: `7805a39c`
- Local blob: `f0023cf6`
- Patch: `patches/0008-streamconcatfifo-hold-drained-output.patch`
- Evidence: the wire-v9 MiniGL blended_quad capture (two triangles with
  blending over a shared diagonal) committed unknown bytes to the framebuffer
  and aborted with "framebuffer contains unknown byte". Tracing the first X
  showed the joined stream's registered-read sfifo outputs go undefined once a
  fifo drains (unwritten BRAM slots); the per-fragment pipeline latches its
  input lines every ready cycle, so an idle gap inside the blended triangle
  stream contaminated the color blender's factor registers and produced X
  fragment colors on the first beats after the gap. The patched run renders
  the full 640x480 frame with zero coverage mismatches and zero pixels beyond
  one LSB (bit-exact against the CPU reference), and gears, textured_lightmap
  and the synthetic probe/s1-s4 scenes are unchanged.
- Stream semantics: AXIS requires stable data only during transfers. While a
  transfer is accepted the beat is steered combinationally from the fifo
  outputs as before; between transfers the last accepted beat is held on the
  lines instead of exposing the drained fifo's undefined BRAM read. Valid
  generation, the pop condition, and every per-stream fifo are unchanged.
- Synthesis safety: adds one hold register with reset and an output mux; no
  timing, latency, or handshake change on any accepted transfer.

## XACP G4C EF comparison and adopted read-credit fix

On 2026-10-29 the EF-sensitive files were blob-compared with Xanxi-Amiga's
published `XACP-ZZ9000` `2646a23` tree.  The local/XACP SHA-256 pairs are:

| File | Local | XACP |
| --- | --- | --- |
| `RasterIX_EF.v` | `fcbce77fdd3b93b8c2369fd1c9bf6917ce582b4683a6d200e16942c423573e5f` | `91336062f48183e099bacd2121884cf6ffffc952d159fa7cbf24ff6d1e7354de` |
| `RasterIXCoreEF.v` | `8e0efe4ba93ceb4c6c47fc5f2f957705c49fcf01aa366faffca34401fd232c7f` | `10023fbce176daad34860ee74bbff61bad3a7ec194dee2dc0aaf1ffbc769d8d0` |
| `FrameStreamingCore.v` | `6a2bf1948292b728f2bd0d7b8c7a0d5ffc759a8bbd5f261182c1e26c2bea0941` | `ce8fcfd492f36760540a35cc9e8cc936319ec37d779a90e6dc032198d05a3a51` |
| `CommandParser.v` | `6aa12d1d8104a08e4b0ac9d81616a0d9640b1448b47812fe442127cde1337220` | `2882f15f99d764d9b1392839bca98aecb489facfda70fccf14e97f2056866429` |
| `InternalFramebufferCommandHandler.v` | `bcf8757757b89d3f115c926818389172b00b89d26b55c5ecc0480e461239652f` | `853cee9950255486feb95cdf51c00e9ce9d58cd12b6cfb212a431840b2dd4419` |
| `AxisToAxiAdapter.v` | `03c93b8345ca5cc5c893943cd5c8ee0bb38e2e529ac7dc5da091866e4b435adc` | `686f4b2ab13ded10d7e61a571e46989c419ff61f6451966e46017dea03c2dc30` |

XACP is an older RasterIX snapshot and lacks this tree's external-texture
cache family, so its divergent EF files were not copied wholesale.  The
verified independent change is `RasterIX/XACPCommandReadCredit.v`
(`b80c25957169593eed5cb3270dad52cd1af2582ff7db53d4a69b00827dcb10be`
in XACP): it is GPL-3.0-or-later and is inserted after the 64-bit
FrameStreamingCore `commonAxiAdapter` and before crossbar port 0.  Its
64-beat reserved-response queue keeps list-read backpressure from holding the
fragment-memory read channel.  This preserves the newer external texture
cache and all local fixes above while taking the specific silicon-proven fix.

## EF memory coalescing stays disabled

The EF wrapper instantiates RasterIX with `ENABLE_MEMORY_COALESCING(0)`
(`rtl/zz9k_rix/zz9k_rasterix.v`).  Measured on the textured scene
(`ef-s3`, LATENCY=2, 2026-10-29):

| Configuration | Result cycles | Frame |
| --- | --- | --- |
| coalescing off (bypass) | 412,053 | bit-exact vs reference |
| coalescing on | 330,230 | corrupted: clear/exterior pixels 0x0000 |

The 20% cycle win is not usable: `Coalescer.v` hardwires `s_mem_axi_rlast =
1` ("we always return one beat") and `CoalesceAddrGen.v` fatals on any
incoming `axlen != 0`, so the merged EF framebuffer traffic silently loses
write data.  Making coalescing safe requires redesigning both modules for
multi-beat INCR transactions; the bypass is silicon-proven and is kept.
