# Post-build utilization and timing reports for the RasterIX EF variant.
# Opens the implemented design and writes the same report set the IF hw1
# build produced under timing_reports/, into timing_reports-ef/.
open_project ZZ9000_proto/ZZ9000_proto.xpr
open_run impl_1

set rpt_dir timing_reports-ef
file mkdir $rpt_dir

report_utilization -file [file join $rpt_dir utilization.rpt]
report_timing_summary -delay_type min_max -report_unconstrained -check_timing_verbose \
    -file [file join $rpt_dir overall_timing_summary.rpt]
report_cdc -details -file [file join $rpt_dir cdc.rpt]
report_clock_interaction -delay_type min_max -significant_digits 3 \
    -file [file join $rpt_dir clock_interaction.rpt]
report_drc -file [file join $rpt_dir drc.rpt]

# Per-clock setup/hold for the release-gate clocks.
foreach {clk rpt} {dvi_pixel_runtime_150 runtime_pixel_150mhz} {
    report_timing -from [get_clocks $clk] -to [get_clocks $clk] -delay_type max \
        -max_paths 10 -nworst 2 -file [file join $rpt_dir ${rpt}_setup.rpt]
    report_timing -from [get_clocks $clk] -to [get_clocks $clk] -delay_type min \
        -max_paths 10 -nworst 2 -file [file join $rpt_dir ${rpt}_hold.rpt]
}

close_design
close_project
puts "EF_REPORTS: done -> $rpt_dir"
