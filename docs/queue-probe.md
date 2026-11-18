# Queue coherence probe

The queue probe is a temporary diagnostic for the AAR cutover. It checks that
a control block and a queue pair in an SDK shared buffer (the host-visible
heap, on the normal write-back shareable mapping) stay coherent between the
Amiga and firmware code on both ARM cores. It is removed when the AAR
transport replaces it.

The Amiga side is `zz9k-qprobe` (see `sdk/docs/zz9k-library.md`). The wire
contract, the memory layout, and the entry format are in
`sdk/tools/zz9k-qprobe.h`; the firmware is `sdk_qprobe.c`. The mailbox opcode
is `SDK_OP_DIAG_QPROBE` (`0x0904`) in the DIAG service.

## Firmware behaviour

- A queue has an owner. CORE0 queues are served by the core-0 main loop,
  CORE1 queues by the core-1 system thread, and DOORBELL queues only when the
  Amiga sends KICK. The core-1 worker does not sleep while a CORE1 queue is
  attached.
- Each pass consumes the up ring, then fills the down ring. The consumer reads
  the head, then a barrier, then the entries, then a barrier, then writes the
  tail. The producer writes the entry, then a barrier, then the head.
- Firmware writes use the cached mapping of the buffer's ARM address and do
  no cache maintenance. The ACP is what makes the Amiga's view coherent, and
  that is what the probe tests.
- The up ring carries PING and COMPLETE. Every entry is checked with
  `zz9k_qp_entry_check`; the first error code and its sequence number are kept.
  COMPLETE must name a down entry that was produced and be higher than the one
  before. A PING is answered with a PONG; if the down ring is full the PONG
  waits, and up consumption pauses when 8 PONGs are waiting.
- The queue state (a few KiB) lives in the coherent task-queue region at
  `SDK_QPROBE_STATE_ADDRESS`, so both cores see it without cache maintenance.

## Teardown

Freeing the buffer (`FREE_SHARED`), `CLIENT_RELEASE`, the allocator sanitiser,
and the Amiga reset detach a queue first. Core 1 may be inside a pass when the
buffer is freed, so a CORE1 queue uses a handshake on two words of its state:

1. Core 1, per pass: set `busy`, barrier, work only if `attached` is still
   set, barrier, clear `busy`.
2. Core 0, detach: clear `attached`, barrier, wait for `busy` to be 0.

Either core 1 sees `attached` clear and leaves the pair alone, or core 0 sees
`busy` and waits for the pass to end. The wait is bounded (20 ms). Core 1
counts as gone when the scheduler reports it unavailable or parked, because a
fault can leave `busy` set; a cold restart of core 1 clears every `busy` word.
If core 1 does not leave, `FREE_SHARED` and `CLIENT_RELEASE` answer BUSY and
keep the buffer; the Amiga reset restarts core 1.

## Tests

`make -C test/qprobe test` runs the production `sdk_qprobe.c` against a fake
Amiga written with the contract header, and the production `sdk_mailbox.c`
free paths against the probe. It is part of the CI host suites.
