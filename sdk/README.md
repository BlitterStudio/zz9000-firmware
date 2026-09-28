# ZZ9000 SDK v2 - BlitterStudio fork

> **Fork notice.** This repository is an independent fork and continued
> development of the original MNT ZZ9000 ARM SDK. It is maintained by
> Dimitris Panokostas / **BlitterStudio** and is **not affiliated with,
> endorsed by, or supported by MNT Research GmbH**. The ZZ9000 hardware
> itself is designed and manufactured by MNT Research GmbH - hardware
> questions belong with them; SDK issues and fork-specific discussion
> belong here. The SDK v2 service-runtime implementation lives under
> `include/zz9k/`, `host/include/zz9k/`, `amiga/`, `tools/`, `examples/`,
> and `docs/`.
>
> Upstream (pre-fork): https://source.mnt.re/amiga/zz9000-sdk

The ZZ9000 is a graphics and ARM coprocessor card for Amiga computers equipped
with Zorro slots. It is based on a Xilinx ZYNQ Z-7020 chip that combines
7-series FPGA fabric with dual ARM Cortex-A9 CPUs clocked at 666 MHz. The
current hardware has 1 GB of DDR3 RAM and no soldered eMMC.

This repository is focused on SDK v2: a firmware-integrated mailbox/service ABI
exposed to AmigaOS through `zz9k.library`, with public helper headers under
`include/zz9k/` and Amiga examples under `examples/amiga-*`. The obsolete
fixed-address ARM launcher, standalone ARM examples, and their bare-metal
support libraries have been removed from this branch.

## What Changed Since the Original MNT SDK

The original MNT SDK mainly demonstrated how software could launch code on the
ZZ9000's ARM processor. This independent BlitterStudio fork turns that idea
into a reusable AmigaOS service platform with applications that ordinary users
can install and run. The biggest additions are:

| Improvement | What it delivers |
|---|---|
| **ZZPlay** | A Workbench media player that sends MPEG-1 video and MP3/MP2 audio to the ZZ9000's ARM cores for decoding. It supports a resizable hardware video window and a dedicated-screen presentation, while the Amiga remains responsible for the familiar user interface. |
| **Faster image handling** | Card-assisted JPEG/PNG decoding and scaling powers image-viewing tools and an optional `zz9k-picture.datatype`, allowing DataTypes-aware Amiga applications to benefit without each application learning a private hardware protocol. |
| **AmiSSL acceleration for existing software** | A drop-in AmiSSL build uses the ZZ9000 for supported TLS key exchange, signature verification and encrypted data. Compatible browsers and network tools benefit without being rewritten; anything unsupported falls back to the Amiga CPU. |
| **Audio and archive offload** | `mpega.library`, streaming audio helpers and LHA/LZH decompression services move useful work away from the classic Amiga CPU. The archive tools include verification and safe software fallback paths. |
| **Useful user and diagnostic tools** | The package includes service inspection, release checks, benchmarks, image viewers, media and audio diagnostics, palette-aware display tools, cryptography checks, and archive utilities rather than only developer examples. |
| **A real application API** | `zz9k.library` and SDK v2 provide one versioned, asynchronous interface for shared memory, images, surfaces, video, audio, compression and cryptography. Applications no longer need to start a private ARM program at a fixed address and hope no other client is using the card. |
| **Support beyond Zorro III** | Matched 2 MB and 4 MB Zorro II releases can use compact image, archive, audio and AmiSSL services safely. The 4 MB profile also allows one bounded ZZPlay PIP source; the smaller profile deliberately omits it. |
| **Reproducible packages and compatibility tests** | Docker builds, a checksum manifest, host-side tests, ABI drift checks and a hardware smoke guide make the SDK a releasable product rather than a collection of experiments. |

End users normally receive these SDK components through the
[ZZ9000 Drivers installer](https://github.com/BlitterStudio/zz9000-drivers),
so they do not need to compile this repository. Developers should start with
[`docs/zz9k-library.md`](docs/zz9k-library.md); the service catalogue is in
[`docs/zz9k-modules.md`](docs/zz9k-modules.md), and the exact Zorro II limits
are in the [Zorro II service matrix](docs/zz9k-zorro2-services.md).

The accelerated AmiSSL design, supported algorithms and measured roadmap are
documented in [`docs/zz9k-amissl-provider.md`](docs/zz9k-amissl-provider.md)
and [`docs/zz9k-crypto-acceleration.md`](docs/zz9k-crypto-acceleration.md).
Third-party or loadable services in the `0x8000+` range are covered by
[`docs/zz9k-vendor-services.md`](docs/zz9k-vendor-services.md).

## Quick Start

Build the AmigaOS 3 SDK tools and package with Docker:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-m68k-amigaos.ps1
powershell -ExecutionPolicy Bypass -File scripts\package-m68k-amigaos.ps1
```

or from a POSIX shell:

```sh
./scripts/build-m68k-amigaos.sh
./scripts/package-m68k-amigaos.sh
```

The package is written to `build/package/amigaos3`. It contains:

- `Libs/zz9k.library`
- `Libs/mpega.library` as the accelerated exact-name drop-in
- CLI tools such as `zz9k-info`, `zz9k-services`, `zz9k-bench`,
  `zz9k-surfaceops`, `zz9k-mp3`, `zz9k-mpega-smoke`, `zz9k-jpeg`, `zz9k-png`,
  `zz9k-view`, `zzplay`, `zz9k-hash`, `zz9k-chacha`, `zz9k-aead`, and
  archive/decompression tools including `zz9k-archive`
- `Classes/DataTypes/zz9k-picture.datatype` plus JPEG/PNG descriptors packaged
  inactive under `Storage/DataTypes` for explicit opt-in activation
- developer headers under `Developer/Include`
- public docs under `Docs`
- examples under `Examples`
- `MANIFEST.sha256` with SHA-256 checksums for every packaged file

SDK v2 requires matching ZZ9000 SDK-service firmware. Firmware v2.2.0 is the
older baseline for the service ABI, but current matched firmware, SDK payloads,
and drivers are expected for acknowledged Zorro 2 aperture layouts and
host-window clients, 4 MB Zorro 2 PIP allocation, `ZZ9000.CFG` query support,
and the newest service capability flags. The shipped Zorro 2 profiles both
provide one shared 64 KiB host heap; this is not a per-process allocation, so
concurrent image, archive, audio, and TLS clients can contend. After
installing the SDK package and booting that firmware, run this hardware smoke
check:

```text
zz9k-services --check-release
```

The command should end with `release check ok`.
For a broader package-level runtime pass, follow
[`docs/zz9k-release-smoke.md`](docs/zz9k-release-smoke.md).

For most application-side helper code, include `zz9k/sdk.h`; it pulls in the
stable SDK v2 ABI, host/request/reply types, and helper headers. Include
`proto/zz9k.h` as well when calling `zz9k.library` from AmigaOS.

Useful public helper headers for narrow includes:

- `zz9k/sdk.h`: application-facing umbrella header
- `zz9k/caps.h`: capability and service-flag checks plus stable bit names
- `zz9k/surface.h`: surface layout, colors, fill/copy descriptors
- `zz9k/image_geometry.h`: scale and clipped-scale descriptors
- `zz9k/image.h`: one-shot and streaming image decode descriptors
- `zz9k/shared.h`: bounds-checked shared-buffer byte access
- `zz9k/audio.h`: MP3 decode and streaming audio descriptors
- `zz9k/compression.h`: decompression and streaming decompression descriptors
- `zz9k/crypto.h`: hash, HMAC, Poly1305, ChaCha20, and AEAD descriptors
- `zz9k/text.h`: stable status text for user-facing tools

## Requirements

For AmigaOS 3 tools, the supported local path is Docker with the
`sacredbanana/amiga-compiler:m68k-amigaos` image, driven by the scripts above.

## Third-Party Code

The SDK carries third-party code where current SDK v2 tools need it:

- LHa for UNIX decoder subset (tools/lha-unix), from jca02266/lha
  1.14i-ac20220213, retaining the original LHa for UNIX redistribution terms

## License / Copyright

SDK v2, including the firmware-integrated service ABI, `zz9k.library`, AmigaOS
service tools, public SDK v2 headers, docs, examples, and packaging work in
this fork, is:

Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio

Older pre-fork material from the original MNT ZZ9000 ARM SDK has been removed
from this branch unless retained in individual files with their own notices.

Unless a file carries a narrower notice or a third-party license, this
repository is distributed under:

SPDX-License-Identifier: GPL-3.0-or-later
https://spdx.org/licenses/GPL-3.0-or-later.html
