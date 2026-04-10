open_project ZZ9000_proto/ZZ9000_proto.xpr

set njobs [exec nproc]
puts "Using $njobs parallel jobs"

reset_run synth_1
launch_runs synth_1 -jobs $njobs
wait_on_run synth_1
set synth_status [get_property STATUS [get_runs synth_1]]
puts "Synthesis: $synth_status"
if { [string match "*ERROR*" $synth_status] || [string match "*FAILED*" $synth_status] } {
    puts "ERROR: Synthesis failed!"
    exit 1
}

launch_runs impl_1 -jobs $njobs
wait_on_run impl_1
puts "Implementation: [get_property STATUS [get_runs impl_1]]"

launch_runs impl_1 -to_step write_bitstream -jobs $njobs
wait_on_run impl_1
puts "Bitstream: [get_property STATUS [get_runs impl_1]]"

close_project
puts "Build complete. Bitstream at: ZZ9000_proto/ZZ9000_proto.runs/impl_1/zz9000_ps_wrapper.bit"
