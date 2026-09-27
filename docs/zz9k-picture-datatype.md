# ZZ9000 Picture DataType

Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio

`zz9k-picture.datatype 42.151` is the validated SDK v2 DataType for
the current package. It is packaged as a side-by-side subclass of the system
`picture.datatype` and must not replace `Classes/DataTypes/picture.datatype`.
OS3.1 remains the minimum target: the class opens against
`picture.datatype` v39 and dynamically uses newer superclass features only
when they are available.

The class binary installs as:

```text
Classes/DataTypes/zz9k-picture.datatype
```

The JPEG and PNG recognition descriptors are packaged inactive under `Storage/DataTypes`
so activation is an explicit install step:

```text
Storage/DataTypes/ZZ9000-JPEG
Storage/DataTypes/ZZ9000-JPEG.info
Storage/DataTypes/ZZ9000-PNG
Storage/DataTypes/ZZ9000-PNG.info
```

To activate the validated DataType path on a test or release-install system,
install the class and copy the descriptors into `DEVS:DataTypes`:

```text
copy Classes/DataTypes/zz9k-picture.datatype TO SYS:Classes/DataTypes/
copy Storage/DataTypes/ZZ9000-JPEG#? TO DEVS:DataTypes/
copy Storage/DataTypes/ZZ9000-PNG#? TO DEVS:DataTypes/
AddDataTypes DEVS:DataTypes/ZZ9000-JPEG
AddDataTypes DEVS:DataTypes/ZZ9000-PNG
AddDataTypes LIST
```

`AddDataTypes LIST` should show `ZZ9000-JPEG` and `ZZ9000-PNG` before
MultiView or browser clients are expected to route matching files to
`zz9k-picture.datatype`. Keeping the descriptors in `Storage/DataTypes` by
default prevents accidental global routing on systems that only want the SDK
tools or manual smoke tests.

The package also includes `C:zz9k-dtprobe` for isolating DataTypes failures:

```text
zz9k-dtprobe XYZ.png
zz9k-dtprobe --client XYZ.png
zz9k-dtprobe --read-pixels XYZ.png
zz9k-dtprobe --screen-remap --layout --client --color-tables XYZ.png
zz9k-dtprobe --draw-window XYZ.png
```

If the descriptor does not match, `NewDTObject` should fail with a DataTypes
lookup error. If the descriptor matches but object creation fails inside the
class, the command reports the `IoErr()` value from that class path.
The `--client` mode also queries bitmap and mask attributes that applications
such as image viewers and browsers may request from picture objects.
The `--read-pixels` mode reads row checksums through `PDTM_READPIXELARRAY`
for comparing the pixel data returned by different picture datatypes.
The `--screen-remap`, `--layout`, and `--color-tables` modes provide a closer
client-style probe by passing the public screen/remap context, running
`DTM_PROCLAYOUT`, and printing palette/remap table attributes.
The `--draw-window` mode opens a temporary public-screen window, runs layout
if needed, adds the datatype object to that window, refreshes it, and hashes
the actual screen pixels over a capped diagnostic rectangle.

The class decodes JPEG and PNG, including transparent PNGs. On
`picture.datatype v43` and `v47`, JPEG and PNG both use the validated
`PDTM_WRITEPIXELARRAY` path. On 32-bit screens, alpha PNGs keep their alpha
state, request `RGBA8888` tiles from the SDK image service, prepare the picture
object with `bmh_Masking = mskHasAlpha`, and write `PBPAFMT_RGBA` pixels through
the superclass. On 8-, 15-, and 16-bit screens, version 42.149 instead requests
RGB tiles and clears the alpha contract before handoff. This matches the
low-depth behavior of PNGdt44 and avoids repeated 68040 software alpha
compositing when MUI applications load many images. 32-bit screens retain
per-pixel alpha.

With a negotiated Zorro 2 aperture, compressed input is capped at a 24 KiB
host-window buffer and output is emitted through geometry-derived tiles capped
at 32 KiB. A single row that cannot fit that cap fails the hardware path
without allocating outside the driver-reserved window. Zorro 3 keeps the
existing larger staging and tile targets.

On `picture.datatype v39-v42`, the class decodes into a legacy 8-bit remapped
bitmap instead of aborting PNG decode. That OS3.1 fallback uses a 216-color
palette, publishes a normal `PDTA_BitMap`, and degrades transparent PNG alpha to
a transparent palette index because the v43/v47 alpha contracts are not
available there.

`42.149` selects the low-depth RGB compatibility path from the `PDTA_Screen`
passed when the object is created. Objects without a screen remain RGBA so
pixel-reading clients do not silently lose alpha, and 32-bit display objects
continue to use the validated alpha route.

`42.150` adds a PNGdt44-compatible LUT8 path for indexed PNGs. When the PNG
is colour type 3, the class captures the file's own `PLTE` and `tRNS`
palette metadata during the header scan, decodes through the same firmware
tile pipeline as before, maps the truecolour pixels back to the file's own
palette indices on the Amiga side, and publishes the picture as depth 8
with `PDTA_NumColors`, `PDTA_ColorRegisters`, `PDTA_CRegs`, and - when
`tRNS` marks at least one fully transparent entry - `mskHasTransparentColor`
plus `bmh_Transparent`. All fully transparent palette entries are mapped
onto the published transparent index; indexed PNGs whose `tRNS` table
contains partial alpha, or whose fully transparent entries share an RGB
triple with an opaque entry, keep the per-pixel alpha path instead. This
is the picture contract MUI converts through its fast palette path; it
removes the multi-second startup slowdown reported for MUI applications
such as IBrowse and YAM when their 8-bit PNG image sets are routed through
`zz9k-picture.datatype` (GitHub issue #73), on every screen depth.
Truecolour PNGs with alpha keep the 42.149 behaviour.

`42.151` decodes non-interlaced PNGs through firmware streaming tiles.
The previous full-height PNG tile could exceed the shared heap on files
past roughly 1.3 megapixels, so Multiview reported `Invalid data` for
large PNGs (for example 1920x1080 indexed files) while `zz9k-view`
decoded the same file. Non-interlaced sources now use the JPEG-style
streaming tile layout (bounded at 768 KB per tile); interlaced PNGs keep
the full-height layout their combine path requires. Requires the matched
v2.8 firmware that streams PNG sessions through partial tiles.

`42.148` makes `GM_RENDER` lock-safe. The old undocumented framebuffer
`fill`/`scale*` diagnostic modes synchronously called the SDK mailbox while
Intuition held the window layer, which could deadlock the GUI during resize.
Those legacy `ENV:ZZ9K_PICTURE_RENDER_MODE` values now select the validated
DataType pixel path; the default path is unchanged.

`42.147` was a stability-hardening release with no behaviour change on correct
input. It fills the superclass-owned LUT8 color tables in place, which fixes
black output on `picture.datatype v39-v42`; validates firmware tile heights from
untrusted replies against the allocated tile buffer; adds a missing overflow
check in the PNG full-image layout path; and rounds the per-row pixel buffer up
to the 16-pixel multiple `graphics.library` requires.

Hardware validation passed with real transparent PNGs in MultiView and a browser
client.

Both descriptors use the `zz9k-picture` base name, `pict` group, binary magic
masks for JPEG and PNG, and priority `10` for the eventual active path.
