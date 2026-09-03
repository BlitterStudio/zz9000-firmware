# ZZ9000 USB proxy protocol

The USB proxy is a single-owner command/data aperture shared by Zynq firmware and `zzusbhw.device`. Multi-byte protocol fields are big-endian unless noted for USB setup packet fields. Commercial Poseidon 4.5 is above this contract and remains unchanged.

## Aperture layout

| Offset | Size | Owner while pending | Contents |
|---|---:|---|---|
| `0` | 48 | Driver, then firmware | Legacy `ZZUSBCommand` prefix |
| `48` | 16 | Driver, then firmware | v2 extension |
| `64` | 16384 | Matching request only | v2 transfer staging payload |
| `16448` | 4032 | Reserved | Future bounded transport metadata |
| `20480` | 4096 | Firmware publisher | v2 diagnostic snapshot page (capability-gated) |

The physical aperture remains 24576 bytes. A legacy peer may use the historical payload extent from offset 64; v2 never stages more than 16 KiB. The legacy prefix and data offset never move.

## Legacy prefix

The packed 48-byte prefix is, in order: command, status, device address, endpoint, direction, transfer type, maximum packet size, requested length, actual length, timeout, speed, interval, the eight-byte USB setup packet, split-hub address, split-hub port, flags, and reserved word. Setup `wValue`, `wIndex`, and `wLength` remain USB little-endian; the other multi-byte values are big-endian. Persistent periodic commands use the reserved word as a device generation so a reused USB address cannot inherit an earlier endpoint.

`ZZUSB_FLAG_BULK_IN_POLL` (bit 3) marks an explicitly open-ended bulk-IN slice. An error-free qTD deadline returns `NAK` only with this flag; finite bulk requests return `TIMEOUT`. The flag is invalid for bulk OUT.

## v2 extension

| Relative offset | Type | Field | Rule |
|---|---|---|---|
| `0` | `u16` | version | Exactly `2` |
| `2` | `u16` | header size | Exactly `64` |
| `4` | `u32` | request ID | Nonzero, monotonically advances within a negotiated session |
| `8` | `u32` | controller epoch | Zero only in `QUERY_CAPS`; otherwise must match firmware |
| `12` | `u32` | capabilities | Firmware capabilities on completion |

A v2 driver ORs `ZZUSB_DOORBELL_V2` into the existing command-register write. Old firmware ignores the doorbell value and therefore returns `BADPARAM` to the appended `QUERY_CAPS` command; new firmware uses the marker to distinguish v2 from stale bytes left in the previously unused gap. An old driver never sets the marker, so new firmware always interprets it as v1 even when the gap contains old v2 data.

## Negotiation and lifecycle

1. A new driver sends `QUERY_CAPS` with v2 version/header, request ID 1, and epoch 0.
2. New firmware returns its nonzero controller epoch and only implemented capabilities. The matched base capability set covers v2 framing, request IDs, epochs, validation, bounded diagnostics, persistent periodic interrupt endpoints, simple and realtime ISO transport, the USB event IRQ, and precise errors.
3. `BADPARAM` selects constrained legacy mode. No v2 capability is advertised.
4. Every later v2 command carries the negotiated epoch and a new request ID. Firmware rejects zero, duplicate/out-of-order, or wrong-epoch work before EHCI access.
5. A successful controller reset advances the epoch. A completion is accepted only when its request ID matches; a non-reset response must also carry the expected epoch.
6. A local legacy timeout quarantines the driver transport. It must not overwrite the uncertain mailbox. Later lifecycle work replaces polling with acknowledged retirement/reset semantics.

Firmware snapshots the command and OUT payload before starting EHCI. It copies IN data and completion metadata back only if the shared request still matches. This is exact for v2 IDs/epochs and a best-effort immutable-field fence for v1.

## Persistent interrupt transport

`PERIODIC_ARM` creates or reuses one EHCI interrupt queue keyed by epoch, device generation, address, endpoint, direction, speed, maximum packet, interval, and split topology. Full/low-speed split queues encode the transaction-translator think time and multi-TT slot in legal start/complete masks. Poseidon normalizes high-speed descriptor `bInterval` exponents to power-of-two microframe intervals before submitting `PERIODIC_ARM` or `PERIODIC_REAP`; the proxy carries those normalized values unchanged. Firmware retains at most 16 endpoints and one unread completion per endpoint; it pauses that endpoint rather than overwrite unread data.

`PERIODIC_REAP` returns one matching completion. `NAK`, a zero-length IN transaction, and an all-zero hub change report are non-terminal to the Poseidon request. A successful reap rearms the retained descriptor no sooner than its declared interval. The first `STALL` from a split IN endpoint is deferred for 50 ms and retried once, giving the parent hub change endpoint time to report child removal before a disappearing HID endpoint enters Poseidon's clear-halt loop. Before queuing a nonzero hub change report, firmware retires any already-stalled split periodic child whose recorded hub address and port match an asserted port bit; unrelated live children and unrelated hub changes are untouched. Poseidon's subsequent `AbortIO` therefore observes an already quiesced child instead of unlinking its halted QH during class teardown. A repeated stall and all other transfer errors are terminal and retire the endpoint. `PERIODIC_STOP`, reset, detach, epoch advance, and controller recovery retire matching queues through the fenced EHCI unlink path.

Firmware raises interrupt-source bit `16` in `REG_ZZ_CONFIG` only after it queues data or a terminal error. The Amiga interrupt server acknowledges only that source by writing `8 | 256` and signals the mailbox-owning worker; it never accesses the mailbox itself. More unread completions cause firmware to reassert the coalesced source after a reap. The driver installs this shared server on INT6 by default or INT2 when `ZZ9000.CFG` selects `INT2`.

## Isochronous transport

`ISO_QUEUE`, `ISO_REAP`, and `ISO_STOP` use transfer type ISO and are available only in negotiated v2 mode. The firmware retains at most eight batches globally, with at most 32 packets and 15840 data bytes per batch. High-speed endpoints use iTDs; full-speed endpoints are accepted only behind a high-speed hub and use split-transaction siTDs. Low-speed ISO is invalid. Poseidon normalizes high-speed intervals to microframes and full-speed intervals to frames before submitting hardware requests; the proxy carries those normalized power-of-two values unchanged.

The batch header is 32 bytes:

| Offset | Type | Field |
|---|---|---|
| `0` | `u32` | magic `0x5a49534f` (`ZISO`) |
| `4` | `u16` | batch version `1` |
| `6` | `u16` | flags; bit 0 requests ASAP scheduling |
| `8` | `u32` | nonzero batch ID |
| `12` | `u16` | starting frame, modulo 2048 |
| `14` | `u16` | packet count, 1-32 |
| `16` | `u32` | total packet data extent |
| `20` | `u8` | starting microframe, 0-7 |
| `21` | 11 bytes | reserved, zero |

Each 16-byte packet entry contains requested length at offset 0, actual length at 2, status at 4, completed frame at 6, payload-relative data offset at 8, and completed microframe at 12. Remaining bytes are reserved. Packet status values are OK `0`, pending `1`, short `2`, missed `3`, underrun `4`, overrun `5`, cancelled `6`, offline `7`, transaction error `8`, and babble `9`.

An OUT queue contains header, packet entries, then the complete data extent. An IN queue contains metadata only. A successful reap returns metadata plus the complete data extent for IN and metadata only for OUT. Packet offsets describe the requested layout, so a short packet does not change later packet offsets. The driver accepts a reap only when its endpoint identity and batch ID are still outstanding.

Explicit scheduling preserves the requested frame and microframe. ASAP scheduling chooses a wrap-safe lead of at least four frames; later ASAP batches for the same endpoint chain after the last queued packet instead of choosing overlapping starts. Firmware raises the shared USB event source only for a reapable batch or actionable terminal condition. Pumping and harvesting are bounded in the main loop.

`ISO_STOP`, reset, detach, and epoch invalidation unlink active descriptors, wait for the asynchronous schedule to retire, release bandwidth reservations, and discard both active and completed-but-unreaped batches before their storage can be reused.

## Diagnostic snapshot page

`ZZUSB_CAP_DIAGNOSTICS` makes the final 4096 bytes directly readable while the command mailbox is pending. The page is big-endian and starts with:

| Offset | Type | Field |
|---|---|---|
| `0` | `u32` | magic `0x5a554447` |
| `4` | `u32` | seqlock generation; odd while publishing |
| `8` | `u16` | snapshot version `1` |
| `10` | `u16` | header size `128` |
| `12` | `u32` | total page size |
| `16` | `u32` | firmware capabilities |
| `20` | `u32` | controller epoch |
| `24` | `u32` | last request ID |
| `28` | `u32` | next physical event-ring slot (`0`-`63`); when the ring is full this is also the oldest retained slot |
| `32` | `u32` | retained event count |
| `36` | `u32` | overwritten/lost event count |
| `40` | `u32` | queue state: mailbox in bits 0-7, periodic ready depth in bits 8-15, active periodic endpoints in bits 16-23 |
| `44` | `u32` | EHCI command/status bits in the low half, aggregate periodic S/C masks in the high half |
| `48` | 16 × `u32` | fixed counters |
| `128` | 64 × 32 bytes | events in physical ring slots |

Each event contains sequence, request ID, epoch, detail, timestamp, type, status, address, topology, endpoint, direction, and schedule bits. Before the first wrap, the retained events occupy slots zero through `count - 1`. Once `count` is 64, readers start at the slot from offset 28 and wrap modulo 64 to traverse oldest-to-newest. The sequence stored in each event is the authoritative monotonic identity and lets readers detect overwritten or concurrently replaced slots. A reader copies only when the generation is even and unchanged before and after the copy. Firmware flushes the odd generation, page body, and final even generation in that order. A legacy peer neither advertises the capability nor has its aperture tail interpreted as a snapshot.

## Validation

Firmware rejects malformed values before controller access: device address above 127, endpoint above 15, invalid direction or speed, payload beyond the negotiated extent, inconsistent setup length, illegal transfer type or interval, invalid maximum packet size, high-speed split use, or incomplete split hub/port topology. Split hub address and port are 1-127. TT mode and think time are meaningful only when the split flag is present.

## Append-only commands

Commands `0x01`-`0x0c` retain their legacy values. v2 appends capability query, endpoint retire/cancel, diagnostic snapshot, periodic arm/reap/stop, and ISO queue/reap/stop commands at `0x0d`-`0x16`. Periodic and ISO commands are capability-gated and implemented by the matched firmware/driver release.

## Append-only statuses

Legacy status values through `BADPARAM` (`0xf6`) remain unchanged. v2 appends `UNSUPPORTED`, `STALE`, `CANCELLED`, `HOSTERROR`, `BUSY`, and `NOMEM` down through `0xf0`. Unknown appended statuses are host errors to legacy drivers. Actual length is valid only for the matching completion and never exceeds the negotiated staging extent.
