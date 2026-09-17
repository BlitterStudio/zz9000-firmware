# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Imported EARLY only by --capture-c28 builds. Use the fastest physical
# NTSC clock (PAL is slower): C28 about 28.636 MHz, E7M one quarter of it.
# The M=32/O=32 capture MMCM therefore has an actual 34.9 ns STA period.
# No legacy four-cycle capture multicycle belongs to this clock source.
create_clock -period 34.900 -name amiga_c28 [get_ports ZORRO_C28D]
create_clock -period 139.600 -name amiga_e7m [get_ports ZORRO_E7M]

# B20 is regular I/O; permit only this selected input-to-MMCM route.
set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets ZORRO_C28D]
set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets ZORRO_C28D_IBUF]
