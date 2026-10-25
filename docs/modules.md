# Firmware modules

Optional MPEG-2, WebM and WebP decoding resides in fixed slots in the 16 MiB
module arena at `0x09000000`. Modules are FAT 8.3 files in `0:/ZZ9K/`:
`MPEG2.ZZM`, `WEBM.ZZM`, and `WEBP.ZZM`.

A `.ZZM` file begins with the 128-byte `zzm_header_t` in
`ZZ9000_proto.sdk/ZZ9000OS/src/zzm_format.h`. It identifies a fixed slot,
contains the image and BSS sizes, init address, ABI version, and SHA-256 of
the following image bytes. The loader rejects a malformed, truncated,
wrong-ABI, wrong-slot, or wrong-hash image before it can run.

Slots are defined only in `zzm_manifest.c`. Adding a module means assigning a
new non-overlapping slot there, linking it with `util/zzm_module.ld`, packing
it with the host packer, and adding its file to the release bundle. Modules
must use the services table passed to `zzm_module_init`; they do not own a
separate heap.

At boot firmware loads modules before accepting mailbox work. A missing or
rejected module is recorded in `ZZ9K_OP_MODULE_STATUS` and emitted on UART.
The corresponding optional operation must return the module-missing or
module-mismatch status rather than crash.
