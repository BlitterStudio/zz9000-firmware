# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# This file is imported with PROCESSING_ORDER LATE so the clock-wizard IP's
# generated 75 MHz clock exists before it is grouped with the alternate clock.
# clk_wiz_0 powers up at 75 MHz, but firmware reprograms this PLL output through
# DRP for every display mode.  The fastest supported runtime mode is the
# 150 MHz 1920x1080 native-video container.  Keep the power-on configuration
# unchanged while making implementation close timing against that real maximum.

set runtime_pixel_clock_pin [get_pins zz9000_ps_i/clk_wiz_0/inst/CLK_CORE_DRP_I/clk_inst/plle2_adv_inst/CLKOUT0]
set default_pixel_clocks [get_clocks -of_objects $runtime_pixel_clock_pin]

create_clock -period 6.667 -name dvi_pixel_runtime_150 -add $runtime_pixel_clock_pin
set_clock_uncertainty 0.300 [get_clocks dvi_pixel_runtime_150]

# The clocks describe mutually exclusive DRP configurations of one PLL output.
set_clock_groups -physically_exclusive \
  -group $default_pixel_clocks \
  -group [get_clocks dvi_pixel_runtime_150]

# Match the bounded asynchronous FCLK boundary used for the default pixel
# clock in zz9000.xdc (blanket false paths there used to override the XPM
# CDC set_max_delay -datapath_only constraints - methodology TIMING-24).
set_max_delay -datapath_only -from [get_clocks clk_fpga_0] -to [get_clocks dvi_pixel_runtime_150] 20.000
set_max_delay -datapath_only -from [get_clocks dvi_pixel_runtime_150] -to [get_clocks clk_fpga_0] 20.000

# VGA_R/G/B/HS/VS/DE feed the SiI9022 HDMI transmitter, which samples them
# against the forwarded VGA_PCLK (an OBUF output of the same pixel clock).
# The data registers are fabric flip-flops, not IOB registers; their
# clock-to-pin delays (about 2.4-10.5 ns on the routed trial netlist) and
# the SiI9022 setup/hold window relative to VGA_PCLK are not modelled here.
# Output delays referenced to the internal pixel clock fail by 8-12 ns at
# 150 MHz on that netlist, because they ignore the forwarded clock, so they
# do not describe the interface.  The outputs keep the unconstrained
# placement of the qualified baseline; the false path only makes that
# explicit.  A correct source-synchronous model (a generated clock on
# VGA_PCLK plus the SiI9022 tSU/tHD) is future work.
set_false_path -to [get_ports {VGA_R[*] VGA_G[*] VGA_B[*] VGA_HS VGA_VS VGA_DE}]
