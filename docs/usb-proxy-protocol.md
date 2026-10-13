# ZZ9000 USB proxy protocol

The USB proxy lets a host driver submit USB work to the ARM firmware through a
24 KiB shared aperture. Firmware recognizes the legacy command IDs and version 2
framing. Version 2 adds request IDs, controller epochs, diagnostics, persistent
interrupt queues, ISO batches, and a maintenance mailbox.

All multi-byte fields are big-endian, except `setup_wValue`, `setup_wIndex`, and
`setup_wLength`. Those three setup-packet fields remain USB little-endian.

## Aperture and submission

| Byte range | Size | Use |
|---|---:|---|
| `0–47` | 48 | Primary `ZZUSBCommand` header |
| `48–63` | 16 | Primary `ZZUSBProtocolExtension` header for v2 |
| `64–16447` | 16384 | Primary v2 staging payload |
| `16448–16511` | 64 | v2 maintenance mailbox headers |
| `16512–20479` | 3968 | Maintenance mailbox staging payload |
| `20480–24575` | 4096 | Diagnostic snapshot page |

Version 1 stages data from offset `64` through the end of the aperture, for a
maximum transfer of `24512` bytes. Version 2 limits primary staging to
`ZZUSB_V2_DATA_MAX` (`16384` bytes). The maintenance mailbox uses the same
48-byte command header and 16-byte v2 extension, with a maximum payload of
`ZZUSB_MAINT_DATA_MAX` (`3968` bytes).

The host owns a request until firmware completes it. It writes the primary
headers and any OUT payload, then writes `REG_ZZ_USB_PROXY_CMD`. OR
`ZZUSB_DOORBELL_V2` (`0x8000`) into that register value for a v2 request.
Firmware snapshots the request before processing it and writes a completion
only when the shared header still describes the same request. The v2 check also
matches the request ID and controller epoch. A mismatch records a late
completion and advances the controller epoch.

The maintenance mailbox is polled when no primary request is pending. It
accepts `QUERY_CAPS`, `ISO_QUEUE`, `ISO_REAP`, and `ISO_STOP`; other validated
commands return `UNSUPPORTED`. It does not use the primary doorbell.

## Headers and request lifetime

The packed `ZZUSBCommand` header is 48 bytes.

| Offset | Type | Field |
|---|---|---|
| `0` | `u16` | command |
| `2` | `u16` | status |
| `4` | `u32` | device address |
| `8` | `u16` | endpoint |
| `10` | `u16` | direction, `0` for OUT or `0x80` for IN |
| `12` | `u16` | transfer type |
| `14` | `u16` | maximum packet size |
| `16` | `u32` | requested data length |
| `20` | `u32` | actual data length on completion |
| `24` | `u32` | timeout in milliseconds |
| `28` | `u16` | speed, low `0`, full `1`, high `2` |
| `30` | `u16` | interval |
| `32` | 8 bytes | USB setup packet |
| `40` | `u16` | split-hub address |
| `42` | `u16` | split-hub port |
| `44` | `u16` | flags |
| `46` | `u16` | reserved, used as the device generation by persistent endpoints |

A v2 extension immediately follows the primary command header, or the
maintenance command header.

| Relative offset | Type | Field | Required value or completion value |
|---|---|---|---|
| `0` | `u16` | version | `2` |
| `2` | `u16` | header size | `64` |
| `4` | `u32` | request ID | Nonzero |
| `8` | `u32` | controller epoch | Matches firmware except for `QUERY_CAPS` |
| `12` | `u32` | capabilities | Firmware capability mask on a completed validated command |

`QUERY_CAPS` requires a zero data length and accepts any nonzero request ID.
It returns the firmware epoch and `ZZUSB_CAP_BASE`. Later v2 requests require a
strictly newer request ID and the returned epoch. Firmware resets its remembered
request ID whenever it advances the epoch. A port reset, a detected detach, a
recovery reset, or a rejected late completion advances that epoch.

`ZZUSB_CAP_BASE` currently includes these capability bits:

| Bit | Identifier |
|---:|---|
| 0 | `ZZUSB_CAP_PROTOCOL_V2` |
| 1 | `ZZUSB_CAP_REQUEST_ID` |
| 2 | `ZZUSB_CAP_CONTROLLER_EPOCH` |
| 3 | `ZZUSB_CAP_VALIDATION` |
| 4 | `ZZUSB_CAP_DIAGNOSTICS` |
| 5 | `ZZUSB_CAP_PERIODIC` |
| 6 | `ZZUSB_CAP_ISO_SIMPLE` |
| 7 | `ZZUSB_CAP_ISO_REALTIME` |
| 8 | `ZZUSB_CAP_EVENT_IRQ` |
| 9 | `ZZUSB_CAP_PRECISE_ERRORS` |
| 10 | `ZZUSB_CAP_MAINTENANCE` |

## Commands, statuses, and validation

Legacy command IDs remain in the range `0x01–0x0c`. Version 2 additions occupy
`0x0d–0x16`.

| Command | Value | Firmware action |
|---|---:|---|
| `CONTROL_XFER` | `0x01` | Control transfer |
| `BULK_XFER` | `0x02` | Bulk transfer |
| `INT_XFER` | `0x03` | One interrupt transfer |
| `ISO_XFER` | `0x04` | Returns `UNSUPPORTED` |
| `RESET_PORT` | `0x05` | Reset the root port |
| `RESUME_PORT`, `SUSPEND_PORT` | `0x06`, `0x07` | Return `UNSUPPORTED` |
| `ENUMERATE` | `0x08` | Complete without payload work |
| `QUERY_DEVICE`, `SET_ADDRESS` | `0x09`, `0x0a` | Return `UNSUPPORTED` |
| `CLEAR_STALL` | `0x0b` | Clear the endpoint halt |
| `CHECK_PORT` | `0x0c` | Read root-port state |
| `QUERY_CAPS` | `0x0d` | Negotiate v2 framing and capabilities |
| `RETIRE_EP` | `0x0e` | Clear the cached data toggle for an endpoint |
| `CANCEL_EP` | `0x0f` | Stop matching periodic and ISO work |
| `DIAG_SNAPSHOT` | `0x10` | Publish the diagnostic page |
| `PERIODIC_ARM`, `PERIODIC_REAP`, `PERIODIC_STOP` | `0x11–0x13` | Persistent interrupt queue operations |
| `ISO_QUEUE`, `ISO_REAP`, `ISO_STOP` | `0x14–0x16` | ISO batch operations |

| Status | Value |
|---|---:|
| `OK` | `0x00` |
| `PENDING` | `0x01` |
| `ERROR` through `BADPARAM` | `0xff–0xf6` |
| `UNSUPPORTED`, `STALE`, `CANCELLED`, `HOSTERROR`, `BUSY`, `NOMEM` | `0xf5–0xf0` |

Firmware checks device addresses through `127`, endpoints through `15`,
direction, speed, payload extent, transfer type, interval, packet size, and
split topology. A split request uses hub address and port values from `1`
through `127`; high-speed split requests are invalid. For v2 requests without
the split flag, the split fields, multi-TT flag, and TT think-time bits must be
zero. `ZZUSB_FLAG_BULK_IN_POLL` (`0x0008`) is valid only for bulk IN.

Control transfers require endpoint zero, a packet size through `64`, matching
setup `wLength`, and matching setup direction. Periodic requests use interrupt
transfer type, a nonzero interval, and a payload through `1024` bytes. The v2
periodic commands and ISO commands use normalized powers-of-two intervals through
`32768` for high-speed endpoints.
ISO accepts high-speed endpoints or full-speed split endpoints. Low-speed ISO is
invalid.

## Persistent interrupt queues and event source

`PERIODIC_ARM` creates or reuses an endpoint keyed by epoch, device generation,
address, endpoint, direction, speed, packet size, interval, and split topology.
Firmware retains at most 16 endpoints and one unread completion per endpoint.
It pauses an endpoint with an unread completion. `PERIODIC_REAP` returns the
matching completion, and an IN endpoint rearms after a successful reap.

A zero-length IN result and `NAK` do not queue a completion. For split IN
endpoints, firmware defers the first `STALL` for 50 ms before reporting it. A
terminal error or available data queues a completion.

`AMIGA_INTERRUPT_USB` is bit 4 of the shared Amiga interrupt mask. Firmware
asserts it while a periodic completion is ready or an ISO batch is ready, then
clears it when neither condition remains. `ZZUSB_EVENT_ACK_GATE` (`1 << 3`) and
`ZZUSB_EVENT_ACK_USB` (`1 << 8`) acknowledge that source together.

## ISO batches

ISO operations require v2 framing. Firmware retains at most eight batches,
with at most 32 packets and `15840` data bytes in each batch. The primary
mailbox can contain a maximum-size batch: 32 bytes of batch header, 32
16-byte packet entries, and 15840 bytes of data. The maintenance mailbox has
its separate 3968-byte payload limit.

`ISO_QUEUE` receives a 32-byte batch header followed by 16-byte packet entries.
OUT batches append data after the entries; IN batches contain metadata only.
`ISO_REAP` returns the same metadata and appends data for IN batches. Packet
offsets describe the requested layout, so later offsets stay unchanged after a
short packet.

| Header offset | Type | Field |
|---|---|---|
| `0` | `u32` | magic `0x5a49534f` (`ZISO`) |
| `4` | `u16` | version `1` |
| `6` | `u16` | flags, bit 0 is `ZZUSB_ISO_FLAG_ASAP` |
| `8` | `u32` | nonzero batch ID |
| `12` | `u16` | starting frame, modulo 2048 |
| `14` | `u16` | packet count, `1–32` |
| `16` | `u32` | total data extent |
| `20` | `u8` | starting microframe, `0–7` |
| `21` | 11 bytes | reserved |

Each packet entry stores requested length at offset `0`, actual length at `2`,
status at `4`, completed frame at `6`, data offset at `8`, and completed
microframe at `12`. The packet status values are `OK` `0`, `PENDING` `1`,
`SHORT` `2`, `MISSED` `3`, `UNDERRUN` `4`, `OVERRUN` `5`, `CANCELLED` `6`,
`OFFLINE` `7`, transaction error `8`, and `BABBLE` `9`.

An explicit schedule uses the requested frame and microframe. An ASAP batch
starts at least four frames ahead of the current frame and follows a matching
active batch when necessary. Firmware reports `BUSY` for an overlapping
schedule or when all batch slots are occupied. `ISO_STOP`, controller reset,
and epoch invalidation retire matching descriptors before their batch storage is
reused.

## Diagnostic snapshot

`ZZUSB_CAP_DIAGNOSTICS` exposes the final 4096 bytes of the aperture as a
big-endian snapshot. Firmware publishes it when a v2 primary request arrives,
after v2 command processing, and for `DIAG_SNAPSHOT`. Version 1 staging overlaps
this page, so a legacy peer must not interpret it as diagnostics.

| Offset | Type | Field |
|---|---|---|
| `0` | `u32` | magic `0x5a554447` |
| `4` | `u32` | generation, odd during publication |
| `8` | `u16` | snapshot version `1` |
| `10` | `u16` | header size `128` |
| `12` | `u32` | page size `4096` |
| `16` | `u32` | capability mask |
| `20` | `u32` | controller epoch |
| `24` | `u32` | last request ID |
| `28` | `u32` | next physical event slot |
| `32` | `u32` | retained event count |
| `36` | `u32` | lost event count |
| `40` | `u32` | queue state |
| `44` | `u32` | controller and schedule bits |
| `48` | 16 × `u32` | counters |
| `128` | 64 × 32 bytes | event slots |

Events use physical ring slots. The next-slot field is the oldest retained slot
after the 64-entry ring has wrapped. Each event sequence identifies its entry.
Readers copy the page only when the generation is even and unchanged before and
after the copy. Firmware writes an odd generation, flushes the page body, then
writes and flushes the final even generation.
