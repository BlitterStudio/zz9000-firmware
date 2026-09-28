# ZZ9000 SDK v2 Release Smoke Checklist

This checklist validates a packaged SDK v2 build against the matching
SDK-service firmware. Pre-service firmware is not a supported runtime for this
SDK line.

Run the checks from the installed AmigaOS package. Replace sample paths with
local files that are known to load on the test system.

This checklist covers the full Zorro III service surface. On Zorro II, first
read the [Zorro II service matrix](zz9k-zorro2-services.md) and run only the
adapted clients for the negotiated profile. Several low-level diagnostics use
the legacy default shared heap and are intentionally not Zorro II qualification
tools even when the corresponding production client is supported.

## Service Contract

```text
zz9k-services --check-release
zz9k-services --all
zz9k-info
```

Expected pass signal:

- `zz9k-services --check-release` exits with status `0`.
- The final line is `release check ok`.
- `zz9k-info` prints the SDK ABI, capability names, validated aperture layout,
  service list, and memory diagnostics without reporting query failures.

Failure routing:

- Missing service discovery, required service flags, opcode ranges, or version
  major mismatches are firmware/SDK release-pair blockers.
- `release missing firmware capabilities: AUDIO_STREAM_DRAIN` or
  `HOST_WINDOW_HEAP` means the firmware image predates the matched driver set.
  Missing `APERTURE_LAYOUT` is also a matched-release blocker on Zorro 2;
  matched Zorro 3 firmware deliberately does not advertise that Z2-only bit.
  MHI playback fails at decoder allocation. Reflash a firmware built from the
  release pair rather than debugging the driver.
- Mailbox open or query failures belong in the firmware/driver transport path
  before testing higher-level services.

## Core, Memory, And Surface

```text
zz9k-smoke
zz9k-surface-info
zz9k-surfaceops --hold-ticks 0 --loops 1000 --stats --stats-interval 100
```

Run this generic shared-buffer/surface group on Zorro III. On Zorro II,
`zz9k-info` plus the adapted client sections below are authoritative;
`zz9k-smoke` still requests legacy default shared buffers.

Expected pass signal:

- All commands complete without timeout or SDK status errors.
- Surface operations report nonzero loop progress and no failed operations.
- RTG display state remains usable after the run.

Failure routing:

- Shared-buffer or surface allocation failures route to the memory/surface
  service.
- Slow but completing `zz9k-surfaceops` while audio is active is contention
  evidence, not by itself a correctness failure.

## Crypto

```text
zz9k-hash --alg sha256
zz9k-hash --alg sha1
zz9k-hash --alg poly1305
zz9k-chacha
zz9k-aead
```

Those low-level vector tools still allocate from the legacy default shared
heap and are Zorro III diagnostics. On Zorro II, qualify the adapted path with
the accelerated AmiSSL provider self-test or a real AmiSSL TLS application.

With the accelerated drop-in `amissl.library` installed (the headline v2.2.0
TLS-offload feature, shipped by the drivers package), also confirm end-to-end
TLS offload: run the provider self-test, or perform an HTTPS GET through any
AmiSSL TLS application, as described in the *Verifying on hardware* section of
[`zz9k-amissl-provider.md`](zz9k-amissl-provider.md).

Expected pass signal:

- Hash tools match their built-in vectors.
- ChaCha20 and AEAD tools complete their encrypt/decrypt vector checks.
- The AmiSSL provider self-test prints `encrypt via 'zz9000'` and reports
  `ALL PASS`, or an HTTPS GET through the drop-in `amissl.library` completes;
  `ENV:ZZ9K_DISABLE_OFFLOAD` can A/B the same library against pure software.

Failure routing:

- Digest or vector mismatches route to the crypto service before any archive,
  TLS, or browser integration work.
- Drop-in `amissl.library` self-test failures or TLS handshake errors route to
  the crypto service and the AmiSSL provider before browser integration work.

## Compression And Archives

```text
zz9k-inflate --selftest
zz9k-archive l Work:Archives/test.zip
zz9k-archive t Work:Archives/test.zip
zz9k-archive x --dry-run -o RAM:zz9k-smoke Work:Archives/test.zip
zz9k-archive l Work:Archives/test.lha
zz9k-archive t Work:Archives/test.tar.gz
zz9k-archive l Archives/split-deflate.7z
zz9k-archive t Archives/split-deflate.7z
zz9k-archive x --dry-run -o RAM:zz9k-split Archives/split-deflate.7z
zz9k-archive l Archives/split-lzma.7z
zz9k-archive t Archives/split-lzma.7z
zz9k-archive x --dry-run -o RAM:zz9k-split Archives/split-lzma.7z
zz9k-archive l Archives/split-lzma2.7z
zz9k-archive t Archives/split-lzma2.7z
zz9k-archive x --dry-run -o RAM:zz9k-split Archives/split-lzma2.7z
```

`zz9k-inflate --selftest` uses legacy default shared buffers and is a Zorro III
diagnostic. On Zorro II, run the `zz9k-archive` streamed test/extract cases;
large LHA batch arenas are expected to take their documented per-member or
software fallback instead of consuming the compact host heap.

Expected pass signal:

- `zz9k-inflate --selftest` completes all built-in compressed payload checks.
- Archive list/test/dry-run commands complete without path-safety, CRC, or
  unsupported-method surprises for the chosen fixtures.
- The packaged `Archives/split-*.7z` fixtures list as two files and exercise
  the file-backed compressed multi-substream 7z path.

Failure routing:

- `unsupported` diagnostics for formats not advertised by the codec service are
  acceptable only when the fixture intentionally covers unsupported methods.
- CRC, path-safety, overwrite-policy, or streamed extraction failures route to
  `zz9k-archive` before adding more archive formats.

## Image, Viewer, And DataTypes

If the package was installed with descriptors still inactive, activate the
validated DataType descriptors before the MultiView/browser checks:

```text
copy Storage/DataTypes/ZZ9000-JPEG#? TO DEVS:DataTypes/
copy Storage/DataTypes/ZZ9000-PNG#? TO DEVS:DataTypes/
AddDataTypes DEVS:DataTypes/ZZ9000-JPEG
AddDataTypes DEVS:DataTypes/ZZ9000-PNG
AddDataTypes LIST
```

```text
zz9k-jpeg Work:Pictures/test.jpg
zz9k-jpeg --fb --hold 200 Work:Pictures/test.jpg
zz9k-png Work:Pictures/test.png
zz9k-png --fb --hold 200 Work:Pictures/test.png
zz9k-view Work:Pictures/test.jpg Work:Pictures/test.png
zz9k-dtprobe --client Work:Pictures/test.jpg
zz9k-dtprobe --client Work:Pictures/test.png
MultiView Work:Pictures/test.jpg
MultiView Work:Pictures/test.png
```

Bus/profile note: plain `zz9k-jpeg Work:Pictures/test.jpg` uses a
legacy-default CPU-visible output tile and is a Zorro III diagnostic. On Zorro
II, skip that command; `zz9k-jpeg --fb`, both `zz9k-png` forms, `zz9k-view`,
and the DataType clients are the adapted checks.

Expected pass signal:

- On Zorro III, all listed JPEG and PNG decode paths complete without SDK
  status errors. On Zorro II, all listed image checks except the plain
  `zz9k-jpeg` command complete without SDK status errors.
- `zz9k-view` opens one resizable viewer window, displays each image, and the
  next/previous keys navigate between the images.
- Repeat `zz9k-view` in RGB555 (15-bit), RGB565 (16-bit), and BGRA8888
  (32-bit) RTG modes; JPEG and PNG output has the expected colors in all three.
- Viewer resize and occlusion redraw through visible clips without corrupting
  surrounding RTG contents.
- DataType descriptors are activated from `Storage/DataTypes`, and
  `AddDataTypes LIST` shows `ZZ9000-JPEG` and `ZZ9000-PNG`.
- DataTypes clients display JPEG and PNG through `zz9k-picture.datatype`.
- Repeat the transparent-PNG DataType client and MUI startup checks on 8-,
  15-, 16-, and 32-bit screens. Low-depth modes must not enter the sustained
  software-alpha CPU stall, while 32-bit output must retain per-pixel alpha.
- On Zorro 2, `zz9k-info` reports an active acknowledged layout while viewer
  and datatype decoding run; host-window free space returns after each close.

Failure routing:

- Direct tool decode failures route to the image service or shared-buffer path.
- Viewer restore/occlusion failures route to the image-window clipping path.
- MultiView/browser-only failures route to `zz9k-picture.datatype`.
- Low-depth transparent-PNG startup or rendering stalls route to the
  `PDTA_Screen` alpha-flattening policy in `zz9k-picture.datatype`; missing
  transparency on a 32-bit screen routes to the retained RGBA path.

## Audio And MPEGA

```text
zz9k-mp3 --stats Work:Audio/test.mp3
zz9k-mpega-smoke --trace --null-api-check
zz9k-mpega-smoke --trace --stream-info --stats --freq-max 0 --frames 100 Work:Audio/test.mp3
ZZPlay --audio=ahi Work:Audio/test.mp3
ZZPlay --audio=mhi Work:Audio/test.mp3
ZZPlay --audio=auto Work:Audio/test.mp3
ZZPlay --audio=none --benchmark Work:Audio/test.mp3
ZZPlay --audio=ahi Work:Video/test.mpg
```

The low-level `zz9k-mp3` tool still uses legacy default shared buffers and is
a Zorro III diagnostic. On Zorro II, qualify accelerated MP3 through
`mpega.library`, MHI, or ZZPlay, all of which use the negotiated compact
host-window/card-only layouts.

Expected pass signal:

- `zz9k-mp3 --stats` reports nonzero decoded frames/samples and no stream
  status errors.
- `zz9k-mpega-smoke --trace --null-api-check` reports `null-api check ok`.
- The stream-info check reports the installed `mpega.library` version/revision,
  nonzero frames/samples, and completes without decode errors.
- `zzplay --audio=ahi` reports accelerated Layer III decode plus AHI and plays
  the complete file without a truncated tail.
- On ZZ9000AX with `mhizz9000.library`, explicit MHI and `AUTO` report MHI and
  play without AHI ownership. `AUTO` falls back before playback when MHI is
  unavailable; explicit MHI reports the acquisition error instead.
- `--audio=none --benchmark` decodes to completion without opening an audio
  backend. Ctrl-C and normal EOF both permit an immediate AHI or MHI reopen.

Failure routing:

- `zz9k-mp3` diagnostic failures route to the audio-stream service.
- `zzplay --audio=ahi` failures route to accelerated decode or AHI output;
  MHI-only failures route to `mhizz9000.library` and ZZ9000AX ownership.
- MPEGA-only failures route to the resident compatibility shim.
- Surface slowdown during full-speed audio diagnostics is measured as
  contention unless the graphics or audio command times out or returns errors.

## Streaming Video And P96 PIP

Use a known MPEG-1 Program Stream. Start with normal pacing and inspect the
pixels before collecting uncapped performance:

```text
zzplay --fps Work:Video/test.mpg
zzplay --benchmark Work:Video/test.mpg
```

For firmware with the native FPGA overlay, also resize the playing PIP window
away from its exact source dimensions and then return it to 1:1. Exact-size,
fully visible presentation is eligible for the native plane; resized or
clipped presentation uses the overlay's hardware scaler when the geometry can
be expressed, with the card-local compositor reserved for unsupported cases.

The 2 MiB Zorro II profile has no PIP pool and must skip this section. On a
matched 4 MiB Zorro II stack, begin with 352x288 YUY2 or another frame whose
full aligned allocation fits the 224 KiB pool. An oversized source such as
640x360 must be rejected cleanly without reserving memory or changing overlay
state.

While playback is active, cycle the live `ZZScanlines` modes and bring a
native Amiga PAL/NTSC screen to the front, then return to the P96 RTG screen.
High-resolution RTG intentionally suppresses visible scanline shading, so the
scanline portion is a stability check: the PIP and desktop must remain
unchanged. Native-video takeover must not retain stale PIP/RTG pixels, and the
return must recover cleanly after P96 supplies fresh overlay state.

Expected pass signal:

- `zzplay` identifies the stream and reports `direct planar overlay`.
- Colors, horizontal placement, and every row are correct at 1:1 and resized.
- No flicker, tearing, stale first row, or corruption appears below or outside
  the window.
- Moving, resizing, obscuring, and closing the PIP leaves the desktop clean.
- Scanline-control writes and native-video takeover/RTG return do not expose
  stale surfaces or corrupt either presentation path.
- The hardware pointer/sprite remains topmost.
- Paced playback and uncapped benchmark complete without SDK errors or leaked
  shared buffers/surfaces.

Do not treat FPS output as a performance pass when the displayed pixels are
wrong.

Failure routing:

- Missing video capability/service/streaming-input flags route to the matched
  firmware/SDK release pair.
- Correct resized output with wrong 1:1 output proves the decoder-owned
  planar frame, frame-ready publication, and ARM compositor, but does not
  exercise the native-only planar-to-packed conversion or staging-buffer
  flip. Route that result through packed staging, VDMA, and the PL formatter.
- Wrong output in both states routes to artifact identity, decoder output,
  frame publication, and their common state before changing RTL.
- Any corruption outside the PIP is a stop condition; preserve the UART log
  and exact artifact hashes.
