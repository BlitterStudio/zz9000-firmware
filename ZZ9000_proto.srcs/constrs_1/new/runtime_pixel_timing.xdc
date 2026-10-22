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

# VGA_R/G/B/HS/VS/DE feed the SiI9022A HDMI transmitter.  The final output
# stage registers this complete bundle in IOBs, and an ODDR forwards VGA_PCLK.
# Firmware clears the transmitter's EDGE bit (TPI 0x08 bit 4), so it samples
# on VGA_PCLK's falling edge. SiI-DS-1076-E.01 Table 3.13/Figure 4.2 specifies
# tSU=0.0 ns and tHD=1.8 ns for EDGE=0 at the board's 3.3 V IOVCC; that table
# also applies the limits to RGB, DE, HSYNC, and VSYNC.
#
# The board trace length difference was not measured. Conservatively assume
# data can arrive 0.20 ns later than VGA_PCLK for setup and 0.20 ns earlier
# for hold [INFERENCE]. Thus the source-synchronous output model uses
# max=tSU+0.20=0.20 ns and min=-(tHD+0.20)=-2.00 ns. The ODDR's rising edge
# launches data, while the receiver's falling-edge sample is 3.333 ns later
# at 150 MHz, deliberately centring the external data window.
set vga_pclk_oddr_c [get_pins -quiet vga_output_stage_i/vga_pclk_oddr/C]
if {[llength $vga_pclk_oddr_c] != 1} {
  error "Expected one VGA_PCLK forwarding ODDR C pin, found [llength $vga_pclk_oddr_c]"
}
set default_pixel_clock [lindex $default_pixel_clocks 0]
create_generated_clock -name sii9022_idck_default \
  -source $vga_pclk_oddr_c -master_clock $default_pixel_clock -divide_by 1 \
  [get_ports VGA_PCLK]
create_generated_clock -name sii9022_idck -add \
  -source $vga_pclk_oddr_c -master_clock dvi_pixel_runtime_150 -divide_by 1 \
  [get_ports VGA_PCLK]
set_clock_groups -physically_exclusive \
  -group [get_clocks sii9022_idck_default] \
  -group [get_clocks sii9022_idck]


set sii9022_video_ports [get_ports {VGA_R[*] VGA_G[*] VGA_B[*] VGA_HS VGA_VS VGA_DE}]
set_output_delay -clock sii9022_idck_default -clock_fall -max 0.200 $sii9022_video_ports
set_output_delay -clock sii9022_idck_default -clock_fall -min -2.000 $sii9022_video_ports
set_output_delay -clock sii9022_idck -clock_fall -max 0.200 -add_delay $sii9022_video_ports
set_output_delay -clock sii9022_idck -clock_fall -min -2.000 -add_delay $sii9022_video_ports
