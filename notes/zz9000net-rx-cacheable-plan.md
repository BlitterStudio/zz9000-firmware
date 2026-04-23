<!--
  Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
  SPDX-License-Identifier: GPL-3.0-or-later
-->

# ZZ9000Net RX — what we can fix without a PCB rev

**Status:** superseded by reality. Keeping this doc as the record of
what we tried and why the "right" fix is hardware, not RTL.

**TL;DR:** The planned cacheable-window fix does not help on this PCB.
Zorro III burst reads require the slave to assert `/MTACK`, which is
not routed to the ZZ9000 FPGA on either the R1 or R5 boards. Without
bursts, flipping the Amiga MMU to cacheable produces **no throughput
improvement** — each 68040/060 cache-line fill still degenerates to
four back-to-back single-beat Z3 cycles, exactly what the driver does
today. The driver never re-reads the same longword, so there is no
cache-locality win either.

## Why the cacheable plan died

Three findings from `mntzorro.v` + `zz9000.xdc`:

1. **`/MTACK` is not a pin on this PCB.** The XDC has every Zorro
   signal (FCS, DS0/DS1/UDS/LDS, CCS, SLAVE, CINH, CFGIN/OUT, DTACK,
   IORST, E7M, INT6, BRN/BGN) but no MTACK. Full-text search of the
   entire repo finds zero references. Its absence is deliberate, not
   a typo.
2. **`/MTCR` was routed but got repurposed.** `ZORRO_NMTCR` is
   commented out at [mntzorro.v:81](../mntzorro.v#L81) and
   [zz9000.xdc:118](../ZZ9000_proto.srcs/constrs_1/new/zz9000.xdc#L118);
   pin C20 now drives `VCAP_R7` (video capture). So even if we wanted
   to accept burst requests, we can't see them on this bitstream
   without giving up video capture, and even then MTACK has nowhere
   to go.
3. **`z_ovr` is never asserted on Z3 paths**
   ([mntzorro.v:722-723](../mntzorro.v#L722)), so `ZORRO_NCINH` is
   asserted on every Z3 cycle. The FPGA is telling the Amiga
   "cache-inhibit" at the bus level regardless of MMU setup. Could be
   fixed trivially (one line), but without burst support the cache
   win is zero — so not worth the change.

The push-model plan and this cacheable plan both have the same root
cause: the PCB does not give the FPGA the bus signals needed to
accelerate Zorro III data transfer — bursts require MTCR/MTACK, which
are unrouted.

**Note, however, that hardware is not the whole story.** Observed RX
(~0.28 MB/s, see Results below) is only ~7% of even a conservative
estimate of the Z3 single-longword slave-read ceiling. There is clearly
substantial software-side headroom as well; the MTCR/MTACK gap would
explain a remaining factor of 3–4× above that, not the full ~40×
gap from line rate. Driver-side profiling is the right next step —
see the "What's actually limiting RX" section.

## What we're shipping anyway

Two small changes with limited but real benefit. Neither touches RTL
bus protocol — they change buffering and memory layout only, so the
blast radius is bounded.

### 1. Bigger RX backlog ring (32 → 128 slots)

`FRAME_MAX_BACKLOG` in
[ZZ9000_proto.sdk/ZZ9000OS/src/ethernet.h:30](../ZZ9000_proto.sdk/ZZ9000OS/src/ethernet.h#L30)
goes from 32 to 128. Ring size in DDR grows from 64 KB to 256 KB.

Does not raise the throughput ceiling. Does absorb larger bursts
before the firmware starts dropping, which matters for HTTP-style
traffic where a TCP window worth of data arrives in one clump. Dropped
frames trigger retransmits and collapse TCP congestion window — the
bigger ring makes those drops less frequent, so sustained throughput
should improve even with the same peak rate.

Expected improvement: modest but real. First-pass guess is that
typical browser-style traffic sees the drop rate fall from ~43% to
somewhere in the low teens; measure it, adjust.

### 2. Move `USB_BLOCK_STORAGE_ADDRESS`

Current layout:
```
0x3FE00000  RX_BACKLOG_ADDRESS        (64 KB, 32 slots)
0x3FE10000  USB_BLOCK_STORAGE_ADDRESS
```

256 KB of RX backlog would collide with USB storage. Move
`USB_BLOCK_STORAGE_ADDRESS` to `0x3FE40000` (just past the grown
ring). Stays inside the same 1 MB L1-MMU page so existing ARM TLB
attributes continue to cover it unchanged. Affects two definitions:

- [memorymap.h:20](../ZZ9000_proto.sdk/ZZ9000OS/src/memorymap.h#L20)
- [mntzorro.v:53](../mntzorro.v#L53)

The FPGA-side decode (`USB_BLOCK_STORAGE_ADDRESS - 32'ha000 + ...`) is
a symbolic offset from the macro, so updating the macro in one place
fixes both the firmware pointer and the FPGA AXI address.

## What we're NOT doing (and why)

- **FSM burst support in RTL.** Would need MTACK on the PCB. Dead.
- **Amiga-side MMU cacheable + `CacheClearE` path.** Zero win without
  bursts. Would add driver complexity for nothing.
- **AXI-side prefetch for RX reads.** Could hide ~100-200 ns of AXI
  latency inside each Z3 cycle (~10-20% speedup), but the FSM rewrite
  is non-trivial and we'd need real before/after numbers to justify
  it. Park as a future exploration.

## Results (measured)

Sustained HTTP download on the A4000/060 with Roadshow, before and
after the backlog + USB relocation change.

| Metric | Baseline (backlog=32) | After (backlog=128) |
|---|---|---|
| Sustained RX throughput | ~2300 kbit/s (~0.28 MB/s) | ~2300 kbit/s (~0.28 MB/s) |
| `Overruns` / packet drops | ~43.3% | ~46.1% |
| `BadData` | ~6.5% | **~3.9%** |
| TX throughput (reference) | ~10 Mbit/s (~1.25 MB/s) | ~10 Mbit/s (~1.25 MB/s) |

Interpretation:

- **Throughput unchanged** (~0.28 MB/s). The bigger ring did not raise
  the sustained rate because the consumer side (Amiga CPU draining
  the ring via MMIO) is the bottleneck, not the producer-side buffer
  capacity.
- **Drop rate essentially unchanged** — within noise. The hypothesis
  that "a bigger ring absorbs bursty HTTP arrivals" assumed the Amiga
  drains fast enough on average; it doesn't, so a bigger ring just
  fills more and drops at the same sustained cadence.
- **BadData halved** (6.5% → 3.9%), which is the single quantifiable
  win — fewer torn/late frames reaching the driver. This is why the
  change is still worth keeping despite no throughput gain.
- **RX/TX asymmetry is huge** (RX 0.28 MB/s vs TX 1.25 MB/s, ~4.5×).
  Even on a bus where writes are naturally faster than reads (writes
  are posted, reads stall), this gap is larger than expected.

Conclusion: backlog bump is retained as a small quality improvement,
but the real RX bottleneck is **not** backlog capacity.

## What's actually limiting RX

With RX at ~0.28 MB/s and the conservative Z3 single-longword MMIO
ceiling in the ~4 MB/s range, we are at ~7% of even the unburst-able
ceiling. Burst reads (MTCR/MTACK) can raise that ceiling to ~14-16
MB/s, but they will not close the 14× gap below the current ceiling.
Something else is dominating per-frame cost.

Likely suspects (need profiling on the driver side, `zz9000-drivers`
repo, not firmware):

1. **Per-frame MMIO overhead** — if each RX frame incurs N small
   register reads (size, status, flags) before the payload, per-frame
   latency dominates over per-byte bandwidth. Even 1 KB frames at
   150 frames/sec land at ~0.22 MB/s — which matches what we see.
2. **SANA-II `CopyFromBuff` / pool allocation cost per frame.** TCP
   stack on 040/060 is not free.
3. **RX buffers landing in Chip RAM.** Chip RAM writes are ~3.5 MB/s
   on their own and contend with the OCS/AGA chipset. If the driver
   doesn't force Fast RAM allocation for pool buffers, that alone is
   a hard ceiling.
4. **Driver poll / IRQ cadence.** If the driver drains only at task
   schedule points (~50-200 Hz), sustained rate is capped by how
   many frames can be drained per scheduling slice.

Next action: profile one RX frame end-to-end on the Amiga, attribute
the time, attack the top contributor. This work lives in
`zz9000-drivers`, not this repo.

## The real fix (hardware roadmap)

Separately from the driver-side work above, the next PCB rev should
add the three Zorro signals that are currently `x` (no-connect):

- **`/MTACK`** — Zorro slot pin 48B. Needs a new FPGA output +
  open-collector NPN driver (same pattern as Q1/Q2 for `/DTACK` and
  `/CINH`).
- **`/MTCR`** — Zorro slot pin 18C. Needs a new FPGA input pin
  (resolve the C20/`VCAP_R7` conflict). Pass through a spare 74LVC8T245
  section or add one.
- **`/BGACK`** — Zorro slot pin 62B. Needed for bus mastering.
  Another open-collector NPN.

**Good news from re-reading the schematic** (`gfx/amiga-zz9000.svg`):
the bidirectional transceivers needed for the card to drive the bus
as master are already present (U1-U6 are all 74LVC8T245 with
FPGA-controlled direction on DIRDATA / DIRADDR / DIRADDR2). No buffer
redesign is required — bus mastering just needs BGACK routed and
firmware to drive `ZORRO_ADDRDIR2` dynamically instead of tying it
to 0. See [zz9000-pcb-designer-brief.md](zz9000-pcb-designer-brief.md)
for the full PCB-side ask.

## Cross-references

- Baseline + hypothesis: [notes/zz9000net-rx-throughput.md](zz9000net-rx-throughput.md)
- Archived push-model plan: [notes/zz9000net-rx-pushmodel-plan.md](zz9000net-rx-pushmodel-plan.md)
