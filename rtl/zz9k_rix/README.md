# RasterIX EF hardware contract

Zorro III release images include the external-framebuffer RasterIX EF renderer.
Other release images do not instantiate RasterIX or alter its ports. The
renderer is a 100 MHz FCLK0, one-TMU RGB565/Z16 core with a 64-bit AXI memory
master dedicated to PS HP3. HP0 remains scanout, HP1 remains MNTZorro plus
capture, and audio TX/RX share an interconnect on HP2.

## Discovery and CSR (`0x83c60000`)

Firmware MUST read `IDENTITY` before any other access and require `RIX1`.
`CONTROL.ENABLE` MUST be set before committing a FIFO packet.  This CSR has no
separate command-buffer descriptor engine, completion queue, or interrupt.

| Offset | Name | Description |
| --- | --- | --- |
| `0x00` | `IDENTITY` | `0x52495831` (`RIX1`) |
| `0x04` | `VERSION` | `0x00020000` (EF FIFO transport) |
| `0x08` | `CONTROL` | bit 0 `ENABLE`; bit 1 W1P renderer soft reset.  Reset discards in-flight renderer state; firmware MUST reset the FIFO separately before reuse. |
| `0x0c` | `STATUS` | bit 0 enabled; bit 1 idle; bit 2 renderer busy; bit 3 triangle active; bit 4 rasterizer stall; bits 15:8 outstanding writes; bits 23:16 outstanding reads. |
| `0x10` | `DBG_STREAM` | bit 0 command valid, bit 1 command ready, bit 2 RasterIX response valid, bit 3 response last. |
| `0x14` | `DBG_RESPONSE` | last live RasterIX response word (diagnostic only) |
| `0x38` | `DBG_AW` | accepted memory write addresses since reset |
| `0x3c` | `DBG_W` | accepted memory write beats since reset |
| `0x40` | `DBG_B` | accepted memory write responses since reset |
| `0x44` | `DBG_AR` | accepted memory read addresses since reset |
| `0x48` | `DBG_R` | accepted memory read beats since reset |
| `0x4c` | `DBG_LAST_AR` | last renderer memory read address |
| `0x50` | `DBG_LAST_ARID` | low 6 bits: last renderer memory read ID |

`STATUS.IDLE` is a liveness/drain observation only.  It is not a frame fence:
the command FIFO can contain a not-yet-started packet and neither a CSR counter
nor an IRQ replaces a rendered-pixel fence.  Firmware MUST use the RasterIX
`MEMSET` fence-pixel convention in a non-scanned row, then poll that RGB565
pixel after cache maintenance.  The renderer's `fb_swapped` acknowledgement is
held high; streams MUST NOT request `OP_FRAMEBUFFER SWAP`.

## Command FIFO (registers `0x83c70000`, data `0x83c80000`)

The command ingress is Xilinx `axi_fifo_mm_s` TX-only, depth 4096 words.  Its
`AXI_STR_TXD` connects directly to RasterIX `s_cmd_axis` (FrameStreamingCore
ST0).  The FIFO is the sole command ingress: `zz9k_cmd_fetcher` and all
`CMD_ADDR`/`CMD_WORDS`/`CMD_START` protocol are absent.  The FrameStreamingCore
can either forward inline words from ST0 to the parser on ST1 or fetch parser
words from DDR through RasterIX's HP3 AXI master; memory fetch needs no
additional top-level port.

The FIFO uses the AXI4 data interface (`C_DATA_INTERFACE_TYPE=1`): its
registers stay on the AXI4-Lite `S_AXI` at `0x83c70000` (GP1 `M08`), while
`TDFD` moves to the AXI4 `S_AXI_FULL` window at `0x83c80000` (GP1 `M09`), which
accepts INCR bursts.  The core decodes `TDFD` by comparing each write's
`AWADDR` with the window base exactly, so every single store and every burst
MUST start at `0x83c80000`; an INCR burst then covers its beats.

| Address | Name | Usage |
| --- | --- | --- |
| `0x83c70000` | `ISR` | W1C FIFO completion/error state; not wired to a PS IRQ |
| `0x83c70004` | `IER` | Leave zero; completion is polled |
| `0x83c70008` | `TDFR` | Write `0xa5` to reset/discard staged TX words |
| `0x83c7000c` | `TDFV` | Vacancy in 32-bit words; reset reserves four words, so maximum is 508 on legacy 512-word cores and 4092 on the 4096-word core |
| `0x83c70014` | `TLF` | Commit packet length in bytes; this starts stream delivery |
| `0x83c80000` | `TDFD` | 32-bit little-endian FIFO word write port (single stores or INCR bursts starting here) |

`rix_drv.h` expresses `TDFD` as offset `0x10000` from the register window.

An inline packet is one complete FrameStreamingCore ST0 sequence: write its
`0x90000000 | payload_bytes` header, unused address word, and parser payload
words to `TDFD`, execute the required ARM barrier, then write the total packet
byte count to `TLF`.  Producers MUST wait until `TDFV` covers the entire packet
and never overrun the FIFO.  Firmware samples reset `TDFV`: legacy 512-word
cores retain 248-word payload packets, while the 4096-word core uses up to
4090 payload words plus the two-word header.

A memory-fetch packet contains only two FIFO words:

1. `0xb0000000 | list_bytes` (`out=ST1`, `in=MEM`, byte count in bits 27:0);
2. the exact 32-bit DDR byte address of the parser command list.

The list address and byte count MUST be 64-byte aligned.  The FSC converts the
byte count to 32-bit beats and its linear address generator issues complete
64-byte INCR transfers; a short final transfer would leave unread response
beats.  Consequently lists MUST be padded with parser NOP words to a 64-byte
boundary.  The largest encodable aligned list is `0x0fffffc0` bytes; 16 KiB
ring chunks are a practical software limit, not a hardware limit.  A fetch
envelope is itself a complete FIFO packet, so its `TLF` is exactly 8 bytes.
Multiple envelopes may be queued in order.  The command parser ignores FSC
`TLAST` and retains its command/argument state across envelopes, so an
envelope boundary may split a parser command; ending chunks at command
boundaries is nevertheless RECOMMENDED for diagnostics and independent
validation.

`TLF` MUST equal the bytes written to `TDFD` since the last accepted `TLF`,
header words included.  The FIFO drops any other value and sets `ISR.TSE`
(bit 25); the staged words then never reach RasterIX and every later `TLF`
mismatches too, until `TDFR`.  Firmware reads `ISR` after each `TLF` and
fails the packet on `TSE` or `TPOE` (bit 28).  `TDFR` leaves `ISR` intact, so
enable clears stale error bits (W1C).

Firmware may submit parser words inline or place them in DDR and submit
memory-fetch envelopes.  Inline MMIO and PL330 transport use the `TDFD` data
window; PL330 uses a non-cacheable DDR source with no peripheral handshake.
Firmware MUST first run an integrity/count self-test.  It MUST wait for DMA
completion and only then write `TLF`.  FIFO reset is separate from CSR soft
reset.

The DDR list MUST be visible to HP3 before its envelope is committed.  A
non-cacheable list ring needs only the ordering barrier; a cacheable list MUST
be cleaned to the point of coherency first.  Software MUST NOT overwrite a
ring chunk until an in-order rendered-pixel fence after that chunk has passed.

## EF target rules

EF color and depth buffers are direct DDR render targets and the color target
may be the presented RTG surface: no band binning, `READ`, segment `MEMSET`,
`COMMIT`, or present copy is valid in this contract.  Color is RGB565; depth is
Z16.  Target base addresses and texture pages MUST be 2048-byte aligned, and
surface pitch/extent MUST keep every accessed byte inside the allocated target.
The command stream sets resolution and target addresses, clears with full
color/depth `MEMSET`, and then submits triangles.  A one-TMU lightmapped draw
is two passes: base texture, then lightmap with `ZERO,SRC_COLOR` blending.

`XACPCommandReadCredit.v` is GPL-3.0-or-later and is installed in front of
every RasterIX crossbar read port: port 0 command-list reads, ports 1-3
colour/depth/stencil, and ports 4-5 the two TMUs.  All six ports arbitrate
through one AXI crossbar output dedicated to HP3; the crossbar permits at most
eight issued reads.  Each port reserves response space for a whole burst
before its AR is issued and accepts R unconditionally, so one client cannot
stall the single shared response channel behind it.  Reservation depths are
64 beats for command lists, 16 for each framebuffer port, and 32 for each TMU.
Framebuffer writes present W together with AW.  These credits protect both
inline and memory-fetch command transport.
