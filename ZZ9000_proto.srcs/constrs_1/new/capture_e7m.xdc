# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Legacy E7M capture only. This preserves the existing physical operating
# point and STA workaround; it does not qualify the under-frequency VCO.
set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets ZORRO_E7M]
set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets ZORRO_E7M_IBUF]

create_clock -period 35.000 -name amiga_e7m -add [get_ports ZORRO_E7M]

# The 35.000 ns E7M declaration above is a modeling fiction: it keeps the
# MMCM's modeled VCO (x32) above the Artix-7 FVCOMIN of 600 MHz. Physically
# E7M is 7.094 MHz (PAL) / 7.159 MHz (NTSC), so the MMCM-derived capture
# clock on CLKOUT0 (CLKIN x4) really runs at 28.375/28.636 MHz - period
# 35.24/34.93 ns - while STA analyzes it at 8.750 ns, exactly 4x too fast.
# Restore the physical single-cycle budget for intra-capture-domain paths
# with a 4-cycle setup multicycle (3-cycle hold keeps the original
# adjacent-edge hold relationship). Cross-domain paths are unaffected:
# the CLKOUT0->ACLK and CLKOUT0->CLKOUT1 crossings are false-pathed in zz9000.xdc
# and use XPM/handshake synchronization. Without this correction, deep
# capture-domain logic such as the E7M-locked sample-grid pairing (17
# logic levels, ~12.5 ns) can never meet the fictional 8.75 ns budget and
# the post-route timing gate rejects physically sound builds.
set_multicycle_path -quiet 4 -setup \
    -from [get_clocks -quiet -of_objects [get_pins -quiet zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/mmcm_adv_inst/CLKOUT0]] \
    -to [get_clocks -quiet -of_objects [get_pins -quiet zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/mmcm_adv_inst/CLKOUT0]]
set_multicycle_path -quiet 3 -hold \
    -from [get_clocks -quiet -of_objects [get_pins -quiet zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/mmcm_adv_inst/CLKOUT0]] \
    -to [get_clocks -quiet -of_objects [get_pins -quiet zz9000_ps_i/MNTZorro_v0_1_S00_AXI_0/inst/mmcm_adv_inst/CLKOUT0]]
