# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Check the routed capture synchronizer boundary, including paths which
# must remain timed. Run after implementation with capture_cdc.xdc loaded.
foreach {pattern expected} {
    {^zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/videocap_sampler_inst/calibration_capture/arm_sync_reg\[0\]/D$} 1
    {^zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/vcap_reset_sync_reg\[[0-2]\]/PRE$} 3
    {^zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/videocap_sampler_inst/calibration_capture/reset_cap_reg\[[0-2]\]/PRE$} 3
} {
    set endpoints [get_pins -hierarchical -regexp $pattern]
    if {[llength $endpoints] != $expected} {
        error "Capture CDC endpoint count mismatch: expected $expected, found $endpoints"
    }
    foreach endpoint $endpoints {
        foreach delay_type {max min} {
            # Explicit endpoint queries in Vivado 2018.3 return false-path
            # objects too, without a SLACK property. Check their exception.
            set entry_paths [get_timing_paths -quiet -from [get_clocks clk_fpga_0] \
                -to $endpoint -delay_type $delay_type -max_paths 1]
            if {[llength $entry_paths] != 1 ||
                [get_property EXCEPTION $entry_paths] ne "False Path"} {
                error "Asynchronous AXI entry lacks a false-path exception: $endpoint"
            }
        }
    }
}

foreach {source_pattern target_pattern} {
    {.*calibration_capture/arm_sync_reg\[0\]/Q} {.*calibration_capture/arm_sync_reg\[1\]/D}
    {.*calibration_capture/arm_sync_reg\[1\]/Q} {.*calibration_capture/arm_sync_reg\[2\]/D}
    {.*vcap_reset_sync_reg\[2\]/Q} {.*calibration_capture/reset_cap_reg\[[0-2]\]/PRE}
    {.*calibration_capture/reset_cap_reg\[2\]/Q} {.*calibration_capture/expected_x_reg\[10\]/CLR}
} {
    set sources [get_pins -hierarchical -regexp $source_pattern]
    set destinations [get_pins -hierarchical -regexp $target_pattern]
    if {[llength $sources] == 0 || [llength $destinations] == 0} {
        error "Capture CDC stage/release endpoint missing: $source_pattern -> $target_pattern"
    }
    foreach delay_type {max min} {
        set paths [get_timing_paths -quiet -from $sources -to $destinations \
            -delay_type $delay_type -max_paths 1]
        if {[llength $paths] != 1} {
            error "Capture CDC stage/release timing was masked: $source_pattern -> $target_pattern"
        }
        set slack [get_property SLACK $paths]
        if {![string is double -strict $slack] || !($slack < Inf)} {
            error "Capture CDC stage/release timing has no finite slack: $source_pattern -> $target_pattern"
        }
        if {$slack < 0} {
            error "Capture CDC stage/release timing failed: $source_pattern -> $target_pattern"
        }
    }
}
puts "CAPTURE_CDC_GATE: PASS - seven AXI entry pins excluded, synchronizer stages and capture reset release timed"
