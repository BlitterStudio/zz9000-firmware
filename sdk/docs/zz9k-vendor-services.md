# ZZ9000 Vendor Service Conventions

The service ID space below `0x8000` belongs to the SDK: each value is an
opcode *family* base owned by a firmware subsystem (`ZZ9K_SERVICE_CORE`
`0x0000`, `…MEMORY` `0x0100`, `…SURFACE` `0x0200`, … `…MODULE` `0x0a00`; see
`include/zz9k/abi.h`). `ZZ9K_SERVICE_VENDOR = 0x8000` opens the other half of
the range for services the SDK does not define — loadable modules and
third-party/vendor extensions (the JEDI renderer takes `0x8100`).

These are the rules for living in that space. The registry
(`include/zz9k/service_registry.h`) already *enforces* the hard ones at
registration; this document is the intent behind them and the conventions the
enforcement cannot express.

## ID and opcode-range allocation

A service is described by a `ZZ9KModuleServiceDesc` (`include/zz9k/module.h`)
whose `service_id` doubles as its opcode-family base:

- **Allocate vendor `service_id`s on `0x0100` boundaries** — `0x8100`,
  `0x8200`, … — exactly like the SDK families below `0x8000`. This gives each
  service up to 256 opcodes (`opcode_base = service_id`, `opcode_count <=
  0x100`) and keeps opcodes readable (`0x81xx` is unambiguously "JEDI op xx").
- **`opcode_base` MUST equal `service_id`** and `opcode_count` MUST NOT cross
  into the next `0x0100` slot. The registry rejects a manifest whose services
  have duplicate IDs or overlapping opcode ranges
  (`zz9k_service_registry_services_conflict` → `ZZ9K_STATUS_BAD_REQUEST`), so a
  collision fails loudly at load time rather than misdispatching — but that is
  a backstop, not a substitute for picking a free slot.
- **Record every assigned vendor ID** in this file's table below, the way the
  SDK families are fixed in `abi.h`. Runtime rejection protects a single board;
  the written registry prevents two vendors from ever shipping `0x8100`.

### Assigned vendor services

| `service_id` | Owner | Notes |
|--------------|-------|-------|
| `0x8100`     | JEDI software renderer (TFE port) | ships as a loadable module |

## Discovery — `QUERY_SERVICE`

Vendor services are discovered exactly like firmware services: a
`ZZ9K_OP_QUERY_SERVICE` for the `service_id` returns a `ZZ9KServiceInfoPayload`
(`zz9k_service_registry_make_info_payload`) carrying `version`,
`capability_bits`, `flags`, `opcode_base`, `opcode_count`,
`max_inline_payload`, and the 20-byte `name`. A service that is not registered
is simply not found — a consumer MUST treat "not found" as "feature absent"
and fall back, never assume a fixed vendor ID is present.

`max_inline_payload` is the largest request payload the service accepts inline
in the mailbox entry; a consumer with more data uses a shared buffer and MUST
NOT exceed the advertised inline size.

## Versioning

`ZZ9KModuleServiceDesc.version` is the *service's own* version, independent of
both the module's `module_version` and the SDK wire-ABI version. Convention:

- Start at `1`, increase monotonically.
- Bump on any change that alters an existing opcode's request/reply layout or
  semantics. Adding a new opcode at the end of the range (raising
  `opcode_count`) does **not** require a bump — a consumer that does not know
  the new opcode simply never calls it.
- A consumer reads `version` from `QUERY_SERVICE` and gates version-dependent
  opcodes on it.

## Flags

`ZZ9KModuleServiceDesc.flags` carries the generic `ZZ9K_SERVICE_FLAG_*` bits
(`abi.h`): a vendor/module service MUST set `ZZ9K_SERVICE_FLAG_MODULE` and MUST
NOT set `ZZ9K_SERVICE_FLAG_FIRMWARE` (it is not firmware-provided). `…_ASYNC`
and `…_ZERO_COPY` describe the transport contract as usual. Bits `16` and above
are a **per-service namespace**: their meaning is private to the `service_id`
and may reuse the same bit another service uses for something unrelated (the
SDK already does this — `…_IMAGE_JPEG_BASELINE` and `…_AUDIO_MP3_DECODE` both
sit at bit 16). A vendor service defines and documents its own upper-bit flags;
consumers interpret them only after matching the `service_id`.

## Capabilities — NOT the global `ZZ9K_CAP_*` bits

The global `ZZ9K_CAP_*` bits returned by `ZZ9K_OP_QUERY_CAPS` describe
board-wide firmware families (`ZZ9K_CAP_CRYPTO`, `ZZ9K_CAP_IMAGE_DECODE`, …)
and are owned by the SDK. **A vendor service MUST NOT claim or depend on a
global `ZZ9K_CAP_*` bit** — those bits mean "the firmware family is present",
not "this module is loaded".

Instead a vendor service advertises what it can do through its own
`capability_bits` field, returned by `QUERY_SERVICE`. Those bits are a
per-`service_id` namespace defined by the service's published spec; a consumer
discovers a vendor capability by querying the service and interpreting
`capability_bits` against that spec, exactly as it interprets the upper flag
bits. This keeps the scarce global capability word free for firmware families
and lets any number of vendor services describe themselves without central bit
allocation.

## Checklist for a new vendor service

1. Pick the next free `0x8N00` `service_id`; add it to the table above.
2. Set `opcode_base = service_id`, `opcode_count <= 0x100`,
   `max_inline_payload` to the real inline ceiling.
3. Set `ZZ9K_SERVICE_FLAG_MODULE` (plus `…_ASYNC`/`…_ZERO_COPY` if applicable);
   define any per-service flags in bits `>= 16` and document them.
4. Start `version` at `1`; define the per-service `capability_bits` in a
   published spec — never a global `ZZ9K_CAP_*` bit.
5. Register through a `ZZ9KModuleManifest`; rely on the registry to reject
   overlaps, but do not depend on it to choose your slot.

See `docs/zz9k-modules.md` for the manifest shape and registration flow.
