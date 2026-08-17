# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

open_project ZZ9000_proto/ZZ9000_proto.xpr

set script_dir [file dirname [file normalize [info script]]]

set njobs 4
if { [info exists ::env(NUMBER_OF_PROCESSORS)] } {
    set njobs $::env(NUMBER_OF_PROCESSORS)
} elseif { ![catch {exec nproc} detected_jobs] } {
    set njobs $detected_jobs
}
puts "Using $njobs parallel jobs"

reset_run synth_1
generate_target all [get_files zz9000_ps.bd]
create_ip_run [get_files zz9000_ps.bd]
set formatter_runs [get_runs -quiet -filter {NAME =~ *video_formatter_0_0_synth*}]
if {[llength $formatter_runs] != 1} {
    error "Expected exactly one video_formatter OOC run, found [llength $formatter_runs]"
}
set formatter_run [lindex $formatter_runs 0]
set_property STEPS.SYNTH_DESIGN.TCL.PRE \
    [file join $script_dir apply_formatter_ooc_timing.tcl] $formatter_run
set_property STEPS.SYNTH_DESIGN.TCL.POST \
    [file join $script_dir report_formatter_ooc_timing.tcl] $formatter_run
reset_run $formatter_run
launch_runs $formatter_run -jobs $njobs
wait_on_run $formatter_run
set formatter_status [get_property STATUS $formatter_run]
puts "Formatter OOC synthesis: $formatter_status"
if { [string match "*ERROR*" $formatter_status] || [string match "*FAILED*" $formatter_status] } {
    puts "ERROR: Formatter OOC synthesis failed!"
    exit 1
}

source [file join $script_dir verify_formatter_ooc_timing.tcl]

launch_runs synth_1 -jobs $njobs
wait_on_run synth_1
set synth_status [get_property STATUS [get_runs synth_1]]
puts "Synthesis: $synth_status"
if { [string match "*ERROR*" $synth_status] || [string match "*FAILED*" $synth_status] } {
    puts "ERROR: Synthesis failed!"
    exit 1
}

launch_runs impl_1 -to_step {phys_opt_design (Post-Route)} -jobs $njobs
wait_on_run impl_1
set impl_status [get_property STATUS [get_runs impl_1]]
puts "Implementation: $impl_status"
if { [string match "*ERROR*" $impl_status] || [string match "*FAILED*" $impl_status] } {
    puts "ERROR: Implementation failed!"
    exit 1
}

open_run impl_1
source [file join $script_dir verify_runtime_pixel_timing.tcl]
close_design

launch_runs impl_1 -to_step write_bitstream -jobs $njobs
wait_on_run impl_1
set bitstream_status [get_property STATUS [get_runs impl_1]]
puts "Bitstream: $bitstream_status"
if { [string match "*ERROR*" $bitstream_status] || [string match "*FAILED*" $bitstream_status] } {
    puts "ERROR: write_bitstream failed!"
    exit 1
}

close_project
puts "Build complete. Bitstream at: ZZ9000_proto/ZZ9000_proto.runs/impl_1/zz9000_ps_wrapper.bit"
