# MiniGL conformance probe

`mgl-conformance.elf` exercises the in-process ARM MiniGL API.  The matching
68k executable is `mgl-conformance` from the `amiberry-minigl-library` checkout.
Both compile the one shared matrix in `mgl_conformance_matrix.h`; set
`MGL_CONFORMANCE_MATRIX_DIR` when the two repositories are not sibling
checkouts.

Build the ARM app with the normal SDK build, then copy it to the Amiga:

```text
make -C sdk/zzapp ZZAPP_PIC_SYSROOT=/path/to/pic-newlib build/mgl-conformance.elf
ZZRun RAM:mgl-conformance.elf
```

Build the 68k app with the installed-kit library contract and run it from an
Amiga CLI after installing `minigl.library` and `MINIGL.ZZM`:

```text
make -C D:/Github/amiberry-minigl-library \
  MGL_CONFORMANCE_MATRIX_DIR=D:/Github/zz9000-firmware/sdk/zzapp/templates \
  all importlib mgl-conformance
mgl-conformance
```

By default, each probe writes one `MGL` line for every texture-environment,
format, and draw-path group, followed by `MGL summary`. `pass` and `fail` are
the 112 cases in that group; `first-fail` records its first mismatched input
and observed/expected RGBA result. The ARM app also writes `MGL start` first
and spaces each UART line by 150 ms. Use `-v` to print every failing case, or
`-vv` to print every case. RGB565 readback is accepted within 8 per component;
a nonzero exit status means at least one case failed.
