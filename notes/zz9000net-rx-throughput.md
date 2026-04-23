<!--
  Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
  SPDX-License-Identifier: GPL-3.0-or-later
-->

# ZZ9000Net RX throughput — root cause and structural fixes

**Status:** root cause identified and measured. Fixes listed at the
bottom are candidates, not implemented.

## The symptom

On a 68060-class Amiga with Roadshow, ZZ9000Net RX tops out around
3000–4000 kb/s (kilobits per second, not bytes) while TX sustains
~10300 kb/s. The RX ceiling shows a hockey-stick curve — a 100 kB
download transfers at ~6400–7500 kb/s, but anything 800 kB or
larger collapses to ~2500–3500 kb/s. Classic TCP-drop-and-retransmit
signature: small downloads fit in a single burst the driver can
drain; larger ones force sustained throughput that crosses some
threshold, frames get dropped, senders retransmit, window shrinks,
throughput falls off a cliff.

## What it turned out to be

Frames are being dropped **at the firmware RX backlog**, not
anywhere in the Amiga driver's packet-matching or BufferManagement
path. The driver cannot read the RX slot over Zorro MMIO fast
enough to keep up with line rate, the backlog saturates, firmware
drops frames at the MAC layer.

Evidence: ZZ9000Net revision ≥ 18 adds a gap-based overrun counter
that watches the firmware `serial` field for skips. During a single
moderate-size download test, the SANA-II stats came back as:

| Metric            | Before  | After   | Δ       |
|-------------------|--------:|--------:|--------:|
| PacketsReceived   |      28 |  16 315 |  16 287 |
| PacketsSent       |      11 |  20 065 |  20 054 |
| Overruns          |       7 |  12 469 |  12 462 |
| BadData           |       1 |   1 879 |   1 878 |

Drop rate at the firmware backlog during the test:
`12 462 / (16 287 + 12 462) = 43.3%`. Almost half of the inbound
frames never reach Roadshow. TCP senders retransmit, congestion
window shrinks, throughput collapses.

This rules out the earlier cache-coherency hypothesis as the
dominant factor — if stale-cache reads were causing the cliff, the
symptom would be corrupt frames (header/payload mismatch) rather
than cleanly dropped ones. Coherency might still be contributing to
the small `BadData` count, but it's not the main story.

## Why the driver can't keep up

Per RX frame the driver must:

1. Read the 4-byte header (size + serial) — done in 1 longword
   Zorro read.
2. Read the 12-byte dst+src MAC — coalesced to 3 longword reads in
   recent driver work.
3. Call Roadshow's BufferManagement `S2_CopyToBuff` callback, which
   reads the payload out of Zorro MMIO one access at a time into
   Roadshow's own buffer.
4. Write a single word to `rx_accept` (slv_reg for `REG_ZZ_ETH_RX`)
   to release the slot.

Step 3 dominates: a ~1500-byte MTU frame is ~375 longword reads off
Zorro. Measured effective Zorro read throughput on this setup works
out to roughly 4 MB/s, which maps to the observed ~30 Mbit ceiling.
Drop rate rises as the 100 Mbit MAC outpaces the driver by 3×.

Everything else — the semaphore window, the packet-type match, the
interrupt path, the header reads — is a rounding error next to the
payload copy.

## Structural fixes (ordered by impact, not effort)

### 1. Push-model RX — firmware DMAs frames into Amiga RAM

Biggest change, biggest win. Today the FPGA stages frames in
`RX_BACKLOG_ADDRESS` (its own DDR) and the Amiga CPU pulls them out
across the Zorro bus one access at a time. Flip the direction:

- Driver allocates an Amiga-side RX ring in Chip or Fast RAM (the
  `MEMF_CHIP` / `MEMF_FAST` split depends on where the DMA engine
  can write).
- Driver publishes the ring base + length via new MMIO registers.
- Firmware `ethernet_recv_handler` DMAs each received frame into
  the ring (Zorro-side DMA writes — the FPGA is master) and
  advances a producer index register.
- Driver reads from plain RAM (cached, fast), not from Zorro MMIO.

The per-frame cost on the Amiga drops from ~375 slow Zorro reads
to ~375 cached RAM reads (two orders of magnitude faster on a 060).
This is the only fix that has a realistic shot at saturating the
100 Mbit MAC.

Cost: new firmware DMA write path on the Zynq side, new register
contract, driver rewrite of `frame_proc`. Non-trivial but the
`RX_FRAME_ADDRESS` → Zorro path for video DMA writes (`mntzorro.v`
around line 2178) already does something similar — the plumbing
exists.

### 2. Bigger firmware backlog

Cheap and local: raise `FRAME_MAX_BACKLOG` in
`ZZ9000_proto.sdk/ZZ9000OS/src/ethernet.h:30` from 32 to, say, 128.
Each backlog slot costs `FRAME_SIZE = 2048` bytes of DDR — going
from 32 to 128 is an extra ~200 KB.

This delays the cliff without removing it. The driver still can't
drain faster than ~4 MB/s; it just has more buffer to hide bursts
behind. For short-lived transfers the user might see fewer drops;
for sustained downloads, the backlog fills 4× later and the cliff
returns. Worth trying as a data point — if `Overruns` drops
noticeably at 128 and collapses back to similar rates at 512, we
know Amiga-side drain is the floor.

### 3. Cacheable RX window + driver cache management

Keep the MMIO pull model, but let the CPU burst-read the backlog
region via cache lines instead of one bus cycle per access. Needs:

- Firmware / autoconfig: flag the RX window as cacheable (instead
  of I/O) in the Zorro descriptor, or expose it via a different
  BAR that the Amiga MMU setup maps cacheable.
- Driver: call `CacheClearE(slot_addr, slot_size, CACRF_ClearD)`
  before reading each frame, so the CPU always goes back to the
  bus for the first line of a new frame instead of serving a
  stale cached line.

The 060 data cache is 8 KB with 16-byte lines. A 1500-byte frame is
~94 cache lines. Burst reads can be substantially faster than
single-beat MMIO. But: every driver that touches the card has to
know about the cacheable/non-cacheable regions, and if the RX
window ends up sharing a Zorro BAR with the RTG framebuffer, the
MMU-per-page control needed to keep them different gets ugly.

Smaller win than option 1, comparable implementation complexity.

## How to reproduce the measurement

1. Cold boot. Load `ZZ9000Net.device` rev ≥ 18.
2. Let Roadshow come up and settle (~30 s of idle).
3. `zznetstats > t:before` — captures the SANA-II stats baseline.
4. Run the slow-download test (a large file, ≥ 1 MB).
5. `zznetstats > t:after` — captures post-test stats.
6. Diff by eye. The `Overruns` delta divided by
   `PacketsReceived Δ + Overruns Δ` is the backlog drop rate.
   `BadData` is an anomaly counter (delta > 128 frames between
   successive reads — torn reads, init state, or very long driver
   stalls).

## Cross-references

- Driver-side work: `zz9000-drivers/net/device.c` (branch
  `net-gcc-port` as of 2026-04-23).
- Driver diagnostic tool: `zz9000-drivers/net/zznetstats/`.
- Firmware RX path: `ZZ9000_proto.sdk/ZZ9000OS/src/ethernet.c`
  functions `ethernet_recv_handler` and `ethernet_receive_frame`,
  `FRAME_MAX_BACKLOG` in `ethernet.h`.
- Firmware MMIO/Zorro wiring: `mntzorro.v` around line 1815
  (`eth_rx_frame_select` selects which backlog slot the Amiga
  sees) and line 2178 (existing RX DMA write path for reference
  when implementing push-model RX).
- Related firmware DMA oddities: `notes/zz9k-dma-problem.txt`.
