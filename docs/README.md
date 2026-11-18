# ZZ9000 firmware documentation

[BUILD.md](../BUILD.md) covers toolchains, builds, and the host test suites.
[LICENSING.md](../LICENSING.md) covers the licences of the firmware, the SDK,
and programs that use the card.

## Set up the card

| Document | Contents |
| --- | --- |
| [configuration.md](configuration.md) | `ZZ9000.CFG` settings: native video capture, scanlines, Fast RAM, network, SD boot, audio, Picasso96 modelines, USB, and Amiga MMU and cache settings. |

## Write Amiga programs that use card services

| Document | Contents |
| --- | --- |
| [sdk/README.md](../sdk/README.md) | SDK v2 layout, the AmigaOS package, and how to build it. |
| [zz9k-library.md](../sdk/docs/zz9k-library.md) | `zz9k.library`: headers, calls, service jobs, and the ARM app client API. |
| [zz9k-zorro2-services.md](../sdk/docs/zz9k-zorro2-services.md) | Which services work in the Zorro II host window. |
| [zz9k-crypto-acceleration.md](../sdk/docs/zz9k-crypto-acceleration.md) | Mailbox crypto operations. |
| [zz9k-amissl-provider.md](../sdk/docs/zz9k-amissl-provider.md) | The OpenSSL 3 provider for AmiSSL applications. |
| [zz9k-picture-datatype.md](../sdk/docs/zz9k-picture-datatype.md) | `zz9k-picture.datatype` for JPEG, PNG, and WebP. |
| [zz9k-sound-datatype.md](../sdk/docs/zz9k-sound-datatype.md) | `zz9k-sound.datatype` for MP3, FLAC, and Ogg Vorbis. |
| [zzplay.md](../sdk/docs/zzplay.md) | The ZZPlay media player. |
| [zz9k-68k-offload.md](../sdk/docs/zz9k-68k-offload.md) | The experimental 68k runner and offload model. |
| [zz9k-modules.md](../sdk/docs/zz9k-modules.md) | Service manifests and the service registry helpers. |
| [zz9k-vendor-services.md](../sdk/docs/zz9k-vendor-services.md) | The vendor service namespace. |
| [zz9k-release-smoke.md](../sdk/docs/zz9k-release-smoke.md) | The release check for a packaged SDK and matching firmware. |

## Write ARM apps for core 1

| Document | Contents |
| --- | --- |
| [zzapp-developer-guide.md](../sdk/docs/zzapp-developer-guide.md) | Building (including freestanding apps), running, memory, scheduling, bit-identical maths, and porting apps. |
| [zzapp-api.md](../sdk/docs/zzapp-api.md) | Every `zz_*` call and `ZZ_*` constant. |
| [app-runtime.md](app-runtime.md) | The firmware and launcher contract: slots, limits, address spaces, lifecycle, exit and reset teardown, and the SVC interface. |
| [sdk/zzapp/host/README.md](../sdk/zzapp/host/README.md) | Running apps on a PC with the SDL host runner. |
| [sdk/zzapp/templates/README.md](../sdk/zzapp/templates/README.md) | The app templates, including the freestanding, FPU probe, and ring conformance apps. |

## Work on the firmware

| Document | Contents |
| --- | --- |
| [memory-map.md](memory-map.md) | DDR reservations, the linker image, and the card pool. |
| [queue-probe.md](queue-probe.md) | The temporary queue coherence probe for the AAR cutover: firmware behaviour, core-1 teardown handshake, tests. |
| [modules.md](modules.md) | Module runtime v2: verified, on-demand `.ZZM` modules. |
| [audio-fabric.md](audio-fabric.md) | Mixing SDK playback and direct-ring audio into the TX ring. |
| [audio-conversion.md](audio-conversion.md) | PCM rate conversion to and from the 48 kHz period. |
| [audio-saturation-ceiling.md](audio-saturation-ceiling.md) | Paula and AX saturation calibration. |
| [ahi-recording-protocol.md](ahi-recording-protocol.md) | The ZZ9000AX recording ring and its protocol. |
| [native-p96-pip.md](native-p96-pip.md) | The Picasso96 PIP overlay plane. |
| [usb-proxy-protocol.md](usb-proxy-protocol.md) | The USB proxy protocol, version 2. |
| [minigl-module.md](minigl-module.md) | Plan for moving card-side MiniGL into `MINIGL.ZZM` (GL work is parked). |
| [minigl-v29-zz9000-transport.md](minigl-v29-zz9000-transport.md) | The MiniGL wire-v9 transport (GL work is parked). |
| [testing/pixel-retiming.md](testing/pixel-retiming.md) | FPGA timing evidence for the pixel-domain paths. |
| [testing/zz9000ax-dig0-feasibility.md](testing/zz9000ax-dig0-feasibility.md) | Why `DIG0` alone cannot carry lossless stereo capture. |
