# RasterIX local patches

## XSim declaration-order compatibility

`test/rix_xsim/prepare_xsim_sources.py` copies `rtl/rasterix/` into the
simulation output directory. It names the depth-framebuffer generate block and
moves declarations before their uses in `FloatAdd.v`, `IntToFloat.v`, and
`RasterIXRenderCore.v`, required by Vivado 2018.3's Verilog parser.
(`TextureCacheDirectMappedContext.v` and `TextureReader.v` no longer need an
XSim-only edit: the lane-forwarding patch 0011 carries the declaration order and
the legal output-port connection directly.)

The transformations do not change widths, expressions, parameters, or
sequential behavior. The XSim runner compiles the copied files exclusively to
make the integration simulation possible on Vivado 2018.3. Synthesis uses
`rtl/rasterix/` as documented in `rtl/rasterix/PATCHES.md`.
