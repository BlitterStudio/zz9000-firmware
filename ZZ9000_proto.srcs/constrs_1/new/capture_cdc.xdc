# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# The AXI and native capture clocks are asynchronous. ASYNC_REG preserves
# synchronizer placement but does not remove impossible first-stage timing.
# Cut only the AXI-origin entry paths; retain all stage-to-stage timing.
set_false_path -from [get_clocks clk_fpga_0] -to [get_pins -hierarchical -regexp {^zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/videocap_sampler_inst/calibration_capture/arm_sync_reg\[0\]/D$}]

# Clock readiness asynchronously presets all three reset stages. Their
# capture-clock shift chain provides synchronous release to the sampler.
set_false_path -from [get_clocks clk_fpga_0] -to [get_pins -hierarchical -regexp {^zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/vcap_reset_sync_reg\[[0-2]\]/PRE$}]

# AXI reset crosses into the capture domain through its own three-stage
# synchronizer: stage 0 samples the axi_resetn port directly (no LUT), and
# the synchronized result is combined with same-domain cap_reset after the
# synchronizer. Cut only the first-stage entry; everything downstream stays
# timed.
set_false_path -from [get_clocks clk_fpga_0] -to [get_pins -hierarchical -regexp {^zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/videocap_sampler_inst/calibration_capture/axi_resetn_sync_cap_reg\[0\]/D$}]

# The capture-domain reset (cap_reset, sourced from vcap_reset_sync[2]
# through the sampler recovery counters on the capture MMCM clock) crosses
# back into the AXI domain through its own three-stage synchronizer. Cut
# only that first-stage entry; the stage-to-stage shifts and the combined
# output stay timed (bounded by the capture MMCM CLKOUT0 -> ACLK max_dpo
# in zz9000.xdc).
set_false_path -quiet -from [get_clocks -quiet -of_objects [get_pins -quiet zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/mmcm_adv_inst/CLKOUT0]] -to [get_pins -hierarchical -regexp {^zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/videocap_sampler_inst/calibration_capture/cap_reset_sync_axi_reg\[0\]/D$}]
