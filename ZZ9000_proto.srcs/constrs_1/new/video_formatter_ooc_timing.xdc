# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# The block-design module-reference run otherwise synthesizes the formatter
# out of context without any user clocks.  Constrain both clock domains here
# so synthesis optimizes the pixel logic for the fastest runtime PLL mode.
# This file is synthesis-only and scoped to video_formatter; implementation
# uses runtime_pixel_timing.xdc on the real clock-wizard output instead.

create_clock -period 10.000 -name formatter_axis_ooc [get_ports m_axis_vid_aclk]
create_clock -period 6.667 -name formatter_pixel_ooc [get_ports dvi_clk]

set_false_path -from [get_clocks formatter_axis_ooc] -to [get_clocks formatter_pixel_ooc]
set_false_path -from [get_clocks formatter_pixel_ooc] -to [get_clocks formatter_axis_ooc]
