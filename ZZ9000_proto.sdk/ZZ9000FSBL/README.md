# ZZ9000 FSBL — building from source

The first-stage bootloader (FSBL) runs from OCM before anything else,
programs the PS registers (clocks, DDR timing, MIO, AFI bus widths) via
the generated `ps7_init` code, loads the bitstream and ZZ9000OS from the
SD card's BOOT.bin, and hands off. Historically the repo shipped only an
opaque prebuilt `bootimage_work/FSBL_exec.elf` (May 2026). That bit us:
the rtg-1080p32 work changed `PCW_S_AXI_HP0_DATA_WIDTH` 32→64, but the
stale FSBL kept programming AFI0 into 32-bit mode (SLCR 0xF8008000 /
0xF8008014 `32BIT_EN`), corrupting half of every 64-bit VDMA beat.
`video_hp0_bus_width_init()` in ZZ9000OS `video.c` works around that one
register pair at runtime — it stays as belt-and-suspenders — but any
future PS-config change the OS cannot patch (clocks, DDR, MIO) needs the
FSBL itself rebuilt. Hence this build system.

## Build

    ./build_fsbl.sh                     # → ZZ9000_proto.sdk/ZZ9000FSBL/build/ZZ9000FSBL.elf
    ./build_firmware_docker.sh --fsbl   # same, inside the CI container

Toolchain: Arm GNU Toolchain with newlib (same constraint as ZZ9000OS —
see its Makefile header). The output is a normal linked ELF; bootgen
accepts it directly as the `[bootloader]` partition (that is also how
MNT's original XSDK flow packaged it, see `ZZ9000OS/bootimage/ZZ9000OS.bif`).

## PS init code (ps7_init_gpl.c/h)

`src/ps7_init_gpl.c/h` are vendored, generated files — never hand-edit.
Every bitstream build regenerates them inside
`ZZ9000_proto/ZZ9000_proto.runs/impl_1/zz9000_ps_wrapper.sysdef`;
`util/refresh_ps7_init.sh` re-extracts them and
`util/refresh_ps7_init.sh --check` (run automatically by build_fsbl.sh)
fails the build if the vendored copy drifts from the sysdef. After ANY
PS-configuration (`PCW_*`) change: rebuild the bitstream, run the
refresh script, rebuild + revalidate + re-bench the FSBL.

The GPL variant (not `ps7_init.c/h`) is vendored for license
compatibility with this GPLv3 repo; `src/ps7_init.h` is a local shim
mapping the name `fsbl.h` expects.

## Validating a rebuilt FSBL

Compare the decoded PS register tables against the currently shipping
binary — they must be register-for-register identical except for
deliberate PS-config deltas:

    python util/compare_ps7_tables.py bootimage_work/FSBL_exec.elf \
        ZZ9000_proto.sdk/ZZ9000FSBL/build/ZZ9000FSBL.elf

Validated 2026-07-07 (Arm GNU Toolchain 13.2.rel1, `text 72068 /
data 11796 / bss 71356`): the only deltas vs the May-2026 binary were
the six AFI0 32BIT_EN maskwrites (`0xF8008000`/`0xF8008014` ×3 silicon
revs) present only in the OLD image — the 64-bit-HP0 PS config makes
them unnecessary (64-bit is the reset default). All other 639 ops
matched (old: 645 total, new: 639). Boot-path message strings were also
verified present in the rebuild.

## Shipping (hardware gate)

A broken FSBL = the card does not boot, no recovery over Zorro. Always:

1. Keep a known-good SD card (current release BOOT.bin) untouched.
2. `./build_fsbl.sh --install` (copies over bootimage_work/FSBL_exec.elf)
   and `./build_firmware_docker.sh` to package BOOT.bin.
3. Write the new BOOT.bin to a SECOND card; bench-test: boot, video
   modes across depths (exercises the AFI0/VDMA path), network, audio.
4. Only after the bench pass, commit the rebuilt FSBL_exec.elf.
