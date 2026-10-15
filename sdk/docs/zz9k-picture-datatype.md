# ZZ9000 Picture DataType

Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio

`zz9k-picture.datatype 42.152` is a `picture.datatype` subclass for JPEG,
PNG, and WebP images. It sends compressed input to the ZZ9000 image service,
then publishes the decoded pixels through the Amiga DataTypes picture-object
interface. The class is separate from the system class. Install it as
`Classes/DataTypes/zz9k-picture.datatype`; do not replace
`Classes/DataTypes/picture.datatype`.

The class opens `datatypes.library` and `datatypes/picture.datatype` at version
39. On version 43 and later it uses the pixel-array interface. On version 39
through 42 it publishes an 8-bit `PDTA_BitMap` using the legacy 216-colour
palette path.

## Install and activate

The package keeps the recognition descriptors inactive in `Storage/DataTypes`.
Copy the class and descriptors, then register each descriptor:

```text
copy Classes/DataTypes/zz9k-picture.datatype TO SYS:Classes/DataTypes/
copy Storage/DataTypes/ZZ9000-JPEG#? TO DEVS:DataTypes/
copy Storage/DataTypes/ZZ9000-PNG#? TO DEVS:DataTypes/
copy Storage/DataTypes/ZZ9000-WebP#? TO DEVS:DataTypes/
AddDataTypes DEVS:DataTypes/ZZ9000-JPEG
AddDataTypes DEVS:DataTypes/ZZ9000-PNG
AddDataTypes DEVS:DataTypes/ZZ9000-WebP
AddDataTypes LIST
```

Check that `AddDataTypes LIST` reports `ZZ9000-JPEG`, `ZZ9000-PNG`, and
`ZZ9000-WebP` before opening matching files in MultiView or another
DataTypes client. Leaving the descriptors in `Storage/DataTypes` prevents them
from changing system-wide file recognition until they are deliberately enabled.

| Descriptor | DataTypes group and ID | Recognition | Priority |
| --- | --- | --- | --- |
| `ZZ9000-JPEG` | `pict,jpeg` | `ff d8 ff` | 10 |
| `ZZ9000-PNG` | `pict,png<nul>` | `89 50 4e 47 0d 0a` | 10 |
| `ZZ9000-WebP` | `pict,webp` | `52 49 46 46 ?? ?? ?? ?? 57 45 42 50` | 10 |

Each descriptor has the `zz9k-picture` base name and is generated from its
`.dtid` source under `amiga/datatypes/descriptors/`.

## Inspect a decode

Use `C:zz9k-dtprobe` to identify whether recognition, object creation, layout,
or drawing failed:

```text
zz9k-dtprobe XYZ.png
zz9k-dtprobe --client XYZ.png
zz9k-dtprobe --read-pixels XYZ.png
zz9k-dtprobe --screen-remap --layout --client --color-tables XYZ.png
zz9k-dtprobe --draw-window XYZ.png
```

| Option | Action |
| --- | --- |
| `--client` | Prints bitmap and mask attributes requested by picture clients. |
| `--read-pixels` | Reads row checksums with `PDTM_READPIXELARRAY`. |
| `--screen-remap` | Passes `PDTA_Screen` and `PDTA_Remap` at object creation. |
| `--layout` | Runs `DTM_PROCLAYOUT` and prints the result. |
| `--color-tables` | Prints palette and remap-table attributes. |
| `--draw-window` | Adds the object to a temporary window, refreshes it, and hashes a rectangle no larger than 320×240 pixels. |

When no descriptor matches, `ObtainDataType` and `NewDTObject` report their
`IoErr()` values. If object creation reaches the class and then fails, the
probe reports the error from that path as well.

## Decode and picture contracts

The class recognizes JPEG, PNG, and WebP headers. WebP header parsing accepts
lossy `VP8 `, lossless `VP8L`, and extended `VP8X` containers. A `VP8X`
animated file supplies its canvas dimensions for the still-image path; timed
multi-frame playback is outside this DataType.

PNG output depends on the source and the target picture DataType interface:

- Indexed colour-type-3 PNGs whose `PLTE` and `tRNS` entries can survive a
  truecolour round trip are published as depth-8 `LUT8` with the file palette.
  The class sets `PDTA_NumColors`, `PDTA_ColorRegisters`, and `PDTA_CRegs`.
  A fully transparent `tRNS` entry becomes `mskHasTransparentColor` and
  `bmh_Transparent`.
- Indexed PNGs with partial alpha, or with an opaque entry that has the same
  RGB value as a fully transparent entry, remain on the per-pixel-alpha path.
- PNG and WebP images with alpha use an RGBA pixel array when the DataTypes
  interface provides one. If `PDTA_Screen` identifies a target of 16 bits per
  pixel or less, PNG alpha is flattened and the class uses RGB output.
- Non-interlaced PNGs use partial-height streaming tiles. Interlaced PNGs use
  the full-height layout required by their combine path.

Memory-backed input is copied directly into the shared staging buffer.
File-backed input retains a separate public read buffer. These paths leave the
pixel, palette, alpha, and rendering contracts unchanged.

## Limits

A negotiated aperture layout selects the Zorro 2 host-window path. It caps
compressed-input staging at 24 KiB and output tiles at 32 KiB. A row that
cannot fit the tile cap fails the decode path. Other configurations use the
normal 256 KiB input staging target. Non-interlaced PNG RGB tiles can target
768 KiB.

The DataType does not decode GIF. The image-codec enum reserves an ABI value
for GIF, but the class has no GIF codec selection or descriptor.

## Build descriptors and add a format

The `.dtid` files in `amiga/datatypes/descriptors/` are the descriptor sources.
The package scripts generate the IFF `DTYP` files and copy their matching
`.info` icons to `Storage/DataTypes`. Generate descriptors from the SDK
directory with:

```text
python3 scripts/generate-datatype-descriptors.py --source-dir amiga/datatypes/descriptors --output-dir build/datatype-descriptors
```

`Recog` accepts hexadecimal bytes and `??` wildcards. Use `Recog=none` only
with `Code=<file>`; the generator embeds that m68k hunk executable as the
`DTCD` recognition hook. Every descriptor source needs a same-name `.info`
icon for packaging.

Adding a descriptor does not implement a decoder. Add the firmware decoder,
codec ID, advertised service capability, SDK capability check, and class
header parsing and codec selection. Still images can use the existing image
session and pixel-output paths. Animation needs its own frame, timing, and
compositing contract.

## Status and history

| Revision | Current relevance |
| --- | --- |
| 42.147 | Added LUT8 colour-table initialization, firmware tile-height validation, PNG-layout overflow checking, and 16-pixel row rounding for `graphics.library`. |
| 42.148 | Made `GM_RENDER` use the DataType pixel path when legacy `ENV:ZZ9K_PICTURE_RENDER_MODE` diagnostic values are present, avoiding mailbox calls while Intuition owns the window layer. |
| 42.149 | Added screen-depth-based PNG alpha flattening. |
| 42.150 | Added the indexed-PNG `LUT8` path. |
| 42.151 | Added streaming tiles for non-interlaced PNGs. |
| 42.152 | Added direct memory-source staging without the intermediate public read buffer. |

Recorded qualification for 42.152: the m68k staging helper passed bounds,
EOF, and failed-copy position checks under Vamos. Amiberry with ZZ9000 RTG
passed file-backed indexed PNG, alpha PNG, and JPEG pixel checks and displayed
the Workbench wallpaper. Physical Zorro 2 and Zorro 3 hardware were not tested
for this revision. Earlier revisions were tested with transparent PNGs in
MultiView and a browser client; those results do not qualify later revisions.

Run the host `datatype_descriptor_test` for descriptor generation and
recognition coverage. Run the m68k staging harness from the SDK directory:

```text
sh tests/run_picture_datatype_staging_test.sh
```

The runner uses Docker to compile the production staging code and runs the
result with Vamos. DataTypes layout and rendering require an AmigaOS smoke
run.
