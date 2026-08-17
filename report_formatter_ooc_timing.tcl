# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# This post-hook executes inside the OOC synthesis process while its
# synthesis-only clocks are active.
set pixel_clocks [get_clocks -quiet formatter_pixel_ooc]
set axis_clocks [get_clocks -quiet formatter_axis_ooc]
if {[llength $pixel_clocks] != 1 || [llength $axis_clocks] != 1} {
    error "Formatter OOC synthesis did not apply both clocks"
}
set pixel_clock [lindex $pixel_clocks 0]
set pixel_period [get_property PERIOD $pixel_clock]
if {abs($pixel_period - 6.667) > 0.001} {
    error "Formatter OOC pixel period is $pixel_period ns, expected 6.667 ns"
}
set pixel_registers [all_registers -clock $pixel_clock]
if {[llength $pixel_registers] == 0} {
    error "Formatter OOC 150 MHz clock has no timed registers"
}
set hook_dir [file dirname [file normalize [info script]]]
set report_dir [file join $hook_dir ZZ9000_proto timing_reports]
file mkdir $report_dir
report_timing_summary -delay_type min_max -report_unconstrained \
    -file [file join $report_dir formatter_ooc_timing_summary.rpt]
puts "OOC_TIMING_POST: pixel_clock=[get_property NAME $pixel_clock] period=${pixel_period}ns registers=[llength $pixel_registers]"
