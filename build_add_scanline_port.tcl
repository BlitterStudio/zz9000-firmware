open_project ZZ9000_proto/ZZ9000_proto.xpr
open_bd_design [get_files *.bd]

# Force rebuild IP catalog to pick up modified Verilog ports
update_ip_catalog -rebuild

# Find cells by name pattern
set vf_cells [get_bd_cells -hierarchical -filter {NAME =~ *video_formatter*}]
set mz_cells [get_bd_cells -hierarchical -filter {NAME =~ *MNTZorro*}]
puts "video_formatter cells: $vf_cells"
puts "mntzorro cells: $mz_cells"

if { [llength $vf_cells] == 0 } { error "ERROR: Could not find video_formatter cell" }
if { [llength $mz_cells] == 0 } { error "ERROR: Could not find MNTZorro cell" }

set vf_cell [lindex $vf_cells 0]
set mz_cell [lindex $mz_cells 0]

# Debug: list all pins on both cells to verify scanline ports exist
puts "=== video_formatter pins ==="
foreach p [get_bd_pins -of_objects $vf_cell] { puts "  $p" }
puts "=== MNTZorro pins ==="
foreach p [get_bd_pins -of_objects $mz_cell] { puts "  $p" }

# Detect hierarchy from cell path string
set vf_path "$vf_cell"
set path_parts [split [string trimleft $vf_path /] /]
set hier_name ""

if { [llength $path_parts] > 1 } {
    set hier_name [lindex $path_parts 0]
    puts "Detected hierarchy from cell path: $hier_name"
} else {
    puts "Cell appears to be at top level"
}

if { $hier_name ne "" } {
    # Navigate into the hierarchy, create input port, connect to video_formatter
    set old_inst [current_bd_instance /]
    current_bd_instance /$hier_name

    create_bd_pin -dir I -from 7 -to 0 scanline_intensity
    puts "Created /${hier_name}/scanline_intensity port"

    connect_bd_net [get_bd_pins scanline_intensity] \
                   [get_bd_pins -of_objects [get_bd_cells video_formatter_0] -filter {NAME == scanline_intensity}]
    puts "Connected: /${hier_name}/scanline_intensity -> video_formatter_0/scanline_intensity"

    # Go back to top level, connect MNTZorro output -> hierarchy input
    current_bd_instance /

    connect_bd_net [get_bd_pins -of_objects $mz_cell -filter {NAME == scanline_intensity_out}] \
                   [get_bd_pins /${hier_name}/scanline_intensity]
    puts "Connected: ${mz_cell}/scanline_intensity_out -> /${hier_name}/scanline_intensity"
} else {
    # Both at top level - direct connection
    connect_bd_net [get_bd_pins -of_objects $mz_cell -filter {NAME == scanline_intensity_out}] \
                   [get_bd_pins -of_objects $vf_cell -filter {NAME == scanline_intensity}]
    puts "Connected directly at top level"
}

validate_bd_design
save_bd_design
make_wrapper -files [get_files *.bd] -top -force
close_project
puts "Block design updated successfully."
