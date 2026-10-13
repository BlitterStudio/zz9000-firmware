# ZZ9000 Vendor Service Conventions

`ZZ9K_SERVICE_VENDOR` (`0x8000`) marks the vendor-service namespace in the
SDK ABI. A vendor service is described by `ZZ9KModuleServiceDesc` metadata and
can be discovered with `ZZ9K_OP_QUERY_SERVICE` once its runtime registers it.

Current firmware uses the static `sdk_services` catalog for service discovery.
It has no vendor-service entry and does not dynamically load modules. A query
for an unregistered service returns `ZZ9K_STATUS_NOT_FOUND`.

## Define a vendor service

Use `ZZ9KModuleManifest` and `ZZ9KModuleServiceDesc` from
`include/zz9k/module.h` to describe a service. Follow these conventions when
assigning a vendor service ID and its opcodes:

1. Choose an unused `service_id` on a `0x0100` boundary at or above `0x8000`.
   The mailbox opcode is 16 bits, so the available vendor slots run through
   `0xff00`.
2. Set `opcode_base` to `service_id` and keep `opcode_count` at `0x100` or
   less. This reserves one `0x0100` opcode family per service.
3. Start `version` at `1`. Increase it when an existing opcode changes its
   request or reply layout, or changes its meaning. Adding an opcode at the
   end of the range only changes `opcode_count`.
4. Set `ZZ9K_SERVICE_FLAG_MODULE`. Do not set
   `ZZ9K_SERVICE_FLAG_FIRMWARE` for a vendor service. Add
   `ZZ9K_SERVICE_FLAG_ASYNC` or `ZZ9K_SERVICE_FLAG_ZERO_COPY` only when the
   service implements that transport contract.
5. Define `capability_bits` and any flag bits from bit 16 upward in the
   service's published interface. They are private to that `service_id`.

Record each allocation here before publishing it, so two vendors never ship
the same ID.

| `service_id` | Owner | Notes |
| --- | --- | --- |
| `0x8100` | JEDI software renderer (TFE port) | Allocated for a loadable module. Current firmware does not register it. |

## Discover and use a service

1. Send `ZZ9K_OP_QUERY_SERVICE` with a big-endian `service_id` in
   `ZZ9KQueryServicePayload`.
2. Treat `ZZ9K_STATUS_NOT_FOUND` as an unavailable feature. Do not assume
   that a vendor ID is present.
3. On success, read `ZZ9KServiceInfoPayload`. It contains `service_id`,
   `version`, `capability_bits`, `flags`, `opcode_base`, `opcode_count`,
   `max_inline_payload`, and a 20-byte `name`.
4. Check `version`, `capability_bits`, and flags before calling optional
   opcodes. Do not use `ZZ9K_CAP_*` to advertise a vendor service: those bits
   describe SDK-defined firmware capabilities.
5. Keep an inline request payload within `max_inline_payload`. Use a shared
   buffer for larger data when the service interface provides one.

## Metadata and registry limits

`ZZ9KModuleManifest` and `ZZ9KModuleServiceDesc` are each 64 bytes. A manifest
can describe at most `ZZ9K_MODULE_MAX_SERVICES` (`16`) services. Service names
are 20-byte, NUL-terminated fields. A manifest is rejected when a service has
an `opcode_count` of zero or a `max_inline_payload` above 48 bytes, the mailbox
payload capacity.

`ZZ9KServiceRegistry` stores at most
`ZZ9K_SERVICE_REGISTRY_MAX_SERVICES` (`64`) services. It rejects duplicate
service IDs and overlapping opcode ranges with `ZZ9K_STATUS_BAD_REQUEST`.
The registry does not enforce the vendor ID range, `0x0100` alignment, or the
requirement that `opcode_base` equal `service_id`; enforce those conventions in
the service definition.

## Flags and capability bits

The shared flags in bits 0–3 are `ZZ9K_SERVICE_FLAG_FIRMWARE`,
`ZZ9K_SERVICE_FLAG_MODULE`, `ZZ9K_SERVICE_FLAG_ASYNC`, and
`ZZ9K_SERVICE_FLAG_ZERO_COPY`. SDK services reuse bits 16 and above for
service-specific meanings, including
`ZZ9K_SERVICE_FLAG_IMAGE_JPEG_BASELINE` and
`ZZ9K_SERVICE_FLAG_AUDIO_MP3_DECODE`, both at bit 16. A vendor service may do
the same because consumers interpret those bits only after identifying the
service.

`ZZ9K_OP_QUERY_CAPS` reports the global `ZZ9K_CAP_*` capability word. The SDK
maps those bits to its own service families, and `ZZ9K_SERVICE_VENDOR` has no
mapping. Report vendor capabilities through the service's `capability_bits`
field returned by `ZZ9K_OP_QUERY_SERVICE`.

See [SDK Modules](zz9k-modules.md) for the manifest layout and current module
metadata support.
