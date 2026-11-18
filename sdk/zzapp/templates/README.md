# ZZApp templates

Templates are MIT-licensed `zz_main` programs. Each one calls `ZZAPP_NOTE()`
from `zzapp_note.h`, so the image carries the `.zzapp` note without a separate
assembler file. Do not include `zzapp_proto.h`. The public API is `zzapp.h`.

Apps link against PIC newlib from `build_pic_newlib.sh`, except freestanding
apps (see "Freestanding" below). Stock newlib is not PIC and fails
`zzapp-check`. Set `ZZAPP_PIC_SYSROOT` to that sysroot and
`ZZAPP_SDK` to this `sdk/zzapp` directory (or `/opt/zzapp-sdk` in the image).

## Make

A third-party Makefile is:

```make
ZZAPP_SDK ?= /opt/zzapp-sdk
APP := hello
SRCS := hello.c
include $(ZZAPP_SDK)/mk/zzapp.mk
```

`make` writes `build/hello.elf`. `make check` runs `zzapp-check`. `make clean`
removes the build. Optional `CFLAGS` and `LDLIBS` are appended to the SDK
flags. This repository builds every template through that include:

```text
make -C sdk/zzapp check
```

ELFs land in `sdk/zzapp/build/<name>.elf`.

`ZZAPP_FREESTANDING=1` links without newlib; see "Freestanding" below and
[the developer guide](../../docs/zzapp-developer-guide.md#freestanding-apps).

## CMake

```text
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$ZZAPP_SDK/cmake/zzapp-toolchain.cmake
cmake --build build
```

`zzapp_add_executable(name [FREESTANDING] SOURCES ...)` in `cmake/zzapp.cmake`
links the same way and runs `zzapp-check` after the link. `cmake/example`
builds `hello` and `freestanding`.

## Docker

`docker/Dockerfile` is Debian slim plus Arm GNU Toolchain 15.3.rel2 (pinned
URL and sha256), PIC newlib, Python, make, and CMake. The image sets
`ZZAPP_SDK`, `ZZAPP_PIC_SYSROOT`, and `PATH`.

```text
docker build -f sdk/zzapp/docker/Dockerfile -t zz9000-app-sdk sdk/zzapp
docker run --rm -v "$PWD":/src -w /src zz9000-app-sdk make check
```

`ghcr.io/blitterstudio/zz9000-app-sdk` is pushed only on tag pushes and
manual workflow dispatch.

Without Docker, install Arm GNU Toolchain 15.3.rel2, run
`build_pic_newlib.sh`, and export the two variables above. The Make and CMake
entry points are the same.

## Check

`tools/zzapp-check` rejects an image the loader would reject: not a
little-endian ARM PIE, a relocation other than `R_ARM_RELATIVE`, `PT_INTERP`,
`DT_NEEDED`, a writable and executable segment, or a ZZAPP note that is
missing, has the wrong magic, an abi major other than 0, unknown flag bits,
or a stack other than 0 or 64 KiB–1 MiB aligned to 8.

`zzapp-check --no-libc app.map` checks the link map of a freestanding build and
fails if any archive other than `libzzapp.a` and `libgcc.a` was linked.

## Freestanding

`ZZAPP_FREESTANDING=1` (make), `FREESTANDING` on `zzapp_add_executable`, or
`-DZZAPP_FREESTANDING=ON` at CMake configure time links `crt0`, the app,
`libzzapp` and `libgcc` without newlib and without the PIC sysroot. `crt0`
calls `zz_exit` directly, `zz_alloc` without `ZZ_ALLOC_HOST_VISIBLE` returns
NULL, and `libzzapp` supplies weak `memcpy`, `memmove`, `memset` and
`memcmp`. The developer guide has the full list of differences. `make check`
builds `freestanding.c` this way, checks its link map, and links a fixture
(`tests/freestanding_leak.c` with `-lc`) that must be rejected.

## Templates

- `hello.c`: writes one diagnostic line through `zz_log`.
- `pixels.c`: opens a display surface and handles input.
- `fault.c`: deliberately faults for lifecycle testing.
- `isolate.c`: faults on forbidden stores, execution, or host heap access; its
  `svc` mode verifies that a text pointer is rejected by a pointer-taking call.
- `spin.c`: CPU-bound scheduler fairness probe; run it as `spin [name]`.
- `memhog.c`: allocates and touches 1 MiB blocks until `malloc` fails, reports
  each 16 MiB and low-memory events, holds the memory until Ctrl-C, then frees
  every block and exits normally.
- `svcbench.c`: times table calls in `zz_time_us` batches and reports
  667 MHz timer-derived cycle estimates because PMU access is unavailable in
  user mode.
- `svcprobe.c`: with firmware built using `ZZAPP_SVC_PROBES=1`, times
  individual SVC path boundaries with PMCCNTR.
- `zzmembench.c`: memory bandwidth benchmark. It times sequential and
  pointer-chase traffic with `zz_time_us`. PMU, SCTLR/ACTLR, and PL310/DDRC
  registers are not available to apps.
- `filter.c`: file-in/file-out Shell template. It maps an input through the
  ZZRun file proxy, computes SHA-256 with the card service, and writes the
  32-byte digest to its output path. Invoke it as `filter input output`.
- `tone.c`: opens the normal stereo S16LE `zzapp_audio` ring and produces a
  440 Hz square wave. It is route-agnostic: ZZRun's default card-fabric route
  and its `AUDIO=AHI` route use the same app binary.
- `echo.c`: echoes each fixed 32-byte client message. It parks in
  `zz_msg_wait` between messages, falls back to `zz_msg_poll` with `zz_yield`
  on firmware older than API 0.4, and replies with `zz_msg_send`; use it with
  the `zz9k.library` app-client smoke tool.
- `console.c`: Shell console example. Prints its arguments to stdout and a
  line to stderr, echoes each stdin line in upper case, reports Ctrl-D/E/F
  break events, and exits at EOF (Ctrl-\ in a Shell window).
- `freestanding.c`: no newlib. A bump allocator over `zz_heap_range` and
  `zz_heap_grow`, a hand-written hash, and its own number formatting; build
  it with `ZZAPP_FREESTANDING=1`. The host runner builds it too and a test
  compares its hash with an independent implementation.
- `fpenv.c`: prints FPSCR as the app sees it on entry and checks that a VFP add
  keeps a denormal result while a NEON add flushes it. ARM only. Exit status 0
  means FPSCR was 0 and both denormal results held; it is the card test for
  the developer guide's bit-identical maths section.
- `ringflood.c`: message ring conformance load. Starts quiet, parked in
  `zz_msg_wait`, and answers PING messages. On CTRL FLOOD it sends
  sequence-numbered, pattern-checked 32-byte messages as fast as the
  app-to-host ring takes them and bumps a counter word in a host-visible
  block on every loop; CTRL QUIET stops it. Its message layout is in the file
  header. Use it with the `zz9k-app-ringtest` tool; `ringflood N` on the host
  runner leaves the flood after N data messages, and the host tests check its
  output against an independent implementation of the pattern.
