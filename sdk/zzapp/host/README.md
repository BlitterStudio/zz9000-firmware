# zzapp-host

`libzzapp-host` runs a native build of a ZZ9000 app. SDL2 stands in for the
card. The app includes `zzapp.h`, defines `zz_main`, and links this library.
The library provides `main()`, the `zz_*` wrappers, and a fully populated
API 0.2 table. It does not link the ARM `crt0`, `syscalls.c`, or `gl_wrap.c`.

## Build

Linux (CI uses this):

```sh
sudo apt-get install cmake libsdl2-dev python3
cmake -S sdk/zzapp/host -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

macOS: `brew install cmake sdl2 pkg-config python3`, then the same cmake
commands. Windows: install SDL2 (vcpkg `sdl2` or a CMake package) and a C11
compiler, then the same commands. The sources avoid POSIX-only calls without
a Windows fallback. CI itself is Ubuntu only.

A third-party app in its own CMake project:

```cmake
add_subdirectory(sdk/zzapp/host)
zzapp_host_executable(myapp SOURCES myapp.c)
```

`myapp.c` defines `zz_main`. Do not define `main`.

## Run

```text
./pixels --root . --scale 2
./filter --root "$PWD" -- in.bin out.bin
./console -- one
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  ./pixels --headless --frames 2 --dump-front frame.bin
```

`argv[0]` passed to `zz_main` is this program's name. Arguments after the
runner options, or after `--`, are the app's arguments.

## Options

| Option | Meaning |
| --- | --- |
| `--root DIR` | Root for relative app paths. Default `.`. |
| `--abs` | Allow a path that contains `:`. The root check does not apply. |
| `--scale N` | Window scale, 1..8. The surface stays at the app's size. |
| `--headless` | Force SDL's dummy video and audio drivers. No visible window. |
| `--frames N` | Set the stop flag after N presented frames. |
| `--input FILE` | Scripted input events, injected before `zz_main`. |
| `--audio-null` | Consume PCM on `zz_yield` instead of opening SDL audio. |
| `--audio-frames N` | Stop after N consumed PCM frames. |
| `--audio-dump FILE` | Write consumed S16LE stereo frames (`--audio-null`). |
| `--msg-script FILE` | Inject host-to-app messages. |
| `--msg-out FILE` | Record app-to-host messages as 64 hex digits per line. |
| `--dump-front FILE` | Write the last presented surface, raw bytes, `pitch * height`. |
| `--help` | Print the option list. |

Runner paths (`--root`, scripts, dumps) are host paths. App paths go through
the file API.

On exit the runner prints `zzapp-host: exit=<code> flips=<n> audio_frames=<n>`
to stderr.

### Input script

```text
# comment
key down 0x22 8
key up 34
mouse 2 -1 1
mousebtn 1 down
joy 1 -1 0 1
break 4
resize 320 240
stop
```

`key` takes an Amiga raw code and optional qualifier bits. `break` is
`ZZ_EV_BREAK` (`4`, `5`, `6` for Ctrl-D/E/F). `stop` sets the stop flag and
enqueues `ZZ_EV_STOP`.

### Message script

Without `--msg-script`, `zz_msg_send` loops back into the queue `zz_msg_poll`
reads. With a script, lines are host-to-app messages and sends are recorded
instead of fed back. A line is 64 hex digits, a quoted string padded with
NULs to 32 bytes, or `stop`. `stop` requests stop once every injected message
has been sent back. Both rings hold 64 messages; a full send returns
`ZZ_EBUSY`.

## Keys

SDL scancodes become Amiga raw codes. Key-up is `ZZ_EV_KEY_UP`; the up bit is
not stored in `code`. Qualifiers are Amiga `IEQUALIFIER` bits in `buttons`:
left/right shift `0x0001`/`0x0002`, caps `0x0004`, control `0x0008`, left/right
alt `0x0010`/`0x0020`, left/right Amiga (GUI) `0x0040`/`0x0080`, numeric pad
`0x0100`, repeat `0x0200`.

Ctrl-D/E/F (raw `0x22`/`0x12`/`0x23`) also enqueue `ZZ_EV_BREAK` with codes
`4`/`5`/`6`, on key-down only. Window close and Ctrl-C enqueue one
`ZZ_EV_STOP` and set the stop flag. Unmapped scancodes are ignored. The queue
holds 256 events and drops the newest when full.

| SDL | Raw | SDL | Raw |
| --- | --- | --- | --- |
| `` ` `` | `0x00` | A S D F G H J K L | `0x20`–`0x28` |
| 1–0 | `0x01`–`0x0A` | `;` `'` | `0x29` `0x2A` |
| `-` `=` `\` | `0x0B` `0x0C` `0x0D` | ISO `\` | `0x30` |
| keypad 0 | `0x0F` | Z–M | `0x31`–`0x37` |
| Q–P | `0x10`–`0x19` | `,` `.` `/` | `0x38`–`0x3A` |
| `[` `]` | `0x1A` `0x1B` | keypad `.` 7 8 9 | `0x3C`–`0x3F` |
| keypad 1 2 3 | `0x1D`–`0x1F` | space backspace tab | `0x40`–`0x42` |
| keypad 4 5 6 | `0x2D`–`0x2F` | keypad enter, return, esc, del | `0x43`–`0x46` |
| arrows up down right left | `0x4C`–`0x4F` | F1–F10 | `0x50`–`0x59` |
| keypad `-` `/` `*` `+` | `0x4A` `0x5C` `0x5D` `0x5E` | shifts, caps, ctrl, alts, GUI | `0x60`–`0x67` |

Both Control keys are raw `0x63`. SDL joystick 0 is Amiga port 1 (the game
port); joystick 1 is port 0. `dx`/`dy` are `-1`, `0`, or `1`. Fire is button
0 in `buttons` bit 0. Mouse movement is deltas. Mouse button codes are 1, 2,
3 for left, right, middle, and `buttons` bits 0, 1, 2 match.

## Differences from the card

- Headless vsync does not wait for a 50/60 Hz blank. A windowed renderer uses
  SDL vsync when the driver provides it. `zz_sleep_us` is host-clock sleep,
  not the 5 ms firmware tick.
- `zz_log` is printed immediately. The card drops lines above 32 per second.
- Pitch is `width * bytes per pixel`. The card's pitch is the P96 stride.
  Apps must use `surface.pitch`. Default layouts match ZZRun: RGB565 is
  `ZZ_LAYOUT_RGB565_BE`, 32-bit is `ZZ_LAYOUT_BGRA8888`, 8-bit is
  `ZZ_LAYOUT_CLUT8`. Palette entries are `0x00RRGGBB`.
- One display. `ZZ_DISPLAY_MAILBOX` uses three buffers: a flip replaces the
  pending page and `zz_display_flip_idle` stays 1. Without mailbox, an async
  flip stays pending until `zz_yield` or `zz_display_vsync`.
- Audio grants only 22050 Hz stereo, 8192 frames, S16LE, matching the fabric
  producer. `ZZ_ALLOC_UNCACHED` is accepted and ignored. Host-visible memory
  is ordinary process memory under a 16 MiB quota. `zz_host_address` returns
  a synthetic offset from `0x033F0000`, not a Zorro address, and rejects
  interior pointers. `zz_map_file` reads at most 16 MiB; a larger file is
  `ZZ_ENOSPC`, and a zero-length file is `ZZ_ENOMEM`.
- `printf` uses the host C library. On the card, newlib routes fd 1 and fd 2
  through one console ring. `zz_console_write` writes stdout; `zz_console_read`
  reads stdin and returns `ZZ_OK` with zero bytes at EOF.
- SHA-256 is implemented. `zz_decompress`, `zz_image_open` / `zz_image_decode`
  / `zz_image_close`, and `zz_audio_decode_open` / `zz_audio_decode` /
  `zz_audio_decode_close` return `ZZ_ENOSYS`. The in-tree decoders are GPL
  firmware glue, downloaded third-party trees, or an LHA decoder with its own
  licence. GL is parked: `zz_gl_bind` returns NULL, and `glBegin` is not linked.
