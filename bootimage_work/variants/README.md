# Release Variant Bitstreams

GitHub Actions cannot rebuild FPGA bitstreams because the project still
requires Vivado 2018.3. Tagged release CI requires every bitstream listed
here; branch and PR CI package the variants that are present.

| Variant | Bitstream |
|---|---|
| Zorro III / A3000 / A4000 | `bootimage_work/zz9000_ps_wrapper.bit` |
| Zorro II 4MB / A2000 | `bootimage_work/variants/zz9000_ps_wrapper-zorro2.bit` |
| Zorro II 2MB / A2000 | `bootimage_work/variants/zz9000_ps_wrapper-zorro2-2mb.bit` |
| A500 4MB / ZZ9500CX Denise adapter | `bootimage_work/variants/zz9000_ps_wrapper-a500.bit` |
| A500 2MB / ZZ9500CX Denise adapter | `bootimage_work/variants/zz9000_ps_wrapper-a500-2mb.bit` |
| A500+ / Super Denise / ZZ9500CX Denise adapter | `bootimage_work/variants/zz9000_ps_wrapper-a500plus.bit` |

The deprecated no-USB-autoboot builds are intentionally not part of the
release matrix.
