# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds a simulation-only block design holding the real RasterIX command
# ingress of the EF image: ps7_0_axi_periph (the GP1 AXI interconnect) and
# gpu_cmd_fifo (axi_fifo_mm_s 4.2), copied cell-for-cell from the block
# design that zz9000_project.tcl --rasterix generates.  Everything around
# them becomes external ports carrying the properties of the real neighbours:
# S_AXI_GP1 those of processing_system7_0/M_AXI_GP1, M00..M07 those of the
# slave each master interface feeds, M_AXIS_CMD the FIFO's TX stream, and
# the clocks those of FCLK1 (GP1 and the interconnect) and FCLK0 (FIFO,
# RasterIX).  The interconnect's internal topology after validation must
# match the real one exactly, or the script fails.
#
# usage: vivado -mode batch -source fifo_path.tcl -tclargs <repo> <work-dir>
# Output: <work-dir>/export/xsim (export_simulation compile lists) and
#         <work-dir>/topology.txt (the matched interconnect topology).

set repo [file normalize [lindex $argv 0]]
set work [file normalize [lindex $argv 1]]
file mkdir $work
cd $work

set ::argv [list --origin_dir $repo --project_name rix_fifo_path --rasterix \
            --rasterix-tmu-count 1 --rasterix-variant ef]
set ::argc [llength $::argv]
source [file join $repo zz9000_project.tcl]

proc fail {message} {
  puts "FIFO_PATH_ERROR: $message"
  exit 1
}

proc config_of {object} {
  set values [dict create]
  foreach name [list_property $object CONFIG.*] {
    dict set values $name [get_property $name $object]
  }
  return $values
}

# Every cell below the interconnect with its VLNV and full configuration,
# relative to the interconnect so both designs compare line by line.
proc interconnect_topology {} {
  set rows {}
  set root [get_bd_cells ps7_0_axi_periph]
  set prefix [get_property PATH $root]
  foreach cell [lsort [get_bd_cells -hierarchical -filter "PATH =~ ${prefix}/*"]] {
    set row "[string range [get_property PATH $cell] [string length $prefix] end] [get_property VLNV $cell]"
    foreach name [lsort [list_property $cell CONFIG.*]] {
      if {[string match -nocase *Component_Name $name]} continue
      append row " $name=[get_property $name $cell]"
    }
    lappend rows $row
  }
  return $rows
}

# PROTOCOL first: it decides which other parameters the port leaves
# writable (an AXI4LITE port fixes its ID/USER/burst parameters itself).
proc apply_config {object values} {
  if {[dict exists $values CONFIG.PROTOCOL]} {
    set_property CONFIG.PROTOCOL [dict get $values CONFIG.PROTOCOL] $object
  }
  dict for {name value} $values {
    if {[get_property $name $object] eq $value} continue
    if {[catch {set_property $name $value $object} message]} {
      puts "FIFO_PATH_NOTE: [get_property NAME $object] keeps $name ($message)"
    }
  }
}

open_bd_design [get_files zz9000_ps.bd]
validate_bd_design
set real_topology [interconnect_topology]

set gp1 [config_of [get_bd_intf_pins processing_system7_0/M_AXI_GP1]]
set gp_clock [config_of [get_bd_pins processing_system7_0/FCLK_CLK1]]
set rix_clock [config_of [get_bd_pins processing_system7_0/FCLK_CLK0]]

# Real slave behind each master interface and its address window.
set data_segments [get_bd_addr_segs -of_objects [get_bd_addr_spaces processing_system7_0/Data]]
set masters {}
for {set index 0} {$index < 9} {incr index} {
  set mi [format M%02d_AXI $index]
  set peer [find_bd_objs -relation connected_to -thru_hier [get_bd_intf_pins ps7_0_axi_periph/$mi]]
  if {[llength $peer] != 1} { fail "$mi connects to '$peer'" }
  set window {}
  foreach segment $data_segments {
    set target [get_bd_addr_segs -of_objects $segment]
    if {[lsearch -exact [get_bd_addr_segs -of_objects $peer] $target] >= 0} {
      set window [list [get_property OFFSET $segment] [get_property RANGE $segment]]
    }
  }
  if {$window eq {}} { fail "$mi has no GP1 address window" }
  lappend masters [list $mi [get_property PATH $peer] [config_of $peer] $window]
  puts "FIFO_PATH_MI: $mi -> [get_property PATH $peer] [lindex $window 0]/[lindex $window 1]"
}
if {[lindex [lindex $masters 8] 1] ne "/gpu_cmd_fifo/S_AXI"} {
  fail "M08_AXI does not feed gpu_cmd_fifo"
}
set cells [get_bd_cells {ps7_0_axi_periph gpu_cmd_fifo}]

create_bd_design rix_fifo_path
copy_bd_objs -from_design zz9000_ps / $cells
if {[get_bd_intf_nets -quiet -of_objects [get_bd_intf_pins gpu_cmd_fifo/S_AXI]] eq ""} {
  connect_bd_intf_net [get_bd_intf_pins ps7_0_axi_periph/M08_AXI] [get_bd_intf_pins gpu_cmd_fifo/S_AXI]
}

set gp_aclk [create_bd_port -dir I -type clk gp_aclk]
set gp_aresetn [create_bd_port -dir I -type rst gp_aresetn]
set rix_aclk [create_bd_port -dir I -type clk rix_aclk]
set rix_aresetn [create_bd_port -dir I -type rst rix_aresetn]
apply_config $gp_aclk $gp_clock
apply_config $rix_aclk $rix_clock

set gp_port [create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 S_AXI_GP1]
apply_config $gp_port $gp1
connect_bd_intf_net $gp_port [get_bd_intf_pins ps7_0_axi_periph/S00_AXI]
set gp_busif S_AXI_GP1
for {set index 0} {$index < 8} {incr index} {
  set mi [format M%02d_AXI $index]
  set port [create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 $mi]
  connect_bd_intf_net $port [get_bd_intf_pins ps7_0_axi_periph/$mi]
  apply_config $port [lindex [lindex $masters $index] 2]
  if {$index < 7} { append gp_busif ":$mi" }
}
set stream [create_bd_intf_port -mode Master -vlnv xilinx.com:interface:axis_rtl:1.0 M_AXIS_CMD]
connect_bd_intf_net $stream [get_bd_intf_pins gpu_cmd_fifo/AXI_STR_TXD]
set_property CONFIG.ASSOCIATED_BUSIF $gp_busif $gp_aclk
set_property CONFIG.ASSOCIATED_RESET gp_aresetn $gp_aclk
set_property CONFIG.ASSOCIATED_BUSIF M07_AXI:M_AXIS_CMD $rix_aclk
set_property CONFIG.ASSOCIATED_RESET rix_aresetn $rix_aclk

connect_bd_net $gp_aclk [get_bd_pins ps7_0_axi_periph/ACLK] [get_bd_pins ps7_0_axi_periph/S00_ACLK]
connect_bd_net $gp_aresetn [get_bd_pins ps7_0_axi_periph/ARESETN] [get_bd_pins ps7_0_axi_periph/S00_ARESETN]
for {set index 0} {$index < 7} {incr index} {
  connect_bd_net $gp_aclk [get_bd_pins ps7_0_axi_periph/[format M%02d_ACLK $index]]
  connect_bd_net $gp_aresetn [get_bd_pins ps7_0_axi_periph/[format M%02d_ARESETN $index]]
}
connect_bd_net $rix_aclk [get_bd_pins ps7_0_axi_periph/M07_ACLK] \
  [get_bd_pins ps7_0_axi_periph/M08_ACLK] [get_bd_pins gpu_cmd_fifo/s_axi_aclk]
connect_bd_net $rix_aresetn [get_bd_pins ps7_0_axi_periph/M07_ARESETN] \
  [get_bd_pins ps7_0_axi_periph/M08_ARESETN] [get_bd_pins gpu_cmd_fifo/s_axi_aresetn]

set space [get_bd_addr_spaces S_AXI_GP1]
for {set index 0} {$index < 9} {incr index} {
  set window [lindex [lindex $masters $index] 3]
  if {$index < 8} {
    set target [get_bd_addr_segs [format M%02d_AXI $index]/Reg]
  } else {
    set target [get_bd_addr_segs gpu_cmd_fifo/S_AXI/Mem0]
  }
  create_bd_addr_seg -offset [lindex $window 0] -range [lindex $window 1] $space $target [format SEG_M%02d $index]
}
# Connecting and addressing re-derive some port parameters (M07 ADDR_WIDTH
# returns to 32); the real slave values go last.
for {set index 0} {$index < 8} {incr index} {
  apply_config [get_bd_intf_ports [format M%02d_AXI $index]] [lindex [lindex $masters $index] 2]
}

validate_bd_design
save_bd_design
set copy_topology [interconnect_topology]
set report [open [file join $work topology.txt] w]
puts $report [join $copy_topology "\n"]
close $report
if {$real_topology ne $copy_topology} {
  set real_file [open [file join $work topology-real.txt] w]
  puts $real_file [join $real_topology "\n"]
  close $real_file
  fail "interconnect topology differs from zz9000_ps (see topology-real.txt)"
}
puts "FIFO_PATH_TOPOLOGY: [llength $copy_topology] interconnect cells match zz9000_ps"

set bd_file [get_files rix_fifo_path.bd]
set wrapper [make_wrapper -files $bd_file -top]
add_files -norecurse $wrapper
generate_target simulation $bd_file
export_ip_user_files -of_objects $bd_file -no_script -sync -force -quiet
export_simulation -of_objects $bd_file -simulator xsim -force \
  -directory [file join $work export]
file copy -force $wrapper [file join $work export rix_fifo_path_wrapper.v]
puts "FIFO_PATH_READY: [file join $work export xsim]"
exit 0
