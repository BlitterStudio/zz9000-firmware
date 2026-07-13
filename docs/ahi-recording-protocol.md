# ZZ9000AX AHI recording firmware protocol

The FPGA design already routes the ZZ9000AX I2S input through an S2MM audio
formatter into an eight-period DDR ring and connects its completion interrupt
to the ARM. AHI recording therefore needs firmware and driver changes only;
the Verilog, Vivado project and committed bitstreams are unchanged.

## Registers

`REG_ZZ_AUDIO_CONFIG` (`0xF4`) retains codec presence in read bit 0. Writes are
a control mask: bit 0 enables playback-period interrupts and bit 1 enables
capture-ready interrupts. Older drivers write only zero or one and remain
compatible.

`REG_ZZ_AUDIO_RX_STATUS` (`0xF6`) is read-only:

| Bits | Meaning |
| --- | --- |
| 15 | Capture protocol implemented. |
| 14:12 | Newest completed receive-period index (0 through 7). |
| 11:0 | Completion sequence, incremented modulo 4096. |

Old firmware returns zero at `0xF6`, allowing a new driver to retain playback
without advertising recording.

## Publication order

The receive formatter writes 960 48 kHz S16LE stereo frames into each
3840-byte period. While capture interrupts are enabled, the receive ISR:

1. invalidates the completed DMA period;
2. converts it in place to the `REG_ZZ_AUDIO_SCALE` frame count using integer
   linear resampling;
3. stores the result as Amiga-native S16BE stereo;
4. flushes the valid `frame_count * 4` bytes and completes a memory barrier;
5. publishes the period index and incremented sequence;
6. asserts the existing shared Amiga audio interrupt.

The sequence update is the publication boundary: the driver must not consume a
period before observing its new sequence. Values outside 1 through 960 in
`REG_ZZ_AUDIO_SCALE` fall back to 960 frames.

The driver selects the ring using the existing `AP_RX_BUF_OFFS_HI/LO`
parameters. Its stride remains 3840 bytes even when the requested rate produces
fewer frames. At most the newest seven completed periods are recoverable after
a delayed interrupt; the eighth ring slot is already the formatter's active
write target and is never published as backlog.
