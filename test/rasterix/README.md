# RasterIX Verilator performance harness

Run from Git Bash on the Windows workstation:

```bash
test/rasterix/run.sh
```

The script runs Ubuntu 24.04 in Docker, installs its packaged compiler, CMake and
Verilator, and prints their actual versions. The observed container versions are
GCC 13.3.0, CMake 3.28.3, and Verilator 5.020 (Debian package `5.020-1`). A named
Docker volume (`zz9000-rasterix-harness-cache`) retains only the disposable clone
and compiled model between runs; the worktree remains mounted read-only. The script
clones RasterIX at `9269a01c9bd4c3342bfa70ef8e3cdd751c15c6d0` with submodules, and
compares the vendored `rtl/rasterix/{RasterIX,Float,3rdParty}` dependency closure
byte for byte against that checkout. It substitutes that verified vendored closure
into the container build. The rest of the upstream tree supplies the upstream
`rtl/top/Verilator/top.v`, display support and C++17 GL frontend. A temporary,
audited copy of that top applies the ZZ9000 baseline: `RasterIX_IF`, 640x480,
16,384-pixel internal RGB565/Z16 bands, stencil disabled, bilinear filtering,
256-pixel maximum textures, 25-bit fixed-point interpolation and either one or
two TMUs. No vendored RTL is modified.
For regression triage only, `RASTERIX_PROFILE=upstream-verilator` selects
upstream's unpatched Verilator top and matching historical library settings;
`RASTERIX_REVISION=<commit>` checks out that revision in the disposable cache.
Non-pinned revisions deliberately do not use the vendored RTL closure.


Results and PNG frames are written to `test/rasterix/out/` by default (pass a
different directory as the sole argument). Each `results-tmu*.json` contains
Verilator clock cycles, fps calculated at exactly 100 MHz, command-stream bytes,
x86 container frontend/display-list time, 19 bands, average elapsed cycles per
band, and an RGB-frame FNV-1a-64 hash. The script prints the same per-scene,
per-TMU table. PNGs use those exact hashed RGB bytes.

## Workload model

All scenes use RasterIX's own C++ OpenGL frontend and Verilator bus connector
protocol; they do not feed private triangle descriptors.

| Scene | Workload |
| --- | --- |
| `fill` | One full-screen bilinear-textured quad, the fill-rate ceiling. |
| `quake_world` | 2,048 deterministic depth-tested textured triangles. Their isosceles screen-space bases/heights are uniformly 16..40 pixels, giving approximately 384 covered pixels each before clipping, or approximately 2.56x 640x480 screen coverage. This is the explicit [INFERENCE] GLQuake `timedemo demo1` world-pass model: a few thousand mostly small BSP triangles plus lightmaps. One TMU renders base then a `GL_EQUAL`/`GL_DST_COLOR,GL_ZERO` lightmap pass; two TMUs use one modulate pass. |
| `particles_hud` | 256 alpha-blended 12..46-pixel sprite quads plus two alpha HUD quads. |
| `mipmapped_floor` | A perspective 32x32 grid (2,048 triangles) with repeating trilinear-mipmapped texture coordinates. |

The reported band value is `ceil(640*480/16384) = 19`. `average_cycles_per_band`
is total frame elapsed cycles divided by those 19 internal-framebuffer bands; it
is an observed per-band-inclusive cost, **not** an isolated RTL band-switch
latency. RasterIX does not expose a separate band-transition signal at this
upstream pin.

## Model boundary

This is a cycle model of the pinned RasterIX RTL plus its C++17 GL command
frontend. It provides a reproducible command-byte and output-frame oracle for a
future ZZ9000 display-list generator. `frontend_ms_x86` is deliberately labeled
as a container x86 measurement; it is not a Cortex-A9 estimate. RasterIX's
Verilator top uses an in-model AXI RAM. Therefore the model does **not** include
real DDR arbitration/contention, ARM cache maintenance, the Zynq HP-port AXI
latency, Zorro DMA/control-ring delivery, ZZ9000 scanout/capture/audio traffic,
or post-route timing. It is a go/no-go throughput model, not A4000 hardware FPS.
