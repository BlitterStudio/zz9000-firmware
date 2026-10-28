# RasterIX local patches

## XSim declaration-order compatibility

`test/rix_xsim/prepare_xsim_sources.py` copies `rtl/rasterix/` into the
simulation output directory. It names the depth-framebuffer generate block and
moves declarations before their uses in `FloatAdd.v`, `IntToFloat.v`,
`TextureCacheDirectMappedContext.v`, and `RasterIXRenderCore.v`, required by
Vivado 2018.3's Verilog parser. In `TextureReader.v` it replaces an illegal
output-port concatenation with an equivalent temporary wire and unpack
assignment, required by XSim's port-connect rules.

The transformations do not change widths, expressions, parameters, or
sequential behavior. The vendored
`rtl/rasterix/` tree remains byte-identical to RasterIX commit `9269a01c` and is
what synthesis uses. The XSim runner compiles the copied files exclusively to
make the integration simulation possible on Vivado 2018.3.
