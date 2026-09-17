# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# The AXI and native capture clocks are asynchronous. ASYNC_REG preserves
# synchronizer placement but does not remove impossible first-stage timing.
# Cut only the AXI-origin entry paths; retain all stage-to-stage timing.
set_false_path -from [get_clocks clk_fpga_0] -to [get_pins -hierarchical -regexp {^zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/videocap_sampler_inst/calibration_capture/arm_sync_reg\[0\]/D$}]

# Clock readiness asynchronously presets all three reset stages. Their
# capture-clock shift chain provides synchronous release to the sampler.
set_false_path -from [get_clocks clk_fpga_0] -to [get_pins -hierarchical -regexp {^zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/vcap_reset_sync_reg\[[0-2]\]/PRE$}]

# AXI reset is also an asynchronous input to the snapshot reset bridge.
# The source-clock qualifier deliberately keeps the capture-clock release
# from vcap_reset_sync[2] into these same PRE pins timed, as well as the
# bridge's final output to functional reset pins.
set_false_path -from [get_clocks clk_fpga_0] -to [get_pins -hierarchical -regexp {^zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/videocap_sampler_inst/calibration_capture/reset_cap_reg\[[0-2]\]/PRE$}]
