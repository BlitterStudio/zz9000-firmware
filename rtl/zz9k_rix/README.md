# RasterIX EF hardware contract

`--rasterix` builds the external-framebuffer RasterIX EF image only.  The
renderer is a 100 MHz FCLK0, one-TMU RGB565/Z16 core with a 64-bit AXI memory
master dedicated to PS HP3.  HP0 remains scanout, HP1 remains MNTZorro plus
capture, and audio TX/RX share an interconnect on HP2.  Default images do not
instantiate any RasterIX or alter those ports.

## Discovery and CSR (`0x83c60000`)

Firmware MUST read `IDENTITY` before any other access and require `RIX1`.
`CONTROL.ENABLE` MUST be set before committing a FIFO packet.  This CSR has no
command-buffer descriptors, fetcher, completion queue, or interrupt.

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

## Command FIFO (`0x83c70000`)

The command ingress is Xilinx `axi_fifo_mm_s` TX-only, depth 512 words.  Its
`AXI_STR_TXD` connects directly to RasterIX `s_cmd_axis` (FrameStreamingCore
ST0).  It is the sole normal command route: `zz9k_cmd_fetcher` and all
`CMD_ADDR`/`CMD_WORDS`/`CMD_START` protocol are absent.

| Offset | Name | Usage |
| --- | --- | --- |
| `0x00` | `ISR` | W1C FIFO completion/error state; not wired to a PS IRQ |
| `0x04` | `IER` | Leave zero; completion is polled |
| `0x08` | `TDFR` | Write `0xa5` to reset/discard staged TX words |
| `0x0c` | `TDFV` | Vacancy in 32-bit words; after reset hardware reserves four words, so normal maximum is 508 |
| `0x10` | `TDFD` | 32-bit little-endian FIFO word write port |
| `0x14` | `TLF` | Commit packet length in bytes; this starts stream delivery |

A packet is one complete FrameStreamingCore ST0 sequence: write its two-word
DSE header/address followed by parser payload words to `TDFD`, execute the
required ARM barrier, then write the byte count to `TLF`.  Producers MUST wait
until `TDFV` covers the entire packet, never overrun the FIFO, and limit normal
packets to 248 words so two packets fit while one drains.

PL330 transport uses a non-cacheable DDR source and fixed destination
`0x83c70010` (`TDFD`), with no peripheral handshake.  Firmware MUST first run
an integrity/count self-test.  It MAY use 16-beat, 32-bit fixed-destination
bursts only if that test passes; otherwise it MUST use single-beat stores.
It MUST wait for DMA completion and only then write `TLF`.  FIFO reset is
separate from CSR soft reset.

## EF target rules

EF color and depth buffers are direct DDR render targets and the color target
may be the presented RTG surface: no band binning, `READ`, segment `MEMSET`,
`COMMIT`, or present copy is valid in this contract.  Color is RGB565; depth is
Z16.  Target base addresses and texture pages MUST be 2048-byte aligned, and
surface pitch/extent MUST keep every accessed byte inside the allocated target.
The command stream sets resolution and target addresses, clears with full
color/depth `MEMSET`, and then submits triangles.  A one-TMU lightmapped draw
is two passes: base texture, then lightmap with `ZERO,SRC_COLOR` blending.

`XACPCommandReadCredit.v` is GPL-3.0-or-later and is installed between the
FrameStreamingCore list-read adapter and RasterIX's shared crossbar.  It
reserves response FIFO capacity on AR acceptance, so parser backpressure cannot
block fragment-memory responses.  It does not change the FIFO transport rules.
