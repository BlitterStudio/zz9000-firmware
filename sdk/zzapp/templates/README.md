# ZZApp templates

Templates are MIT-licensed `zz_main` programs. Each one calls `ZZAPP_NOTE()`
from `zzapp_note.h`, so the image carries the `.zzapp` note without a separate
assembler file. Do not include `zzapp_proto.h`. The public API is `zzapp.h`.

Apps link against PIC newlib from `build_pic_newlib.sh`. Stock newlib is not
PIC and fails `zzapp-check`. Set `ZZAPP_PIC_SYSROOT` to that sysroot and
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

## CMake

```text
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$ZZAPP_SDK/cmake/zzapp-toolchain.cmake
cmake --build build
```

`zzapp_add_executable(name SOURCES ...)` in `cmake/zzapp.cmake` links the same
way and runs `zzapp-check` after the link. `cmake/example` builds `hello`.

## Docker

`docker/Dockerfile` is Debian slim plus Arm GNU Toolchain 13.2.rel1 (pinned
URL and sha256), PIC newlib, Python, make, and CMake. The image sets
`ZZAPP_SDK`, `ZZAPP_PIC_SYSROOT`, and `PATH`.

```text
docker build -f sdk/zzapp/docker/Dockerfile -t zz9000-app-sdk sdk/zzapp
docker run --rm -v "$PWD":/src -w /src zz9000-app-sdk make check
```

`ghcr.io/blitterstudio/zz9000-app-sdk` is pushed only on tag pushes and
manual workflow dispatch.

Without Docker, install Arm GNU Toolchain 13.2.rel1, run
`build_pic_newlib.sh`, and export the two variables above. The Make and CMake
entry points are the same.

## Check

`tools/zzapp-check` rejects an image the loader would reject: not a
little-endian ARM PIE, a relocation other than `R_ARM_RELATIVE`, `PT_INTERP`,
`DT_NEEDED`, a writable and executable segment, or a ZZAPP note that is
missing, has the wrong magic, an abi major other than 0, unknown flag bits,
or a stack other than 0 or 64 KiB–1 MiB aligned to 8.

## Templates

- `hello.c`: writes one diagnostic line through `zz_log`.
- `pixels.c`: opens a display surface and handles input.
- `fault.c`: deliberately faults for lifecycle testing.
- `zzmembench.c`: allocator benchmark.
- `mgl_conformance.c`: MiniGL conformance client.
- `filter.c`: file-in/file-out Shell template. It maps an input through the
  ZZRun file proxy, computes SHA-256 with the card service, and writes the
  32-byte digest to its output path. Invoke it as `filter input output`.
- `tone.c`: opens the normal stereo S16LE `zzapp_audio` ring and produces a
  440 Hz square wave. It is route-agnostic: ZZRun's default card-fabric route
  and its `AUDIO=AHI` route use the same app binary.
- `echo.c`: echoes each fixed 32-byte client message using `zz_msg_poll` and
  `zz_msg_send`; use it with the `zz9k.library` app-client smoke tool.
- `console.c`: Shell console example. Prints its arguments to stdout and a
  line to stderr, echoes each stdin line in upper case, reports Ctrl-D/E/F
  break events, and exits at EOF (Ctrl-\ in a Shell window).
