<!--
SPDX-FileCopyrightText: 2026 Dimitris Panokostas <midwan@gmail.com>
SPDX-License-Identifier: MIT
-->

# ZZApp developer guide

One trusted ARM program runs on core 1. AmigaOS starts it. The card loads
the ELF, passes a function table, and reclaims every resource when the app
stops or faults. This is not a sandbox. The app runs in SVC, the same mode
as the firmware, with no per-app page tables. A wild store into firmware
data does not abort. Isolation is later work. App source does not change
for that later work if it keeps calling through the table.

The public header is `sdk/zzapp/include/zzapp.h`. The call reference is
[zzapp-api.md](zzapp-api.md). Firmware and launcher layouts live in
`docs/app-runtime.md`. Do not include `zzapp_proto.h` or any firmware header.

## Lifecycle

Core 1 runs one app. A second load gets `ZZ_EBUSY`. States are idle,
loading, starting, running, stopping, and faulted. Paused exists in the
wire enum and is unused.

ZZRun, or a `zz9k.library` client, streams the ELF. Firmware checks the
note, applies `R_ARM_RELATIVE` relocations, zeroes BSS, and enters at
`e_entry` with `r0` = the API table, `r1` = argc, `r2` = argv, and `sp` at
the top of the arena stack. crt0 calls `zz_main`. Return is `zz_exit` of
that status.

Stop is Ctrl-C, `ZZ9KAppStop`, or the library handle closing. The app sees
`zz_should_stop` and `ZZ_EV_STOP`. Grace is 500 ms, then the runtime
force-stops. A normal stop does not cold-restart core 1. ZZRun returns the
app exit code. It returns 20 on fault, an unavailable runtime, a display
failure, or a launcher error, and 10 on bad launcher arguments.

A fault whose PC is in the app arena is an app fault. Firmware captures
`pc`, `sp`, `lr`, fault address, status, and `cpsr`, reclaims the app, and
leaves other firmware services running. A fault inside an API call or the
GL front end takes the existing core-1 cold restart. After that restart,
68k MiniGL contexts are gone, as they are after any core-1 restart.

Amiga reset reclaims the app. If the client vanishes without a reset, the
2 second lease expires and the app is stopped. The launcher heartbeat and
the app watchdog are different clocks. Refreshing one does not refresh the
other.

## Watchdog

Call the API at least every few seconds during a long compute. The runtime
stops the app if 30 seconds pass with no API call, or if one call runs for
10 seconds. `zz_yield` is enough. `zz_sleep_us`, file calls, console reads,
and a flip wait already count, because they are inside an API call.
`zz_should_stop` and `zz_time_us` also count: a loop that only polls them
is not a stall. A loop that only touches app memory does not.

The 5 ms timer runs one service tick between app instructions. It does not
preempt inside an API call. If the app executes `cpsid`, that timer does
not run. Core 0 then cold-restarts core 1 after 2 seconds of silence. That
is the backstop, not a normal stop, and it drops 68k MiniGL clients.

## Memory and the arena

The arena is one 96 MiB card-pool block from the permanent prefix, not from
Fast RAM and not from a module slot. The ELF, BSS, heap, and stack live
there. The note may request less stack, down to 64 KiB, not more than
1 MiB. Zero means 1 MiB. A 64-byte canary sits at the low end of the stack.
A damaged canary force-stops the app. There is no unmapped guard page.

The heap is `[image_end, stack_base)`. newlib `malloc` uses that range.
`zz_alloc` without `ZZ_ALLOC_HOST_VISIBLE` is that malloc, with the
alignment you pass. `zz_heap_range` reports the span. Do not call the
firmware `_sbrk`.

`ZZ_ALLOC_HOST_VISIBLE` comes from the shared heap below `0x08200000`, up
to 16 MiB per app, at most 64 blocks. It is shareable and non-cacheable.
An Amiga client reads it coherently. It is for sharing, not for the app's
working set. `zz_host_address` returns the board-relative offset of that
base pointer only. `zz_mem_info` reports arena and host-visible accounting.
`zz_free` returns a host-visible block immediately. Exit, fault, restart,
and reset release the rest.

`ZZ_ALLOC_UNCACHED` is accepted and ignored while the arena is cacheable.
Pass it if you need the uncached meaning once user mode exists.

FPSCR is zero at entry: round-to-nearest, flush-to-zero clear, default-NaN
clear. The hard-float ABI is required. A VFP-only build is fine. The
runtime saves `d0`–`d31` and FPSCR across ticks.

## Files

The app does not see the SD card. `zz_file_*` and `zz_map_file` go through
the launcher, with that process's AmigaDOS rights. Paths are Amiga paths,
at most 255 bytes plus NUL.

ZZRun's root is the Shell current directory, the Workbench icon directory,
or ToolType `ROOT`. A relative path that would leave that root is rejected.
A path containing `:` is rejected unless `ABS=1`. That check stops a bad
path. It is not a security boundary.

Sixteen files and sixteen directory lists, ids 3..18. Each transfer waits
up to 30 seconds. A short read with `ZZ_OK` is EOF. `zz_map_file` reads a
whole file into host-visible memory, 16 MiB maximum, and holds it until
`zz_unmap_file`. newlib `open` is not connected. Use `zz_file_*` or
`zz_map_file`.

## Console

`printf`, `puts`, and `write` of fd 1 or 2 go to the console-output ring
through `zz_console_write`. The ring holds 32 KiB. A full ring waits. No
byte is dropped. `zz_log` is a different, lossy diagnostic ring: 32 records
per second to ZZRun, 8 lines per second to the UART, 1024 bytes per record.
Use `zz_log` for card traces. Use stdout for the user.

`read` of fd 0 and `zz_console_read` wait for launcher input. At EOF they
return success with zero bytes. The input ring holds 4 KiB. Shell launches
use `Output()` and `Input()`. Workbench launches open a console only after
the app first touches console I/O, as `CON:0/0/640/160/ZZRun/CLOSE/WAIT`.
If that open fails, ZZRun stops the app.

On OS 3.2, Ctrl-\ does not end keyboard input. It does not end `Type *`
either, on the standard handler or on CCON. Stop a keyboard-fed app with
Ctrl-C. To deliver EOF, redirect input (`ZZRun app.elf <file`). If the app
is blocked in a keyboard read when it stops, ZZRun prints
`press Return to finish.`: the console cannot cancel the line read it has
open.

Ctrl-D, Ctrl-E, and Ctrl-F on the display window are raw keys and also
`ZZ_EV_BREAK` with codes 4, 5, and 6. They are not console EOF.

## Building

The SDK image `ghcr.io/blitterstudio/zz9000-app-sdk` has Arm GNU Toolchain
15.3.rel2, a PIC newlib 4.6.0.20260123 sysroot (stock newlib is not PIC and
cannot satisfy the loader) and the SDK at `/opt/zzapp-sdk`, with
`ZZAPP_SDK` and `ZZAPP_PIC_SYSROOT` set. Build it yourself with
`docker build -f sdk/zzapp/docker/Dockerfile -t zz9000-app-sdk sdk/zzapp`.
Outside the image, set `ZZAPP_SDK` to `sdk/zzapp` and `ZZAPP_PIC_SYSROOT` to
the output of `sdk/zzapp/build_pic_newlib.sh`.

One source file declares the note:

```c
#include "zzapp.h"
#include "zzapp_note.h"

ZZAPP_NOTE(ZZAPP_NOTE_FLAG_DISPLAY, 0) /* flags, stack bytes (0 = 1 MiB); no ';' */
```

With make:

```make
ZZAPP_SDK ?= /opt/zzapp-sdk
APP := myapp
SRCS := main.c render.c
include $(ZZAPP_SDK)/mk/zzapp.mk
```

`make` builds `build/myapp.elf`, `make check` also runs `zzapp-check`, and
`make clean` removes the build. `CFLAGS` and `LDLIBS` set before the include
are added to the SDK's flags.

With CMake:

```cmake
cmake_minimum_required(VERSION 3.16)
project(myapp C ASM)
include("${ZZAPP_SDK}/cmake/zzapp.cmake")
zzapp_add_executable(myapp SOURCES main.c render.c)
```

Configure with
`-DCMAKE_TOOLCHAIN_FILE=$ZZAPP_SDK/cmake/zzapp-toolchain.cmake`; the build
runs `zzapp-check` after linking.

`sdk/zzapp/tools/zzapp-check app.elf` checks a finished image: a
position-independent little-endian hard-float ELF for Cortex-A9/NEON, no
`PT_INTERP` and no `DT_NEEDED`, allocated relocations `R_ARM_RELATIVE` only
(stock GNU ld `SHT_REL` output loads directly), and a valid `ZZAPP` note
(magic `0x50505A41`, ABI 0, known flags, a stack of 0 or 64 KiB to 1 MiB).

Flags in the note: bit 0 display, bit 1 audio, bit 2 GL. Request only what
the app uses.

## Running on a PC

`sdk/zzapp/host` builds the same app natively, with SDL2 in place of the card:

```cmake
add_subdirectory(path/to/sdk/zzapp/host)
zzapp_host_executable(myapp SOURCES main.c render.c)
```

`./myapp --root . --scale 2 -- args` runs it in a window; `--headless`,
`--frames N`, `--input FILE` and `--msg-script FILE` make it scriptable for
tests. The runner implements the whole API 0.2 table with host semantics.
Decompress, image and audio decode and GL return `ZZ_ENOSYS` there. Timing,
memory limits and the watchdog are not the card's, so check performance and
limits on hardware. Options and the differences from the card are in
`sdk/zzapp/host/README.md`.

## Running with ZZRun

ZZRun is a Shell command and a Workbench tool. One app at a time. Zorro III
only. It needs `zz9k.library` and, for a display, Picasso96.

Shell:

```text
Stack 32768
ZZRun RAM:app.elf [AUDIO=AHI] [args]
```

`argv[0]` is the ELF file name. Further tokens are the app's arguments, at
most 15 of them. `AUDIO=AHI` before those arguments selects AHI. The default
route is the card audio fabric.

Workbench: the project icon's default tool is ZZRun (`DEFAULT_TOOL=ZZRun`).
Dragging an ELF onto the ZZRun icon overrides ToolType `APP`.

| ToolType | Effect |
|---|---|
| `APP` | ELF path when no project file was dropped |
| `ROOT` | Root for relative app paths. Default is the icon directory |
| `ABS=1` | Allow paths that contain `:` . The root check does not apply to those |
| `MODE` | Optional Picasso96 DisplayID, decimal, `$` hex, or `0x` hex. The app still chooses width, height, and format |
| `ARGS` | Space-separated arguments. Quoting is not parsed |
| `AUDIO=AHI` | PCM to AHI instead of the card fabric |
| `JOYPORT0=1` | Also read joystick port 0. Port 1 is read while the display window is active |

ZZRun reads `JOYxDAT` and the CIA-A fire bits. It does not drive the port
pins.

A display open makes ZZRun open the P96 screen and bind the bitmap's real
pitch and card addresses. RGB565 is the first path. CLUT8 is supported.
BGRA8888 is accepted when P96 returns that format. If the mode cannot be
opened, ZZRun sets the stop word and `zz_display_open` returns `ZZ_EIO`.

## zz9k.library client

`zz9k.library` revision 35 exposes the same runtime without ZZRun. Include
`proto/zz9k.h`. The calls are `ZZ9KAppOpen`, `ZZ9KAppLoadFile`,
`ZZ9KAppLoadMemory`, `ZZ9KAppStart`, `ZZ9KAppStop`, `ZZ9KAppStatus`,
`ZZ9KAppSendMessage`, `ZZ9KAppGetMessage`, `ZZ9KAppMessageSignal`, and
`ZZ9KAppClose`.

Open returns `ZZ9K_STATUS_BUSY` to a second opener. The owner keeps the app
until `ZZ9KAppClose`. Close publishes stop. The library's VBlank server
renews the lease while the handle is open. A task that dies holding the
handle is reclaimed by lease expiry or Amiga reset. There is no callback
for that death.

`ZZ9KAppStart` passes `argv` unchanged. Put the program name in `argv[0]`
if the app expects the ZZRun convention. Messages are 32 opaque bytes.
`ZZ9KAppGetMessage` returns `ZZ9K_STATUS_BUSY` when the ring is empty.
`notify_port` receives at most one outstanding `ZZ9KAppEvent` (message,
completion, or fault). Dequeue it before close. The message signal is also
available without a port.

The app side is `zz_msg_poll` and `zz_msg_send`. A full app-to-host ring
returns `ZZ_EBUSY`. Poll does not block.

## Debugging

On fault, ZZRun prints `pc`, `sp`, `lr`, fault address, status, `cpsr`,
exit code, and generation, then returns 20. `ZZ9KAppStatus` returns the
same fields in `ZZ9KAppStatusInfo`.

Resolve `pc` and `lr` against the app ELF, not the firmware ELF. The
addresses are the relocated VAs, which equal the card addresses in this
runtime. Subtract the arena base if your `addr2line` expects the ELF's
unrelocated VMA. The loader treats `p_vaddr` as an offset from the arena
base, so an ELF linked at VMA 0 matches `addr2line` once you subtract the
runtime load address from the captured `pc`.

```text
arm-none-eabi-addr2line -e app.elf -f -C <pc minus load address>
```

`zz_log` lines arrive on the card UART as `[app] ...` and in ZZRun's
stdout, subject to the rate limits above. stdout and stderr are the
console ring, not that log. A missing console on Workbench means the app
has not called a console function yet.

There is no gdb stub.

## Performance

The APU runs at 666666687 Hz. Compile `-mcpu=cortex-a9 -marm -mfpu=neon
-mfloat-abi=hard`. NEON and the hard-float ABI are the app ABI. Use NEON
for bulk pixel and sample work. Do not ship a soft-float object. It will
not link against `libzzapp`.

Core 1 is one thread. Firmware audio, MiniGL, and SDK jobs run in the 5 ms
tick and at the end of each API call, and they preempt the app only at
those points. A GL call does not return until that call's RasterIX work
has drained. Between GL calls the engine is idle and one 68k packet may
run. Do not stay inside one GL call for long. A call over 50 ms is logged.
A call over 10 seconds stops the app.

The arena is cacheable. Host-visible memory and the audio ring are not.
Do not use host-visible memory as a framebuffer scratch pad. Draw in the
arena or in the surface back buffer, then present. Pitch is the bitmap
stride, not `width * bytes_per_pixel`. After a flip, refresh the surface
before writing the new back buffer.

`zz_display_flip` waits for acknowledgement. `zz_display_flip_async` plus
`zz_display_flip_idle` overlaps the next frame with scanout. Mailbox mode,
if the launcher granted it, replaces a pending frame instead of waiting.
`zz_input_poll` never blocks. Pace a game loop with `zz_sleep_us` or
`zz_yield`, not a spin. A spin still burns the 5 ms slices and can sit
next to the 30 second watchdog if you never enter the API.

Audio is 22050 Hz stereo S16LE, 8192 frames. Write ahead of `*read_frame`
and do not pass it. 11025 is not granted. AHI does not resample. If AHI
cannot open 22050 stereo, the open fails.

SHA-256, decompress, and image or audio decode run on core 1 inside the
call. They are the right tool for a codec you do not want to port. They
are not a second core.

## Porting a POSIX command-line tool

1. Rename `main` to `zz_main`. Return the exit code. Do not call `exit`
   unless you mean `zz_exit`.
2. Keep `printf` and `fprintf(stderr, ...)`. They reach the Shell. Replace
   `open`/`read`/`write`/`lseek`/`stat`/`opendir` with `zz_file_*` or, for
   one read-only input up to 16 MiB, `zz_map_file`.
3. Treat stdin EOF as a zero-length read. Do not wait for Ctrl-\ on OS 3.2.
   Document that the user redirects input, or stop on Ctrl-C via
   `zz_should_stop`.
4. In any loop that can run for more than a few seconds without other API
   calls, call `zz_yield`.
5. Put data files under `ROOT`. Do not hard-code a volume unless the icon
   sets `ABS=1`.
6. Link as a PIE ELF with the note macro. Flag bits stay 0 for a tool that
   has no display, audio, or GL.

A filter that hashes a file and writes 32 bytes is `sdk/zzapp/templates/filter.c`.

## Porting an SDL-style game loop

There is no SDL window and no event pump thread. The loop is ordinary C.

1. `zz_display_open` with the size and `ZZ_FMT_*` you draw. Inspect
   `layout` before storing pixels. `format` stays the logical request.
2. Draw `surface.back`. `zz_display_flip`, or queue `zz_display_flip_async`
   and wait with `zz_display_flip_idle` before drawing the next frame.
   Then `zz_display_refresh`.
3. `zz_input_poll` each frame. Honour `ZZ_EV_STOP`. Map raw keys yourself.
   Mouse deltas are in the event. Joystick axes are -1, 0, or 1.
4. `zz_audio_open(22050, 2, &audio)` and fill interleaved S16LE up to
   `*read_frame + frames`.
5. Sleep until the next frame with `zz_sleep_us`. Do not busy-wait on
   `zz_time_us`.
6. Set note flag bit 0, and bit 1 if you open audio.

`sdk/zzapp/templates/pixels.c` is the display and input shape.
`sdk/zzapp/templates/tone.c` is the audio ring. GL goes through
`zz_gl_bind` and the `gl*` wrappers, not through a second library. The GL
front end stays in the firmware. Closed apps reach it only through the
table.

## Versioning

The table is append-only. A new call is a new field at the end and a minor
bump. `ZZAPP_API_MINOR` is 2. Minor 0 published the table through
`gl_unbind`. Minor 1 appended the async flip, refresh, palette, mailbox
counter, and `gl_bind_flags` slots. Minor 2 appended host-visible memory,
file maps, card services, the console, and messages.

Gate every appended slot with `ZZ_API_COVERS` and a non-NULL function
pointer. `libzzapp` already does this and returns `ZZ_ENOSYS` when the
slot is missing. A direct call that skips the test reads off the end of an
older table.

A breaking change needs a new major. The loader rejects a note major other
than 0. The runtime is expected to keep the previous table for one release
after a major bump. That has not happened. Do not renumber or reorder
fields. Do not change the meaning of a `ZZ_*` value.

The note ABI is 0.0. That number is the loader note, not `ZZAPP_API_MINOR`.

## Licensing

`libzzapp`, `zzapp.h`, the note macro, the linker script, and the templates
are MIT. The firmware, including the runtime and the in-firmware GL build,
is GPL-3.0-or-later. The GL front end that the firmware compiles is
`PolyForm-Noncommercial-1.0.0 OR GPL-3.0-or-later`.

The boundary is the API table. An app links MIT `libzzapp` only. It reaches
GL, files, and the display through that table. It must not be linked
against firmware objects, the GL front-end objects, or `libzzm_rt`. That
is a project rule, not a legal opinion. Revisit it before a closed app
ships. The firmware sources are not a public header.
