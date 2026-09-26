# Release Variant Bitstreams

GitHub Actions cannot rebuild FPGA bitstreams because the project still
requires Vivado 2018.3. Build these on the Vivado machine with:

```bash
./build_variant_bitstreams.sh
```

Tagged release CI requires every bitstream listed here; branch and PR CI
package the variants that are present.

These are hardware/autoconfig variants. They are still required for
release packaging, but they are not separate firmware behavior flavors.
PAL/native-video defaults, scanline defaults, INT2, MAC, and HDF
selection are handled by `ZZ9000.CFG`; the former `ns-pal` firmware
flavor is not part of the release matrix.

| Variant | Bitstream |
|---|---|
| Zorro III / A3000 / A4000, E7M capture | `bootimage_work/zz9000_ps_wrapper.bit` |
| Zorro III / A3000 / A4000, E7M capture, no Zorro RAM | `bootimage_work/variants/zz9000_ps_wrapper-zorro3-nofast.bit` |
| A4000 video-slot C28, Fast RAM | `bootimage_work/variants/zz9000_ps_wrapper-zorro3-a4000-c28.bit` |
| A4000 video-slot C28, no Zorro RAM | `bootimage_work/variants/zz9000_ps_wrapper-zorro3-nofast-a4000-c28.bit` |
| Zorro II 4MB / A2000 | `bootimage_work/variants/zz9000_ps_wrapper-zorro2.bit` |
| Zorro II 2MB / A2000 | `bootimage_work/variants/zz9000_ps_wrapper-zorro2-2mb.bit` |
| A500 4MB / ZZ9500CX Denise adapter | `bootimage_work/variants/zz9000_ps_wrapper-a500.bit` |
| A500 2MB / ZZ9500CX Denise adapter | `bootimage_work/variants/zz9000_ps_wrapper-a500-2mb.bit` |
| A500+ / Super Denise / ZZ9500CX Denise adapter | `bootimage_work/variants/zz9000_ps_wrapper-a500plus.bit` |

The A4000 C28 bitstreams require a connected 28 MHz video-slot signal;
they are not substitutes for A3000 or Denise-adapter bitstreams. All nine
images use the same ARM firmware ELF.

`a4000-c28-builds.json` records the routed C28 bitstream fingerprints
used by the release BOOT-payload check. Regenerate and requalify those
images before updating the record after any FPGA rebuild.

The deprecated no-USB-autoboot builds are intentionally not part of the
release matrix.
