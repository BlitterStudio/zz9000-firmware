# Compress the configuration bitstream (skips repeated frames); the Zynq
# PCAP path loads compressed bitstreams transparently and BOOT.bin shrinks
# by roughly 2.5 MB.
set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]

set_property PACKAGE_PIN K17 [get_ports VCAP_B2]
set_property PACKAGE_PIN F20 [get_ports VCAP_B1]
set_property PACKAGE_PIN F19 [get_ports VCAP_B0]
set_property PACKAGE_PIN M20 [get_ports VCAP_R2]
set_property PACKAGE_PIN M19 [get_ports VCAP_R1]
set_property PACKAGE_PIN G14 [get_ports VCAP_R0]
set_property PACKAGE_PIN N15 [get_ports VCAP_R3]
set_property PACKAGE_PIN H17 [get_ports VCAP_G3]
set_property PACKAGE_PIN K18 [get_ports VCAP_B3]
set_property PACKAGE_PIN E18 [get_ports VCAP_B4]
set_property PACKAGE_PIN E19 [get_ports VCAP_B5]
set_property PACKAGE_PIN E17 [get_ports VCAP_B6]
set_property PACKAGE_PIN D18 [get_ports VCAP_B7]
set_property PACKAGE_PIN B19 [get_ports VCAP_G0]
set_property PACKAGE_PIN A20 [get_ports VCAP_G1]
set_property PACKAGE_PIN H16 [get_ports VCAP_G2]
set_property PACKAGE_PIN N16 [get_ports VCAP_R4]
set_property PACKAGE_PIN H15 [get_ports VCAP_G4]
set_property PACKAGE_PIN J20 [get_ports VCAP_R5]
set_property PACKAGE_PIN G15 [get_ports VCAP_G5]
set_property PACKAGE_PIN H20 [get_ports VCAP_R6]
set_property PACKAGE_PIN M14 [get_ports VCAP_G6]
set_property PACKAGE_PIN C20 [get_ports VCAP_R7]
set_property PACKAGE_PIN M15 [get_ports VCAP_G7]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[22]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[21]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[20]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[19]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[18]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[17]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[16]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[15]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[14]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[13]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[12]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[11]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[10]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[9]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[8]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[7]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[6]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[5]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_ADDR[0]}]

set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_DATA[15]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_DATA[14]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_DATA[13]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_DATA[12]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_DATA[11]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_DATA[10]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_DATA[9]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_DATA[8]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_DATA[7]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_DATA[6]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_DATA[5]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_DATA[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_DATA[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_DATA[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_DATA[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {ZORRO_DATA[0]}]

set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_ADDRDIR]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_DATADIR]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_ADDRDIR2]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_NBRN]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_NBGN]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_C28D]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_READ]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_DOE]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_NDS0]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_NDS1]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_NLDS]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_NUDS]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_NFCS]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_NCCS]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_NSLAVE]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_NCINH]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_NCFGIN]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_NCFGOUT]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_NDTACK]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_NIORST]
set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_E7M]

set_property IOSTANDARD LVCMOS33 [get_ports ZORRO_INT6]


set_property PACKAGE_PIN V20 [get_ports {ZORRO_DATA[15]}]
set_property PACKAGE_PIN W15 [get_ports {ZORRO_DATA[14]}]
set_property PACKAGE_PIN V15 [get_ports {ZORRO_DATA[13]}]
set_property PACKAGE_PIN T20 [get_ports {ZORRO_DATA[12]}]
set_property PACKAGE_PIN P20 [get_ports {ZORRO_DATA[11]}]
set_property PACKAGE_PIN N20 [get_ports {ZORRO_DATA[10]}]
set_property PACKAGE_PIN J15 [get_ports {ZORRO_DATA[9]}]
set_property PACKAGE_PIN L15 [get_ports {ZORRO_DATA[8]}]
set_property PACKAGE_PIN F17 [get_ports {ZORRO_DATA[7]}]
set_property PACKAGE_PIN F16 [get_ports {ZORRO_DATA[6]}]
set_property PACKAGE_PIN H18 [get_ports {ZORRO_DATA[5]}]
set_property PACKAGE_PIN K16 [get_ports {ZORRO_DATA[4]}]
set_property PACKAGE_PIN J16 [get_ports {ZORRO_DATA[3]}]
set_property PACKAGE_PIN K14 [get_ports {ZORRO_DATA[2]}]
set_property PACKAGE_PIN J14 [get_ports {ZORRO_DATA[1]}]
set_property PACKAGE_PIN L14 [get_ports {ZORRO_DATA[0]}]

set_property PACKAGE_PIN U9 [get_ports ZORRO_ADDRDIR]
set_property PACKAGE_PIN T15 [get_ports ZORRO_ADDRDIR2]
set_property PACKAGE_PIN U17 [get_ports ZORRO_NBRN]
set_property PACKAGE_PIN T14 [get_ports ZORRO_NBGN]
set_property PACKAGE_PIN B20 [get_ports ZORRO_C28D]
set_property PACKAGE_PIN Y12 [get_ports ZORRO_DATADIR]
set_property PACKAGE_PIN Y19 [get_ports ZORRO_READ]
set_property PACKAGE_PIN W10 [get_ports ZORRO_DOE]
#set_property PACKAGE_PIN C20 [get_ports ZORRO_NMTCR]
set_property PACKAGE_PIN Y14 [get_ports ZORRO_NDS0]
set_property PACKAGE_PIN W14 [get_ports ZORRO_NDS1]
set_property PACKAGE_PIN Y18 [get_ports ZORRO_NLDS]
set_property PACKAGE_PIN W20 [get_ports ZORRO_NUDS]
set_property PACKAGE_PIN V5 [get_ports ZORRO_NFCS]
set_property PACKAGE_PIN U13 [get_ports ZORRO_NCCS]
set_property PACKAGE_PIN U15 [get_ports ZORRO_NSLAVE]
set_property PACKAGE_PIN U14 [get_ports ZORRO_NCINH]
set_property PACKAGE_PIN W13 [get_ports ZORRO_NCFGIN]
set_property PACKAGE_PIN P14 [get_ports ZORRO_NCFGOUT]
set_property PACKAGE_PIN W9 [get_ports ZORRO_NDTACK]
set_property PACKAGE_PIN V12 [get_ports ZORRO_NIORST]
set_property PACKAGE_PIN R14 [get_ports ZORRO_E7M]
set_property PACKAGE_PIN U20 [get_ports ZORRO_INT6]

set_property PACKAGE_PIN Y13 [get_ports {ZORRO_ADDR[22]}]
set_property PACKAGE_PIN W11 [get_ports {ZORRO_ADDR[21]}]
set_property PACKAGE_PIN Y11 [get_ports {ZORRO_ADDR[20]}]
set_property PACKAGE_PIN Y9 [get_ports {ZORRO_ADDR[19]}]
set_property PACKAGE_PIN Y8 [get_ports {ZORRO_ADDR[18]}]
set_property PACKAGE_PIN Y7 [get_ports {ZORRO_ADDR[17]}]
set_property PACKAGE_PIN Y6 [get_ports {ZORRO_ADDR[16]}]
set_property PACKAGE_PIN T5 [get_ports {ZORRO_ADDR[15]}]
set_property PACKAGE_PIN U5 [get_ports {ZORRO_ADDR[14]}]
set_property PACKAGE_PIN W6 [get_ports {ZORRO_ADDR[13]}]
set_property PACKAGE_PIN V6 [get_ports {ZORRO_ADDR[12]}]
set_property PACKAGE_PIN W8 [get_ports {ZORRO_ADDR[11]}]
set_property PACKAGE_PIN V8 [get_ports {ZORRO_ADDR[10]}]
set_property PACKAGE_PIN V7 [get_ports {ZORRO_ADDR[9]}]
set_property PACKAGE_PIN U7 [get_ports {ZORRO_ADDR[8]}]
set_property PACKAGE_PIN U8 [get_ports {ZORRO_ADDR[7]}]
set_property PACKAGE_PIN U10 [get_ports {ZORRO_ADDR[6]}]
set_property PACKAGE_PIN T11 [get_ports {ZORRO_ADDR[5]}]
set_property PACKAGE_PIN V13 [get_ports {ZORRO_ADDR[4]}]
set_property PACKAGE_PIN V10 [get_ports {ZORRO_ADDR[3]}]
set_property PACKAGE_PIN T9 [get_ports {ZORRO_ADDR[2]}]
set_property PACKAGE_PIN T10 [get_ports {ZORRO_ADDR[1]}]
set_property PACKAGE_PIN V11 [get_ports {ZORRO_ADDR[0]}]


set_property IOSTANDARD LVCMOS33 [get_ports VCAP_B7]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_B6]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_B5]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_B4]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_B3]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_B2]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_B1]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_B0]

set_property IOSTANDARD LVCMOS33 [get_ports VCAP_R7]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_R6]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_R5]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_R4]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_R3]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_R2]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_R1]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_R0]

set_property IOSTANDARD LVCMOS33 [get_ports VCAP_VSYNC]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_HSYNC]

set_property IOSTANDARD LVCMOS33 [get_ports VCAP_G0]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_G1]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_G2]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_G3]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_G4]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_G5]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_G6]
set_property IOSTANDARD LVCMOS33 [get_ports VCAP_G7]

set_property PACKAGE_PIN Y17 [get_ports VCAP_VSYNC]
set_property PACKAGE_PIN Y16 [get_ports VCAP_HSYNC]

#set_property PULLUP TRUE [get_ports VCAP_VSYNC]
#set_property PULLUP TRUE [get_ports VCAP_HSYNC]


set_property IOSTANDARD LVCMOS33 [get_ports {VGA_R[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_R[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_R[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_R[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_R[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_R[5]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_R[6]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_R[7]}]

set_property IOSTANDARD LVCMOS33 [get_ports {VGA_G[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_G[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_G[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_G[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_G[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_G[5]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_G[6]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_G[7]}]

set_property IOSTANDARD LVCMOS33 [get_ports {VGA_B[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_B[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_B[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_B[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_B[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_B[5]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_B[6]}]
set_property IOSTANDARD LVCMOS33 [get_ports {VGA_B[7]}]

set_property PACKAGE_PIN D20 [get_ports {VGA_R[0]}]
set_property PACKAGE_PIN M17 [get_ports {VGA_R[1]}]
set_property PACKAGE_PIN M18 [get_ports {VGA_R[2]}]
set_property PACKAGE_PIN G17 [get_ports {VGA_R[3]}]
set_property PACKAGE_PIN G18 [get_ports {VGA_R[4]}]
set_property PACKAGE_PIN K19 [get_ports {VGA_R[5]}]
set_property PACKAGE_PIN J19 [get_ports {VGA_R[6]}]
set_property PACKAGE_PIN J18 [get_ports {VGA_R[7]}]

set_property PACKAGE_PIN V17 [get_ports {VGA_G[0]}]
set_property PACKAGE_PIN G19 [get_ports {VGA_G[1]}]
set_property PACKAGE_PIN G20 [get_ports {VGA_G[2]}]
set_property PACKAGE_PIN L19 [get_ports {VGA_G[3]}]
set_property PACKAGE_PIN L20 [get_ports {VGA_G[4]}]
set_property PACKAGE_PIN L16 [get_ports {VGA_G[5]}]
set_property PACKAGE_PIN L17 [get_ports {VGA_G[6]}]
set_property PACKAGE_PIN D19 [get_ports {VGA_G[7]}]

set_property PACKAGE_PIN W19 [get_ports {VGA_B[0]}]
set_property PACKAGE_PIN W18 [get_ports {VGA_B[1]}]
set_property PACKAGE_PIN P19 [get_ports {VGA_B[2]}]
set_property PACKAGE_PIN N18 [get_ports {VGA_B[3]}]
set_property PACKAGE_PIN T19 [get_ports {VGA_B[4]}]
set_property PACKAGE_PIN R19 [get_ports {VGA_B[5]}]
set_property PACKAGE_PIN P15 [get_ports {VGA_B[6]}]
set_property PACKAGE_PIN P16 [get_ports {VGA_B[7]}]

set_property IOSTANDARD LVCMOS33 [get_ports VGA_PCLK]
set_property IOSTANDARD LVCMOS33 [get_ports VGA_HS]
set_property IOSTANDARD LVCMOS33 [get_ports VGA_DE]
set_property IOSTANDARD LVCMOS33 [get_ports VGA_VS]
set_property PACKAGE_PIN R17 [get_ports VGA_VS]
set_property PACKAGE_PIN R16 [get_ports VGA_HS]
set_property PACKAGE_PIN N17 [get_ports VGA_DE]
set_property PACKAGE_PIN V18 [get_ports VGA_PCLK]

# audio output
set_property IOSTANDARD LVCMOS33 [get_ports I2SO_LRCLK]
set_property IOSTANDARD LVCMOS33 [get_ports I2SO_BCLK]
set_property IOSTANDARD LVCMOS33 [get_ports I2SO_D0]
set_property IOSTANDARD LVCMOS33 [get_ports I2SO_MCLK]
set_property IOSTANDARD LVCMOS33 [get_ports I2SI_D0]
set_property IOSTANDARD LVCMOS33 [get_ports {I2SO_RESETn[0]}]

create_clock -period 80.000 -name i2s_mclk -add [get_ports I2SO_BCLK]

# ADAU1701 TDM8 serial output data and LRCLK change after OUTPUT_BCLK falls.
# Register them on the following rising edge. The 40 ns maximum is the
# datasheet tSODM limit (Table 7, "SDATA_OUTx delay. Time from OUTPUT_BCLK
# falling in master mode"); zero is the conservative minimum because no
# minimum clock-to-output delay is specified.  Because tSODM consumes the
# entire half period, the setup margin here is structurally only
# (BCLK insertion 1.6 ns) - (pin-to-register delay ~1.4 ns) - uncertainty;
# the post-route phys_opt_design step in the build flow exists to close
# exactly this margin (trial build: routed WNS -0.090 -> +0.143 after
# post-route phys_opt, fast corner).
set_input_delay -clock i2s_mclk -clock_fall -max 40.000 [get_ports {I2SI_D0 I2SO_LRCLK}]
set_input_delay -clock i2s_mclk -clock_fall -min 0.000 [get_ports {I2SI_D0 I2SO_LRCLK}]

set_property PACKAGE_PIN U18 [get_ports I2SO_BCLK]
set_property PACKAGE_PIN W16 [get_ports I2SO_LRCLK]
set_property PACKAGE_PIN V16 [get_ports I2SO_D0]
set_property PACKAGE_PIN R18 [get_ports I2SO_MCLK]
set_property PACKAGE_PIN T17 [get_ports I2SI_D0]
set_property PACKAGE_PIN U19 [get_ports {I2SO_RESETn[0]}]

#set_property IOSTANDARD LVCMOS33 [get_ports HDMI_INTN]
#set_property PACKAGE_PIN W19 [get_ports HDMI_INTN]


# ZORRO_NFCS is the ODDR clock (C input) for the z3_nslave_oddr and
# z3_ncinh_oddr primitives that drive /SLAVE and /CINH.  Declaring it as a
# clock keeps the port-to-ODDR address-phase claim paths edge-timed
# (zorro_fcs fall to fall) and anchors the ZORRO_ADDR/DATA//CFGIN input
# delays below.  ZORRO_NFCS idles high and falls to start a Z3 bus cycle
# (active-low /FCS).  Minimum Z3 cycle ≈ 140 ns; waveform {0 70} = rising
# at 0 ns, falling at 70 ns.
create_clock -period 140.000 -name zorro_fcs -waveform {0 70} -add [get_ports ZORRO_NFCS]

# Address, data, and /CFGIN are sampled combinatorially into the address-phase
# claim before the ODDR captures D2 on the falling /FCS edge.  Per Z3 spec,
# ADDR/DATA are stable at least 15 ns before /FCS falls, giving about 55 ns of
# slack with 100 ns max input delay relative to the falling edge at t=70 ns.
set_input_delay -clock zorro_fcs -clock_fall -max 100.000 [get_ports {ZORRO_DATA[*]}]
set_input_delay -clock zorro_fcs -clock_fall -min 0.000 [get_ports {ZORRO_DATA[*]}]
set_input_delay -clock zorro_fcs -clock_fall -max 100.000 [get_ports {ZORRO_ADDR[*]}]
set_input_delay -clock zorro_fcs -clock_fall -min 0.000 [get_ports {ZORRO_ADDR[*]}]
set_input_delay -clock zorro_fcs -clock_fall -max 100.000 [get_ports ZORRO_NCFGIN]
set_input_delay -clock zorro_fcs -clock_fall -min 0.000 [get_ports ZORRO_NCFGIN]

# The same ports are also consumed by ACLK-domain registers: the Z3
# phase-sampled latches z3addr2 / z3_din_* / zdata_in_sync2 and the
# znCFGIN_sync chain.  Zorro has no FPGA-known capture clock, and the design
# guarantees the margin by construction (bus stable >= 15 ns before the
# /FCS edge that matters, cycle >= 140 ns; single-bit controls cross through
# synchronizers), so these port-to-ACLK paths are cut.  They must stay port
# scoped: a datapath-only bound from zorro_fcs still adds the 100 ns input
# delay above to the arrival time and fails by about 82 ns.
# report_exceptions lists these lines as "Non-existent path" on the
# implemented netlist even though they do cut the paths (Vivado 2018.3
# status quirk for -clock_fall input-delay launches).
set_false_path -from [get_ports {ZORRO_DATA[*]}] -to [get_clocks clk_fpga_0]
set_false_path -from [get_ports {ZORRO_ADDR[*]}] -to [get_clocks clk_fpga_0]
set_false_path -from [get_ports ZORRO_NCFGIN] -to [get_clocks clk_fpga_0]

# ZORRO_NFCS itself is also sampled as data by the znFCS_sync two-stage
# synchronizer (its only data consumer); the first stage of a synchronizer is
# unanalyzable by construction, so cut just that entry.  The create_clock
# above keeps the /FCS-driven ODDR paths and the bus input delays timed.
set_false_path -quiet -from [get_ports -quiet ZORRO_NFCS]

# ACLK-domain registers driving the ODDR D2 combinatorial claim path
# (z_confout, z3_ram_low, z3_reg_low, z3_fast_low) are intentionally
# asynchronous to zorro_fcs: the FSM writes them and they stay stable for
# many full Z3 bus cycles before /FCS falls.  A datapath-only bound keeps
# that guarantee measurable (settle well inside one 140 ns cycle, and well
# under the 15 ns /FCS setup budget) without the bogus hold relationships an
# edge-based exception produces across unrelated clocks.
set_max_delay -datapath_only -from [get_clocks clk_fpga_0] -to [get_clocks zorro_fcs] 20.000

# ACLK <-> default (75 MHz power-on) pixel-clock crossings: the formatter
# snapshots configuration in vblank, crosses status through two-stage
# synchronizers (need_frame_sync_reg / need_line_fetch_reg /
# video_control_*_blank), and moves stream data through XPM / FIFO CDC
# primitives that carry their own, more specific set_max_delay
# -datapath_only and false-path exceptions.  A blanket false path here used
# to override those generated constraints (methodology TIMING-24) and left
# the whole boundary unanalyzed; a clock-pair bound restores analysis while
# XPM's per-cell exceptions keep precedence by specificity.  The 150 MHz
# runtime reconfiguration of the same PLL output is bounded separately in
# runtime_pixel_timing.xdc.
set_max_delay -datapath_only -from [get_clocks clk_fpga_0] -to [get_clocks -of_objects [get_pins zz9000_ps_i/clk_wiz_0/inst/CLK_CORE_DRP_I/clk_inst/plle2_adv_inst/CLKOUT0]] 20.000
set_max_delay -datapath_only -from [get_clocks -of_objects [get_pins zz9000_ps_i/clk_wiz_0/inst/CLK_CORE_DRP_I/clk_inst/plle2_adv_inst/CLKOUT0]] -to [get_clocks clk_fpga_0] 20.000

# The active capture source XDC declares E7M in both builds and C28 only
# in the opt-in build. A non-empty clock collection avoids conditional Tcl,
# which Vivado 2018.3 does not support inside XDC files.  The one path this
# cuts today is the capture_clock_control/e7m_sync_reg[0] two-stage
# synchronizer entry, for which a false path is the canonical treatment.
set_false_path -quiet -from [get_clocks -quiet {amiga_e7m amiga_c28}] -to [get_clocks -quiet clk_fpga_0]

# Capture clock -> ACLK: sample data and status cross through videocap XPM
# CDC primitives (their generated max_dpo / false-path exceptions are more
# specific than this bound and keep precedence) and through quasi-static
# readback registers (rr_data and friends).  The previous blanket false path
# overrode the XPM bounds (methodology TIMING-24).  The two lines that used
# to follow it (capture MMCM CLKOUT1 -> clk_fpga_0 and CLKOUT0 -> CLKOUT1)
# matched no paths on the implemented netlist and were removed; the real
# CLKOUT1 -> CLKOUT0 capture-domain crossing stays timed by the MMCM phase
# relationship.
set_max_delay -datapath_only -quiet -from [get_clocks -quiet -of_objects [get_pins -quiet zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/mmcm_adv_inst/CLKOUT0]] -to [get_clocks -quiet clk_fpga_0] 20.000

# ---------------------------------------------------------------------------
# Unconstrained I/O closure (check_timing hygiene).
#
# VCAP_* RGB/sync inputs sample the Amiga video pipeline.  The source is
# asynchronous to every FPGA-declared clock, and the capture MMCM phase is
# trained at runtime to centre the sampling eye (videocap calibration), so
# any numbered input delay would be fiction.  False-path the capture entry;
# all intra-capture-domain timing stays constrained via capture_e7m.xdc /
# capture_c28.xdc.
set_false_path -quiet -from [get_ports -quiet {VCAP_B? VCAP_G? VCAP_R? VCAP_HSYNC VCAP_VSYNC}]

# Zorro bus strobes and reset are asynchronous control inputs; they enter
# the ACLK domain only through the two-stage synchronizer chains
# (zREAD_sync, znDS0/znDS1_sync, znUDS/znLDS_sync, znRST_sync - ASYNC_REG).
set_false_path -quiet -from [get_ports -quiet {ZORRO_NDS0 ZORRO_NDS1 ZORRO_NLDS ZORRO_NUDS ZORRO_READ ZORRO_NIORST}]

# Zorro bus outputs (/DTACK, /SLAVE, /CINH, /CFGOUT, direction and interrupt
# pins, and the master-cycle address/data outputs) follow an asynchronous
# handshake: the bus master samples them late in cycles that last >= 140 ns
# (Z3) / >= 500 ns (Z2), so there is no FPGA-known capture edge to constrain
# against.  False-path the output boundary; the input direction of the bus
# pins stays constrained by the zorro_fcs input delays above.
set_false_path -quiet -to [get_ports -quiet {ZORRO_ADDR[*] ZORRO_DATA[*] ZORRO_ADDRDIR ZORRO_DATADIR ZORRO_INT6 ZORRO_NCFGOUT ZORRO_NCINH ZORRO_NDTACK ZORRO_NSLAVE}]

# I2SO_D0 is launched by the I2S transmitter's BCLK rising-edge register
# (rSDataOut, launch edge 0 ns on the routed netlist) and captured by the
# ADAU1701 half a BCLK period later; the constraint therefore references
# the BCLK falling edge.  [Inference: with rising-edge capture the
# 1.7-5.7 ns clock-to-pin delay could not meet tSIH, yet audio works.]
# tSIS = tSIH = 10 ns (datasheet Table 7).  Trial netlist: setup +18.6 ns,
# hold +33.3 ns.
set_output_delay -clock i2s_mclk -clock_fall -max 10.000 [get_ports I2SO_D0]
set_output_delay -clock i2s_mclk -clock_fall -min -10.000 [get_ports I2SO_D0]

# I2SO_MCLK is a forwarded audio master clock with no companion data at the
# codec's MCLKI pin (the codec PLL only requires pulse widths, tMP), and
# I2SO_RESETn is a quasi-static reset (tRLPW only).  No edge relationship to
# constrain; false-path both.
set_false_path -quiet -to [get_ports -quiet {I2SO_MCLK I2SO_RESETn[*]}]

