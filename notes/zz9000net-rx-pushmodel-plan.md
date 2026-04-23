<!--
  Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
  SPDX-License-Identifier: GPL-3.0-or-later
-->

# ZZ9000Net Push-Model RX (Zorro III Bus Mastering) — Implementation Plan

**Status: ARCHIVED — not executable on existing ZZ9000 hardware.**

Investigation of the FPGA source (`mntzorro.v:712-720`) and the xdc
constraints confirmed the ZZ9000 PCB (both R1 and R5) routes the
Zorro strobes (`/FCS`, `/DS0-3`, `/UDS`, `/LDS`, `/READ`, `DOE`,
`/DTACK`) through unidirectional level shifters. The FPGA only
exposes three direction-control outputs (`ZORRO_DATADIR`,
`ZORRO_ADDRDIR`, `ZORRO_ADDRDIR2`) — none of them cover the strobes.

Enabling bus mastering would require a PCB respin with bidirectional
transceivers on those signals and new DIR traces routed to spare
Zynq GPIOs. That is out of scope for this project.

The fallback from §11 (cacheable RX window + burst reads) is the
path we are taking. See `notes/zz9000net-rx-cacheable-plan.md`.

The material below is preserved for reference in case a future
hardware revision enables bus mastering.

---

## 1. Prerequisite: can the ZZ9000 PCB actually support bus mastering?

### Evidence gathered from the repo

- `mntzorro.v:70-88` — every strobe a Zorro master must drive
  (`ZORRO_NUDS`, `ZORRO_NLDS`, `ZORRO_NDS0`, `ZORRO_NDS1`,
  `ZORRO_NFCS`, `ZORRO_READ`, `ZORRO_DOE`) is declared as
  `input wire`. `ZORRO_ADDR[22:0]` and `ZORRO_DATA[15:0]` are
  `inout`. `ZORRO_NBRN` is an `output` and `ZORRO_NBGN` is an
  `input`.
- `ZZ9000_proto.srcs/constrs_1/new/zz9000.xdc` — all Zorro strobes
  use `LVCMOS33` on regular FPGA pins (see pin assignments). No XDC
  entry gates these through an external unidirectional buffer
  direction pin. The only FPGA-owned direction-control outputs are
  `ZORRO_DATADIR`, `ZORRO_ADDRDIR`, `ZORRO_ADDRDIR2`, plus the
  master-side `ZORRO_DOE` (currently an input, meaning DOE is driven
  by whoever is bus master — we listen as a slave and would have to
  participate in DOE when we become master).
- `mntzorro.v:713-720` — the FPGA already gates data/address
  tri-state on `ZORRO_DOE & dataout_z3`, i.e. the design already
  treats DOE as the "is it OK to drive the bus" gate. This is the
  Zorro III semantic.
- `notes/zz9000-r1-bringup-notes.txt:766` — the line `"master must
  be well behaved"` in a debug list refers to an AXI toggle in the
  Vivado block design, not the Zorro bus.
- The BOM-level question of "is there a 74LVC245 /
  74LVT162245-style unidirectional buffer between the FPGA and the
  Zorro slot strobes" cannot be answered from the repo files in
  this tree. The schematic is not here.

### Conclusion

- The Verilog port directions are the biggest FPGA-side blocker:
  strobes are declared `input`. That is a source-code limitation,
  easy to fix.
- The PCB question is not conclusively resolvable from the firmware
  repo. The strongest on-disk evidence is indirect: Z3 *requires*
  the master to drive DS0/DS1/UDS/LDS/FCS/READ, the ZZ9000 targets
  Z3, and NBRN/NBGN are broken out as FPGA-owned pins — it would be
  odd to wire up bus request/grant without wiring the strobes
  bidirectionally.
- **#1 prerequisite before committing to the work:** user must
  check the ZZ9000 schematic (and confirm hardware revision — R1 vs
  later) for the following nets: do `/UDS`, `/LDS`, `/DS0`, `/DS1`,
  `/FCS`, and `/READ` pass through any fixed-direction buffer
  between the Zorro card-edge and the FPGA? If any of them does,
  bus mastering is impossible without a PCB respin; abandon this
  plan in favour of option 3 (cacheable RX window) from
  `notes/zz9000net-rx-throughput.md`.
- Assuming the PCB is drivable (reasonable working assumption given
  the design target), proceed.

## 2. Zorro III bus master state machine

### 2.1 Signals and high-level cycle

A Zorro III master write cycle, per Commodore's Z3 Bus
Specification (AHRM Appendix + Z3 Bus Spec rev 1.7):

| Phase         | FPGA (master) drives                                                                                                 | FPGA reads         | Notes                                                   |
|---------------|----------------------------------------------------------------------------------------------------------------------|--------------------|---------------------------------------------------------|
| Idle          | NBRN high, DOE floating / driven by whoever has the bus                                                              | NBGN               | No bus ownership                                        |
| Request       | NBRN low                                                                                                             | NBGN               | Arbiter (Buster) sees request                           |
| Granted       | NBRN low                                                                                                             | NBGN low, FCS high | Grant + bus idle = we can start                         |
| Address phase | `ZORRO_ADDR[22:0]` = A[24:1] of target Z3 addr, `ZORRO_DATA[15:8]` = A[31:25], `READ=0`, then FCS low, FC[2:0] on DATA[7:1] | —                  | FCS falling edge latches address in target              |
| Data setup    | re-tristate A/D upper, drive 32-bit write data on multiplexed A/D, drive DS0/DS1/UDS/LDS per byte lane               | —                  | Z3 multiplexes A/D; after FCS falls we switch to data   |
| DS assert     | DS0..DS3 low for the bytes being written                                                                             | NDTACK             |                                                         |
| Wait          | hold data                                                                                                            | NDTACK falling     | Target asserts DTACK to ack                             |
| End cycle     | deassert DS and FCS (rising), then tri-state data, deassert DOE                                                      | —                  |                                                         |
| Release bus   | optional: pulse NBRN high, or hold low for another cycle (burst)                                                     | —                  |                                                         |

For RX push we only issue **writes** from FPGA to Amiga RAM. No
read path needed. Eliminates a big chunk of FSM surface.

### 2.2 State list (additions to the existing FSM, declared around `mntzorro.v:867-937`)

New localparams, numbered above the last existing one
(`Z2_WRITE_FINALIZE2 = 61`):

```
BM_IDLE             = 70
BM_REQUEST          = 71   // assert NBRN, wait for NBGN
BM_WAIT_BUS_IDLE    = 72   // NBGN low AND FCS high — safe to take bus
BM_ADDR_SETUP       = 73   // drive addr bits + READ=0
BM_ADDR_FCS_FALL    = 74   // assert FCS low
BM_ADDR_HOLD        = 75   // one-cycle addr hold after FCS (Z3 spec tAH)
BM_DATA_DRIVE       = 76   // switch A/D pins from addr to data, drive wdata
BM_DS_ASSERT        = 77   // assert DS0-DS3 according to byte enables
BM_WAIT_DTACK       = 78   // wait for target NDTACK low (with timeout)
BM_DS_RELEASE       = 79   // deassert DS, FCS
BM_TRISTATE         = 80   // let bus settle, release A/D/DS/FCS
BM_NEXT_OR_DONE     = 81   // if more longwords in burst, loop to BM_ADDR_SETUP
BM_RELEASE_BUS      = 82   // deassert NBRN, release DOE
```

### 2.3 Tri-state control during a master write

Wire into the existing `ZORRO_DATA_T` / `ZORRO_ADDR_T` and the
slave/dtack assigns at `mntzorro.v:713-727`:

| Signal              | Slave mode (today)                                  | Master write phase                                                             |
|---------------------|-----------------------------------------------------|--------------------------------------------------------------------------------|
| `ZORRO_DATA_T`      | `~(DOE & (dataout_enable \| dataout_z3))`           | `0` (drive) during address + data phases, `1` elsewhere                        |
| `ZORRO_ADDR_T`      | `~(DOE & dataout_z3)`                               | `0` during address phase, `1` during data phase and idle                       |
| `ZORRO_DATADIR`     | `DOE & (dataout_enable \| dataout_z3)`              | `1` during master addr + data (we drive)                                       |
| `ZORRO_ADDRDIR`     | `DOE & dataout_z3`                                  | `1` during master addr, `0` during master data (Z3 A/D mux)                    |
| `ZORRO_DOE`         | input (read)                                        | becomes `inout`/`output`; the master asserts DOE — **port direction change**   |
| `ZORRO_READ`        | input                                               | becomes `inout`; master drives low for write                                   |
| `ZORRO_NFCS`, `ZORRO_NDS0..3`, `ZORRO_NUDS`, `ZORRO_NLDS` | input                     | become `inout`; IOBUF with T controlled by master FSM                          |
| `ZORRO_NDTACK`      | output in slave mode                                | becomes `inout` — as master, tri-state our own driver and sample the line      |
| `ZORRO_NBRN`        | output, tied 1                                      | output, FSM-driven                                                             |

**Critical:** the slave FSM and the bus-master FSM must be
**mutually exclusive by construction**. Master FSM can only leave
`BM_IDLE` when:
1. Main FSM is in `Z3_IDLE`.
2. `ZORRO_NFCS` is high (no pending Amiga cycle).
3. A request flag is set.

Once in BM_* states, the slave path must observe `bus_master_active`
and decline to respond. Transition back to slave mode only after
`BM_RELEASE_BUS` fully releases all strobes and waits one clock for
everything to tri-state.

### 2.4 Timing sketch for a single 32-bit write

```
clk:        ___/‾‾\__/‾‾\__/‾‾\__/‾‾\__/‾‾\__/‾‾\__/‾‾\__/‾‾\__/‾‾\__
NBRN:       ‾‾‾‾‾‾‾‾‾\________________________________________/‾‾‾‾‾
NBGN:       ‾‾‾‾‾‾‾‾‾‾‾‾‾\________________________/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
NFCS:       ‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\__________________/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
NDSx:       ‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\_____________/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
READ:       ‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\____________________/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
A/D:        zzzzzzzzzzzzz<ADDR><DATA-----><zzz>zzzzzzzzzzzzzzzzzzzzz
NDTACK:     ‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\______/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
```

On an 83.3 ns Z3 bus cycle period, a write completes in roughly
6–10 clocks depending on slave DTACK latency. ~300 ns nominal,
~500 ns worst-case per longword. 1500-byte frame = ~375 longwords
× 500 ns = **~187 µs per frame**, ~8 MB/s single-beat. Adding
**burst mode** (keep FCS low, advance DS for consecutive
longwords) pushes this toward 16 MB/s. That's enough to saturate
100 Mbit.

### 2.5 Burst mode (optional but recommended)

Z3 supports "quick" cycles where multiple longwords transfer by
keeping FCS asserted and cycling DS. FSM loops
`BM_DS_ASSERT → BM_WAIT_DTACK → BM_DS_RELEASE → BM_DATA_DRIVE` for
successive longwords within the same FCS window, up to the Z3
burst length limit (typically 4 longwords per burst).

Start single-beat for bring-up (§8). Add burst only after
single-beat works end-to-end.

## 3. Tri-state strategy

- Change `ZORRO_READ`, `ZORRO_NUDS`, `ZORRO_NLDS`, `ZORRO_NDS0`,
  `ZORRO_NDS1`, `ZORRO_NFCS`, `ZORRO_DOE`, and `ZORRO_NDTACK` from
  `input` / fixed `output` to `inout`, each wrapped in an `IOBUF`
  primitive like the existing generate blocks at
  `mntzorro.v:736-758`.
- Introduce `reg bus_master_active` that gates a mux: when low, the
  slave FSM owns the strobe drivers (all strobe `T=1` → input);
  when high, the master FSM drives them.
- Transition from slave to master is only legal when
  `znFCS_sync == 5'b11111` for at least 3 clocks (bus demonstrably
  idle) **and** NBGN is granted **and** the main zorro_state FSM is
  in `Z3_IDLE`. Encode as guard on `BM_IDLE → BM_REQUEST`.
- No "mid-flight switch": never start a master cycle while a slave
  cycle is in progress. Period.
- On reset (`z_reset`): `bus_master_active <= 0`, NBRN deasserted,
  all strobe IOBUF T = 1.

## 4. Register contract for the RX ring

### 4.1 Current slv_reg allocation (from `mntzorro.v:298-303, 2320-2329`)

| slv_reg | Current use                                             | Writable from AXI (ARM) | Read path                        |
|---------|---------------------------------------------------------|-------------------------|----------------------------------|
| slv_reg0 | ARM handshake flags (bit 30 = read_flag, bit 31 = write_flag) | yes                     | out_reg0 = last z3addr           |
| slv_reg1 | unused                                                  | yes                     | out_reg1 = zorro_ram_write_data  |
| slv_reg2 | ARM video control op + flag bit 31                      | yes                     | out_reg2 = last_z3addr           |
| slv_reg3 | ARM video control data                                  | yes                     | out_reg3 = state debug           |
| slv_reg4 | eth_rx_frame_select (21 bits)                           | yes                     | —                                |
| slv_reg5 | IRQ pulse + peripheral reset                            | yes                     | —                                |

`OPT_MEM_ADDR_BITS = 2` → 8 registers (3'h0..3'h7) — slots 3'h6
and 3'h7 are free and unimplemented. We need more than that, so
bump `OPT_MEM_ADDR_BITS` to 3 (16 registers, 3'h0..3'hF) and widen
the case statements in the write block at lines 431-483 and the
read mux at 2370-2376.

### 4.2 New slv_reg allocation for RX push

| slv_reg | Dir (from ARM) | Name                 | Description                                                                                       |
|---------|----------------|----------------------|---------------------------------------------------------------------------------------------------|
| slv_reg6 | W              | `RX_RING_BASE`       | Zorro-space physical address (32-bit) of ring start. Written once after driver handshake.        |
| slv_reg7 | W              | `RX_RING_GEOMETRY`   | `[31:16]` slot count (power of 2), `[15:0]` slot size (multiple of 4, typically 2048).           |
| slv_reg8 | W              | `RX_RING_PRODUCER`   | Firmware-owned producer index. Exposed to Amiga via REGREAD offset 0x40.                         |
| slv_reg9 | W              | `RX_RING_CONTROL`    | `[0]` enable push mode, `[1]` IRQ-per-frame, `[7:4]` IRQ coalescing threshold, `[15:8]` reserved. |
| slv_regA | R/W            | `RX_RING_CONSUMER`   | Amiga writes via REGWRITE 0x42; FPGA latches. Firmware reads for flow control.                   |
| slv_regB | R (ARM)        | stats readback       | master-cycle error counter, DTACK timeouts.                                                       |

### 4.3 MMIO (Amiga-side) register contract

Extend REGWRITE decode at `mntzorro.v:2265-2286` and REGREAD at
`mntzorro.v:2237-2254`:

| MMIO offset | Access     | Meaning                                                                                                 |
|-------------|------------|---------------------------------------------------------------------------------------------------------|
| 0x40        | R (Amiga)  | `producer_index` — exposed copy of slv_reg8                                                             |
| 0x42        | W (Amiga)  | `consumer_index` — latched into slv_regA                                                                |
| 0x44        | W (Amiga)  | `ring_base_lo` (word) — Amiga publishes base address low 16 bits                                        |
| 0x46        | W (Amiga)  | `ring_base_hi` (word) — high 16 bits; write triggers a handshake bit the firmware polls                  |
| 0x48        | W (Amiga)  | `ring_geometry` (longword) — slot count << 16 \| slot size                                              |
| 0x4A        | W (Amiga)  | `ring_control` (word)                                                                                   |
| 0x4C        | R (Amiga)  | capability flags — bit 0 = "push-mode supported", bit 1 = "bus mastering OK" (set after self-test).      |

Existing REGWRITE offsets 0x00..0x34 untouched.

## 5. Firmware changes to `ethernet.c`

Current RX path (from `ZZ9000_proto.sdk/ZZ9000OS/src/ethernet.c:381-453`):

- `XEmacPsRecvHandler` pulls frames off EmacPS BD ring, invalidates
  cache, memcpys into `RX_BACKLOG_ADDRESS + frames_backlog * FRAME_SIZE`.
- Amiga reads each byte across Zorro.

### 5.1 New push-mode path

Add static `push_mode_enabled` sourced from MMIO. Workflow in
`XEmacPsRecvHandler`:

```
for each received frame:
  invalidate cache
  if push_mode_enabled and ring_control.enable:
    slot = producer_index % ring_slot_count
    dst_amiga = ring_base + slot * ring_slot_size
    write 4-byte header [size:2][serial:2] at dst_amiga+0
    enqueue bus-master DMA src=frame_ptr → dst=dst_amiga+RX_FRAME_PAD, len=rx_bytes
    wait/poll for DMA completion (or push to completion queue)
    producer_index++; publish via slv_reg8
    if IRQ-per-frame: pulse Amiga IRQ (via slv_reg5)
    else accumulate; pulse after N frames or timer
  else:
    [fallback: current backlog path, unchanged]
```

### 5.2 FPGA DMA command interface

New block in `mntzorro.v`: simple AXI-lite-triggered DMA command
FIFO. Firmware writes `{dst_addr, src_axi_addr, length}` into
three more slv_regs (or reuses regs between transactions), sets a
"start" bit. FPGA FSM:

1. Reads frame data from local DDR via existing `m00_axi_*` master.
2. Pushes longwords into a small internal FIFO (16–64 deep).
3. Feeds the bus-master FSM from that FIFO, advancing target
   Amiga address per longword.
4. Sets "done" flag when length hits zero and bus is released.

Separation keeps the bus-master FSM concerned only with Zorro
timing; firmware interface is an AXI-visible "DMA A → B" primitive.

### 5.3 Back-pressure (ring full)

When `producer_index - consumer_index >= ring_slot_count`:

1. **Drop at firmware** (recommended). Increment dropped-frame
   stat, don't advance producer, free EmacPS BD. Matches current
   semantics; `Overruns` in SANA-II still grows on Amiga side.
2. Spill to local DDR backlog. Complicated; breaks the "push
   bypasses backlog" invariant.
3. Stall EmacPS. Bad.

Recommend #1. Expose dropped-frame counter via stats register.

### 5.4 `frame_serial` semantics

Keep unchanged — still increments per EMAC frame, still in header.
Pushed frames still carry a serial, so gap-detection in
`zznetstats` continues to work during bring-up.

## 6. Driver changes to `zz9000-drivers/net/device.c`

### 6.1 Ring allocation

On driver init:

- `AllocMem(slot_count * slot_size, MEMF_FAST | MEMF_CLEAR)` —
  Fast RAM, 32-bit addressable from Z3 space. **MEMF_24BITDMA not
  required** — Z3 is 32-bit.
- Align to 64 bytes (cache line × 4). Allocate `size + 63` and
  round up.
- Translate Amiga pointer to Zorro-bus-visible physical address.
  Fast RAM is mapped 1:1 in Z3 address space — pointer *is* the
  address. Publish via MMIO 0x44/0x46.

### 6.2 Replace `frame_proc` RX path (~`zz9000-drivers/net/device.c:760-887`)

- Gate off `rx_accept` / `frm = ZZ9K_REGS+ZZ9K_RX` path behind
  detection flag.
- New path: on IRQ wakeup, read `producer_index` from MMIO 0x40.
  While `consumer != producer`:
  - Read 4-byte header **from RAM** (cached, fast).
  - `CacheClearE(ring_base + consumer*slot_size, slot_size, CACRF_ClearD)`
    before first access — 060 has separate I/D caches, DMA target
    is data cache only, guards against stale cached lines from a
    previous ring lap.
  - Packet-type match; BufferManagement `CopyToBuffer` from RAM
    address directly. No Zorro reads.
  - Advance `consumer`. After batch, write updated consumer to
    MMIO 0x42.

### 6.3 Feature detection

At init, read MMIO 0x4C. If bit 0 = 1 and bit 1 = 1, push mode.
Else fall back to pull mode. Keeps driver compatible with old
firmware; firmware self-test at bring-up clears bit 1 if a master
cycle fails.

## 7. Migration / coexistence

Keep old pull path intact in firmware. FPGA keeps existing slave
RX backlog decode (`mntzorro.v:2146`) as forever fallback. New push
path gated by `ring_control.enable`; if driver never writes the
ring base, firmware stays in pull mode. Ship new bitstream with
push mode disabled by default, enable per-driver-revision.

Detection: read REVISION first; if `REVISION < new_revision_number`,
pull mode. Else read 0x4C capability flags.

## 8. Test and bring-up plan

### 8.1 Verilog sim

- Testbench instantiates `MNTZorro_v0_1_S00_AXI` with a fake Z3
  target (pulls DTACK low after programmable latency).
- Trigger single master write of `0xDEADBEEF` to a target address.
  Verify waveform matches §2.4.
- Assertions: FCS rises within N clocks after DTACK fall, DS
  follows FCS down, data stable from DS-fall to DTACK-fall.

### 8.2 ILA on hardware

- Mark NBRN, NBGN, NFCS, NDSx, NDTACK, ZORRO_DATA, ZORRO_ADDR with
  `(* mark_debug = "true" *)` (repo already does this, see
  `mntzorro.v:602-612`).
- First real test: firmware triggers **one** master write of
  sentinel (`0xCAFEBABE`) to a known Amiga Fast RAM address that
  the driver has pre-cleared and is spinning to read. Sentinel
  appears → capture ILA trace.

### 8.3 Firmware dummy DMA test

- Debug command (REGWRITE hook) issues N sentinel pushes to a
  pre-arranged address. Amiga-side test polls and counts successful
  receives. Baseline the master path at zero ethernet activity.

### 8.4 Real RX path, IRQ disabled

- Enable push with `ring_control.irq_enable = 0`. Driver polls
  producer index. Verify no drops at iperf UDP 30 Mbit. If
  `zznetstats` shows `Overruns = 0`, bus master is keeping up.

### 8.5 Real RX path, IRQ enabled

- Enable IRQ-per-frame. Verify no regression. Then enable
  coalescing (IRQ every 4 frames).

### 8.6 What to watch

- `zznetstats` `Overruns`: was 12469 (`notes/zz9000net-rx-throughput.md`).
  Target: < 100 on a 1 GB download.
- `BadData`: near zero. If climbs → torn cacheline reads; revisit
  cache invalidation.
- RTG framebuffer tearing during RX bursts → master FSM holding
  bus too long, starving display DMA. Mitigation: limit burst
  length.
- Audio underruns: same as above.
- USB block storage: I/O errors during concurrent RX + disk write.

## 9. Risks and open questions

1. **PCB bus-master capability** (§1). #1 prerequisite.
2. **Arbitration fairness.** Sustained RX must not hog NBRN and
   starve the 68060 from chip RAM. Policy: max N longwords per bus
   grant, then release + re-arbitrate. Tune empirically.
3. **NDTACK electrical behaviour as master.** Existing slave
   driver (`mntzorro.v:727`) uses pull-down on output with 1k
   pullup. As master we read NDTACK — pullup still there, slave
   pulls low. Fine electrically; tri-state our own driver while
   reading.
4. **Z3 burst cycle rules.** Z3 burst reads are common; burst
   writes are more restrictive on some platforms. Single-beat
   first, bursts later.
5. **68060 cache coherency on Amiga side.** MMU must map ring
   region as cacheable data (default for MEMF_FAST); driver
   `CacheClearE` before each read. Ring region must not be
   relocated mid-flight.
6. **Interrupt latency on busy 060.** If Amiga can't service IRQ
   fast enough, ring fills. Coalescing helps. Worst ring depth:
   ~64 slots × 2 KB = 128 KB of Fast RAM — negligible.
7. **Slot size / alignment.** 2048 matches firmware `FRAME_SIZE`.
   Revisit for jumbo frames.
8. **Autoconfig impact.** Ring is in Amiga RAM — no autoconfig
   change. New MMIO registers in existing card BAR. Zero risk.
9. **ZZ9000AX audio reset path** (`slv_reg5[3]`) untouched.
10. **Existing DMA oddities** — see `notes/zz9k-dma-problem.txt`.
    That's the AXI-HP → PS DDR path, not Zorro mastering;
    unrelated but worth reading before cutting into AXI side.

## 10. Ordered work plan

1. Get schematic confirmation on strobe drivability (§1 prerequisite).
2. Change port directions in `mntzorro.v:66-127` (strobes `inout`).
   Rebuild slave path unchanged; verify no regression.
3. Bump `OPT_MEM_ADDR_BITS` to 3; add slv_reg6..slv_regB.
4. Add MMIO offsets 0x40..0x4C to REGREAD/REGWRITE decodes.
5. Implement bus-master FSM (single-beat writes only), gated by a
   safety bit that defaults off.
6. Sim testbench, run §8.1.
7. Firmware DMA-command wrapper; capability-flag handshake.
8. Bring-up §8.2–8.3 with Amiga driver still in pull mode.
9. Port driver to push mode behind capability detection.
10. §8.4–8.5.
11. Optimize: burst writes, IRQ coalescing.

## Fallback if bus mastering is not possible

Implement option 3 from `notes/zz9000net-rx-throughput.md`:
cacheable RX window + `CacheClearE` on the driver side. Smaller
win (~2–3×, not 4×), but no FPGA timing-closure risk and no PCB
question.

## Critical files for implementation

- `mntzorro.v`
- `ZZ9000_proto.sdk/ZZ9000OS/src/ethernet.c`
- `ZZ9000_proto.sdk/ZZ9000OS/src/memorymap.h`
- `ZZ9000_proto.srcs/constrs_1/new/zz9000.xdc`
- `/Users/midwan/Github/zz9000-drivers/net/device.c` (branch
  `net-gcc-port`)
