# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Check the routed capture synchronizer boundary, including paths which
# must remain timed. Run after implementation with capture_cdc.xdc loaded.
foreach {pattern expected from_clock} {
    {^zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/videocap_sampler_inst/calibration_capture/arm_sync_reg\[0\]/D$} 1 clk_fpga_0
    {^zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/vcap_reset_sync_reg\[[0-2]\]/PRE$} 3 clk_fpga_0
    {^zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/videocap_sampler_inst/calibration_capture/axi_resetn_sync_cap_reg\[0\]/D$} 1 clk_fpga_0
} {
    set endpoints [get_pins -hierarchical -regexp $pattern]
    if {[llength $endpoints] != $expected} {
        error "Capture CDC endpoint count mismatch: expected $expected, found $endpoints"
    }
    foreach endpoint $endpoints {
        foreach delay_type {max min} {
            # Explicit endpoint queries in Vivado 2018.3 return false-path
            # objects too, without a SLACK property. Check their exception.
            set entry_paths [get_timing_paths -quiet -from [get_clocks $from_clock] \
                -to $endpoint -delay_type $delay_type -max_paths 1]
            if {[llength $entry_paths] != 1 ||
                [get_property EXCEPTION $entry_paths] ne "False Path"} {
                error "Asynchronous AXI entry lacks a false-path exception: $endpoint"
            }
        }
    }
}

# The capture-domain reset crosses back into the AXI domain through its own
# synchronizer; its entry is cut from the capture MMCM clock (see
# capture_cdc.xdc). Verify that cut on one representative endpoint.
set cap_reset_entry [get_pins -hierarchical -regexp \
    {^zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/videocap_sampler_inst/calibration_capture/cap_reset_sync_axi_reg\[0\]/D$}]
if {[llength $cap_reset_entry] != 1} {
    error "Capture CDC endpoint count mismatch: expected 1, found $cap_reset_entry"
}
set cap_entry_clocks [get_clocks -quiet -of_objects [get_pins -quiet \
    zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/mmcm_adv_inst/CLKOUT0]]
if {[llength $cap_entry_clocks] == 0} {
    error "Capture MMCM CLKOUT0 clock object missing for the reset crossing check"
}
foreach delay_type {max min} {
    set entry_paths [get_timing_paths -quiet -from $cap_entry_clocks \
        -to $cap_reset_entry -delay_type $delay_type -max_paths 1]
    if {[llength $entry_paths] != 1 ||
        [get_property EXCEPTION $entry_paths] ne "False Path"} {
        error "Capture-domain reset entry lacks a false-path exception: $cap_reset_entry"
    }
}

# The readiness crossing (sampler combinational capture_ready ->
# vcap_ready_sync) is a capture->ACLK entry that intentionally keeps a LUT
# before stage 0 (readiness must drop without a capture-clock edge); it
# stays bounded by the capture-CLKOUT0->ACLK max_delay -datapath_only rule
# rather than being false-pathed: assert it can never silently regress to
# unbounded or unanalyzed.
set ready_entry [get_pins -hierarchical -regexp \
    {^zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/vcap_ready_sync_reg\[0\]/D$}]
if {[llength $ready_entry] != 1} {
    error "Capture CDC endpoint count mismatch: expected 1, found $ready_entry"
}
# set_max_delay -datapath_only bounds setup only; Vivado reports the hold
# side of the same entry as "False Path", so only the max query is checked.
set ready_paths [get_timing_paths -quiet -from $cap_entry_clocks \
    -to $ready_entry -delay_type max -max_paths 1]
if {[llength $ready_paths] != 1} {
    error "Readiness capture->ACLK entry is unanalyzed: $ready_entry"
}
set ready_exception [get_property EXCEPTION $ready_paths]
# Vivado 2018.3 reports "MaxDelay Path <n>ns -datapath_only".
if {[string first "MaxDelay" $ready_exception] != 0} {
    error "Readiness capture->ACLK entry lost its max_delay bound: $ready_entry ($ready_exception)"
}
set ready_slack [get_property SLACK $ready_paths]
if {![string is double -strict $ready_slack] || $ready_slack < 0} {
    error "Readiness capture->ACLK entry timing failed: $ready_entry"
}

foreach {source_pattern target_pattern} {
    {.*calibration_capture/arm_sync_reg\[0\]/Q} {.*calibration_capture/arm_sync_reg\[1\]/D}
    {.*calibration_capture/arm_sync_reg\[1\]/Q} {.*calibration_capture/arm_sync_reg\[2\]/D}
    {.*calibration_capture/axi_resetn_sync_cap_reg\[0\]/Q} {.*calibration_capture/axi_resetn_sync_cap_reg\[1\]/D}
    {.*calibration_capture/axi_resetn_sync_cap_reg\[1\]/Q} {.*calibration_capture/axi_resetn_sync_cap_reg\[2\]/D}
    {.*calibration_capture/cap_reset_sync_axi_reg\[0\]/Q} {.*calibration_capture/cap_reset_sync_axi_reg\[1\]/D}
    {.*calibration_capture/cap_reset_sync_axi_reg\[1\]/Q} {.*calibration_capture/cap_reset_sync_axi_reg\[2\]/D}
    {.*calibration_capture/axi_resetn_sync_cap_reg\[2\]/Q} {.*calibration_capture/expected_x_reg\[10\]/(R|S|D)}
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
puts "CAPTURE_CDC_GATE: PASS - six asynchronous entry pins excluded, synchronizer stages and capture reset release timed"
