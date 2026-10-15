# ZZ9000 FSBL

The first-stage bootloader initializes the Zynq PS from the generated
`ps7_init` tables, then walks the boot image partitions to load the FPGA
bitstream and firmware. The FSBL runs before ZZ9000OS and is packaged as the
`[bootloader]` partition in `BOOT.bin`.

After the bitstream loads, `FsblHookAfterBitstreamDload()` calls
`ps7_post_config()` so the PL-to-PS path is available before the firmware
handoff. A PS partition without an execution address is treated as data, which
allows `build_bootimage.sh` to place the autoboot ROM in DDR before the
bitstream.

## Build

Use the Arm GNU Toolchain with newlib. Run this from the repository root:

```sh
./build_fsbl.sh
```

The output is:

```text
ZZ9000_proto.sdk/ZZ9000FSBL/build/ZZ9000FSBL.elf
```

`./build_firmware_docker.sh --fsbl` also rebuilds the FSBL in the container.
It does not replace `bootimage_work/FSBL_exec.elf`. To replace that packaged
input after validation, run:

```sh
./build_fsbl.sh --install
```

## PS initialization tables

`src/ps7_init_gpl.c` and `src/ps7_init_gpl.h` are generated from the Vivado
sysdef and are vendored here. Do not edit them by hand. The local `src/ps7_init.h`
shim gives the FSBL the header name it expects.

After changing any `PCW_*` setting, rebuild the bitstream, refresh the tables,
and rebuild the FSBL:

```sh
./util/refresh_ps7_init.sh
./build_fsbl.sh
```

`build_fsbl.sh` runs `util/refresh_ps7_init.sh --check` first. When a local
`ZZ9000_proto/ZZ9000_proto.runs/impl_1/zz9000_ps_wrapper.sysdef` exists, the
check fails if it differs from the vendored files. In a clone without a sysdef,
it reports that no local bitstream build is available and skips the comparison.

## Validate and ship

Compare the rebuilt image with the currently packaged FSBL:

```sh
python util/compare_ps7_tables.py bootimage_work/FSBL_exec.elf \
    ZZ9000_proto.sdk/ZZ9000FSBL/build/ZZ9000FSBL.elf
```

Inspect every difference. The tool compares decoded PS register operations in
order and returns success only when they match.

A broken FSBL prevents the card from booting. Keep an untouched SD card with a
known-good `BOOT.bin`, then use a second card for the rebuilt image. Test boot,
video modes at multiple depths, network, and audio before committing a new
`FSBL_exec.elf`.

## Historical validation

| Date | Result |
|---|---|
| 2026-07-07 | An Arm GNU Toolchain 13.2.rel1 rebuild contained 639 PS operations, compared with 645 in the May 2026 image. The six removed operations were AFI0 `32BIT_EN` mask writes at `0xF8008000` and `0xF8008014` for three silicon revisions. |
| Current firmware safeguard | `video_hp0_bus_width_init()` clears those AFI0 bits before VDMA traffic, protecting scanout when an older FSBL is used. |
