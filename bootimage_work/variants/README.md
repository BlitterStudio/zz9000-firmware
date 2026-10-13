# Release variant bitstreams

These seven FPGA bitstreams select the card's hardware and autoconfig variant.
Release archives pair each one with the same current `ZZ9000OS.elf`.
`ZZ9000.CFG` controls runtime choices such as video capture settings,
scanlines, INT2, MAC address, HDF selection, and Zorro III Fast RAM.

## Variants

| Variant | Bitstream | Hardware |
|---|---|---|
| `zorro3` | `bootimage_work/zz9000_ps_wrapper.bit` | Zorro III, A3000/A4000, E7M capture |
| `zorro3-aga` | `bootimage_work/variants/zz9000_ps_wrapper-zorro3-aga.bit` | A4000/A4000T AGA video-slot capture, Zorro III Fast RAM |
| `zorro2` | `bootimage_work/variants/zz9000_ps_wrapper-zorro2.bit` | Zorro II 4 MB, A2000 |
| `zorro2-2mb` | `bootimage_work/variants/zz9000_ps_wrapper-zorro2-2mb.bit` | Zorro II 2 MB, A2000 |
| `a500` | `bootimage_work/variants/zz9000_ps_wrapper-a500.bit` | A500 4 MB, ZZ9500CX Denise adapter |
| `a500-2mb` | `bootimage_work/variants/zz9000_ps_wrapper-a500-2mb.bit` | A500 2 MB, ZZ9500CX Denise adapter |
| `a500plus` | `bootimage_work/variants/zz9000_ps_wrapper-a500plus.bit` | A500+ or Super Denise, ZZ9500CX Denise adapter |

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
directory. On a Windows Vivado host, invoke the PowerShell builder from Git
Bash as documented in `build_variant_bitstreams.sh`.

## Release requirements

CI packages the bitstreams present in the tree. Tagged releases pass
`--require-all`, so every image in the table must be present. The
`zorro3-aga` C28 image is also checked against
`a4000-c28-builds.json`; update its routed bitstream and record together after
a C28 rebuild.

The retired no-USB-autoboot images, `zorro3-nofast` images, and the `ns-pal`
firmware flavor are outside the release matrix.
