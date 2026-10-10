# SPDX-License-Identifier: GPL-3.0-or-later

# impl_1 STEPS.POST_ROUTE_PHYS_OPT_DESIGN.TCL.POST hook.
#
# The ADAU1701 I2S input capture (I2SO_LRCLK -> i2s_transmitter_0 CE) has
# structurally almost no fast-corner setup margin (see zz9000.xdc), so the
# post-route phys_opt_design step is load-bearing for it.  A single pass can
# stop a few picoseconds short of the result a second pass reaches from the
# state the first one left behind.  Re-run the step only while the worst
# per-clock-group setup slack (the same query the release gate uses) is
# negative; a design that already meets timing is left untouched, so passing
# variants are bit-for-bit the same as without this hook.

proc zz9000_post_route_worst_setup {} {
    set worst {}
    foreach clock [get_clocks] {
        set paths [get_timing_paths -quiet -from $clock -delay_type max \
            -max_paths 1 -nworst 1]
        if {[llength $paths] == 0} {
            continue
        }
        set slack [get_property SLACK [lindex $paths 0]]
        if {![string is double -strict $slack]} {
            continue
        }
        if {$worst eq {} || $slack < $worst} {
            set worst $slack
        }
    }
    return $worst
}

set zz9000_retry_limit 2
for {set pass 1} {$pass <= $zz9000_retry_limit} {incr pass} {
    set wns [zz9000_post_route_worst_setup]
    if {$wns eq {} || $wns >= 0.0} {
        break
    }
    puts "POST_ROUTE_RETRY: pass $pass, worst group setup slack ${wns}ns; re-running phys_opt_design"
    phys_opt_design -directive AggressiveExplore
}
puts "POST_ROUTE_RETRY: worst group setup slack [zz9000_post_route_worst_setup]ns"
