# Release variant bitstreams

These seven FPGA bitstreams select the card's hardware and autoconfig variant.
The two Zorro III archives pair RasterIX bitstreams with the RasterIX-enabled
firmware ELF; the other five use the standard `ZZ9000OS.elf`. `ZZ9000.CFG`
controls runtime choices such as video capture settings, scanlines, INT2, MAC
address, HDF selection, and Zorro III Fast RAM.

## Variants

| Variant | Bitstream | Hardware |
|---|---|---|
| `zorro3` | `bootimage_work/zz9000_ps_wrapper.bit` | Zorro III, A3000/A4000, E7M capture, RasterIX |
| `zorro3-aga` | `bootimage_work/variants/zz9000_ps_wrapper-zorro3-aga.bit` | A4000/A4000T AGA video-slot capture, Zorro III Fast RAM, RasterIX |
| `zorro2` | `bootimage_work/variants/zz9000_ps_wrapper-zorro2.bit` | Zorro II 4 MB, A2000 |
| `zorro2-2mb` | `bootimage_work/variants/zz9000_ps_wrapper-zorro2-2mb.bit` | Zorro II 2 MB, A2000 |
| `a500` | `bootimage_work/variants/zz9000_ps_wrapper-a500.bit` | A500 4 MB, ZZ9500CX Denise adapter |
| `a500-2mb` | `bootimage_work/variants/zz9000_ps_wrapper-a500-2mb.bit` | A500 2 MB, ZZ9500CX Denise adapter |
| `a500plus` | `bootimage_work/variants/zz9000_ps_wrapper-a500plus.bit` | A500+ or Super Denise, ZZ9500CX Denise adapter |

`variant-matrix.sh` is the canonical release matrix consumed by the shell
variant builder, release packager, and PowerShell timing sweep. Update it
before adding, removing, or changing a release variant.

## Build

Build all variants on the Vivado host from the repository root:

```sh
./build_variant_bitstreams.sh
```

Pass one or more variant names to rebuild only those images:

```sh
./build_variant_bitstreams.sh zorro2 a500plus
```

The script writes the canonical `zorro3` image to
`bootimage_work/zz9000_ps_wrapper.bit` and writes the other images to this
directory. The two Zorro III matrix rows derive their RasterIX builder options.
On a Windows Vivado host, invoke the PowerShell builder from Git Bash as
documented in `build_variant_bitstreams.sh`.

## Release requirements

CI packages the bitstreams present in the tree in two ELF-set passes. Tagged
releases pass `--require-all`, which requires every image in the table to have
a matching passing SHA-256 entry in `timing-qualification.json`. The timing
sweep updates that record with its selected result; copy the selected bitstream
into the listed release path and commit both changes together. The
`zorro3-aga` image is also checked against `a4000-c28-builds.json`; update its
routed bitstream and record together after a C28 rebuild.

The retired no-USB-autoboot images, `zorro3-nofast` images, and the `ns-pal`
firmware flavor are outside the release matrix.
