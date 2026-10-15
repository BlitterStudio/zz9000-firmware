# ZZ9000 SDK v2

The ZZ9000 SDK v2 supplies an AmigaOS interface to the firmware service ABI. It
builds `zz9k.library`, `mpega.library`, command-line tools, DataTypes classes,
public headers, examples, and AmigaGuide manuals. Applications use the library
and the headers to submit requests to services provided by matching ZZ9000
firmware.

This is the BlitterStudio continuation of the original MNT ZZ9000 ARM SDK. It
is maintained independently and is not affiliated with, endorsed by, or
supported by MNT Research GmbH. Direct hardware questions belong with MNT
Research GmbH. SDK issues and fork-specific discussion belong here.

| Path | Contents |
|---|---|
| `include/zz9k/` | Public SDK v2 ABI and helper headers |
| `host/` | Mailbox host implementation and request/reply headers |
| `amiga/` | `zz9k.library`, `mpega.library`, DataTypes, AmiSSL provider code, Amiga headers, and icons |
| `tools/` | AmigaOS command-line tools and ZZPlay sources |
| `examples/` | AmigaOS library, JPEG streaming, typed decode, and crypto examples |
| `docs/` | SDK and component reference material |
| `scripts/` | m68k build, package, descriptor-generation, and icon-generation scripts |
| `tests/` | Host-side tests and test fixtures |

## Build an AmigaOS 3 package

Run the packaging script from `sdk/`. It invokes the m68k build unless you pass
its skip-build option, then writes the package to `build/package/amigaos3`.

On Windows, install Docker and Python, then run:

```powershell
cd sdk
powershell -ExecutionPolicy Bypass -File scripts\package-m68k-amigaos.ps1
```

On a POSIX host, install Docker, Python 3, and `realpath`, then run:

```sh
cd sdk
./scripts/package-m68k-amigaos.sh
```

Both build scripts use the `sacredbanana/amiga-compiler:m68k-amigaos` Docker
image by default. Supply `-Image` to the PowerShell build script or set `IMAGE`
when running the shell build script to use another image.

Run the build step separately when you need only the generated binaries:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-m68k-amigaos.ps1
```

```sh
./scripts/build-m68k-amigaos.sh
```

The package scripts accept these output controls:

| Shell | Skip the build | Choose an output directory |
|---|---|---|
| PowerShell | `-SkipBuild` | `-OutputDir <path>` |
| POSIX shell | `--skip-build` | `--output-dir <path>` |

The chosen output directory must remain under `build/package`.

## Package contents

The generated `build/package/amigaos3` tree contains:

- `Libs/zz9k.library` and `Libs/mpega.library`
- `C/` command-line tools, including `zz9k-services`, `zz9k-jpeg`,
  `zz9k-png`, `zz9k-view`, `zzplay`, and the diagnostics
- `Classes/DataTypes/zz9k-picture.datatype` and
  `Classes/DataTypes/zz9k-sound.datatype`
- DataType descriptors in `Storage/DataTypes/`
- Public headers and FD files in `Developer/`
- AmigaGuide manuals in `Docs/` and source examples in `Examples/`
- `MANIFEST.sha256`, containing a SHA-256 digest for every other packaged file

Install the package with matching SDK-service firmware. After booting the
firmware and installing the package, run:

```text
zz9k-services --check-release
```

A successful check ends with `release check ok`. For the package-level smoke
procedure, follow [`docs/zz9k-release-smoke.md`](docs/zz9k-release-smoke.md).

## Develop with the SDK

Include `zz9k/sdk.h` for the public ABI, mailbox host interface, request and
reply types, and common helper headers. Include `proto/zz9k.h` when calling
`zz9k.library` from AmigaOS.

```c
#include <zz9k/sdk.h>
#include <proto/zz9k.h>
```

Use the focused headers when a smaller include set suits the program:

| Header | Provides |
|---|---|
| `zz9k/caps.h` | Capability and service-flag checks |
| `zz9k/surface.h` | Surface layouts plus fill and copy descriptors |
| `zz9k/image_geometry.h` | Scaling and clipped-scaling descriptors |
| `zz9k/image.h` | One-shot and streaming image decode descriptors |
| `zz9k/shared.h` | Shared-buffer byte-access helpers |
| `zz9k/audio.h` | MP3 decode and streaming audio descriptors |
| `zz9k/compression.h` | Decompression and streaming decompression descriptors |
| `zz9k/crypto.h` | Hash, HMAC, Poly1305, ChaCha20, and AEAD descriptors |
| `zz9k/text.h` | Status text for tools and applications |

Start with [`docs/zz9k-library.md`](docs/zz9k-library.md) for the library
interface. The service catalogue is in [`docs/zz9k-modules.md`](docs/zz9k-modules.md),
and Zorro II service limits are in
[`docs/zz9k-zorro2-services.md`](docs/zz9k-zorro2-services.md). The SDK also
includes documentation for [the picture DataType](docs/zz9k-picture-datatype.md),
[the sound DataType](docs/zz9k-sound-datatype.md),
[ZZPlay](docs/zzplay.md), and [the AmiSSL provider](docs/zz9k-amissl-provider.md).

## Compatibility

The SDK depends on matching ZZ9000 SDK-service firmware. Check service
capabilities before using an optional operation, because the available services
and their limits depend on the firmware and hardware profile. Current
generation-2 Zorro II profiles share a 16 KiB host-window heap, so concurrent
clients can contend for it.

Firmware v2.2.0 is the older baseline for the service ABI. Use matched current
firmware, SDK payloads, and drivers for acknowledged Zorro II aperture layouts
and host-window clients, 4 MB Zorro II PIP allocation, `ZZ9000.CFG` queries,
and the newest service capability flags.

## History

| Period | SDK direction |
|---|---|
| Original MNT SDK | Demonstrated launching code on the ZZ9000 ARM processor |
| This fork | Provides the SDK v2 service ABI, AmigaOS libraries, tools, examples, and packaging |

The original SDK is available at
<https://source.mnt.re/amiga/zz9000-sdk>.

## Third-party code

`tools/lha-unix/` contains the LHa for UNIX decoder subset from
`jca02266/lha`, version `1.14i-ac20220213`. It retains the original LHa for
UNIX redistribution terms.

## License

SDK v2, including the firmware-integrated service ABI, `zz9k.library`,
AmigaOS tools, public headers, documentation, examples, and packaging work in
this fork, is copyright (C) 2024-2026 Dimitris Panokostas / BlitterStudio.

Unless a file has its own notice or a third-party license, it is distributed
under [GPL-3.0-or-later](https://spdx.org/licenses/GPL-3.0-or-later.html).
