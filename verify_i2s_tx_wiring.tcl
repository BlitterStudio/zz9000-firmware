# The ADAU1701 clocks this I2S transmitter (C_IS_MASTER=0).  audio_clock
# samples its LRCLK one BCLK later and regenerates a phase-locked LRCLK edge
# at the transmitter, avoiding the direct pad-to-serializer timing path.
proc require_one {description objects} {
    if {[llength $objects] != 1} {
        error "$description: expected exactly one object, found [llength $objects] ($objects)"
    }
    return [lindex $objects 0]
}

set lrclk_port [require_one "I2SO_LRCLK port" [get_ports I2SO_LRCLK]]
if {[get_property UNCONNECTED $lrclk_port]} {
    error "I2SO_LRCLK is unconnected; it must drive audio_clock_0"
}
set tx_lrclk [require_one "transmitter lrclk_in" \
    [get_pins -hier -regexp {.*i2s_transmitter_0/lrclk_in}]]
set tx_lrclk_net [require_one "transmitter lrclk net" [get_nets -of_objects $tx_lrclk]]

set bridge_lrclk [require_one "audio clock lrclk_in" \
    [get_pins -hier -regexp {.*audio_clock_0/lrclk_in}]]
set bridge_lrclk_net [require_one "audio clock lrclk net" [get_nets -of_objects $bridge_lrclk]]
if {$tx_lrclk_net eq $bridge_lrclk_net} {
    error "i2s_transmitter_0/lrclk_in is directly driven by $bridge_lrclk_net, expected regenerated LRCLK"
}

set regenerated_lrclk [require_one "audio regenerated LRCLK output" \
    [get_pins -hier -regexp {.*audio_clock_0/tx_lrclk_out}]]
set tx_lrclk_drivers [get_pins -of_objects $tx_lrclk_net -filter {DIRECTION == OUT}]
if {[lsearch -exact $tx_lrclk_drivers $regenerated_lrclk] < 0} {
    error "i2s_transmitter_0/lrclk_in drivers ($tx_lrclk_drivers) omit $regenerated_lrclk"
}

set mclk_port [require_one "I2SO_MCLK port" [get_ports I2SO_MCLK]]
if {[get_property UNCONNECTED $mclk_port]} {
    error "I2SO_MCLK is unconnected; audio_clock_0/bclk_out must drive the serial clocks"
}
set tx_sclk [require_one "transmitter sclk_in" \
    [get_pins -hier -regexp {.*i2s_transmitter_0/sclk_in}]]
set tx_sclk_net [require_one "transmitter sclk net" [get_nets -of_objects $tx_sclk]]
set bridge_bclk [require_one "audio clock bclk_out" \
    [get_pins -hier -regexp {.*audio_clock_0/bclk_out}]]
set tx_sclk_drivers [get_pins -of_objects $tx_sclk_net -filter {DIRECTION == OUT}]
if {[lsearch -exact $tx_sclk_drivers $bridge_bclk] < 0} {
    error "i2s_transmitter_0/sclk_in drivers ($tx_sclk_drivers) omit $bridge_bclk"
}

puts "I2S TX wiring: regenerated LRCLK and serial clock topology verified"
