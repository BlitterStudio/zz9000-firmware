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

`video_overlay_filter` has one additional interpolation stage between the
horizontal sum and vertical interpolation. The formatter changes
`OVERLAY_FILTER_DELAY` from 9 to 11 and `OVERLAY_PIPE_DELAY` from 13 to 16,
covering the staged source position, the filter stage, and converter-output
stage; RGB, draw, sprite, palette select, split, and sync delay lines retain
their relative timing.

## Formatter OOC timing

The pre-change `build/memmap/zorro3-default/reports/formatter_ooc_timing_summary.rpt`
reports formatter-pixel WNS/TNS/WHS of -0.793/-6.342/+0.164 ns (8 setup
endpoints). The isolated post-change OOC synthesis, run on 2026-10-19, reports
the same formatter-pixel WNS/TNS/WHS: -0.793/-6.342/+0.164 ns (8 setup
endpoints) in `ZZ9000_proto/timing_reports/formatter_ooc_timing_summary.rpt`.
It completed with zero errors and zero critical warnings. The tracked
`verify_formatter_ooc_timing.tcl` rejected this isolated run only because its
post-hook writes the report in `ZZ9000_proto/timing_reports`, while the
verifier expects `<isolated-project>/timing_reports`; the report was produced
and inspected at the former path.

## I2S LRCLK timing

`audio_clock` samples `I2SO_LRCLK` in its existing `IOB=TRUE` BCLK input
register and sends that registered value to `i2s_transmitter_0/lrclk_in` via
`tx_lrclk_out`. This removes the pad-to-transmitter combinational LRCLK path
that entered the transmitter's `rSDataOut` control logic through two LUTs.
The ADAU1701 data sheet, [Table 7, Serial Input/Output Ports](https://www.analog.com/media/en/technical-documentation/data-sheets/ADAU1701.pdf), specifies serial-output-data delay `tSODM` up to 40 ns after falling BCLK and serial-input setup/hold `tSIS`/`tSIH` of 10 ns. Registering LRCLK at the next BCLK rising edge gives the transmitter a stable input for the following serial-data decision, instead of asking its control path to absorb the 40 ns pad-arrival interval. The added test checks this registered transmitter-facing LRCLK over a complete TDM frame.

The unchanged `audio_clock_tb.sv` data checks pass with the new registered
LRCLK assertion (`audio_clock fixed TDM8 slot-0/1 bridge PASS`), and the
unchanged Xilinx receiver integration passes (`audio_clock + Xilinx I2S
receiver fixed TDM PASS`).
