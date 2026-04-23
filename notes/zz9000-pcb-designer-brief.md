# ZZ9000 — PCB changes to unlock Zorro III burst reads and bus mastering

**Audience:** hardware/PCB designer of the next ZZ9000 revision.
**Author side:** firmware / FPGA (this repo).
**Date:** 2026-04-23.
**Schematic source used:** `gfx/amiga-zz9000.svg` in this repo (confirmed reading the actual parts — 74LVC8T245 transceivers, open-collector NPN drivers for wired-OR bus outputs). If this is the wrong/older revision of the schematic, please correct the table.

## 1. Why this matters (one paragraph)

On this PCB the ZZ9000Net driver tops out at ~4 MB/s with ~43% RX drop on a sustained TCP download (line rate is 12.5 MB/s on 100 Mbit). Bottleneck is not the FPGA, DDR, or firmware — the card is forced into single-longword Zorro III slave reads because two optional Zorro III features are not wired:

- **/MTCR + /MTACK** (multi-transfer cycle — Zorro III burst reads). Adding this alone is expected to push sustained throughput from ~4 MB/s to ~14–16 MB/s (saturates 100 Mbit).
- **/BGACK** (the missing piece for bus mastering — card drives the bus). Optional; not needed for RX, useful for future DMA paths.

Good news: **the PCB is already 90% ready for both features.** The bidirectional level-shifting transceivers on data, address, and strobes are all present (U1–U6 are 74LVC8T245 with FPGA-controlled direction). The only missing pieces are three specific signals that were never routed to a Zorro slot finger, plus their open-collector drivers where applicable.

## 2. What is already on the PCB and working (do NOT change)

From `gfx/amiga-zz9000.svg`:

| Signal group | Transceiver | Direction control (FPGA net) | Status |
|---|---|---|---|
| `D[0..15]` (data) | U1, U2 (74LVC8T245) | `DIRDATA` → FPGA pin Y12 (`ZORRO_DATADIR`) | Bidirectional, already correct. |
| `A[1..7]`, `/CCS` (low addr + chip-select) | U3 (74LVC8T245) | `DIRADDR2` → FPGA pin T15 (`ZORRO_ADDRDIR2`) | Bidirectional — firmware currently forces direction input-only, but the hardware path is there. |
| `A[8..15]` | U4 (74LVC8T245) | `DIRADDR` → FPGA pin U9 (`ZORRO_ADDRDIR`) | Bidirectional, driven correctly. |
| `A[16..23]` | U5 (74LVC8T245) | `DIRADDR` → FPGA pin U9 | Bidirectional, driven correctly. |
| `/FCS`, `/DS0`, `/DS1`, `/UDS`, `/LDS`, `READ`, `DOE` (strobes) | U6 (74LVC8T245) | `DIRADDR2` → FPGA pin T15 | **Bidirectional already** — my earlier brief was wrong about this. Firmware just needs to flip `DIRADDR2` during bus-master cycles. |
| `/BGn`, `E7M`, `/IORST`, `/CFGINn`, `C28D`, `VHSYNC`, `VVSYNC` (inputs-only) | U7 (74LVC8T245 with DIR=GND) | — | Permanently bus→FPGA; correct, these are never driven by the card. |
| `/DTACK` output (open-collector on bus) | Q1 (NPN, open-collector driver) | — | Correct wired-OR scheme for active-low bus outputs. |
| `/CINH` output (open-collector on bus) | Q2 (NPN, open-collector driver) | — | Same pattern as /DTACK. |
| `/BRn`, `/BGn` | routed | FPGA U17 (`/BRn` out), T14 (`/BGn` in) | Already wired — bus request/grant path exists. |

**Implication:** the biggest worry I raised in my first draft — "swap the strobes from unidirectional to bidirectional transceivers" — is unnecessary. U6 is already a 74LVC8T245. The firmware just needs to drive `DIRADDR2` (T15) dynamically instead of tying it to 0.

## 3. What is missing on the PCB (Zorro slot pins with an `x` in the schematic)

These Zorro III slot fingers are physically **not connected** to anything on the current PCB:

| Zorro pin | Signal | Needed for |
|---|---|---|
| **18C** | `/MTCR_XRDY` (multi-transfer request from host) | Feature A — burst reads |
| **48B** | `/MTACK` (multi-transfer accept from card) | Feature A — burst reads |
| **62B** | `/BGACK` (bus grant acknowledge from card) | Feature B — bus mastering |
| 46B | `/BERR` | not currently needed |
| 40B, 42B, 44B | `/EINT7, /EINT5, /EINT4` | not currently needed |
| 50B | `ECLK` | not currently needed |
| 91D | `SenseZ3` | not currently needed |
| 14C, 16C | `/C3, /C1` | not currently needed |

All signals above are shown on the symbol with `x` (no-connect) on the current rev.

## 4. Feature A — MTCR / MTACK (Zorro III burst slave reads)

Highest-value change. Lifts ZZ9000Net RX from ~4 MB/s to line rate; also benefits any other bulk read path (USB bulk, framebuffer readback, etc.). No Amiga-side software changes.

### 4.1 PCB changes

| Change | Detail |
|---|---|
| Route **/MTCR** (Zorro pin 18C) to a new FPGA input pin | B-side is 5 V; pass through a spare section of a 74LVC8T245 with DIR tied appropriately (bus→FPGA). U7 already does this for other inputs — an additional 8T245 sitting next to U7 would work, or a dedicated smaller part like a single-bit 74LVC1T45. |
| Route **/MTACK** (Zorro pin 48B) from a new FPGA output pin | /MTACK is active-low, wired-OR on the bus, so drive it through an **open-collector NPN** exactly like Q1/Q2 drive /DTACK and /CINH. Pull-up lives on the motherboard side. |
| Resolve the C20 / VCAP_R7 conflict | Constraint file originally had `#set_property PACKAGE_PIN C20 [get_ports ZORRO_NMTCR]` commented out (`zz9000.xdc` line 118) — C20 got re-used as `VCAP_R7` (HDMI capture). Either (preferred) assign /MTCR to a different spare 3.3 V FPGA pin, or move VCAP_R7 off C20. Option 1 is lower risk since VCAP routing is timing-sensitive. |

Cost estimate: 2 new Zorro slot traces, 1 new NPN transistor + 2 resistors, at most one extra 8T245 section, 2 FPGA pin reassignments.

### 4.2 Firmware side (already prepared)

- `mntzorro.v` line 81 has `//input wire ZORRO_NMTCR,` (commented out — it was stubbed precisely because the pin was never wired).
- Uncomment that, add a matching output `ZORRO_NMTACK`, add pin constraints, implement the four-beat burst responder. That's a firmware/FPGA task once the pins exist.

## 5. Feature B — Bus mastering (/BGACK)

Optional. Not needed to fix RX. Useful for future DMA features.

### 5.1 PCB changes

| Change | Detail |
|---|---|
| Route **/BGACK** (Zorro pin 62B) from a new FPGA output pin | Active-low, wired-OR — use an **open-collector NPN**, same topology as Q1/Q2. |

That is actually the only PCB-level change required for bus mastering, because:

- `/BRn` and `/BGn` are already routed.
- The data, address, and strobe transceivers (U1–U6) are already bidirectional with FPGA-controlled direction — the hardware supports the card driving /FCS, /DSx, /UDS, /LDS, READ, DOE, A[1..23], D[0..15] when it owns the bus. No transceiver swaps needed.

### 5.2 Firmware side

- `mntzorro.v` lines 82–88: strobes are declared `input wire`. Change to `inout wire` and add direction-controlled drivers.
- Stop tying `ZORRO_ADDRDIR2` to 0 — actually drive it from the bus-master state machine (H during card-drives cycles, L during slave cycles). Same for `ZORRO_ADDRDIR` and `ZORRO_DATADIR` during master write/read phases.
- Implement BRN/BGN/BGACK arbitration state machine and the master-side Z3 protocol.

This is a meaningful firmware job but needs **one** extra PCB signal (BGACK), not a buffer redesign.

## 6. What is not a PCB change but worth flagging for consistency

- **`ZORRO_ADDRDIR2` (FPGA pin T15)** is declared in the FPGA constraints and physically connected to U3 and U6's DIR pins. The firmware currently hardwires it to 0 (`assign ZORRO_ADDRDIR2 = 0;` in `mntzorro.v`). This is a firmware fix for bus mastering — no PCB change. Flagged here so the PCB designer knows T15's net **must keep** going to U3/U6 DIR on any rev; don't repurpose T15.
- **`/NCINH` is never asserted on Z3 cycles today** (firmware limitation, not hardware) — the Q2 open-collector driver exists and works. Unrelated to throughput; called out only because cacheable MMIO reads would need /NCINH deasserted, which is already the default.

## 7. Priority recommendation

If only **one** change is made to the next PCB rev, make it **Feature A** (MTCR + MTACK + the C20/VCAP_R7 resolution). That alone lifts RX from ~4 MB/s to ~14–16 MB/s (saturates 100 Mbit), unlocks bursts for every future bulk-read path, and costs only:

- 2 extra Zorro slot traces,
- 1 open-collector NPN driver (MTACK, same circuit as Q1/Q2),
- 1 extra 8T245 section (MTCR input buffering — or reuse a spare),
- 2 FPGA pin assignments + resolving the C20 conflict.

**Feature B** (adding /BGACK routing + one more open-collector driver) is cheap enough to piggyback on the same rev if convenient, but not required for the RX fix.

## 8. Appendix — quick cross-reference for the FPGA / schematic side

| Thing | Where it lives |
|---|---|
| Full schematic | `gfx/amiga-zz9000.svg` |
| Zorro signal ports | `mntzorro.v` lines 70–126 |
| `NMTCR` stub (commented out, waiting for pin) | `mntzorro.v` line 81 |
| `ADDRDIR2` tied to 0 (firmware limitation, not PCB) | `mntzorro.v`, search for `ZORRO_ADDRDIR2` |
| `NCINH` assertion (never fires on Z3 today) | `mntzorro.v`, search for `z_ovr` |
| FPGA pin constraints | `ZZ9000_proto.srcs/constrs_1/new/zz9000.xdc` |
| C20 conflict (`VCAP_R7` vs. commented-out `NMTCR`) | xdc lines 23 and 118 |
| `NBRN` / `NBGN` routed, BGACK not routed | xdc lines 112–113; confirm BGACK absence on board |
| Open-collector drivers (existing pattern to copy for MTACK, BGACK) | Schematic section "Open Collector", transistors Q1 (/DTACK) and Q2 (/CINH), `Q_NPN_BCE` |
| Bidirectional transceivers (don't change) | Schematic U1, U2, U3, U4, U5, U6 (all 74LVC8T245); U7 is input-only (DIR=GND) |

Zorro III spec references: **Multi-Transfer Cycle** (MTCR/MTACK handshake) and **Bus Mastering Arbitration** (BRN/BGN/BGACK) — Commodore Zorro III Bus Specification.
