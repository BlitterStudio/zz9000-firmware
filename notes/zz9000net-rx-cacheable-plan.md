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
accelerate Zorro III data transfer. The ~4 MB/s ceiling is a hardware
ceiling on these cards.

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

## Validation plan

1. Baseline zznetstats with current firmware — capture
   `PacketsReceived` / `Overruns` / `BadData` deltas.
2. Flash new bitstream + firmware with bumped backlog.
3. Re-run same download test on both A4000/040 and A4000/060.
4. Compare drop rates. If they don't fall meaningfully, the bottleneck
   is fully at the per-cycle Z3 rate, and the backlog bump is
   wasted DDR.

## The real fix (hardware roadmap)

Documented here so the next PCB rev has the full ask:

- Route `/MTACK` from FPGA to Zorro slot pin E11. Enables burst slave.
- Route `/MTCR` back (undo the VCAP_R7 reassignment, or reuse another
  pin). Without this the FPGA can't detect the master's burst
  request.
- Wire bidirectional transceivers so the FPGA can drive address and
  data strobes, not just listen. Needed for push-model RX (see
  [zz9000net-rx-pushmodel-plan.md](zz9000net-rx-pushmodel-plan.md)).
- With all three, both the burst-slave path and the push-model path
  become implementable.

## Cross-references

- Baseline + hypothesis: [notes/zz9000net-rx-throughput.md](zz9000net-rx-throughput.md)
- Archived push-model plan: [notes/zz9000net-rx-pushmodel-plan.md](zz9000net-rx-pushmodel-plan.md)
