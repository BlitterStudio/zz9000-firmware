# Local RasterIX patches

The seven files below intentionally differ from those blobs; every other file in `RasterIX`, `Float`, and `3rdParty` remains byte-identical and is checked by `test/rasterix/run.sh`.

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
