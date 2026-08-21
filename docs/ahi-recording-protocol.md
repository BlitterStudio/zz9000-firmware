# ZZ9000AX AHI recording firmware protocol

The FPGA design already routes the ZZ9000AX I2S input through an S2MM audio
formatter into an eight-period DDR ring and connects its completion interrupt
to the ARM. AHI recording therefore needs firmware and driver changes only;
the Verilog, Vivado project and committed bitstreams are unchanged.

## Registers

`REG_ZZ_AUDIO_CONFIG` (`0xF4`) retains codec presence in read bit 0. Writes are
a control mask: bit 0 enables playback-period interrupts and bit 1 enables
capture-ready interrupts. Older drivers write only zero or one and remain
compatible. On reads, bit 1 advertises the packed transmit-period status at
`0xF8`; the distinct read/write meaning makes this a stable capability marker
that cannot be confused with an older firmware's raw 16-bit TX sequence.

`REG_ZZ_AUDIO_RX_STATUS` (`0xF6`) is read-only:

| Bits | Meaning |
| --- | --- |
| 15 | Capture protocol implemented. |
| 14:12 | Newest completed receive-period index (0 through 7). |
| 11:0 | Completion sequence, incremented modulo 4096. |

Old firmware returns zero at `0xF6`, allowing a new driver to retain playback
without advertising recording.

`REG_ZZ_AUDIO_TX_STATUS` (`0xF8`) is read-only:

| Bits | Meaning |
| --- | --- |
| 15 | Transmit-period publication implemented. |
| 14:12 | Most recently completed transmit-period index (0 through 7). |
| 11:0 | Completion sequence, incremented modulo 4096. |

Firmware publishes the status after every transmit period completes, before
asserting the shared audio interrupt. A capable driver samples the sequence
when playback starts and services playback only after observing a different
value. It refills the published completed period rather than assuming that the
continuously running formatter began at ring offset zero. This both
distinguishes playback completions from capture-only wake-ups and prevents a
startup write from touching the period MM2S is actively reading.

A new driver uses this layout only when both read bit 1 at `0xF4` and status
bit 15 are set, otherwise it falls back to the legacy sequential cursor. An
old driver still observes a changing 16-bit status when used with new
firmware.

## Publication order

The receive formatter writes 960 48 kHz S16LE stereo frames into each
3840-byte period. While capture interrupts are enabled, the receive ISR:

1. invalidates the completed DMA period;
2. converts it to the `REG_ZZ_AUDIO_SCALE` frame count through the shared
   qualified band-limited kernel (`audio_convert.c`): the period is first
   copied to CPU-side scratch (a long kernel reads inputs that early
   outputs would overwrite in place), and the result is written back as
   Amiga-native S16BE stereo. Filter history carries across periods
   inside the converter instance while the exact per-period counts keep
   the rational phase landing on every period boundary;
3. flushes the valid `frame_count * 4` bytes and completes a memory barrier;
4. publishes the period index and incremented sequence;
5. asserts the existing shared Amiga audio interrupt;

The sequence update is the publication boundary: the driver must not consume a
period before observing its new sequence. Supported conversion counts are
160/240/480/640/882/960 frames (8/12/24/32/44.1/48 kHz); any other value in
the register's 1-960 range, including invalid values, converts as native
48 kHz identity (960 frames, endian swap only).

## Receive-ring retargeting

Writes to `AP_RX_BUF_OFFS_HI/LO` select a pending receive ring; the formatter
does not use that address until the main loop performs its deferred I2S/audio
formatter reinitialization. Firmware disables capture publication before
changing the CPU-side ring pointer. RX interrupts during this window are
acknowledged but do not convert data, advance the receive sequence or interrupt
the Amiga. Publication resumes only after the formatter DMA has restarted on
the new ring.

The receive sequence remains monotonic across reinitialization. This lets a
driver sample the status immediately after the buffer-register writes and
still observe a distance of exactly one when the first period from the new
ring is published.

The driver selects the ring using the existing `AP_RX_BUF_OFFS_HI/LO`
parameters. Its stride remains 3840 bytes even when the requested rate produces
fewer frames. At most the newest seven completed periods are recoverable after
a delayed interrupt; the eighth ring slot is already the formatter's active
write target and is never published as backlog.
