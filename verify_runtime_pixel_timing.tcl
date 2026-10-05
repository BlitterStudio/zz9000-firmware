# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Run against the routed impl_1 design.  clk_wiz_0 is dynamically
# reconfigured at runtime, so the ordinary 75 MHz power-on clock is not an
# adequate release gate for the DVI/formatter domain.

set runtime_clocks [get_clocks -quiet dvi_pixel_runtime_150]
if {[llength $runtime_clocks] != 1} {
    error "Expected exactly one dvi_pixel_runtime_150 clock, found [llength $runtime_clocks]"
}
set runtime_clock [lindex $runtime_clocks 0]

set runtime_registers [all_registers -clock $runtime_clock]
if {[llength $runtime_registers] == 0} {
    error "The 150 MHz runtime clock has no timed registers"
}
set formatter_registers [lsearch -all -inline $runtime_registers "*video_formatter_0*"]
if {[llength $formatter_registers] == 0} {
    error "The 150 MHz runtime clock does not reach video_formatter_0"
}

set project_dir [get_property DIRECTORY [current_project]]
set report_dir [file normalize [file join $project_dir timing_reports]]
file mkdir $report_dir

report_timing_summary -delay_type min_max -report_unconstrained \
    -check_timing_verbose \
    -file [file join $report_dir overall_timing_summary.rpt]
report_timing -from $runtime_clock -to $runtime_clock -delay_type max \
    -max_paths 100 -nworst 1 -unique_pins \
    -file [file join $report_dir runtime_pixel_150mhz_setup.rpt]
report_timing -from $runtime_clock -to $runtime_clock -delay_type min \
    -max_paths 20 -nworst 5 \
    -file [file join $report_dir runtime_pixel_150mhz_hold.rpt]
report_clock_interaction \
    -file [file join $report_dir clock_interaction.rpt]
report_cdc -details -file [file join $report_dir cdc.rpt]
report_drc -file [file join $report_dir drc.rpt]

# Surface dead or fully-overridden timing exceptions (report_exceptions
# -ignored).  The constraint cleanup of 2026-10 removed every user-owned
# entry from this list; IP-generated entries are outside our control, so
# this is informational, not a gate failure.
set ignored_rpt [file join $report_dir exceptions_ignored.rpt]
report_exceptions -ignored -file $ignored_rpt
set fh [open $ignored_rpt r]
set ignored_text [read $fh]
close $fh
puts "TIMING_GATE: ignored_exceptions=[regexp -all -line {^[0-9]+[ \t]} $ignored_text] (see exceptions_ignored.rpt)"

set setup_paths [get_timing_paths -quiet -from $runtime_clock -to $runtime_clock \
    -delay_type max -max_paths 1 -nworst 1]
set hold_paths [get_timing_paths -quiet -from $runtime_clock -to $runtime_clock \
    -delay_type min -max_paths 1 -nworst 1]
if {[llength $setup_paths] == 0 || [llength $hold_paths] == 0} {
    error "The 150 MHz runtime clock has no setup or hold timing paths"
}

set setup_slack [get_property SLACK [lindex $setup_paths 0]]
set hold_slack [get_property SLACK [lindex $hold_paths 0]]

# Exhaustive per-clock-group check: any negative setup or hold slack in any
# clock group (including input-delay port launches, whose from-clock is the
# port clock such as i2s_mclk or zorro_fcs) fails the gate with the
# offending group named.  A single global worst-path query historically
# reported only one number and hid which domain was marginal; the trial
# i2s_mclk path (fast-corner input setup, +0.143 ns only after post-route
# phys_opt) is exactly the kind of near-zero group this must surface.
set failed_groups {}
foreach gate_clock [get_clocks] {
    set gate_clock_name [get_property NAME $gate_clock]
    foreach {delay_type check_name} {max setup min hold} {
        set group_paths [get_timing_paths -quiet -from $gate_clock \
            -delay_type $delay_type -max_paths 1 -nworst 1]
        if {[llength $group_paths] == 0} {
            continue
        }
        set group_slack [get_property SLACK [lindex $group_paths 0]]
        if {![string is double -strict $group_slack]} {
            # Vivado 2018.3 returns false-pathed/unanalyzed path objects
            # without a SLACK property for explicit -from queries; an
            # excepted path cannot violate timing, so skip it.
            continue
        }
        puts "TIMING_GATE: group=$gate_clock_name ${check_name}_slack=${group_slack}ns"
        if {$group_slack < 0.0} {
            lappend failed_groups "$gate_clock_name $check_name ${group_slack}ns"
        }
    }
}

set overall_setup_path [get_timing_paths -quiet -delay_type max -max_paths 1 -nworst 1]
set overall_hold_path [get_timing_paths -quiet -delay_type min -max_paths 1 -nworst 1]
set overall_setup_slack [get_property SLACK [lindex $overall_setup_path 0]]
set overall_hold_slack [get_property SLACK [lindex $overall_hold_path 0]]

puts "TIMING_GATE: reports=$report_dir"
puts "TIMING_GATE: runtime_clock=[get_property NAME $runtime_clock] period=[get_property PERIOD $runtime_clock]ns registers=[llength $runtime_registers] formatter_registers=[llength $formatter_registers]"
puts "TIMING_GATE: runtime_150mhz_setup_slack=${setup_slack}ns runtime_150mhz_hold_slack=${hold_slack}ns"
puts "TIMING_GATE: overall_setup_slack=${overall_setup_slack}ns overall_hold_slack=${overall_hold_slack}ns"

if {[llength $failed_groups] > 0} {
    error "Timing failed in [llength $failed_groups] clock group check(s): [join $failed_groups {; }]"
}
if {$setup_slack < 0.0 || $hold_slack < 0.0 ||
    $overall_setup_slack < 0.0 || $overall_hold_slack < 0.0} {
    error "Timing failed: runtime setup/hold ${setup_slack}/${hold_slack} ns, overall setup/hold ${overall_setup_slack}/${overall_hold_slack} ns"
}

puts "TIMING_GATE: PASS - runtime 150 MHz and overall setup/hold slack are non-negative"
