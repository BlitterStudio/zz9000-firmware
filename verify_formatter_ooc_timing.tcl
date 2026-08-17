# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Prove that the freshly generated module-reference run actually consumed the
# synthesis-only 150 MHz constraint.  A project-file source contract is not
# sufficient because Vivado regenerates each OOC run script.

set formatter_runs [get_runs -quiet -filter {NAME =~ *video_formatter_0_0_synth*}]
if {[llength $formatter_runs] != 1} {
    error "Expected exactly one video_formatter OOC run, found [llength $formatter_runs]"
}
set formatter_run [lindex $formatter_runs 0]
set formatter_run_dir [get_property DIRECTORY $formatter_run]
set formatter_run_scripts [glob -nocomplain -directory $formatter_run_dir *video_formatter_0_0.tcl]
if {[llength $formatter_run_scripts] != 1} {
    error "Expected one generated formatter OOC Tcl script in $formatter_run_dir"
}
set formatter_run_script [lindex $formatter_run_scripts 0]
set script_channel [open $formatter_run_script r]
set script_text [read $script_channel]
close $script_channel

set hook_position [string first "apply_formatter_ooc_timing.tcl" $script_text]
set synth_position [string first "synth_design" $script_text]
set post_position [string first "report_formatter_ooc_timing.tcl" $script_text]
if {$hook_position < 0 || $synth_position < 0 || $hook_position > $synth_position} {
    error "Generated formatter OOC Tcl does not source the timing pre-hook before synth_design"
}
if {$post_position < 0 || $post_position < $synth_position} {
    error "Generated formatter OOC Tcl does not source the timing proof after synth_design"
}
set hook_path [file join [file dirname [file normalize [info script]]] apply_formatter_ooc_timing.tcl]
set hook_channel [open $hook_path r]
set hook_text [read $hook_channel]
close $hook_channel
if {[string first "read_xdc -unmanaged" $hook_text] < 0 ||
    [string first "video_formatter_ooc_timing.xdc" $hook_text] < 0} {
    error "Formatter OOC timing pre-hook does not read the tracked XDC"
}

set project_dir [get_property DIRECTORY [current_project]]
set report_dir [file normalize [file join $project_dir timing_reports]]
set ooc_report [file join $report_dir formatter_ooc_timing_summary.rpt]
if {![file exists $ooc_report]} {
    error "Formatter OOC synthesis post-hook did not produce $ooc_report"
}
set report_channel [open $ooc_report r]
set report_text [read $report_channel]
close $report_channel
if {[string first "formatter_pixel_ooc" $report_text] < 0 ||
    [string first "6.667" $report_text] < 0 ||
    [string first "formatter_axis_ooc" $report_text] < 0 ||
    [string first "10.000" $report_text] < 0} {
    error "Formatter OOC synthesis report does not contain both required clock periods"
}

puts "OOC_TIMING: run=[get_property NAME $formatter_run] script=$formatter_run_script"
puts "OOC_TIMING: report=$ooc_report"
puts "OOC_TIMING: PASS - generated OOC Tcl sources the read_xdc pre-hook before synth_design and its post-hook reports 6.667/10.000 ns clocks"
