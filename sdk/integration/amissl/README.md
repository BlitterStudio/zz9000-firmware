# ZZ9000 provider built into AmiSSL

This directory builds a replacement AmiSSL library with the ZZ9000 OpenSSL 3
provider compiled into it. When an application initializes that library, the
provider is registered in its OpenSSL context and preferred with
`?provider=zz9000`. Applications continue to use AmiSSL normally. Operations
that the provider does not advertise, and hardware paths that cannot run, use
AmiSSL's default software provider.

For an application that compiles and registers the provider itself, see
[`../../docs/zz9k-amissl-provider.md`](../../docs/zz9k-amissl-provider.md).

## Integration

`amissl-zz9000.patch` changes two AmiSSL files:

| AmiSSL file | Change |
| --- | --- |
| `Makefile` | Builds the provider, SDK host, and software-reference objects from `$(ZZ9000_SDK)` and links them into `LIBOBJS`. It also adds the provider include paths while compiling `amissl_library.c`. |
| `src/amissl_library.c` | Includes `zz9k_amissl_builtin.h` and calls `zz9k_provider_register_builtin(NULL)` from `InitAmiSSLA`, after OpenSSL setup. |

The patch builds these sources from this repository's SDK tree:

- `amiga/provider/zz9k_provider.c`, `zz9k_algorithms.c`, `zz9k_x25519.c`,
  `zz9k_aead.c`, `zz9k_ecdsa.c`, `zz9k_rsa.c`, `zz9k_offload.c`, and
  `zz9k_amissl_builtin.c`
- `host/src/zz9k_host.c`
- `tools/zz9k-crypto-soft.c`

The built-in registration loads both the `zz9000` and `default` providers. It
sets `?provider=zz9000`, which prefers the board provider while allowing the
default provider to serve algorithms outside the ZZ9000 implementation.

## Build

`AMISSL_REF` identifies the AmiSSL revision that `build.sh` checks out before
it applies `amissl-zz9000.patch`. The script resets the two patched files before
applying the patch, builds AmiSSL, and copies stripped libraries to `work/out/`.
It fails if no `amissl_v*.library` is produced.

### GitHub Actions

Run **Build AmiSSL + ZZ9000 provider** from the Actions tab. The workflow builds
both `os3-68020` and `os3-68060`, then uploads
`amissl-zz9000-<platform>` artifacts. It runs on pushes that change the
provider, its SDK dependencies, this integration directory, or the workflow.

The workflow builds the `sdk/integration/amissl/Dockerfile` image, clones the
repository to the container filesystem, and runs `build.sh` with
`ZZ9000_SDK=/build/zz9000-fw/sdk`. The Dockerfile supplies the adtools m68k
compiler and utilities used by the build. Workflow artifacts place the two
libraries under `68020-40/` and `68060/`.

### Local build

Run the script from this directory with the adtools tools on `PATH`:

```sh
sh build.sh
```

The default target is `os3-68020`. Build the other target with:

```sh
OS=os3-68060 sh build.sh
```

The script accepts these environment variables:

| Variable | Default | Effect |
| --- | --- | --- |
| `ZZ9000_SDK` | SDK root, resolved as `../..` from this directory | SDK tree containing `amiga/`, `host/`, `include/`, and `tools/`. |
| `AMISSL_DIR` | unset | Existing AmiSSL checkout. When unset, the script clones AmiSSL into `work/amissl`. |
| `AMISSL_REPO` | `https://github.com/jens-maus/amissl.git` | Repository used when cloning AmiSSL. |
| `OS` | `os3-68020` | AmiSSL target: `os3-68020` or `os3-68060`. |
| `DEBUG` | empty | Flags passed to AmiSSL's `DEBUG` make variable. Empty selects the release build. |
| `WORK` | `work` | Scratch directory and output root. |

For example, build against an existing checkout and a separately located SDK:

```sh
AMISSL_DIR=/path/to/amissl \
ZZ9000_SDK=/path/to/zz9000-firmware/sdk \
OS=os3-68060 sh build.sh
```

Release output is written to `work/out/<OS>/amissl_v*.library`. The script uses
`m68k-amigaos-strip -p -R.comment` unless `STRIP` overrides it, matching the
release packaging step in its source comment. It removes the linker's stabs
and symbol hunk, which the script records as approximately 3.4 MB.

The supported local environment is Linux with an ext4 working tree. The
adtools 2017 cross compiler is a 32-bit program and can overflow on Windows
bind-mount inode values. It also rejects AmiSSL source continuations followed
by CR. Copy the source off the bind mount and use LF line endings, or use the
workflow container path above.

## Install and check

1. Choose the artifact directory that matches the host CPU: `68020-40/` or
   `68060/`.
   The drivers-repo installer selects the target for the detected CPU. When
   installing manually, choose this directory yourself.
2. Back up the matching `amissl_v*.library` in `LIBS:AmiSSL/`.
3. Copy the artifact's matching `amissl_v*.library` into `LIBS:AmiSSL/`.
4. Run a TLS application. The provider is registered during AmiSSL
   initialization, so no application-side registration call is required.

A recorded hardware comparison found that the `os3-68020` build runs on a
68060 but makes crypto about 2.5 times slower there than the 68060 build.

`build-cryptoprofile.sh` builds `zz9k-cryptoprofile`, an AmiSSL client that
times the installed library's default context against a software-only context.
It expects the `sacredbanana/amiga-compiler:m68k-amigaos` image and writes the
executable to `work/out/`. Use it after installation when a per-operation
comparison is needed.

## Offload coverage and limits

The provider opens the board once for its lifetime and queries the crypto
service. It advertises operations only when their service flags are present.
If the board, crypto service, or the initial allocation is unavailable, the
provider remains loaded and AmiSSL uses its default provider.

| Operation | Board path | Software behavior |
| --- | --- | --- |
| X25519 | Key management and key exchange when `ZZ9K_SERVICE_FLAG_CRYPTO_X25519` is present. | Once advertised, a failed board operation fails rather than falling back. |
| P-256 | ECDHE key generation and derive when the relevant P-256 capability is present. | Operations that cannot use the board delegate to the default provider. |
| ECDSA P-256 | SHA-256 certificate verification when `ZZ9K_SERVICE_FLAG_CRYPTO_ECDSA_P256` is present. | Failed offloads and unsupported EC operations delegate to the default provider. |
| RSA-2048 | PKCS#1 v1.5/SHA-256 certificate verification when `ZZ9K_SERVICE_FLAG_CRYPTO_RSA_2048` is present. | RSA-PSS, signing, key generation, and unsupported verification delegate to the default provider. |
| AES-128-GCM, AES-256-GCM | Record crypto when `ZZ9K_SERVICE_FLAG_CRYPTO_AES_GCM` is present. | Records below the configured threshold, and failed offloads, use the default provider. |
| ChaCha20-Poly1305 | Record crypto whenever the crypto service is available. | Records below the configured threshold, and failed offloads, use the default provider. |

On Zorro II, the provider first allocates a persistent 32-byte
`ZZ9K_ALLOC_HOST_WINDOW` probe. It does not advertise hardware algorithms when
that allocation fails. Later scratch buffers use exact 16-byte-aligned growth
and share the CPU-visible host-window heap with other SDK clients. Allocation
contention can therefore trigger the per-operation outcomes in the table.

The record-crypto threshold defaults to 2048 bytes. Set
`ENV:ZZ9K_AEAD_OFFLOAD_MIN` to a non-negative byte value to change it. The
provider reads that value once per process.

Set `ENV:ZZ9K_DISABLE_OFFLOAD` to disable board access without rebuilding:

```sh
setenv ZZ9K_DISABLE_OFFLOAD 1
```

The provider still loads, but its operations resolve to AmiSSL software. Use
this switch to compare the installed library with and without board access.

`ENV:ZZ9K_OFFLOAD_TIMEOUT_MS` sets the unarmed polling timeout. Its default is
250 ms; empty, invalid, and non-positive values retain the default. Use it only
when diagnosing display or bus contention.

## Licensing

The provider sources are GPL-3.0-or-later. AmiSSL and OpenSSL 3.x are
Apache-2.0. The resulting combined `amissl.library` is a GPLv3 work, so a
redistributor must meet the GPLv3 source and notice requirements.
