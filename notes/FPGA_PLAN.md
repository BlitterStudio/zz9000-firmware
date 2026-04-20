<!--
  Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
  SPDX-License-Identifier: GPL-3.0-or-later
-->

# FPGA plan — SD-boot follow-up

## Context & motivation

The baseline SD-boot feature works: the Amiga mounts an HDF from a
FAT card, PFS3 partitions come up, reads/writes go through. But two
bottlenecks are baked into the current design and **cannot be fixed
in firmware alone**:

1. **Polling completion.** Every block I/O ends with the Amiga CPU
   spinning on a Zorro register until the firmware clears a BUSY
   sentinel. With ~10 ms average response time per 24 KB chunk, the
   CPU is fully occupied by Zorro reads for the duration — no other
   Amiga task (including Intuition's input handler) gets cycles, so
   the mouse visibly stutters during any sustained disk activity,
   and bulk throughput plateaus around 1–2 MB/s even though the
   shared-buffer transfer itself takes < 1 ms of that window.

2. **8 KB boot ROM window.** The FPGA currently routes
   `cardbase+0x6000..0x8000` to the boot ROM DDR region and
   `cardbase+0x8000..0xA000` to the ethernet TX frame. Everything
   the m68k driver needs (header + diag thunk + driver binary) has
   to fit inside 8 KB. We're at ~6.8 KB today; further driver work
   (IFS/NSD/HD_SCSICMD coverage, SMART, async support) will run
   out of room.

Fixing either requires changing the Vivado bitstream. While we're
re-synthesizing anyway, we should bundle both.

## Scope

Target is a single coordinated bitstream + firmware + driver bump:

* **Completion interrupt** (must have):
  - Wire a Zorro interrupt line from the firmware-visible register
    space up through the existing Zorro core so the ARM can assert
    it when `sd_status` or `sd_boot_status` transitions out of BUSY.
  - Add a matching register (likely in the unused `0xCE`/`0xCA`
    window) that the driver writes to arm/disarm the interrupt and
    reads to clear the condition.
  - Replace the m68k driver's polling loop with `Wait(SIGMASK)` on
    an interrupt server hooked via `AddIntServer(INTB_PORTS, ...)`
    (or `INTB_EXTER` depending on which Zorro INT we expose).

* **Wider boot ROM window** (nice to have):
  - Bump the decoded range from `0x6000..0x8000` to `0x6000..0xA000`
    (16 KB) by reclaiming the `0x8000..0xA000` slice that currently
    goes to the ethernet TX frame. Move the ETH TX frame to a
    different range (there's room above 0x10000 in the FPGA map).
  - Firmware-side: update `BOOT_ROM_SIZE` in `bootrom.h` to `0x4000`
    and the read handler in `main.c`. Driver-side: update the
    offsets in `boot.S` and `bootrom.c` so the driver image can
    grow.

## Blast radius / what to check for regressions

Changing these paths touches every read the Amiga does of the
ZZ9000's register space. A synthesis bug or address decode mistake
can take down graphics, ethernet, audio, USB — everything.

Regression surface:

| System | How it could break | Quick regression test |
|--------|--------------------|------------------------|
| RTG graphics | Framebuffer is at `cardbase >= 0x10000`; any shift of lower windows mustn't overlap | Boot to Workbench, verify RTG screens open and update at correct resolution |
| Ethernet RX | `0x2000..0x6000` mapped to `RX_BACKLOG_ADDRESS`; DMA path separate from reads, but also served from main.c when forwarded | `ping` + ARP resolution; watch `[EMAC]` prints on serial |
| Ethernet TX | If we move TX_FRAME_ADDRESS, both the FPGA path and the main.c fallback have to agree on the new range | Outbound `ping`, observe counters |
| Audio DMA | ADAU buffers are at `0x3FC00000..`; shouldn't be touched, but any AXI-HP contention from the new interrupt plumbing can starve audio | Play back a sample, listen for drop-outs |
| USB storage | `0xA000..0x10000` shared buffer is used by both SD boot and USB proxy — must stay 24 KB visible | Connect USB stick, verify block I/O |
| Zorro register reads | A bad decode produces `0xFFFF` for registers that used to return real data | Amiga serial log should still show `[audio]`, `[USB]` etc. init lines, and `ZZTop` on the Amiga should display correct FW version |
| Autoboot | Wrong ROM window routing = driver image read returns garbage = Kickstart either refuses the resident or relocates garbage code (guru 80000003/4) | First boot after flashing; verify `INIT returning dev` on serial |
| DMA cache coherency | AXI-HP (non-coherent) is what we use; if the interrupt plumbing ends up on ACP instead, missing cache flushes become no-ops and writes reach DDR transparently — **not** necessarily safe, revisit each `Xil_DCacheFlushRange` | Re-run SysSpeed disk; compare checksums of a known-good HDF before/after a write cycle |

## Roadmap / ordering

1. **Document the current baseline** (done here) so we can always
   revert to the polling design if the FPGA work goes sideways.
2. **Stand up the interrupt path on paper**: which INT line, which
   FPGA module asserts it, how the driver services it. Cross-check
   with existing use of INT2/INT6 in the ZZ9000 codebase.
3. **Minimal FPGA change**: just the interrupt line. Keep the ROM
   window at 8 KB for now. Test end-to-end. If perf/responsiveness
   jumps as expected (mouse smooth during I/O, bulk > 5 MB/s), good.
4. **Second FPGA pass**: bump the ROM window to 16 KB, rebalance
   the ETH TX frame region. Test that ethernet still works.
5. **Driver rewrite** to use `Wait()` on the interrupt signal
   instead of polling. Keep the polling path behind a build flag so
   we can A/B under the same firmware.
6. **Re-benchmark SysSpeed**, compare with the baseline we just
   committed, and lock in the numbers.

## Non-goals for this round

* Async/overlapped I/O from the Amiga side. Once the interrupt is in,
  we could queue multiple requests and fan them out to multiple CPU
  cores in the firmware, but that's a much bigger project.
* Writable config file for the HDF path. Orthogonal and can ship
  independently.
* Raw-SD compat. Intentionally dropped; HDF-only is the supported
  layout.

## What lives in the repo right now

The working baseline at this point:

* `zz9000-drivers/sd-boot/` — m68k driver, stable, polling-based.
* `zz9000-firmware/ZZ9000_proto.sdk/ZZ9000OS/src/sd_boot.{c,h}`,
  `sd_storage.c`, `bootrom.{c,h}` — firmware side.
* `zz9000-firmware/notes/sd-boot.md` — firmware architecture doc.
* `zz9000-drivers/sd-boot/README.md` — user/integrator guide.

The bitstream (`bootimage_work/zz9000_ps_wrapper.bit`) is the
polling-era one. The FPGA work above will produce a new `.bit`.
