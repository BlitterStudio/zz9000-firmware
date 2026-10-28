# RasterIX DDR command transport

`zz9k_rasterix` is mapped at `0x83c60000`.  Commands are 32-bit little-endian
words in ARM-visible DDR.  The command reader is a 64-bit AXI4 INCR master on
HP1: it issues no more than 16 beats and splits every 4 KiB boundary.  It uses
a dedicated S02 slot in `axi_interconnect_2`; capture remains S00 at priority
15, while RasterIX texture/framebuffer traffic (S01) and command reads (S02)
remain priority 0.  This avoids an in-wrapper AXI arbiter and preserves capture
priority.

The fetcher forwards every DDR word directly to the selected RasterIX
`s_cmd_axis`, which is FrameStreamingCore's DSE st0 port, not CommandParser.
Thus the ARM buffer **must** contain a DSE operation: a `0x90000000 | byte_count`
header (st0 input to st1 output), an address beat (zero for inline parser
payload), then the raw RasterIX parser words.  The wrapper fetcher adds no DSE
headers.  The final fetched word carries `tlast`.

## CSR map

| Offset | Name | Bits / meaning |
| --- | --- | --- |
| `0x00` | `IDENTITY` | `RIX1` (`0x52495831`) |
| `0x04` | `VERSION` | `0x00010001` |
| `0x08` | `CONTROL` | bit 0 `ENABLE`; bit 1 request idle-only soft reset; bit 2 W1C `ERROR`, `PIO_REJECT`, `START_REJECT`, and `IRQ_STATUS` |
| `0x0c` | `STATUS` | bit 0 renderer clock enabled; 1 `IDLE`; 2 renderer busy; 3 triangle rendering; 4 renderer stall; 5 fetch queue active; 6 fetch submission ready; 7 completion is awaiting renderer/write drain; 8 sticky error; 9 PIO rejected; 10 start rejected; 11 level `IRQ_STATUS`; 23:16 renderer AXI writes outstanding; 31:24 renderer AXI reads outstanding |
| `0x10` | `PIO_WORD` | debug PIO command data latch |
| `0x14` | `PIO_PUSH` | write bit 0 to enqueue `PIO_WORD`; bit 1 is `tlast`; read is FIFO occupancy.  A push while fetch is active is rejected and sets `PIO_REJECT`. |
| `0x18` | `RESPONSE` | low 16 bits of the currently consumed RasterIX response (debug only) |
| `0x1c` | `CMD_ADDR` | physical DDR command-buffer address; must be 8-byte aligned |
| `0x20` | `CMD_WORDS` | nonzero count of 32-bit command words |
| `0x24` | `CMD_START` | write bit 0 to enqueue `CMD_ADDR` / `CMD_WORDS`; read bit 0 is queue-ready |
| `0x28` | `SUBMITTED` | accepted DDR submissions counter |
| `0x2c` | `COMPLETED` | completed submissions counter |
| `0x30` | `ERROR_STATUS` | bit 0 sticky AXI read response, misalignment, or unsupported RasterIX framebuffer `SWAP`; bit 1 `PIO_REJECT`; bit 2 `START_REJECT` |
| `0x34` | `ERROR_ADDR` | misaligned buffer address or failing AXI read-beat address (unchanged for an unsupported `SWAP`) |

The fetch queue holds two pending descriptors in addition to the active one.
It deliberately waits for each submission's words to be consumed, RasterIX to
be idle, and all renderer AXI writes to retire before releasing the next queued
submission.  This makes `COMPLETED` a hardware completion point while allowing
the ARM to prepare N+1 during N.

`IRQ_STATUS` is a level status bit only; it is set by a completion or sticky
error and cleared by `CONTROL[2]`.  It is intentionally not connected to a PS
IRQ so PS7 configuration remains unchanged.

## Renderer variants

IF is the default `--rasterix-variant if` build and uses the configured
internal framebuffer band.  Opt-in `--rasterix-variant ef` selects
`RasterIX_EF` with RGB565 color, 16-bit depth, stencil disabled, one TMU,
external texture memory/cache, 25-bit fixpoint interpolation, and coalesced
external-memory transfers.  EF stores the complete color and depth targets in
DDR, so its stream sets resolution and target addresses, clears with a full
color/depth `MEMSET`, then sends triangles directly: it has no per-band
offset, `COMMIT`, or `READ`.

## Framebuffer presentation

ZZ9000's RTG path presents the DDR color target; it does not connect
RasterIX's framebuffer-page swap handshake.  The wrapper consequently supplies
an immediate `fb_swapped` acknowledgement so IF color `MEMSET`/`COMMIT` and EF
color `MEMSET` commands can retire.  Command streams MUST NOT issue
`OP_FRAMEBUFFER SWAP` (including vsync swap): the renderer is acknowledged to
avoid a hardware deadlock, but the wrapper sets sticky `ERROR_STATUS[0]` and
`IRQ_STATUS` to flag the unsupported operation.

## Driver sequences

**Submit.** Populate and flush the DDR buffer for the noncoherent HP1 reader;
write `CMD_ADDR`, `CMD_WORDS`, then `CMD_START=1`.  Poll `CMD_START[0]` first
if queue space is required.  Increment of `SUBMITTED` confirms acceptance.

**Poll completion.** Snapshot `SUBMITTED` after the start, then wait until
`COMPLETED` reaches that value.  Completion means the final word handshook with
RasterIX, RasterIX reported idle, and all of its AXI framebuffer writes retired.
`IRQ_STATUS` may be used as a polling hint and must be acknowledged with
`CONTROL[2]` after status/counters have been read.

**Disable / enable clock gate.** To disable, write `CONTROL=0` and poll
`STATUS.IDLE=1`; the gate closes only after the fetch queue, command FIFO,
renderer work, and renderer AXI traffic have drained.  To enable, write
`CONTROL=1` before PIO traffic or a DDR start.  A start also holds the gate open
until its completion point.

**Error recovery.** On `ERROR_STATUS[0]`, read `ERROR_ADDR`, stop submitting,
and wait for `STATUS.IDLE`.  Write `CONTROL[2]=1` to clear sticky/error
notification state; then either retry with a corrected and flushed buffer or
request the idle-only soft reset with `CONTROL[1]`.  Misaligned and zero-length
starts are not accepted; a full two-entry queue sets `START_REJECT` instead.
