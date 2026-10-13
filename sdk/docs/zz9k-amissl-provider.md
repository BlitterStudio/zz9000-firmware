# ZZ9000 AmiSSL Provider

The ZZ9000 AmiSSL provider is a built-in OpenSSL 3 provider for applications
that use AmiSSL 5.x. Add its sources to an Amiga application, register it after
AmiSSL initialization, and normal `EVP_*` and `SSL_*` calls prefer the board
when the firmware advertises the required crypto service. Algorithms or
operations that the provider does not advertise use AmiSSL's `default`
provider.

The provider is application-local. An application must call
`zz9k_amissl_register()` for its own AmiSSL library context. The separate
[Path A integration](../integration/amissl/README.md) builds the provider into
`amissl.library` for applications that cannot be changed.

[zz9k-crypto-acceleration.md](zz9k-crypto-acceleration.md) records the
operation timings and measurement method.

## Available operations

The provider opens the board once during initialization and queries
`ZZ9K_SERVICE_CRYPTO`. It advertises an algorithm only when the corresponding
firmware capability is available. If opening the board, querying the service,
or allocating the initial 32-byte `ZZ9K_ALLOC_HOST_WINDOW` buffer fails, the
provider loads without hardware algorithms and AmiSSL uses `default`.

| OpenSSL operation | Board operation | Capability gate | Fallback |
| --- | --- | --- | --- |
| X25519 `KEYMGMT` and `KEYEXCH` | X25519 key generation and agreement | `ZZ9K_SERVICE_FLAG_CRYPTO_X25519` | Fails after the provider has advertised X25519 if the hardware operation fails. |
| EC `KEYMGMT` and ECDH `KEYEXCH` | P-256 key generation and ECDH agreement | EC is advertised with `ZZ9K_SERVICE_FLAG_CRYPTO_ECDSA_P256` or `ZZ9K_SERVICE_FLAG_CRYPTO_P256_KEYGEN`; ECDH itself requires `ZZ9K_SERVICE_FLAG_CRYPTO_P256`. | Uses a default-provider shadow key when P-256 cannot be run on the board. |
| ECDSA `SIGNATURE` | P-256 SHA-256 verification | `ZZ9K_SERVICE_FLAG_CRYPTO_ECDSA_P256` | Uses the default provider after an offload miss. |
| RSA `KEYMGMT` and `SIGNATURE` | PKCS#1 v1.5/SHA-256 verification for keys up to 4096 bits | `ZZ9K_SERVICE_FLAG_CRYPTO_RSA_2048` | Uses the default provider for RSA-PSS, signing, unsupported sizes or digests, and offload misses. |
| AES-128-GCM and AES-256-GCM | AEAD record encryption and decryption | `ZZ9K_SERVICE_FLAG_CRYPTO_AES_GCM` | Uses the default provider for small records or an offload miss. |
| ChaCha20-Poly1305 | AEAD record encryption and decryption | Available whenever the crypto service is open | Uses the default provider for small records or an offload miss. |

The EC and RSA key-management implementations create a shadow `EVP_PKEY` with
`provider=default`. That preserves operations outside the accelerated cases,
including non-P-256 EC curves, EC and RSA key generation, ECDSA signing,
RSA-PSS, and RSA modes other than PKCS#1 v1.5/SHA-256 verification. Delegated
fetches explicitly request `provider=default` so they cannot select this
provider again.

The AEAD implementations support the TLS 1.2 controls
`EVP_CTRL_AEAD_SET_IV_FIXED` and `EVP_CTRL_AEAD_TLS1_AAD`. They send records of
at least 2048 bytes to the board by default. Smaller records stay in the
default provider. `ENV:ZZ9K_AEAD_OFFLOAD_MIN` overrides that threshold in
bytes; a negative value is treated as zero. The implementation uses the
default provider after an allocation, mailbox, timeout, or authentication
failure. A failed authentication tag therefore remains a failed decrypt.

## Add it to an application

Build these sources with the application:

| Source | Purpose |
| --- | --- |
| `amiga/provider/zz9k_provider.c` | Provider initialization, algorithm selection, and TLS group capabilities. |
| `amiga/provider/zz9k_algorithms.c` | OpenSSL algorithm tables. |
| `amiga/provider/zz9k_x25519.c` | X25519 key management and key exchange. |
| `amiga/provider/zz9k_aead.c` | AES-GCM and ChaCha20-Poly1305 cipher implementations. |
| `amiga/provider/zz9k_ecdsa.c` | EC key management, P-256 ECDH, and ECDSA verification. |
| `amiga/provider/zz9k_rsa.c` | RSA key management and signature verification. |
| `amiga/provider/zz9k_offload.c` | Amiga-only SDK bridge and shared-buffer management. |
| `amiga/provider/zz9k_amissl.c` | AmiSSL registration helpers. |
| `tools/zz9k-crypto-soft.c` | Portable reference used by host provider tests. |

Define `ZZ9K_PROVIDER_OFFLOAD` for the Amiga build. Compile source files that
call OpenSSL through AmiSSL with `-include proto/amissl.h`; this redirects their
OpenSSL calls to AmiSSL. `zz9k_offload.c`, `host/src/zz9k_host.c`, and
`tools/zz9k-crypto-soft.c` do not call OpenSSL and do not need that forced
include.

Open AmiSSL with `AmiSSL_UsesOpenSSLStructs` set to `TRUE`, and obtain both
`AmiSSLBase` and `AmiSSLExtBase`. The extension base provides the
`OSSL_PROVIDER_*` interface. The bundled self-test at
`amiga/provider/zz9k_amissl_selftest.c` shows the required AmiSSL setup and
teardown.

After AmiSSL has initialized, register the provider before the first crypto
operation. Unregister it before `CloseAmiSSL()`.

```c
#include <proto/amisslmaster.h>
#include "zz9k_amissl.h"

struct Library *AmiSSLBase, *AmiSSLExtBase;   /* externed by proto/amissl.h */

OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
               AmiSSL_UsesOpenSSLStructs, TRUE,
               AmiSSL_GetAmiSSLBase,    (ULONG)&AmiSSLBase,
               AmiSSL_GetAmiSSLExtBase, (ULONG)&AmiSSLExtBase,
               AmiSSL_SocketBase,       (ULONG)SocketBase,
               AmiSSL_ErrNoPtr,         (ULONG)&errno,
               TAG_DONE);

if (!zz9k_amissl_register()) {
    /* Continue with AmiSSL software crypto or stop initialization. */
}

/* Use AmiSSL normally. */

zz9k_amissl_unregister();
CloseAmiSSL();
```

`zz9k_amissl_register()` adds the built-in provider, loads `default` for the
fallback path, and calls:

```c
EVP_set_default_properties(NULL, "?provider=zz9000");
```

This replaces an application default property query. If an application already
sets one, combine its clauses with `?provider=zz9000` and register the provider
directly instead of using the helper. `zz9k_amissl_unregister()` clears the
property query and unloads the provider handles. It is safe to call when
registration did not complete.

## Check an Amiga build

Run `zz9k_amissl_selftest.c` on the target. It registers the provider, then:

1. Checks an X25519 agreement against a key exchange pinned to
   `provider=default`.
2. Encrypts AES-256-GCM and ChaCha20-Poly1305 data through the selected
   provider and decrypts it through `provider=default`.
3. Runs the RFC 8439 ChaCha20-Poly1305 known-answer test through each provider
   and checks cross-provider ChaCha round trips.

The program prints the serving provider for fetched operations. A line naming
`zz9000` shows that the fetch selected the provider. It does not by itself show
that a particular AEAD record reached the board: records below the configured
threshold use `default` even when the cipher was fetched from `zz9000`.

Inspect a fetched operation when checking a particular path:

```c
EVP_CIPHER *cipher = EVP_CIPHER_fetch(NULL, "AES-256-GCM", NULL);
const OSSL_PROVIDER *provider = EVP_CIPHER_get0_provider(cipher);
printf("AES-256-GCM served by: %s\n", OSSL_PROVIDER_get0_name(provider));
```

Equivalent provider accessors include `EVP_PKEY_get0_provider`,
`EVP_SIGNATURE_get0_provider`, and `EVP_KEYEXCH_get0_provider`.

The optional host test `tests/provider_tls_handshake_test.c` builds when CMake
finds OpenSSL 3 with `OpenSSL::SSL`. It drives TLS 1.2 and TLS 1.3 handshakes
over memory BIOs, including X25519 and P-256 groups, ECDSA and RSA certificates,
AES-GCM, ChaCha20-Poly1305, and a delegated P-384 certificate case. It verifies
provider behavior on the host; it does not exercise Amiga hardware.

## Limits and diagnostics

`ENV:ZZ9K_DISABLE_OFFLOAD` prevents the provider from opening the board. The
provider still registers, but exposes no hardware algorithms and AmiSSL uses
`default`.

`ENV:ZZ9K_OFFLOAD_TIMEOUT_MS` sets the board polling timeout. Its default is
250 ms. Empty, invalid, or non-positive values use the default.

The offload context keeps shared scratch buffers and expands them in exact
16-byte-aligned capacities. Each allocation uses `ZZ9K_ALLOC_HOST_WINDOW`.
On Zorro II, that heap is shared by CPU-visible SDK clients, so concurrent
image, archive, audio, or DataType work can prevent a later allocation. AEAD,
P-256, ECDSA, and RSA operations use their documented default-provider
fallbacks in that case. X25519 reports an operation failure after it has been
advertised.

On Zorro II, current generation-2 2 MiB and 4 MiB layouts expose a 16 KiB host window. Generation-1 layouts exposed 64 KiB; the generation-2 reduction reserves space for the Z2 audio direct-ring grant. See [Zorro II service support](zz9k-zorro2-services.md) for the layout and release-set requirements.

## Status

The provider sources and host tests are in this repository. The hardware
self-test checks X25519 and AEAD interoperability on an Amiga target. Use the
provider name reported by the self-test or OpenSSL accessors when recording a
hardware qualification result.
