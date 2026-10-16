# Local RasterIX patches

The vendored closure derives from RasterIX superproject `9269a01c9bd4c3342bfa70ef8e3cdd751c15c6d0` (Float submodule `060bde1fc3c480f46495b740ddbe2bdf24958491`). The three files below intentionally differ from those blobs; every other file in `RasterIX`, `Float`, and `3rdParty` remains byte-identical and is checked by `test/rasterix/run.sh`.

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
