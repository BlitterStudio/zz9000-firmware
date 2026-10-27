# Pixel-domain retiming evidence

The paths below recur in the 20-worst setup/hold reports from the master,
ExtraTimingOpt, Default, Explore, and zorro3-default placements. Slack is the
reported positive pre-change margin, not a budget. The report names in the
first column identify the measurement source.

| Path and pre-change slack | Change | Latency and alignment contract | Simulation proof |
| --- | --- | --- | --- |
| `overlay_linebuffer/pixel_ready_line_reg -> overlay_draw_delay_reg_srl8`, ExtraTimingOpt setup +0.084 ns | Register the line-tag match as `displayed_line_ready`. | Adds one line-ready stage; `OVERLAY_FILTER_DELAY`/`OVERLAY_PIPE_DELAY` include it with the RGB path. | `run_overlay_scale_sim.sh` and the 31-case formatter sweep pass. |
| `overlay_pos_x -> overlay_read_addr_even`, zorro3-aga Default setup +0.202 ns | Stage the horizontal source position before address and geometry arithmetic. | Adds one source-position stage, included in `OVERLAY_FILTER_DELAY` and all parallel overlay delay lines. | Overlay scale and 31-case formatter sweeps pass. |
| `vga_viewport_x -> counter_scanout`, master +0.171 ns; ExtraTimingOpt +0.222 ns | Precompute viewport bounds and left-border state. | No raster latency; the same inclusive/exclusive predicates consume registered bounds. | The 31-case formatter sweep, including viewport cases, passes. |
| `counter_x -> fractional_y_error`, zorro3-default setup +0.186 ns | Precompute the line-update counter value from `vga_h_max`. | No raster latency; `counter_x == vga_h_max - 1` is evaluated with the precomputed RHS. | The 31-case formatter sweep passes. |
| `watch2_active -> good_intervals`, master setup +0.124 ns | Register interval plausibility before anchor state consumes it. | Source-sync state only; no RGB/sync/DE latency. | Controller and progressive/interlace formatter integrations pass. |
| `lines_since_anchor -> lace_delay_threshold`, ExtraTimingOpt setup +0.302 ns | Register the watchdog-expired predicate before lace-estimator reset selection. | Source-sync state only; no RGB/sync/DE latency. | Controller and progressive/interlace formatter integrations pass. |
| `overlay_pixel/s3_g -> out_rgb`, ExtraTimingOpt hold +0.008 ns | Add a converter-output register for RGB and draw together. | Adds one pixel stage; RGB and draw advance together and formatter delay lines include it. | Overlay scale and 31-case formatter sweeps pass. |
| `vga_overlay_y_step -> overlay_pos_y`, ExtraTimingOpt hold +0.014 ns | Capture overlay start/step values at frame setup. | No steady-state raster latency; position uses the frame-consistent captured configuration. | The 31-case formatter sweep passes. |
| `vga_h_rez -> vga_h_rez_delayed`, zorro3-aga Default hold +0.044 ns | Pipeline the base delayed resolution before overlay-delay selection. | No active-raster latency after mode setup; only the frame-setup value is staged. | The 31-case formatter sweep passes. |

## Frame-setup retiming

The 20-worst setup reports from all five `652cba73` Zorro III placement
attempts and the passing Zorro III AGA placement show the following recurring
configuration paths.  The Zorro III AGA report's 20 worst paths are all at
least `+0.212 ns`; the table records its applicable range as a placement
cross-check.  These are frame or line setup terms, so they can be prepared
before their once-per-line or once-per-frame consumers without changing the
RGB/sync/DE pipeline.

| Recurring path family | Zorro III setup range before change | Zorro III AGA range | Retiming |
| --- | --- | --- | --- |
| `watch{1,2}_last -> cfg_line_clocks`, `cfg_nominal_len`, `cfg_last_pixel`, and `cfg_ext_max` enables | `+0.036` to `+0.142 ns` | `>= +0.350 ns` | Capture adopted geometry on every frame boundary; only `cfg_adopted` records whether that data may engage source sync. |
| `watch{1,2}_last -> cfg_last_line` and `cfg_vsync_end` enables | `+0.127 ns` | `>= +0.380 ns` | Use the same always-captured frame-geometry bundle. |
| `watch{1,2}_last -> good_intervals` enable | `+0.067 ns` | `+0.350 ns` | Register the live/adopted re-arm predicate before it reaches lock-state enables. |
| `vga_scale_source_rows -> fractional_y_error` and its accumulator feedback | `+0.032` to `+0.117 ns` | `+0.385` to `+0.489 ns` | Calculate the next fractional accumulator state during the first two clocks of the line and commit it at the existing line-update edge. |
| `vga_overlay_y_start -> overlay_row_start_sample` | `+0.031` to `+0.117 ns` | `>= +0.390 ns` | Stage the frame-start position and row limit before the floor/edge/weight calculation. |
| `vga_viewport_height -> dvi_rgb` reset mux | `+0.072` to `+0.111 ns` | `+0.212` to `+0.368 ns` | Precompute the vertical viewport end, mirroring the existing horizontal-end register. |

The source-sync validity predicate remains `cfg_adopt_now` at the frame wrap,
so the established quiet window is unchanged. Geometry data is captured
unconditionally only while source sync is disengaged or re-arming; the
registered re-arm predicate removes the wide live/adopted comparison from
lock-state enables. Fractional scaling snapshots its next state after
`counter_x == 0` and commits it on the same `fractional_line_update_x` edge
as before. The overlay start candidate is captured from VSYNC through the
frame wrap, and the viewport-end register is the same sum that previously
fed the active-pixel comparison directly.

`video_overlay_filter` has one additional interpolation stage between the
horizontal sum and vertical interpolation. The formatter changes
`OVERLAY_FILTER_DELAY` from 9 to 11 and `OVERLAY_PIPE_DELAY` from 13 to 16,
covering the staged source position, the filter stage, and converter-output
stage; RGB, draw, sprite, palette select, split, and sync delay lines retain
their relative timing.

## Formatter OOC timing

Fresh, isolated formatter OOC synthesis at `fe6d813b` completed on
2026-10-19 19:55:32 +0200 with formatter-pixel WNS/TNS/WHS
`-0.793/-6.342/+0.164 ns` (8 setup endpoints, 5520 timed pixel endpoints).
The fresh head run completed on 2026-10-19 19:57:46 +0200 with
`-0.793/-6.342/+0.164 ns` (8 setup endpoints, 5608 timed pixel endpoints).
Thus this OOC constraint has not improved its aggregate WNS/WHS; its evidence
is the recurring placed-design path table above, not a claimed OOC aggregate
improvement. `report_formatter_ooc_timing.tcl` now writes beside the OOC run
and `verify_formatter_ooc_timing.tcl` reads that exact run directory, so the
fresh head report passes the generated-Tcl and 6.667/10.000 ns clock checks.

## I2S LRCLK timing

The first retiming attempt passed LRCLK through
`audio_clock.sampled_lrclk`.  The ADAU1701 changes LRCLK 5–40 ns after BCLK
falls (`tSODM`, [Table 7, Serial Input/Output Ports](https://www.analog.com/media/en/technical-documentation/data-sheets/ADAU1701.pdf)),
so that IOB register changed at the following BCLK rising edge and shifted the
TDM frame by one serial bit.  Hardware validation rejected it: MHI playback
was loudly distorted.

`audio_clock` regenerates transmitter LRCLK. Kept LUTs and a fabric
falling-edge sampler keep the zero-minimum `tSODM` edge arriving after F0
even with pad-to-pad skew; the sample at F1 therefore has about half a BCLK
of setup margin and identifies the prior boundary. A free-running 256-bit
phase counter predicts subsequent edges, and the transmitter receives only
the falling-edge registered output, itself buffered past a coincident
`clkgen[2]` rising edge by kept output LUTs so the transmitter resolves the
previous value exactly as it does with the codec's `tSODM`-delayed LRCLK.
Only a low transition preceded by a nominal half-frame of continuous LRCLK
high (96..160 BCLK) steers the lock; wrong-format transitions, glitches and
the first edge after a long idle are ignored, and a candidate timeout of 512
BCLK drops the lock when the codec stops framing. Three consistent qualified
frames acquire the lock; three consecutive phase mismatches or the timeout
release it. Before lock, the output is held high so playback cannot begin
with a malformed frame. The capture bridge continues to use its independent
rising-edge LRCLK/SDATA samples.

The registered-LRCLK trial's reported `+1.118 ns`, `+35 ns`, and `+29 ns`
margins were from disconnected pins and are not timing evidence. The
regenerator's connected transmitter path has roughly `+1.1 ns` setup
margin in the qualified implementation.

`audio_clock_i2s_tx_tb.sv` drives the production Xilinx transmitter and a
direct-wiring reference through the firmware's boot sequence: absent
clocks across PS reset release, static-high LRCLK with BCLK running, a
64-BCLK wrong serial format before the 0x081E write, TDM8 lock, a
mid-frame glitch pulse, a phase-shifted mid-stream codec restart, and the
full second init pass.  Every segment must end locked with the
regenerated LRCLK frame-aligned to the codec and the transmitter
consuming AXIS words; the first two segments also require bit-exact
SDATA against the direct reference.  The sweep covers tSODM 0, 5, 20, 35
and 40 ns crossed with +-0.4 ns BCLK/LRCLK insertion skew (15 runs, all
passing, three relocks and 61 rejected transitions each).  A negative
topology run holds `i2s_transmitter_0/sclk_in` low; it reproduces the
locked regenerator with no AXIS consumption.  The post-synthesis wiring
gate requires `I2SO_LRCLK -> audio_clock_0/lrclk_in`,
`audio_clock_0/tx_lrclk_out -> i2s_transmitter_0/lrclk_in`, and
`audio_clock_0/bclk_out -> i2s_transmitter_0/sclk_in`.

The release does not expose LRCLK diagnostics through the block design or
the Zorro register map.
