# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Full-rate native-video capture relies on a fixed package-pin-to-register
# delay.  Fail the build if synthesis or implementation ignored the RTL IOB
# attributes and placed any used RGB input register in ordinary fabric.
set vcap_iob_regs [get_cells -quiet -hier -regexp \
    {.*vcap_[rgb]_iob_reg(\[[0-7]\])?$}]
set vcap_iob_count [llength $vcap_iob_regs]

# RGB_MODE 0 uses all 24 pins; the nibble modes use 12.  Any other count means
# the intended input stage was pruned, duplicated, or renamed unexpectedly.
if {$vcap_iob_count != 12 && $vcap_iob_count != 24} {
    error "Expected 12 or 24 VCAP RGB input registers, found $vcap_iob_count"
}

set misplaced {}
foreach cell $vcap_iob_regs {
    set location [get_property LOC $cell]
    if {![string match "ILOGIC_*" $location]} {
        lappend misplaced "$cell ($location)"
    }
}

if {[llength $misplaced] != 0} {
    error "VCAP RGB input registers not packed into ILOGIC: [join $misplaced {, }]"
}

puts "Verified $vcap_iob_count VCAP RGB input registers in ILOGIC"
