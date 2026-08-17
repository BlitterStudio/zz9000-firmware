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

# Match the established asynchronous FCLK boundary for the default pixel clock.
set_false_path -from [get_clocks clk_fpga_0] -to [get_clocks dvi_pixel_runtime_150]
set_false_path -from [get_clocks dvi_pixel_runtime_150] -to [get_clocks clk_fpga_0]
