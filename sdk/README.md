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
| `tools/` | AmigaOS command-line tools, including the ARM-app client smoke tool, and ZZPlay sources |
| `examples/` | AmigaOS library, JPEG streaming, typed decode, and crypto examples |
| `docs/` | SDK reference |
| `scripts/` | m68k build, package, descriptor-generation, and icon-generation scripts |
| `tests/` | Host-side tests and test fixtures |

ARM apps on core 1 use [the developer guide](https://github.com/BlitterStudio/aar/blob/main/docs/aar-developer-guide.md)
and [the API reference](https://github.com/BlitterStudio/aar/blob/main/docs/aar-api.md), which live in BlitterStudio/aar. The Amiga side reaches an app through `aar.library` ([client API](https://github.com/BlitterStudio/aar/blob/main/docs/aar-client-api.md)); the package includes it, its headers, `aar-run` (the launcher that serves the app), and
`zz9k-app-echo-test`, which loads an `echo.elf` built from the ARM app
template and verifies its 32-byte message round trip, and
`zz9k-app-ringtest`, which floods the message ring from `ringflood.elf` and
checks every message, the ping latency and a counter word. The
[License](#license) section lists which files are MIT and which are
GPL-3.0-or-later.

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

Both build scripts use the `amigadev/crosstools:m68k-amigaos` Docker
image (GCC 16), pinned by digest because the tag follows release candidates.
Supply `-Image` to the PowerShell build script or set `IMAGE` when running the
shell build script to try another image. The drivers repository builds with
the same image.

`scripts/m68k-toolchain-pin.sh current` prints the pinned image and
`scripts/m68k-toolchain-pin.sh set <image>@sha256:<digest>` moves every pin.
The daily `m68k toolchain update` workflow does that when the tag moves:
it pushes the change to the `build/m68k-toolchain` branch and opens or
updates a pull request, on which CI builds, packages and starts the SDK
tools with the new image. It acts as the ZZ9000 toolchain GitHub App
(organization variable `M68K_TOOLCHAIN_APP_ID`, secret
`M68K_TOOLCHAIN_APP_PRIVATE_KEY`), so that pull request gets normal CI.

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

- `Libs/zz9k.library`, `Libs/aar.library` and `Libs/mpega.library`
- `C/` command-line tools, including `zz9k-services`, `zz9k-jpeg`,
  `zz9k-png`, `zz9k-view`, `zz9k-app-echo-test`, `zz9k-app-ringtest`, and the
  diagnostics
- `C/ZZPlay`, the media player, with its service modules in
  `C/Providers/` (`radio.zzprov` for internet radio, `plex.zzprov` for Plex
  Media Server)
- `Classes/DataTypes/zz9k-picture.datatype` and
  `Classes/DataTypes/zz9k-sound.datatype`
- DataType descriptors in `Storage/DataTypes/`
- `C/aar-run` and `C/aar-run.info`, the reference launcher that runs an app
  and serves its files, console, display, audio and input through
  `aar.library` (manual: `Docs/aar-run.guide`)
- Public headers and FD files in `Developer/`, with `LICENSE-MIT.txt` and
  `LICENSE-GPL-3.0.txt`; `Developer/Lib/libaarserve.a` is the serving loop of
  `aar-run` as an archive, with its header `libraries/aar_serve.h` in
  `Developer/Include/`, for programs that embed the loop
- AmigaGuide manuals in `Docs/` and source examples in `Examples/`
- `MANIFEST.sha256`, containing a SHA-256 digest for every other packaged file

Install the package with matching SDK-service firmware. `zz9k.library` and
`amissl.library` must also come from the same package release: older statically
linked AmiSSL clients consume mailbox completions directly and cannot safely
share that consumer with the library. After booting the firmware and installing
the package, run:

```text
zz9k-services --check-release
```

A successful check ends with `release check ok`. For the package-level smoke
procedure, follow [`docs/zz9k-release-smoke.md`](docs/zz9k-release-smoke.md).

## mpega.library

`Libs/mpega.library` is a drop-in for the original `mpega.library` 2 (version
2, with the revision in its `$VER:` string, ending in `ZZ9000 SDK`). It
decodes MP3 on the card through the audio MP3 stream service and has the seven
original functions (`MPEGA_open`, `MPEGA_close`, `MPEGA_decode_frame`,
`MPEGA_seek`, `MPEGA_time`, `MPEGA_find_sync`, `MPEGA_scale`) plus
`MPGA_decode_to` at offset -72.

### Software fallback

Installing this library over a software `mpega.library` would end MP3 playback
on an Amiga where the card path is unavailable, so the library passes such a
stream to the software decoder. The software decoder has to be the file
`LIBS:mpega-software.library`. The ZZ9000 drivers installer saves a previous
software `mpega.library` under that name; to do it by hand, copy the old file
there before installing this one.

`MPEGA_open` hands the stream over when the card cannot serve it: `zz9k.library`
is missing or too old, the firmware has no MP3 stream service, shared memory
for the stream cannot be allocated (for example when the Zorro II host window
is taken), or the card refuses to start the stream. A bad argument and data
that is not MPEG audio still fail with `NULL`. A `bs_access` hook sees
`MPEGA_BSFUNC_CLOSE` for the open this library made before the software
decoder opens the stream again with its own `MPEGA_BSFUNC_OPEN`.

The software decoder's own name is also `mpega.library`, so it is not opened
with `OpenLibrary()`. The first stream that needs it loads the file with
`LoadSeg()`, starts it with `InitResident()` and moves its library node from
the system library list to a private list, so no program can open it by name.
A file whose name or `$VER:` string contains `ZZ9000` is refused, because that
would be this library. Only a process loads it; a plain task gets `NULL` from
`MPEGA_open` when the card path is unavailable.

The stream that `MPEGA_open` returns belongs to this library. `MPEGA_close`,
`MPEGA_decode_frame`, `MPEGA_seek`, `MPEGA_time`, `MPEGA_scale` and
`MPGA_decode_to` forward to the software decoder, and the public
`MPEGA_STREAM` fields are copied from its stream after the open and after each
call. `MPEGA_find_sync` is always answered here. The software decoder is
closed and expunged when this library expunges and no stream of it is open;
while one is open, this library stays in memory. If the software decoder keeps
itself in memory because something else still holds it, it stays loaded.

### Rate and channel reduction

`freq_div`, `freq_max` and `force_mono` of `MPEGA_CTRL` shrink the PCM before
it reaches the caller. Firmware that reports
`ZZ9K_SERVICE_FLAG_AUDIO_STREAM_REDUCE` in the audio service flags does that on
the card: the library starts the stream with `output_channels` 1 for a stereo
stream under `force_mono`, and with `output_hz` set to the stream's rate divided
by the chosen divisor. The card mixes stereo to mono as `(L + R) / 2` and
decimates through a low-pass filter, so the PCM ring already holds the
requested layout. Firmware without the flag rejects the request with
`ZZ9K_STATUS_UNSUPPORTED`; the library then begins the stream with zeros and
does the same work on the 68k. That path keeps every `freq_div`th sample
without a filter, so high frequencies alias.

Either way `MPEGA_STREAM` keeps describing the file in `frequency` and
`channels`, and the decoded PCM in `dec_frequency` and `dec_channels`. The 68k
reads the card's PCM ring with aligned longword accesses.

### MPGA_decode_to

`LONG MPGA_decode_to(MPEGA_STREAM *mpds, struct MpgaOut *out)` decodes the next
frame and writes it into the caller's buffers in the format the output wants,
so the program need not copy or convert it again. Its offset, `struct MpgaOut`
and the `MPGA_FMT_*` values are those of `ambmpega.library`
(<https://github.com/luvwagn/mpga.library>, interface MIT), and the formats
follow that library's documentation: `MPGA_FMT_S16` (planar 16-bit),
`MPGA_FMT_S16I` (interleaved 16-bit, as AHI's `AHIST_S16S`), `MPGA_FMT_S8`
(Paula bytes), `MPGA_FMT_S8D` (the same with triangular dither of one 8-bit
step) and `MPGA_FMT_S14` (Paula's 14 bits as a high byte and 6 more bits). The
buffers are rings of `mo_Mask + 1` samples, `mo_Index` is advanced by the
samples written, `mo_Peak[]` receives each channel's peak (0 to 32767), and
`mo_Clipped` is increased by the samples that `MPEGA_scale` above 100 clipped.
On a stream served by the software decoder, `mo_Clipped` is left as it was
because the software decoder clips inside its own scaling.

The vector exists only in `mpega.library` 2.127 or newer from this SDK. Check
`lib_Revision` of the opened library before calling it. An `mpega.library` from
another source, and older builds of this one, have nothing at offset -72.

```c
struct Library *MPEGABase = OpenLibrary("mpega.library", 2);

if (MPEGABase && MPEGABase->lib_Revision >= 127) {
  /* MPGA_decode_to() is available. */
}
```

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

The client side is MIT ([LICENSES/MIT.txt](LICENSES/MIT.txt)): the fd files,
the headers under `amiga/include/`, `include/zz9k/` (except `module.h` and
`service_registry.h`) and `host/include/zz9k/`, and the Amiga examples. A
program under any licence can compile these in to call `zz9k.library`, and may
call `zz9k.library` and `mpega.library` through them under an additional
permission described in [LICENSING.md](../LICENSING.md). The ARM app library,
headers, startup code, linker script, build helpers and templates are MIT too
and live in [BlitterStudio/aar](https://github.com/BlitterStudio/aar).

Unless a file has its own notice or a third-party license, the rest is
distributed under
[GPL-3.0-or-later](https://spdx.org/licenses/GPL-3.0-or-later.html). That
includes `zz9k.library` itself and the `zz9k_host.o` implementation. See
[LICENSING.md](../LICENSING.md) for how the firmware licence applies to
programs that use the card.
