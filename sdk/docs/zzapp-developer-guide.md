<!--
SPDX-FileCopyrightText: 2026 Dimitris Panokostas <midwan@gmail.com>
SPDX-License-Identifier: MIT
-->

# ZZApp developer guide

Up to four trusted ARM programs run on core 1. AmigaOS starts them. The card
loads each ELF, passes a function table, and reclaims every resource when its
app stops or faults. Apps run in ARM user mode with separate address spaces.
The table contains user-mode SVC veneers; keep calling the `zz_*` wrappers
and never call or cache a firmware address.

This protects the card from ordinary app bugs. It is not a sandbox: the user
chooses what code to run, and the app still receives the services and
host-visible resources it requests.

The public header is `sdk/zzapp/include/zzapp.h`. The call reference is
[zzapp-api.md](zzapp-api.md). Firmware and launcher layouts live in
`docs/app-runtime.md`. Do not include `zzapp_proto.h` or any firmware header.

## Lifecycle

Each launch owns one of four slots, its launcher window, and a generation.
States progress from free to claimed, loading, starting, runnable or blocked,
then through stopping and reclaiming back to free. A STOPPING slot remains
unavailable until its audio teardown is acknowledged, then becomes reusable.
A launch fails without changing another slot when every slot is occupied or
admission fails. A `CLAIMED` slot that never begins loading expires after two
seconds and records lease loss.

ZZRun, or a `zz9k.library` client, streams the ELF. Firmware checks the note,
applies `R_ARM_RELATIVE` relocations, zeroes BSS, and enters at `e_entry` with
`r0` = the API table, `r1` = argc, `r2` = argv, and `sp` at the top of the
private stack. crt0 calls `zz_main`. Return is `zz_exit` of that status.

Stop is Ctrl-C, `ZZ9KAppStop`, or the library handle closing. The app sees
`zz_should_stop` and `ZZ_EV_STOP`. Grace is 500 ms, then the runtime stops
that app. Only user-mode data aborts, prefetch aborts, or undefined
instructions stop and reclaim a single slot. A firmware-code fault restarts
core 1 and ends every app. Amiga reset also ends every slot; an exit racing
that reset records the reset reason. Loss of a running slot's heartbeat for
two seconds stops that slot.

Teardown is ordered and idempotent: new calls are blocked; waits and work
holding app pointers are cancelled; display, audio, GL, and decoder resources
are released; mappings, frames, and host-visible blocks are released; then
the terminal reason is published and the slot becomes free. `APP_LIST`
retains one of normal exit, forced stop, user fault, stall, stuck call, lease
loss, Amiga reset, or core restart after teardown.

## Watchdog

Call the API at least every few seconds during a long compute. The runtime
stops that slot if 30 seconds pass with no API call, or if one call runs for
10 seconds. `zz_yield` is enough. `zz_sleep_us`, file calls, console reads,
and a flip wait already count, because they are inside an API call.
`zz_should_stop` and `zz_time_us` also count: a loop that only polls them
is not a stall. A loop that only touches app memory does not.

The private timer preempts runnable app code every 5 ms. It does not make a
long-running API handler safe to interrupt; a call that runs for 10 seconds
still stops its app. Core 0 cold-restarts core 1 only after two seconds with
neither progress nor timer ticks, and records core restart for every non-idle
slot.

## Memory model

Every app has a private virtual window at `0xC0000000`–`0xDFFFFFFF`. Its ELF,
BSS, heap, and stack are mapped there from 1 MiB card-pool frames. The heap
grows upward; the stack is at the top of the window. There is an unmapped gap
between them. Firmware memory, other apps' private frames, device mappings,
and ungranted host memory are not user mappings.

API 0.1 and 0.2 images retain the old contract: their image, heap, and stack
commit 96 MiB at load. API 0.3 images use the dynamic-heap note. They commit
the image, stack, and optional minimum heap at load, then newlib grows or
shrinks the heap in 1 MiB steps. A failed growth makes `malloc` return NULL;
it does not fault and it never moves existing allocations. The API table's
`heap_info` slot reports the current commit, cap, and remaining pool capacity.

The runtime keeps a system reserve. When free app memory crosses the low-water
mark, it appends one low-memory record to every live slot's launcher log ring;
another record is written only after free memory rises above the high-water
mark and falls again. A launcher may cap its own app.
`ZZ_ALLOC_HOST_VISIBLE` remains a separately accounted shared card allocation,
up to 16 MiB per app by default (the launcher can change the cap at load), for
Amiga-visible buffers rather than private working
memory.

## Sharing pixels with AmigaOS

For pixels that an Amiga program reads, allocate a shared card block with
`zz_alloc(bytes, align, ZZ_ALLOC_HOST_VISIBLE)`, then pass that allocation's
base to `zz_host_address`. The returned board-relative Zorro III offset names
the same allocation for the 68k client. `zz_host_address` rejects arena and
interior pointers.

The block is Normal write-back shareable memory like the rest of DDR. Zorro
III accesses go through the Zynq ACP, which is coherent with the ARM caches,
so the app needs no ARM cache call; the API has none. The 68k program must
read the block through a cache-inhibited mapping (for example with
`mmu.library`) or flush its own caches before reading, because the ACP does
not reach the 68k's caches. `ZZ_ALLOC_UNCACHED` is accepted and has no effect.
The CPU can still reorder plain stores. Put a barrier
(`__sync_synchronize()`) between filling the data and updating the flag or
index the 68k polls. The allocation is separate from the private arena and
display surfaces. The total of these allocations is limited to 16 MiB per
app by default, or to the cap the launcher set at load; use `zz_mem_info` to
inspect the used and allowed amounts, and release
each block with `zz_free` when the 68k side no longer needs it.

## Isolation

User code may access its private window, its granted host-visible allocations,
its owned display surfaces, and its own launcher window. Other access takes an
app fault and ends only that app. API and GL veneers also validate every
pointer argument. A bad pointer passed to any call returns `ZZ_EFAULT`; do not
use it as a probing mechanism.

Isolation prevents accidental cross-app and firmware corruption. It does not
turn an app into an untrusted sandbox or grant it a security boundary against
the user who launched it.

## Scheduling and shared resources

Core 1 runs the system thread before app threads. Service work has priority:
SDK tasks, MiniGL, launcher processing, and audio feeding. Runnable apps
share the remaining time round-robin in 5 ms quanta. File and directory
operations, console read and output backpressure, display open and flip,
non-host audio open and close, and sleep park the calling slot. Completion
wakes it; file and directory calls time out after 30 seconds with
`ZZ_ETIMEDOUT`; stop wakes a parked waiter and waiting service calls
return `ZZ_EIO`. `zz_input_poll` and `zz_display_vsync` do not park. Do not
poll or spin for readiness. Use the blocking call, `zz_sleep_us`, or
`zz_yield`.

Two direct card-audio streams may be open across all apps. Only one app may
bind GL at a time. Each app may bind one display; a second display for that
same app fails. Release a resource promptly so another slot can use it.

FPSCR is zero at entry: round-to-nearest, flush-to-zero clear, default-NaN
clear. The hard-float ABI is required. A VFP-only build is fine. The runtime
saves `d0`–`d31` and FPSCR across switches.

## Files

The app does not see the SD card. `zz_file_*` and `zz_map_file` go through
the launcher, with that process's AmigaDOS rights. Paths are Amiga paths,
at most 255 bytes plus NUL.

ZZRun's root is the Shell current directory, the Workbench icon directory,
or ToolType `ROOT`. A relative path that would leave that root is rejected.
A path containing `:` is rejected unless `ABS=1`. That check stops a bad
path. It is not a security boundary.

Sixteen files and sixteen directory lists, ids 3..18. Each transfer parks
for up to 30 seconds. Stop returns `ZZ_EIO`; expiry returns
`ZZ_ETIMEDOUT`. A short read with `ZZ_OK` is EOF. `zz_map_file` reads a
whole file into host-visible memory, 16 MiB maximum, and holds it until
`zz_unmap_file`. newlib `open` is not connected. Use `zz_file_*` or
`zz_map_file`.

## Console

`printf`, `puts`, and `write` of fd 1 or 2 go to the console-output ring
through `zz_console_write`. The ring holds 32 KiB. A full ring parks until
the launcher drains it. No byte is dropped. Stop cancels that wait with
`ZZ_EIO`. `zz_log` is a different, lossy diagnostic ring: 32 records per
second to ZZRun, 8 lines per second to the UART, 1024 bytes per record. Use
`zz_log` for card traces. Use stdout for the user.

`read` of fd 0 and `zz_console_read` park for launcher input. At EOF they
return success with zero bytes. Stop cancels the wait with `ZZ_EIO`. The input
ring holds 4 KiB. Shell launches use `Output()` and `Input()`. Workbench
launches open a console only after the app first touches console I/O, as
`CON:0/0/640/160/ZZRun/CLOSE/WAIT`. If that open fails, ZZRun stops the app.

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

## Freestanding apps

An app can leave newlib out. Build it with `ZZAPP_FREESTANDING=1` in the make
include, with `FREESTANDING` on `zzapp_add_executable`, or configure CMake
with `-DZZAPP_FREESTANDING=ON` to make every target freestanding. The image
links `crt0`, the app, `libzzapp` and `libgcc`. The PIC sysroot is not needed
for make or for the CMake option; a single `FREESTANDING` target in a CMake
project still needs it for the targets next to it. The macro
`ZZAPP_FREESTANDING` is defined for the app's sources, so one file can build
in both modes. The normal mode is unchanged.

What differs:

- `crt0` passes the value `zz_main` returns to `zz_exit`. Nothing flushes
  stdio or runs `atexit` handlers. Neither mode calls `__libc_init_array`, so
  a `constructor` function never runs.
- The link has no `-lc` and no `-lm`, and `--no-undefined` stays. A call to
  `malloc`, `printf`, `strlen` or `sinf` fails at link time. Integer division
  comes from `libgcc` (`__aeabi_uidiv` and friends), because the Cortex-A9
  has no divide instruction.
- `libzzapp` leaves out `syscalls.c`: there is no `_sbrk`, `_write` or
  `_exit`. Write output with `zz_log` or `zz_console_write`.
- `zz_alloc` without `ZZ_ALLOC_HOST_VISIBLE` returns NULL. The app brings its
  own allocator over the heap. `zz_heap_range` reports the committed span,
  `zz_heap_grow` commits whole MiB at its end and returns the old break, and
  `zz_heap_shrink` gives them back. Host-visible blocks work as
  before. `zz_free` releases only host-visible blocks and ignores any other
  pointer.
- `libzzapp` carries weak `memcpy`, `memmove`, `memset` and `memcmp`
  (`sdk/zzapp/src/freestanding.c`). GCC calls them for struct copies and zero
  initialisers even when the source never names them, and `libgcc` does not
  provide them. They move whole words only when every address is 4-byte
  aligned and do not use NEON. `memmove` does its own copying and never calls
  `memcpy`, so replacing `memcpy` with a backward copy is safe. Define your
  own strong versions to replace them. No other `<string.h>` function exists
  in this mode.

After linking, the make include and the CMake function run
`zzapp-check --no-libc` on the link map. It fails when any archive other than
`libzzapp.a` and `libgcc.a` appears, for example when `LDLIBS=-lc` brings
newlib back. `make -C sdk/zzapp check` also links a fixture that does this on
purpose and expects the failure. `sdk/zzapp/templates/freestanding.c` is a
worked app with a bump allocator over the heap, a hand-written hash and its
own number formatting. The SDL host runner links the host C library and
ignores the mode, so an app that calls `malloc` runs there and fails to link
for the card. The make include keeps each mode's objects and archive in
its own directory under `BUILD` (`hosted/` and `freestanding/`), so switching
`ZZAPP_FREESTANDING` in one build directory never reuses the other mode's
`crt0.o` or archive members; `make check` builds normal, freestanding and
normal again and compares the last image with a clean build.

## Running on a PC

The runner implements the API 0.4 table with host semantics, including plain
host-memory dynamic heap growth. Decompress, image and audio decode and GL
return `ZZ_ENOSYS` there. Timing, memory limits and the watchdog are not the
card's, so check performance and limits on hardware. Options and the
differences from the card are in `sdk/zzapp/host/README.md`.

## Running with ZZRun

ZZRun is a Shell command and a Workbench tool. On firmware with slot support,
each instance claims its own slot; an unchanged API 0.2 launcher still uses
slot 0. Zorro III only. It needs `zz9k.library` and, for a display, Picasso96.

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

ZZRun `STATUS` CPU time is user execution plus SVC execution. It excludes idle
and recovery intervals.

## zz9k.library client

`zz9k.library` version 2, revision 39 exposes the same runtime without
ZZRun. The ARM-app client first appeared in revision 35; programs that use it
require at least `ZZ9K_LIBRARY_MIN_REVISION_APP_CLIENT`. Include
`proto/zz9k.h`. The calls are `ZZ9KAppOpen`, `ZZ9KAppLoadFile`,
`ZZ9KAppLoadMemory`, `ZZ9KAppStart`, `ZZ9KAppStop`, `ZZ9KAppStatus`,
`ZZ9KAppSendMessage`, `ZZ9KAppGetMessage`, `ZZ9KAppMessageSignal`, and
`ZZ9KAppClose`.

`ZZ9KAppOpen` creates a handle owned by the calling task. Do not use or close
that handle from another task. New firmware gives a handle its claimed slot
and generation; operations affect only that pair. An unchanged library uses
the legacy slot-0 request shape. `ZZ9KAppLoadMemory` accepts at most 16 MiB.
The library's VBlank server renews each open handle's lease. A task that dies
holding a handle is reclaimed by lease expiry or Amiga reset.

`ZZ9KAppStart` passes `argv` unchanged. Put the program name in `argv[0]`
if the app expects the ZZRun convention. Messages are 32 opaque bytes.
`ZZ9KAppGetMessage` returns `ZZ9K_STATUS_BUSY` when the ring is empty.
`notify_port` receives at most one outstanding `ZZ9KAppEvent` (message,
completion, or fault). Before `ZZ9KAppClose`, drain the notify port and call
`ZZ9KAppGetMessage` until it returns `ZZ9K_STATUS_BUSY`. The message signal
is also available without a port.

`zz9k-app-ringtest` and `templates/ringflood.c` are the ring conformance
test. The app starts quiet and answers pings; on request it floods
sequence-numbered, pattern-checked messages and a counter word in a host-visible
block, then goes quiet again. The tool runs an idle phase, the flood for a
number of seconds, and an idle phase again, and reads the counter through a
cache-inhibited mapping. It reports gaps, wrong bytes, backwards or lagging
counter reads and ping round trips, and ends in `PASS` or `FAIL`. Round trips
are kept per phase. The idle figure times the ring and the app's wake-up and
takes a limit (`MAXLAT`). The flood figure is queueing behind a full ring and
has none. The tool polls the ring instead of waiting on the library signal,
which a VBlank server raises, so the signal path can add up to one frame
(20 ms at 50 Hz) to what it measures. See step 12 of `zz9k-release-smoke.md`.

The app side is `zz_msg_poll`, `zz_msg_send`, and `zz_msg_wait`. A full
app-to-host ring returns `ZZ_EBUSY`. Poll does not block. `zz_msg_wait`
parks the app until the ring holds a message or a timeout in microseconds
passes, so a server loop does not spin on `zz_yield`:

```c
while (!zz_should_stop()) {
    int rc = zz_msg_wait(500000u);

    if (rc == ZZ_ENOSYS) {      /* firmware older than API 0.4 */
        zz_yield();
        rc = ZZ_OK;
    }
    if (rc == ZZ_ETIMEDOUT)
        continue;               /* idle work, then wait again */
    if (rc != ZZ_OK)
        break;                  /* stop, or a corrupt ring */
    while (zz_msg_poll(message) == 1)
        handle(message);
}
```

`zz_msg_wait` returns `ZZ_OK` without taking the message, so drain with
`zz_msg_poll`. A parked app is not counted by the 30 second stall watchdog.

## Debugging

On fault, ZZRun prints `pc`, `sp`, `lr`, fault address, status, `cpsr`,
exit code, and generation, then returns 20. `ZZ9KAppStatus` returns the
same fields in `ZZ9KAppStatusInfo`.

Resolve `pc` and `lr` against the app ELF, not the firmware ELF. They are
private virtual addresses in the `0xC0000000` window. Subtract that window
base if `addr2line` expects the ELF's unrelocated VMA.

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

Four apps share core 1 with the system thread. Service work is scheduled
first; runnable apps then rotate on 5 ms quanta. A GL call does not return
until that call's RasterIX work has drained. Between GL calls the engine is
idle and one 68k packet may run. Do not stay inside one GL call for long. A
call over 50 ms is logged. A call over 10 seconds stops the app.

The arena and host-visible allocations are cacheable. The audio ring is not.
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

## Bit-identical maths

A lockstep simulation or a replay needs the same floating-point results on
every run. This section states what the card does and which compiler flags
keep an app inside it. The code generation claims are checked by
`sdk/zzapp/tests/fp_codegen_test.py` (part of `make -C sdk/zzapp check`) with
arm-none-eabi-gcc 15.3.1. The entry value and the denormal behaviour can be
checked on the card with `sdk/zzapp/templates/fpenv.c`.

Entry state. The kernel clears a new thread's whole context
(`zzapp_kernel_prepare_app` in `zzapp_kernel.c`, user mode CPSR `0x10`), and
the first dispatch loads FPSCR and D0 to D31 from it. FPSCR is therefore 0
when `zz_main` starts: round to nearest, flush-to-zero off, default NaN off,
no trapped exceptions, cumulative flags clear. `ZZAPP_ENTRY_FPSCR` in
`zzapp_runtime.h` names that value. Nothing programs it separately; the zero
comes from the cleared frame. Core 1 enables VFP and NEON for user mode once
at start (`core1_loop` in `core2.c` sets the coprocessor access bits and
`FPEXC.EN`).

Per-thread state. Every switch into or out of an app (tick, doorbell, voluntary
yield or sleep, abort, FIQ) goes through `save_frame` and `restore_frame` in
`zzapp_kernel_asm.S`. They push and pop D0 to D31 and FPSCR with the integer
registers. An app that changes its rounding mode keeps the change across
preemption, and no other app sees it. An API call does not reset FPSCR, so
firmware code running inside a call sees yours; restore the entry value
before a call if you changed it.

NEON and VFP differ. Advanced SIMD floating-point arithmetic always flushes
denormal inputs and results to zero and rounds to nearest, whatever FPSCR
holds (Arm ARM DDI 0406C, "Flush-to-zero"; NEON Programmer's Guide DEN0018A,
"Flush-to-zero mode"). Scalar VFP code follows FPSCR, which is 0 on entry, so
it keeps denormals. The same expression on an S register and on a Q register
gives different bits for denormal data. `fpenv.c` shows the two results on
the card; the Arm documents above are the source for the NEON behaviour, and
the card has not been measured here.

Fused multiply-add. The Cortex-A9 implements VFPv3, which has no fused
multiply-add (VFMA arrived with VFPv4). With the SDK's `-mfpu=neon`:

- GCC defines no `__ARM_FEATURE_FMA`, rejects `vfmaq_f32` ("target specific
  option mismatch") and turns `__builtin_fmaf` into a call to `fmaf` in
  libm. `-mfpu=neon-vfpv4` would emit `vfma`, which this CPU cannot run.
- `a * b + c` compiles to `vmla.f32` (S or D registers) and the intrinsic
  `vmlaq_f32` to `vmla.f32` on Q registers. VMLA is a multiply followed by an
  add, and the Arm ARM ("Floating-point exceptions") describes it as two
  operations, so the product is rounded before the add and the result equals
  a separate `vmul` and `vadd`. The generated code is identical for
  `-ffp-contract=off` and `fast`.

Flags. The SDK's flags already keep float code on VFP and IEEE-exact. Keep it
that way:

- Do not use `-ffast-math` or `-funsafe-math-optimizations`. With them,
  `-O3` moves float loops onto Q registers (flush-to-zero) and reassociates
  reductions such as a dot product. Without them the same loops stay scalar
  VFP, in source order.
- Add `-ffp-contract=off` (`CFLAGS := -ffp-contract=off` before the make
  include). It changes nothing on this target. It keeps a build for the PC
  from fusing `a * b + c`, which GCC does by default on hardware with FMA.
- `__FLT_EVAL_METHOD__` is 0, so no operation is evaluated at a wider type.
- GCC folds a call such as `sinf(0.5f)` with a constant argument at build
  time. Compile with `-fno-builtin` (or `-fno-builtin-sinf`) when the result
  must be what the libm in the image computes.
- libm is linked statically from the PIC newlib sysroot, so results match
  for the same sysroot. Pin the SDK image tag, because another newlib
  version can change the last bit of a function such as `sinf`. A freestanding
  app has no libm and brings its own functions.

The SDL host runner is a native build. Use it for logic. Its denormal
handling, fusing and libm differ from the card, so compare bits only on
the card.

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
`sdk/zzapp/templates/memhog.c` is an allocator-limit probe: it touches 1 MiB
blocks until `malloc` returns `NULL`, reports each 16 MiB and low-memory events,
then holds the memory until stopped and frees every block.

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
bump. `ZZAPP_API_MINOR` is 4. Minor 0 published the table through
`gl_unbind`. Minor 1 appended the async flip, refresh, palette, mailbox
counter, and `gl_bind_flags` slots. Minor 2 appended host-visible memory,
file maps, card services, the console, and messages. Minor 3 appended dynamic
heap growth, shrink, and accounting. Minor 4 appends `msg_wait`. No note
macro requires minor 4, so an app that uses `zz_msg_wait` must handle
`ZZ_ENOSYS` on older firmware.

Gate every appended slot with `ZZ_API_COVERS` and a non-NULL function
pointer. `libzzapp` already does this and returns `ZZ_ENOSYS` when the
slot is missing. A direct call that skips the test reads off the end of an
older table.

A breaking change needs a new major. The loader rejects a note major other
than 0. Do not renumber or reorder fields. Do not change the meaning of a
`ZZ_*` value.

The note ABI is 0.0. That number is the loader note, not `ZZAPP_API_MINOR`.

## Licensing

`libzzapp`, `zzapp.h`, the note macro, the linker script, and the templates
are MIT. The firmware, including the runtime and the in-firmware GL build,
is GPL-3.0-or-later. The GL front end that the firmware compiles is
`PolyForm-Noncommercial-1.0.0 OR GPL-3.0-or-later`.

The boundary is the API table. An app links MIT `libzzapp` only. It reaches
GL, files, and the display through that table. It must not be linked
against firmware objects, the GL front-end objects, or `libzzm_rt`. An app
built that way can use any licence, including a closed one: the firmware
licence does not cover programs that use the card through its published
interfaces. [LICENSING.md](../../LICENSING.md) states this for the project.
The firmware sources are not a public header.
