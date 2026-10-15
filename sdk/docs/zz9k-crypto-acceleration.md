# ZZ9000 crypto and TLS acceleration

Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio

The ZZ9000 firmware provides mailbox crypto operations for an Amiga host. It
runs the supported primitives on the Zynq Cortex-A9 and exchanges data through
shared buffers. The SDK exposes the service directly and the AmiSSL provider
uses it for TLS where the installed firmware advertises the required feature.

The current firmware service is version `0x00020000` and occupies opcodes
`0x0800` through `0x0804`. Its implementations use the firmware crypto code and
BearSSL routines for AES-GCM, P-256, ECDSA, and RSA verification.

## Available operations

Query `ZZ9K_SERVICE_CRYPTO` before submitting work. The service descriptor
advertises `ZZ9K_CAP_CRYPTO`; its flags identify optional primitives that a
caller must gate individually.

| Operation | SDK entry point | Firmware support | Service flag when applicable |
|---|---|---|---|
| Hash and HMAC | `zz9k_crypto_hash()` | SHA-1, SHA-256, HMAC-SHA-1, HMAC-SHA-256 | None |
| MAC | `zz9k_crypto_hash()` | Poly1305 with a 32-byte key | None |
| Stream cipher | `zz9k_crypto_stream()` | ChaCha20 | None |
| AEAD | `zz9k_crypto_aead()` | ChaCha20-Poly1305, AES-128-GCM, AES-256-GCM | `ZZ9K_SERVICE_FLAG_CRYPTO_AES_GCM` for AES-GCM |
| Key exchange | `zz9k_crypto_kx()` | X25519, P-256 ECDH, P-256 public-key generation | `ZZ9K_SERVICE_FLAG_CRYPTO_X25519`, `ZZ9K_SERVICE_FLAG_CRYPTO_P256`, `ZZ9K_SERVICE_FLAG_CRYPTO_P256_KEYGEN` |
| Signature verification | `zz9k_crypto_verify()` | ECDSA P-256/SHA-256 and RSA PKCS#1 v1.5/SHA-256 | `ZZ9K_SERVICE_FLAG_CRYPTO_ECDSA_P256`, `ZZ9K_SERVICE_FLAG_CRYPTO_RSA_2048` |

`ZZ9K_SERVICE_FLAG_CRYPTO_RSA_2048` is the compatibility flag name. The
firmware verification path accepts RSA moduli and signatures up to 4096 bits.
It expects a SHA-256 digest, an RSA public key encoded as modulus followed by a
four-byte big-endian exponent, and a signature as wide as the modulus.

The SDK ABI also names SHA-384, SHA-512, and BLAKE2S hash algorithms. Current
firmware returns `ZZ9K_STATUS_UNSUPPORTED` for them. Do not use the ABI enum as
a support list.

## Calling the service

Use the descriptor builders in `include/zz9k/crypto.h` and the transport API
in `host/include/zz9k/host.h`. Amiga callers use the equivalent `ZZ9K*` library
vectors from `amiga/include/clib/zz9k_protos.h`.

1. Open the board, then call `zz9k_query_service()` with
   `ZZ9K_SERVICE_CRYPTO`. Check the capability and the operation-specific
   service flags.
2. Allocate shared buffers with `zz9k_alloc_shared()`. Fill the input, key,
   nonce, AAD, and destination buffers, then build a descriptor that references
   their handles and offsets.
3. Submit the descriptor with `zz9k_crypto_hash()`, `zz9k_crypto_stream()`,
   `zz9k_crypto_aead()`, `zz9k_crypto_kx()`, or `zz9k_crypto_verify()`.
   Read `ZZ9KCryptoResult` for byte count, algorithm, and flags. Verification
   returns its boolean result through `valid`.
4. Free every shared buffer and close the context when it is no longer needed.

For independent hash, stream, or AEAD records, use
`zz9k_crypto_hash_batch()`, `zz9k_crypto_stream_batch()`, or
`zz9k_crypto_aead_batch()` instead of issuing synchronous requests one at a
time. The batch APIs take `count`, `max_in_flight`, and `timeout_ticks`.
`ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT` is `16U` in the host API.

### Descriptor rules

| Operation | Inputs and output |
|---|---|
| ChaCha20 | 32-byte key, 12-byte nonce, and a caller-supplied counter. Source and destination are the same length. |
| ChaCha20-Poly1305 | 32-byte key and 12-byte nonce. Encryption appends a 16-byte tag; decryption reads the appended tag. |
| AES-GCM | 16-byte AES-128 or 32-byte AES-256 key and a 12-byte nonce. Encryption appends a 16-byte tag; decryption reads the appended tag. `zz9k_crypto_build_aes_gcm_desc()` encodes the selected AES algorithm in the AEAD flags. |
| X25519 | 32-byte scalar, 32-byte peer point, and 32-byte shared-secret output. The firmware rejects an all-zero shared secret. |
| P-256 ECDH | 32-byte private scalar, 65-byte uncompressed peer point (`0x04 || X || Y`), and 32-byte shared-X-coordinate output. |
| P-256 key generation | Set `ZZ9K_CRYPTO_KX_FLAG_KEYGEN`; the 32-byte scalar produces a 65-byte uncompressed public point. Gate this operation on `ZZ9K_SERVICE_FLAG_CRYPTO_P256_KEYGEN`. |
| ECDSA P-256 verification | A 32-byte SHA-256 digest, a 65-byte uncompressed public point, and a raw 64-byte `r || s` signature. |

The AEAD descriptor carries its algorithm in bits 8 through 15 of `flags`.
A zero algorithm field retains the legacy ChaCha20-Poly1305 default. Set only
`ZZ9K_CRYPTO_AEAD_FLAG_DECRYPT` in addition to the algorithm field.

## Performance results

These are recorded hardware measurements, not source-derived results. They
were collected on a 68060 with an EClock of 709 kHz. The older measurements
predate the synchronous wait fix, so retain them for comparison rather than as
current latency guidance.

### Mailbox and ChaCha20-Poly1305 records

The original `zz9k-cryptobench` run used 16 iterations per size.

| Record | Software KiB/s | Synchronous offload KiB/s | Offload time per record |
|---|---:|---:|---:|
| 64 B | 96 | 9 | 6.94 ms |
| 256 B | 146 | 39 | 6.41 ms |
| 1 KB | 161 | 158 | 6.33 ms |
| 4 KB | 158 | 628 | 6.37 ms |
| 16 KB | 162 | 2539 | 6.30 ms |

The recorded synchronous break-even was 2 KB. The same record lists `SDK ping`
at 161 calls/s, or 6.1 ms per call, and `SDK ping pipe` at 11,193 calls/s.
Pipelined ChaCha20-Poly1305 at 16 KB reached about 9,539 KiB/s.

A later probe bypassed the old wait discipline and measured a 225–339 µs
round-trip range, with a 279 µs average. With the completion IRQ explicitly
armed, the following figures were recorded on the same 68060.

| Metric | Before wait fix | After wait fix |
|---|---:|---:|
| `SDK ping` | 6.1 ms/call, 161 calls/s | 0.28 ms/call, 3342 calls/s |
| ChaCha20-Poly1305 synchronous break-even | 2 KB | 64 B |
| ChaCha20-Poly1305 synchronous, 16 KB | 2539 KiB/s | 8550 KiB/s |
| AES-128-GCM synchronous, 16 KB | 2487 KiB/s | 3588 KiB/s |
| AES-128-GCM batch, 16 KB | 3584 KiB/s | 3969 KiB/s |
| X25519 | 6.36 ms/op, 124x | 3.15 ms/op, 250x |
| P-256 ECDH | 12.25 ms/op | 8.91 ms/op |
| P-256 key generation | Not recorded | 6.26 ms/op |
| ECDSA P-256 verification | 18.78 ms/op | 15.90 ms/op |
| RSA-2048 verification | 11.29 ms/op | 6.51 ms/op |

The post-fix record also reports 64-byte batched throughput of 578 KiB/s,
compared with 162 KiB/s for the corresponding synchronous result.

### AES-128-GCM records before the wait fix

The original AES sweep used 16 iterations per size and a batch depth of 16.

| Record | Software KiB/s | Synchronous KiB/s | Batched KiB/s |
|---|---:|---:|---:|
| 64 B | 7 | 9 | 619 |
| 256 B | 9 | 39 | 510 |
| 1 KB | 10 | 157 | 1989 |
| 4 KB | 10 | 635 | 3132 |
| 16 KB | 10 | 2487 | 3584 |

### Handshake primitives before the wait fix

The `zz9k-cryptobench` asymmetric sections used 16 iterations. These results
compare the SDK software fallback with synchronous firmware offload.

| Primitive | Software m68k | Offload | Recorded speedup |
|---|---:|---:|---:|
| X25519 | 790.75 ms/op | 6.36 ms/op | 124x |
| P-256 ECDH | 9593 ms/op | 12.25 ms/op | 783x |
| ECDSA P-256 verification | 19123 ms/op | 18.78 ms/op | 1018x |
| RSA-2048 PKCS#1 verification, `e=65537` | 1379 ms/op | 11.29 ms/op | 122x |

## AmiSSL provider results

The built-in AmiSSL provider is documented in
[zz9k-amissl-provider.md](zz9k-amissl-provider.md). The recorded 68060
application tests used wall-clock seconds, where lower is better. They compare
the drop-in library with offload enabled, that same library with
`ENV:ZZ9K_DISABLE_OFFLOAD` set, and the Aminet AmiSSL `os3-68060` build.

| Workload | Drop-in, offload enabled | Drop-in, offload disabled | Stock AmiSSL `os3-68060` |
|---|---:|---:|---:|
| IBrowse, `https://www.google.com/` | 12.95 | 15.36 | 15.18 |
| DOpus, SFTP transfer | 7.00 | 7.06 | 7.84 |

A separate recorded run used an `os3-68020` library on a 68060 and measured an
approximately 2.5x crypto slowdown. Use the library built for the target CPU.

## Status and history

The firmware implements the service, including AES-GCM, X25519, P-256 ECDH and
key generation, ECDSA P-256 verification, and RSA PKCS#1 SHA-256 verification.
The service catalog advertises the corresponding crypto feature flags. The
optional FPGA AES phase remains unimplemented in this service.

| Phase | Result |
|---|---|
| 0 | Benchmarking and the record-size sweep completed. |
| 1 | X25519 completed. |
| 2 | P-256 ECDH, P-256 ECDSA verification, and RSA verification completed. |
| 3 | AES-GCM completed. |
| 4 | The AmiSSL provider is integrated; the original record notes that end-to-end hardware remeasurement of the P-256 and certificate-verification browsing path was pending. |
| 5 | Optional FPGA AES core remains a future item. |

## Related source

| Path | Contents |
|---|---|
| `include/zz9k/crypto.h` | Crypto descriptor builders and size constants. |
| `include/zz9k/abi.h` | Crypto opcodes, descriptors, algorithms, and service flags. |
| `host/include/zz9k/host.h` | Host transport calls and `ZZ9K_CRYPTO_BATCH_MAX_IN_FLIGHT`. |
| `amiga/include/clib/zz9k_protos.h` | Amiga library vectors. |
| `tools/zz9k-cryptobench.c` | Record and handshake benchmark program. |
| `tools/zz9k-handshake-model.h` | Parameterized offload cost and speedup calculator. |
| `tests/crypto_soft_test.c` | Software crypto reference tests. |
| `tests/cryptobench_logic_test.c` | Benchmark helper tests. |
| `amiga/provider/` | AmiSSL provider and its board offload backend. |
