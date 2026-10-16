# RasterIX XSim scenes

`rix_xsim_gen.c` emits four real ZZ9000 RasterIX command streams for
`tb_zz9k_rasterix.sv` and `run_xsim.bat`.

| Scene | Workload | TMU0 state |
| --- | --- | --- |
| `s1` | Clear and per-band colour commit | Enabled with an 8x8 white texture so the renderer configuration is consistent with draw scenes. |
| `s2` | Flat-shaded green triangle | Enabled with the 8x8 white texture; texenv replaces with primary colour. |
| `s3` | Textured triangle | Enabled with the deterministic 8x8 white texture. |
| `s4` | 48-triangle radial stress scene | Enabled with the deterministic 8x8 white texture. |

TMU0 MUST remain enabled for every scene that reaches the pixel pipeline.
`TextureMappingUnit.v:275-339` always sends and joins the texture-read
handshake before final colour selection, while
`RasterIXRenderCore.v:546-571` only enables the external `TextureReader` when
TMU0 and a texture source are configured. A MiniGL state mapper therefore must
provide a valid texture state/page and retain a texture texenv source even for
flat-primary-colour rendering.

## S4 XSim classification

S4 exposes an upstream RasterIX simulation defect, not an external DDR-model
or command-stream X. At cycles 70051–70064, `TMU0.step2_texel00` is `ffff`
while `step2_texel01`, `step2_texel10`, and `step2_texel11` are X. At cycle
70064, `TexEnv` emits `xxxxxxff`; `PixelPipeline` commits that colour at
cycle 70068 for `(279,91)`, and the colour buffer later writes it to DDR.
The attribute colour and UV are known (`00ff00ff`, `(0,0)`).

The uninitialised values are the unneeded neighbour slots in
`TextureReaderContext.v:47-50`. With nearest filtering,
`TextureReaderController.v:71-76` deliberately treats neighbours as cache
hits, so it fills only texel 00. `TextureReaderContext.v:76-89` nevertheless
copies all four registers when it samples; registers 01/10/11 have no reset.
`TextureFilter.v:115-134` correctly selects texel 00 for nearest filtering,
but `TexEnv` can still observe the stale neighbour-derived pipeline state
during the sparse radial stream. This is an RTL valid/data hazard, not a
defined Xilinx power-up value; a production fix belongs upstream in the
TextureReader/TexEnv pipeline. The testbench retains the full-frame X check
and reports the condition rather than treating it as a simulator artefact.
