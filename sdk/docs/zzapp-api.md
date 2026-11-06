<!--
SPDX-FileCopyrightText: 2026 Dimitris Panokostas <midwan@gmail.com>
SPDX-License-Identifier: MIT
-->

# ZZApp API reference (0.2)

Public contract of `sdk/zzapp/include/zzapp.h`. The firmware entry is the
`zzapp_api` table passed in `r0` at start. Call the `zz_*` wrappers. Do not
cache a firmware address found any other way.

`ZZAPP_API_MAJOR` is 0 and `ZZAPP_API_MINOR` is 2.
`ZZAPP_API_VERSION` is `(major << 16) | minor`. The loader rejects a note
whose major is not 0. A higher note minor is accepted only when the runtime
minor is at least that value.

The table begins with `size`, then `version`, `flags`, and `reserved0`.
Additions are appended. `size` is the compatibility check. `version` names
the runtime that built the table; it is not a substitute for `size`.

Every call runs on core 1, in the app's SVC context. Core 0 does not execute
the call. A wait yields on core 1 until the launcher completes the matching
mailbox operation, or until stop or timeout. Image decode, audio decode,
SHA-256, and decompress run inline on core 1. They are not queued as core-1
jobs, so they cannot deadlock the worker that is already running the app.

The watchdog stops the app if 30 seconds pass with no API call
(`ZZAPP_APP_STALL_US`), or if one call runs for 10 seconds
(`ZZAPP_API_STUCK_US`). A long compute loop must call the API at least every
few seconds. `zz_yield` is enough. Declared waits renew the stall timer
because they are inside an API call.

`libzzapp` reads a slot only when `ZZ_API_COVERS` is true and the function
pointer is non-NULL. A covered NULL pointer is `ZZ_ENOSYS` for the integer
wrappers. A void wrapper with a missing slot returns without calling
firmware, except `zz_exit`, which does not return. Direct table use must do
the same check. A runtime that predates an appended field has no storage
for it.

Minimum minor below is the header revision that first published the slot.
Minor 0 published the table through `gl_unbind`. Minor 1 appends
`display_flip_async` through `build_id`, plus `ZZ_DISPLAY_MAILBOX` and
`ZZ_GL_BIND_*`. Minor 2 appends `host_address` through `msg_poll`, plus
break events, host-visible allocation, compression, and image/audio decode.
API 0.1 at `4b4e3198` is that minor-1 table. Size-gating still applies to
every appended slot.

## ZZ_API_COVERS

```c
#define ZZ_API_COVERS(api, field) \
    ((api) != 0 && (uint32_t)(api)->size >= \
        (uint32_t)(offsetof(zzapp_api, field) + sizeof((api)->field)))
```

True when `api` is non-NULL and `size` covers `field`. A true result does
not mean the function pointer is non-NULL. Check both before a direct call.
Minimum minor: 0. This is a header macro, not a table slot.

## ZZ_GL_COVERS

```c
#define ZZ_GL_COVERS(gl, field) \
    ((gl) != 0 && (uint32_t)(gl)->size >= \
        (uint32_t)(offsetof(zzapp_gl, field) + sizeof((gl)->field)))
```

Same test for `zzapp_gl`. The v0 GL fields end at `Build2DMipmaps`. Array
and client-state slots are GL ABI v1 and require this test. A wrapper whose
field is past `gl->size` is a no-op or returns 0. Minimum minor: 0.

## Error codes

Integer wrappers return 0 or a negative `ZZ_E*` value. Launcher file errors
are returned as the same codes. SDK service statuses map as: OK to `ZZ_OK`,
no memory to `ZZ_ENOMEM`, unsupported to `ZZ_ENOSYS`, bad request to
`ZZ_EINVAL`, not found to `ZZ_ENOENT`, busy to `ZZ_EBUSY`, and any other
status to `ZZ_EIO`.

## ZZ_OK

`0`. The call completed. A short file read with this code is EOF. A console
read with this code and zero bytes is EOF.

## ZZ_EIO

`-1`. The launcher failed the operation, the window is gone, a transfer
ended early, a flip or console ring is corrupt, or stop arrived during a
wait. `zz_display_open` returns this when ZZRun cannot open the requested
mode and sets the stop word.

## ZZ_EINVAL

`-2`. A required pointer is NULL, a flag or format is unknown, a size is
zero or above the call's limit, or an id does not name an open object.

## ZZ_ENOMEM

`-3`. The arena, the 16 MiB host-visible quota, the 64 host-visible slots,
the 16 file slots, or the four image or audio decode sessions are exhausted.
`zz_alloc` reports this as a NULL pointer, not as this code.

## ZZ_EBUSY

`-4`. A second app is already running, the one display is open, audio is
already open or both fabric slots are live, a flip is still pending, or the
app-to-host message ring is full (64 records).

## ZZ_ENOENT

`-5`. `zz_file_list_next` has no further directory entry. A missing file
from the launcher is returned as the launcher's error, which uses this code
when the path does not exist.

## ZZ_EACCES

`-6`. Reserved for a launcher refusal. The runtime does not invent this
code itself. Relative paths that leave `ROOT`, and paths containing `:`
without `ABS=1`, are rejected by ZZRun before the call returns.

## ZZ_ENOSYS

`-7`. The slot is absent or NULL, the codec or format is not implemented,
or CLUT8 was requested and the launcher did not advertise it. This is the
wrapper result on firmware older than the slot.

## ZZ_ETIMEDOUT

`-8`. A file or directory call waited 30 seconds (`ZZAPP_FILE_TIMEOUT_US`)
without a launcher reply. The runtime retires that request before returning,
so a late reply cannot satisfy the next call.

## ZZ_EFAULT

`-9`. The app faulted during `zz_map_file`. Other calls do not return this;
a fault unwinds the app instead.

## ZZ_ENOSPC

`-10`. `zz_map_file` refuses a file larger than 16 MiB. MP3 decode returns
this when `pcm_frames` is below 1152 or the decoded frame does not fit.
FLAC and Vorbis return this when the PCM capacity calculation would
overflow.

## ZZ_EEXIST

`-11`. Reserved. Current wrappers do not return it. A launcher may.

## ZZ_EBADF

`-12`. The file or directory id is outside 3..18, or that slot is not open.

## Display formats

`format` on a surface is the logical format the app requested. Pixel stores
must follow `layout`. Pitch is in bytes and may be larger than
`width * bytes_per_pixel`.

## ZZ_FMT_RGB565

`1`. Two bytes per pixel. Default layout if the launcher writes none is
`ZZ_LAYOUT_RGB565_BE`. Width and height must be 1..4096.

## ZZ_FMT_BGRA8888

`2`. Four bytes per pixel. Default layout is `ZZ_LAYOUT_BGRA8888`. This is
the only output format `zz_image_open` accepts.

## ZZ_FMT_CLUT8

`3`. One byte per pixel. Default layout is `ZZ_LAYOUT_CLUT8`.
`zz_display_open` returns `ZZ_ENOSYS` unless the launcher set the CLUT8
capability flag. Palette entries are `0x00RRGGBB`.

## ZZ_LAYOUT_RGB565_BE

`1`. RGB565, high byte first.

## ZZ_LAYOUT_RGB565_LE

`2`. RGB565, low byte first.

## ZZ_LAYOUT_BGR565_BE

`3`. BGR565, high byte first.

## ZZ_LAYOUT_BGR565_LE

`4`. BGR565, low byte first.

## ZZ_LAYOUT_BGRA8888

`5`. Bytes B, G, R, A.

## ZZ_LAYOUT_RGBA8888

`6`. Bytes R, G, B, A.

## ZZ_LAYOUT_CLUT8

`7`. One palette index per byte.

## Events

`zz_input_poll` copies native-endian `zzapp_event` records. The launcher
ring is big-endian; firmware converts. The ring holds 256 events. ZZRun
drops a new event rather than blocking when the ring is full, and increments
`input_drops`.

`type` is one of the `ZZ_EV_*` values. `code` is the Amiga raw key with the
up bit removed, or a `ZZ_BREAK_*` value. `dx` and `dy` are mouse deltas, or
joystick axes in `-1, 0, 1`. `buttons` holds key qualifiers, or mouse
buttons in bits 0, 1, and 2 (left, right, middle), or joystick fire in bit
0. `time_ms` is the launcher timestamp.

## ZZ_EV_KEY_DOWN

`1`. Key down. `code` is the raw key.

## ZZ_EV_KEY_UP

`2`. Key up. `code` is the raw key without the Amiga up bit.

## ZZ_EV_MOUSE

`3`. Mouse movement or button change.

## ZZ_EV_JOY

`4`. Joystick. ZZRun reads port 1 while its display window is active.
`JOYPORT0=1` also enables port 0. It reads `JOYxDAT` and the CIA-A fire bits
and does not drive the port pins.

## ZZ_EV_STOP

`5`. Ctrl-C, close, or `APP_STOP`. The stop word is set as well.
`zz_should_stop` becomes true. Grace is 500 ms, then the runtime force-stops.

## ZZ_EV_RESIZE

`6`. Defined. v0 does not emit it. Pause and resume are also unused.

## ZZ_EV_BREAK

`7`. Ctrl-D, Ctrl-E, or Ctrl-F from the display window, in addition to the
normal raw-key event. `code` is `ZZ_BREAK_CTRL_D`, `ZZ_BREAK_CTRL_E`, or
`ZZ_BREAK_CTRL_F`. Minimum minor: 2. Older input producers do not emit it.

## ZZ_BREAK_CTRL_D

`4`. `code` for Ctrl-D on `ZZ_EV_BREAK`.

## ZZ_BREAK_CTRL_E

`5`. `code` for Ctrl-E on `ZZ_EV_BREAK`.

## ZZ_BREAK_CTRL_F

`6`. `code` for Ctrl-F on `ZZ_EV_BREAK`.

## Allocation flags

## ZZ_ALLOC_UNCACHED

`1`. Accepted. Ignored while the arena is a normal cacheable mapping. Pass
it now if the app needs the uncached meaning once user mode exists.
`zz_alloc` without `ZZ_ALLOC_HOST_VISIBLE` does not call the table; it uses
newlib `malloc`.

## ZZ_ALLOC_HOST_VISIBLE

`2`. Allocate from the shared host-visible heap below `0x08200000`, not
from the app arena. The memory is shareable and non-cacheable. Zorro III
readers see stores without an extra cache publish. The per-app limit is
16 MiB, in at most 64 firmware slots. `libzzapp` tracks 64 of those pointers
so `zz_free` can tell them from newlib blocks; a 65th success is freed and
the wrapper returns NULL. Minimum minor for the flag's table path: 2
(`alloc` itself is minor 0; the flag is rejected by a wrapper that does not
understand it, and older firmware has no host-visible path).

## Compression algorithms

`zz_decompress` accepts these values. They match the SDK compression
constants. Unknown values become `ZZ_EINVAL` or `ZZ_ENOSYS` through the SDK
status map. `flags` is passed through. Minimum minor: 2.

## ZZ_COMPRESS_NONE

`0`. No compression. A request for this value is a bad algorithm.

## ZZ_COMPRESS_DEFLATE_RAW

`1`. Raw deflate.

## ZZ_COMPRESS_ZLIB

`2`. Zlib wrapper.

## ZZ_COMPRESS_GZIP

`3`. Gzip wrapper.

## ZZ_COMPRESS_LZ4_BLOCK

`4`. One LZ4 block.

## ZZ_COMPRESS_LZMA_ALONE

`5`. LZMA alone.

## ZZ_COMPRESS_LZMA2

`6`. LZMA2.

## ZZ_COMPRESS_LH1

`7`. LHA LH1.

## ZZ_COMPRESS_LH5

`8`. LHA LH5.

## ZZ_COMPRESS_LH6

`9`. LHA LH6.

## ZZ_COMPRESS_LH7

`10`. LHA LH7.

## File flags and seek

Paths are Amiga paths, at most 255 bytes plus NUL. Relative paths stay under
ZZRun's root: the Shell current directory, the icon directory, or ToolType
`ROOT`. A path containing `:` is rejected unless `ABS=1`. A `..` or leading
`/` that would leave the root is rejected. This is path hygiene, not a
sandbox. File ids are 3..18, sixteen slots. Directory ids use the same
range and a separate table. Transfers use a 256 KiB window and wait up to
30 seconds. newlib `open` is not wired; use these calls or `zz_map_file`.

## ZZ_FILE_READ

`1`. Open for read. ZZRun uses `MODE_OLDFILE`.

## ZZ_FILE_WRITE

`2`. Open for write. ZZRun uses `MODE_READWRITE` unless create is also set.

## ZZ_FILE_CREATE

`4`. Create or replace. ZZRun uses `MODE_NEWFILE`. Combine with
`ZZ_FILE_WRITE`. Any other bit is `ZZ_EINVAL`.

## ZZ_SEEK_SET

`0`. Seek from the start.

## ZZ_SEEK_CUR

`1`. Seek from the current position.

## ZZ_SEEK_END

`2`. Seek from the end. Any other `whence` is `ZZ_EINVAL`.

## Display open flags

## ZZ_DISPLAY_VSYNC

`1`. Ask for a flip that waits for scanout acknowledgement. Bit 0.

## ZZ_DISPLAY_MAILBOX

`2`. Request mailbox presentation. If the launcher and firmware both
advertise it, a flip replaces the pending page and returns the previous
pending page to drawing. Otherwise the open falls back to strict vsync.
Bit 1. The flag is optional; an older launcher ignores it.

## Image codecs

`zz_image_open` takes `codec`. Output format must be `ZZ_FMT_BGRA8888`.
At most four image sessions. Minimum minor: 2.

## ZZ_IMAGE_JPEG

`1`. JPEG.

## ZZ_IMAGE_PNG

`2`. PNG.

## ZZ_IMAGE_WEBP

`4`. WebP. The value is 4, not 3.

## Audio decode codecs

Playback `zz_audio_open` is separate from these decode sessions. At most
four decode sessions. PCM out is interleaved S16LE. Rate and channel count
in the result stay 0 until the codec has parsed a header. Minimum minor: 2.

## ZZ_AUDIO_MP3

`1`. One MPEG frame per call. `flags` must be 0. `pcm_frames` must be at
least 1152. A short or invalid frame is `ZZ_EIO`.

## ZZ_AUDIO_FLAC

`2`. A call may consume a header or partial unit and write no PCM. Keep the
unconsumed tail and feed it again with later bytes. Supply room for stereo
(`pcm_frames * 2` samples) even if the stream later proves mono.

## ZZ_AUDIO_VORBIS

`3`. Same feed rule as FLAC. Feed the Ogg pages from the start of the file.

## ZZ_AUDIO_DECODE_EOF

`1`. Tell FLAC or Vorbis this buffer ends the stream. Illegal on MP3.

## ZZ_AUDIO_DECODE_DRAIN

`2`. Ask FLAC or Vorbis to emit buffered PCM without requiring more input.
Illegal on MP3. Combine with EOF only if both are intended; MP3 rejects any
flag.

## GL bind flags

Passed to `zz_gl_bind_flags`. `zz_gl_bind` passes 0. Minimum minor for the
flags slot: 1, and the slot is appended, so gate with `ZZ_API_COVERS`.

## ZZ_GL_BIND_DIRECT_PRESENT

`1`. Request direct present of the GL target when the runtime supports it.

## ZZ_GL_BIND_SYNC_PRESENT

`2`. Debug. Drain and copy every frame before `zz_gl_swap` returns.

## Types

## zzapp_note

```c
struct zzapp_note {
    uint32_t magic;       /* 0x50505A41 */
    uint32_t abi_major;   /* 0 */
    uint32_t abi_minor;   /* 0 */
    uint32_t stack_bytes; /* 0 = 1 MiB */
    uint32_t flags;       /* bit0 display, bit1 audio, bit2 gl */
    uint32_t reserved;
};
```

ELF note, little-endian, name `ZZAPP`, `n_type` 1, descriptor 24 bytes.
`ZZAPP_NOTE_MAGIC` is `0x50505A41`. `ZZAPP_NOTE_TYPE` is 1.
`ZZAPP_NOTE_FLAG_DISPLAY` is 1, `ZZAPP_NOTE_FLAG_AUDIO` is 2,
`ZZAPP_NOTE_FLAG_GL` is 4. Stack request 0 means 1 MiB. The minimum is
64 KiB. More than 1 MiB is rejected. The note may not request an arena
above 96 MiB. Third-party builds emit this with `ZZAPP_NOTE(flags,
stack_bytes)` in `zzapp_note.h`. A host build compiles that macro to nothing.

## zzapp_surface

```c
typedef struct zzapp_surface {
    uint32_t id;
    uint32_t width, height, pitch, format;
    void    *front;
    void    *back;
    uint32_t card_front;
    uint32_t card_back;
    uint32_t layout;
} zzapp_surface;
```

`front` and `back` are CPU mappings, NULL when the app does not draw that
buffer. `card_front` and `card_back` are ARM physical addresses. One display
is open at a time. After a completed flip, call `zz_display_refresh` before
trusting the pointers again.

## zzapp_event

```c
typedef struct zzapp_event {
    uint16_t type, code;
    int16_t  dx, dy;
    uint16_t buttons, reserved;
    uint32_t time_ms;
} zzapp_event;
```

See the event constants. Native endian in the app.

## zzapp_stat

```c
typedef struct zzapp_stat {
    uint64_t size;
    uint32_t mtime;
    uint32_t mode;        /* 1 file, 2 dir */
} zzapp_stat;
```

`mtime` is Unix seconds from an Amiga `DateStamp`, or 0 when absent.

## zzapp_dirent

```c
typedef struct zzapp_dirent {
    uint32_t mode;
    char     name[256];
} zzapp_dirent;
```

`mode` is 1 for a file and 2 for a directory. `name` is NUL-terminated and
at most 255 bytes.

## zzapp_audio

```c
typedef struct zzapp_audio {
    int16_t *samples;
    uint32_t frames;
    volatile uint32_t *read_frame;
    volatile uint32_t *write_frame;
    uint32_t rate;
    uint32_t channels;
} zzapp_audio;
```

Stereo interleaved S16LE. `frames` is 8192, a power of two. The app advances
`*write_frame`. Firmware advances `*read_frame`. The ring is non-cacheable.
`rate` is the granted rate, which is 22050. `channels` is 2.

## zzapp_mem_info

```c
typedef struct zzapp_mem_info {
    uint32_t arena_size;
    uint32_t arena_used;
    uint32_t arena_free;
    uint32_t arena_largest_block;
    uint32_t host_visible_used;
    uint32_t host_visible_limit;
} zzapp_mem_info;
```

All fields are bytes. `host_visible_limit` is 16 MiB, a per-app cap inside
the shared host heap, not a slice of the 96 MiB arena.

## zzapp_service_result

```c
typedef struct zzapp_service_result {
    uint32_t bytes_consumed;
    uint32_t bytes_written;
    uint32_t flags;
} zzapp_service_result;
```

Filled only when `zz_decompress` returns `ZZ_OK`. Counts are the completed
operation, not estimates.

## zzapp_image_open

```c
typedef struct zzapp_image_open {
    uint32_t codec;
    uint32_t output_format;
    void *tile;
    uint32_t stride;
    uint32_t rows;
    uint32_t tile_bytes;
    uint32_t flags;
} zzapp_image_open;
```

`tile` must hold `stride * rows` bytes and remain valid for every decode
call on the session. `tile_bytes` is that capacity. `output_format` must be
`ZZ_FMT_BGRA8888`.

## zzapp_image_result

```c
typedef struct zzapp_image_result {
    uint32_t image_width;
    uint32_t image_height;
    uint32_t bytes_consumed;
    uint32_t bytes_written;
    uint32_t flags;
} zzapp_image_result;
```

Geometry may be 0 until the codec has a header. `flags` are the SDK image
session result flags.

## zzapp_audio_decode_result

```c
typedef struct zzapp_audio_decode_result {
    uint32_t bytes_consumed;
    uint32_t frames_written;
    uint32_t rate;
    uint32_t channels;
} zzapp_audio_decode_result;
```

`frames_written` counts sample frames, not int16 samples. For stereo, the
PCM buffer holds `frames_written * 2` samples.

## zzapp_gl

Named GL 1.1 function pointers, append-only, `size` first. Integer
parameters are `int32_t` or `uint32_t`. No Hyperion or Khronos header. Do
not pack the struct: the ARM EABI has no padding between `size` and `Begin`,
and a host LP64 build pads for pointer alignment. Both sides of a call must
be the same ABI.

`ZZAPP_GL_VERSION_ARRAYS` is 1. Slots from `VertexPointer` through
`UnlockArrays` are that version. `build_id` is the module source revision,
or NULL on older firmware.

`libzzapp` also declares classic `gl*` and `glu*` entry points. They forward
to the bound table. Do not define them in the app. There is no stdcall.
`gluPerspective` is four doubles, not the 68k `GLfloat` signature.

## zzapp_api

The function table. `flags` is runtime-private. `build_id` is the resident
firmware source revision, or NULL on older firmware. `zzapp_api_ptr` is the
pointer crt0 stored from `r0`. Apps may read it. Do not replace it.

## Calls

## zz_main

```c
int zz_main(int argc, char **argv);
```

The app defines this. crt0 calls it and then `zz_exit` of the returned
status. `argv[0]` from ZZRun is the ELF file name. At most 16 argv entries,
each string at most 127 bytes plus NUL. Not a table slot. Core 1. Runs until
it returns or the runtime force-stops it. Minimum minor: 0. No
`ZZ_API_COVERS` gate.

## zz_exit

```c
void zz_exit(int code);
```

Does not return. Publishes `code` as the app exit status. ZZRun returns that
code on a normal stop, or 20 on fault. Gate: `ZZ_API_COVERS(api, exit)`.
If the slot is missing the wrapper loops. Core 1. Minimum minor: 0.

## zz_yield

```c
void zz_yield(void);
```

Runs one service tick and returns. Use it in an idle loop and at least every
few seconds during a long compute. A spinning `zz_input_poll` does not wait
and does not replace this. Gate: `ZZ_API_COVERS(api, yield)`. Missing slot:
return. Core 1. Does not block past one tick. Minimum minor: 0.

## zz_should_stop

```c
int zz_should_stop(void);
```

Non-zero when the launcher has requested stop. Does not block. Gate:
`ZZ_API_COVERS(api, should_stop)`. Missing slot: `ZZ_ENOSYS`. Core 1.
Minimum minor: 0.

## zz_time_us

```c
uint64_t zz_time_us(void);
```

Monotonic microseconds from the global timer. The APU clock is 666666687 Hz.
The global timer counts at half that rate. Does not block. Gate:
`ZZ_API_COVERS(api, time_us)`. Missing slot: 0. Core 1. Minimum minor: 0.

## zz_sleep_us

```c
void zz_sleep_us(uint32_t us);
```

Yields in tick steps until `zz_time_us` passes the deadline, or stop is
requested. Tick is 5 ms. It is not a busy wait. The sleep holds no GL lock,
so a tick may service one queued 68k MiniGL packet. Gate:
`ZZ_API_COVERS(api, sleep_us)`. Missing slot: return immediately. Core 1.
Blocks. Minimum minor: 0.

## zz_alloc

```c
void *zz_alloc(uint32_t bytes, uint32_t align, uint32_t flags);
```

Without `ZZ_ALLOC_HOST_VISIBLE`, this is newlib `malloc` plus the requested
alignment. `align` must be a power of two and at least `sizeof(void *)`.
Failure is NULL. With `ZZ_ALLOC_HOST_VISIBLE`, the table `alloc` is used.
Firmware also requires `align >= 8` and a power of two, and rejects unknown
flag bits. Host-visible failure is NULL (`ZZ_ENOMEM` conditions above).
Does not block. Gate for the host-visible path: `ZZ_API_COVERS(api, alloc)`.
Core 1. Minimum minor: 0 for arena malloc; 2 to honour
`ZZ_ALLOC_HOST_VISIBLE`.

## zz_free

```c
void zz_free(void *p);
```

NULL is ignored. A pointer returned by a host-visible `zz_alloc` is released
through the table and the quota returns immediately. Any other pointer is
the newlib block saved under the aligned address. Does not block. Gate:
`ZZ_API_COVERS(api, free)` for the host-visible case only. Core 1. Minimum
minor: 0. Remaining host-visible blocks are reclaimed on exit, fault,
core-1 restart, and Amiga reset.

## zz_heap_range

```c
void zz_heap_range(void **base, uint32_t *bytes);
```

Reports the newlib heap span `[image_end, stack_base)`. Either out-pointer
may be NULL. Gate: `ZZ_API_COVERS(api, heap_range)`. Missing slot: writes
NULL and 0. Does not block. Core 1. Minimum minor: 0.

## zz_host_address

```c
int zz_host_address(const void *p, uint32_t *host_offset);
```

Writes the board-relative Zorro III offset of a `ZZ_ALLOC_HOST_VISIBLE`
allocation. Arena pointers and interior pointers are `ZZ_EINVAL`. Does not
block. Gate: `ZZ_API_COVERS(api, host_address)`. Missing slot: `ZZ_ENOSYS`.
Core 1. Minimum minor: 2. Limit: the pointer must be one of the at most 64
host-visible bases.

## zz_mem_info

```c
int zz_mem_info(zzapp_mem_info *out);
```

Fills arena and host-visible accounting. NULL `out` is `ZZ_EINVAL`. Does not
block. Gate: `ZZ_API_COVERS(api, mem_info)`. Missing slot: `ZZ_ENOSYS`.
Core 1. Minimum minor: 2.

## zz_map_file

```c
int zz_map_file(const char *path, const void **data, uint32_t *bytes);
```

Reads a whole proxied file into one host-visible allocation. The pointer is
read-only by contract and stays valid until `zz_unmap_file`, exit, fault,
restart, or reset. Files larger than 16 MiB return `ZZ_ENOSPC`. NULL
arguments are `ZZ_EINVAL`. A stat or open error is returned as that call's
code. A short read is `ZZ_EIO`. Fault during the copy is `ZZ_EFAULT`.
Blocks on each file wait, up to 30 seconds per wait, in chunks of at most
64 KiB. Gate: `ZZ_API_COVERS(api, map_file)`. Missing slot: `ZZ_ENOSYS`.
Core 1. Minimum minor: 2. The allocation counts against the 16 MiB
host-visible quota.

## zz_unmap_file

```c
void zz_unmap_file(const void *data);
```

Releases the host-visible allocation from `zz_map_file`. NULL is ignored.
The same bytes can be mapped again afterwards. Does not block. Gate:
`ZZ_API_COVERS(api, unmap_file)`. Missing slot: return. Core 1. Minimum
minor: 2.

## zz_sha256

```c
int zz_sha256(const void *data, uint32_t bytes, uint8_t digest[32]);
```

Writes 32 bytes. Buffers must be app allocations and remain valid for the
call. NULL `data` or `digest` is `ZZ_EINVAL`. Does not block past the inline
hash. Gate: `ZZ_API_COVERS(api, sha256)`. Missing slot: `ZZ_ENOSYS`. Core 1.
Minimum minor: 2.

## zz_decompress

```c
int zz_decompress(uint32_t algorithm, uint32_t flags,
                  const void *src, uint32_t src_bytes,
                  void *dst, uint32_t dst_bytes,
                  zzapp_service_result *out);
```

Synchronous inflate of one buffer. `out` is written only on `ZZ_OK`. NULL
`src`, `dst`, or `out` is `ZZ_EINVAL`. Does not queue a worker job. Gate:
`ZZ_API_COVERS(api, decompress)`. Missing slot: `ZZ_ENOSYS`. Core 1.
Minimum minor: 2. Algorithm values are `ZZ_COMPRESS_*`.

## zz_image_open

```c
int zz_image_open(const zzapp_image_open *open, uint32_t *session,
                  zzapp_image_result *out);
```

Starts one tile-buffer session and writes `*session`. A fifth session is
`ZZ_ENOMEM`. NULL required pointers, or a zero stride, row count, or tile
size, is `ZZ_EINVAL`. Output format other than `ZZ_FMT_BGRA8888` is
`ZZ_EINVAL`. Does not block past decoder setup. Gate:
`ZZ_API_COVERS(api, image_open)`. Missing slot: `ZZ_ENOSYS`. Core 1.
Minimum minor: 2. Limit: four sessions. The tile buffer stays caller-owned.

## zz_image_decode

```c
int zz_image_decode(uint32_t session, const void *src, uint32_t src_bytes,
                    uint32_t flags, zzapp_image_result *out);
```

Feeds one buffer to an open session. Unknown session, or NULL `src` or
`out`, is `ZZ_EINVAL`. `src` must remain valid for the call. Does not block
past the inline decode. Gate: `ZZ_API_COVERS(api, image_decode)`. Missing
slot: `ZZ_ENOSYS`. Core 1. Minimum minor: 2.

## zz_image_close

```c
int zz_image_close(uint32_t session);
```

Ends one session and frees its slot. Unknown session is `ZZ_EINVAL`. Normal
exit and reset close every tracked session. A core-1 fault poisons decoder
state after reclaim and does not run destructors. Does not block past
close. Gate: `ZZ_API_COVERS(api, image_close)`. Missing slot: `ZZ_ENOSYS`.
Core 1. Minimum minor: 2.

## zz_audio_decode_open

```c
int zz_audio_decode_open(uint32_t codec, uint32_t *session);
```

Opens an MP3, FLAC, or Vorbis session. Other codec values, or a NULL
`session`, are `ZZ_EINVAL`. A fifth session is `ZZ_ENOMEM`. Does not block.
Gate: `ZZ_API_COVERS(api, audio_decode_open)`. Missing slot: `ZZ_ENOSYS`.
Core 1. Minimum minor: 2. Limit: four sessions.

## zz_audio_decode

```c
int zz_audio_decode(uint32_t session, const void *src, uint32_t src_bytes,
                    uint32_t flags, int16_t *pcm, uint32_t pcm_frames,
                    zzapp_audio_decode_result *out);
```

Consumes the prefix it reports and writes interleaved S16LE. Unknown
session, NULL `pcm` or `out`, `pcm_frames` of 0, `src` NULL with a non-zero
length, or an unknown flag, is `ZZ_EINVAL`. MP3 also rejects flags and a
frame capacity below 1152 (`ZZ_ENOSPC`). FLAC and Vorbis may return `ZZ_OK`
with `frames_written` 0. Does not block past the inline decode. Gate:
`ZZ_API_COVERS(api, audio_decode)`. Missing slot: `ZZ_ENOSYS`. Core 1.
Minimum minor: 2. Provide `pcm_frames * 2` samples of room.

## zz_audio_decode_close

```c
int zz_audio_decode_close(uint32_t session);
```

Releases one decode session on core 1. Unknown session is `ZZ_EINVAL`. A
core-1 restart drops the ledger and does not call destructors. Does not
block past release. Gate: `ZZ_API_COVERS(api, audio_decode_close)`. Missing
slot: `ZZ_ENOSYS`. Core 1. Minimum minor: 2.

## zz_log

```c
void zz_log(const char *msg, uint32_t len);
```

Copies one diagnostic record into the launcher log ring and, when the UART
budget allows, prints `[app] ` plus the text. NULL `msg` is ignored. Length
above 1024 is truncated. The ring accepts 32 records per second. The UART
accepts 8 lines per second. Excess increments `log_drops` and is discarded.
This is not stdout. Does not block. Gate: `ZZ_API_COVERS(api, log)`.
Missing slot: return. Core 1. Minimum minor: 0. There is no other path from
the app to the firmware console.

## zz_file_open

```c
int zz_file_open(const char *path, uint32_t flags);
```

Returns a file id, or a negative code. See `ZZ_FILE_*` and the path rules.
Empty path or unknown flags: `ZZ_EINVAL`. No free slot: `ZZ_ENOMEM`. No
window: `ZZ_EIO`. Blocks until the launcher replies, stop (`ZZ_EIO`), or
30 seconds (`ZZ_ETIMEDOUT`). Gate: `ZZ_API_COVERS(api, file_open)`. Missing
slot: `ZZ_ENOSYS`. Core 1. Minimum minor: 0. Limit: 16 files, path 255 bytes.

## zz_file_close

```c
int zz_file_close(int fd);
```

Closes one file id. A bad id is `ZZ_EBADF`. The slot is released even when
the launcher returns an error. Blocks as `zz_file_open` does. Gate:
`ZZ_API_COVERS(api, file_close)`. Missing slot: `ZZ_ENOSYS`. Core 1.
Minimum minor: 0.

## zz_file_read

```c
int zz_file_read(int fd, void *buf, uint32_t len, uint32_t *out_n);
```

Reads up to `len` bytes. `*out_n` is the count obtained. A short read with
`ZZ_OK` is EOF. NULL `out_n` is `ZZ_EINVAL`. A bad id or NULL buffer is
`ZZ_EBADF`. Each chunk is at most 256 KiB and each chunk waits. Gate:
`ZZ_API_COVERS(api, file_read)`. Missing slot: `ZZ_ENOSYS`. Core 1.
Minimum minor: 0.

## zz_file_write

```c
int zz_file_write(int fd, const void *buf, uint32_t len, uint32_t *out_n);
```

Writes up to `len` bytes. A short write means the launcher stopped early;
call again with the remainder or treat it as an error. NULL `out_n` is
`ZZ_EINVAL`. NULL `buf` with non-zero `len` is `ZZ_EBADF`. Chunk and wait
rules match read. Gate: `ZZ_API_COVERS(api, file_write)`. Missing slot:
`ZZ_ENOSYS`. Core 1. Minimum minor: 0.

## zz_file_seek

```c
int zz_file_seek(int fd, int64_t off, int whence, int64_t *out_pos);
```

Sets the file position. `out_pos` may be NULL. `whence` must be a
`ZZ_SEEK_*` value. Blocks for one launcher reply. Gate:
`ZZ_API_COVERS(api, file_seek)`. Missing slot: `ZZ_ENOSYS`. Core 1.
Minimum minor: 0.

## zz_file_stat

```c
int zz_file_stat(const char *path, zzapp_stat *out);
```

Fills size, mtime, and mode. NULL `out` is `ZZ_EINVAL`. Does not open a
slot. Blocks for one launcher reply. Gate: `ZZ_API_COVERS(api, file_stat)`.
Missing slot: `ZZ_ENOSYS`. Core 1. Minimum minor: 0.

## zz_file_list_open

```c
int zz_file_list_open(const char *path);
```

Opens a directory and returns a list id in the same numeric range as file
ids, from a separate table of 16. Blocks as open does. Gate:
`ZZ_API_COVERS(api, file_list_open)`. Missing slot: `ZZ_ENOSYS`. Core 1.
Minimum minor: 0.

## zz_file_list_next

```c
int zz_file_list_next(int list, zzapp_dirent *out);
```

Copies the next name. No further entry is `ZZ_ENOENT`. NULL `out` or a bad
id is `ZZ_EBADF`. Names longer than 255 bytes are truncated. Blocks for one
launcher reply. Gate: `ZZ_API_COVERS(api, file_list_next)`. Missing slot:
`ZZ_ENOSYS`. Core 1. Minimum minor: 0.

## zz_file_list_close

```c
int zz_file_list_close(int list);
```

Closes a directory id. Blocks for one launcher reply. Gate:
`ZZ_API_COVERS(api, file_list_close)`. Missing slot: `ZZ_ENOSYS`. Core 1.
Minimum minor: 0.

## zz_display_open

```c
int zz_display_open(uint32_t w, uint32_t h, uint32_t format,
                    uint32_t flags, zzapp_surface *out);
```

Asks ZZRun to open a P96 screen and bind it. Width and height must be
1..4096. Format must be a `ZZ_FMT_*` value. A second open is `ZZ_EBUSY`.
CLUT8 without launcher support is `ZZ_ENOSYS`. NULL `out` or no window is
`ZZ_EINVAL`. Blocks until bind succeeds or stop (`ZZ_EIO`). On success,
`out` has id, pitch, layout, and CPU mappings. One display. Gate:
`ZZ_API_COVERS(api, display_open)`. Missing slot: `ZZ_ENOSYS`. Core 1.
Minimum minor: 0. `ZZ_DISPLAY_MAILBOX` is a request, not a guarantee.

## zz_display_flip

```c
int zz_display_flip(uint32_t surface_id);
```

Presents the back buffer and waits until `flip_ack` matches the new
generation, or stop (`ZZ_EIO`). A flip already pending is `ZZ_EBUSY`. A bad
id is `ZZ_EINVAL`. Mailbox mode returns the replaced page to drawing and
does not wait for vblank inside this call when mailbox is active; the
vsync path waits. Core 1. Gate: `ZZ_API_COVERS(api, display_flip)`. Missing
slot: `ZZ_ENOSYS`. Minimum minor: 0. Flush presented pixels before calling;
firmware also cleans the presented range through L1 and L2 before queueing
a mailbox flip.

## zz_display_flip_async

```c
int zz_display_flip_async(uint32_t surface_id);
```

Queues a flip and returns. `ZZ_OK`, or a negative code. Does not wait for
scanout. A flip still pending is `ZZ_EBUSY`. Gate:
`ZZ_API_COVERS(api, display_flip_async)`. Missing slot: `ZZ_ENOSYS`. Core 1.
Minimum minor: 1. The slot is appended; older firmware has no storage for it.

## zz_display_flip_idle

```c
int zz_display_flip_idle(void);
```

Returns 1 when the last queued flip has been shown and the back buffer may
be written. Returns 0 while a flip is pending. Negative on error, including
`ZZ_ENOSYS` on older firmware. Does not block. Gate:
`ZZ_API_COVERS(api, display_flip_idle)`. Core 1. Minimum minor: 1.

## zz_display_refresh

```c
int zz_display_refresh(zzapp_surface *surface);
```

Reloads front, back, pitch, and layout after a completed flip. Call it when
`zz_display_flip_idle` reports idle, or after `zz_display_flip` returns.
A mismatched id is `ZZ_EINVAL`. A flip still pending is `ZZ_EBUSY`. Does not
block. Gate: `ZZ_API_COVERS(api, display_refresh)`. Missing slot:
`ZZ_ENOSYS`. Core 1. Minimum minor: 1. Appended slot.

## zz_display_mailbox_replaced

```c
uint32_t zz_display_mailbox_replaced(void);
```

Cumulative frames replaced before card vblank in mailbox mode. Zero when
mailbox is inactive or the slot is absent. Does not block. Gate:
`ZZ_API_COVERS(api, display_mailbox_replaced)`. Core 1. Minimum minor: 1.
Appended slot.

## zz_display_set_palette

```c
int zz_display_set_palette(uint32_t surface_id, uint32_t first,
                           uint32_t count, const uint32_t *rgb);
```

Writes `count` CLUT8 entries as `0x00RRGGBB` starting at `first`. The
surface must be CLUT8. `first + count` must be within 0..256, and `count`
must be non-zero. Does not block. Gate:
`ZZ_API_COVERS(api, display_set_palette)`. Missing slot: `ZZ_ENOSYS`.
Core 1. Minimum minor: 1. Appended slot. ZZRun loads changed entries from
the palette window.

## zz_display_vsync

```c
int zz_display_vsync(void);
```

Records a vsync request and yields once. Does not wait for the next raster
line. No bound display is `ZZ_EINVAL`. Gate:
`ZZ_API_COVERS(api, display_vsync)`. Missing slot: `ZZ_ENOSYS`. Core 1.
Minimum minor: 0.

## zz_display_close

```c
int zz_display_close(uint32_t surface_id);
```

Drops the bound display and any runtime-owned back buffer. A bad id is
`ZZ_EINVAL`. Does not block. Gate: `ZZ_API_COVERS(api, display_close)`.
Missing slot: `ZZ_ENOSYS`. Core 1. Minimum minor: 0. ZZRun still closes the
screen it opened on exit, fault, or reset.

## zz_input_poll

```c
int zz_input_poll(zzapp_event *out, uint32_t max, uint32_t *out_n);
```

Copies up to `max` queued events. `*out_n` is the count. An empty ring
returns `ZZ_OK` and 0. Does not block. NULL `out_n` is `ZZ_EINVAL`. NULL
`out` with non-zero `max` is `ZZ_EINVAL`. No window returns 0. If the
producer overran the 256-slot ring, firmware skips to the newest 256.
Gate: `ZZ_API_COVERS(api, input_poll)`. Missing slot: `ZZ_ENOSYS`. Core 1.
Minimum minor: 0. Idle with `zz_yield` or `zz_sleep_us`, not a spin.

## zz_audio_open

```c
int zz_audio_open(uint32_t rate, uint32_t channels, zzapp_audio *out);
```

Opens the one PCM stream. `channels` must be 2. `rate` must be 22050.
Anything else is `ZZ_EINVAL`. A second open, or both fabric slots live, is
`ZZ_EBUSY`. NULL `out` is `ZZ_EINVAL`. The ring is 8192 stereo S16LE frames
in non-cacheable memory. Default route is the card fabric (slot 2 if free,
else slot 1). `AUDIO=AHI` selects the launcher AHI route before start; ZZRun
accepts it only when AHI grants that exact stereo rate and converts S16LE
to S16BE without resampling. Does not block past attach. Gate:
`ZZ_API_COVERS(api, audio_open)`. Missing slot: `ZZ_ENOSYS`. Core 1.
Minimum minor: 0. 11025 is not granted on this path.

## zz_audio_close

```c
int zz_audio_close(void);
```

Detaches the stream, zeros the ring header, and sets route 0 so ZZRun drops
pending AHI completions before a new stream. Not open is `ZZ_EINVAL`. Does
not block past detach. Gate: `ZZ_API_COVERS(api, audio_close)`. Missing
slot: `ZZ_ENOSYS`. Core 1. Minimum minor: 0.

## zz_console_write

```c
int zz_console_write(const void *buf, uint32_t len, uint32_t *out_n);
```

Copies every requested byte to the console-output ring, or returns an error.
A full ring is backpressure: the call waits and yields. No byte is dropped.
Stop during that wait is `ZZ_EIO`. A corrupt head/tail is `ZZ_EIO`. NULL
`out_n`, or NULL `buf` with non-zero `len`, is `ZZ_EINVAL`. The payload
capacity is 32 KiB (`ZZAPP_CONSOLE_OUT_CAP`). The first console call sets
the console flag so a Workbench launch opens `CON:0/0/640/160/ZZRun/CLOSE/WAIT`.
If that open fails, ZZRun stops the app. newlib `write` of fd 1 and fd 2
uses this call. Gate: `ZZ_API_COVERS(api, console_write)`. Missing slot:
`ZZ_ENOSYS`. Core 1. Blocks on backpressure. Minimum minor: 2.

## zz_console_read

```c
int zz_console_read(void *buf, uint32_t len, uint32_t *out_n);
```

Waits until the launcher has bytes, then copies one contiguous span, at most
`len` and at most the 4 KiB input capacity. `ZZ_OK` with `*out_n` 0 is EOF.
`len` 0 returns `ZZ_OK` without waiting. Stop while waiting is `ZZ_EIO`.
While the call waits on an empty ring it sets the stdin-wanted word. ZZRun
reads an interactive console only while that word is set, with an
asynchronous `ACTION_READ` in normal line mode. On OS 3.2, Ctrl-\ does not
end keyboard input, including for `Type *`. Use a redirected handle
(`ZZRun app.elf <file`) or Ctrl-C. A redirected input publishes EOF after
the last byte. newlib `read` of fd 0 uses this call and returns 0 at EOF.
Gate: `ZZ_API_COVERS(api, console_read)`. Missing slot: `ZZ_ENOSYS`. Core 1.
Blocks until data, EOF, or stop. Minimum minor: 2.

## zz_msg_send

```c
int zz_msg_send(const void *msg);
```

Copies one opaque 32-byte record to the app-to-host ring. NULL is
`ZZ_EINVAL`. A full ring (64 records) is `ZZ_EBUSY`; the call does not wait.
A corrupt ring is `ZZ_EIO`. The owning Amiga client is signalled. Does not
block. Gate: `ZZ_API_COVERS(api, msg_send)`. Missing slot: `ZZ_ENOSYS`.
Core 1. Minimum minor: 2. The record size is fixed. Shorter messages are
not a different API; the caller pads to 32 bytes.

## zz_msg_poll

```c
int zz_msg_poll(void *msg);
```

Copies one 32-byte host-to-app record and returns 1. Returns 0 when the ring
is empty. NULL is `ZZ_EINVAL`. A corrupt ring is `ZZ_EIO`. Does not block.
Gate: `ZZ_API_COVERS(api, msg_poll)`. Missing slot: `ZZ_ENOSYS`. Core 1.
Minimum minor: 2. Capacity is 64 records. This ring is not keyboard input.

## zz_gl_bind

```c
const zzapp_gl *zz_gl_bind(uint32_t surface_id);
```

Binds the one GL context to the open surface and returns the table. Failure,
including a second bind before unbind, returns NULL. Equivalent to
`zz_gl_bind_flags(surface_id, 0)`. Core 1. Does not block past context
setup. Gate: `ZZ_API_COVERS(api, gl_bind)`. Missing slot: NULL. Minimum
minor: 0.

## zz_gl_bind_flags

```c
const zzapp_gl *zz_gl_bind_flags(uint32_t surface_id, uint32_t flags);
```

Same as `zz_gl_bind`, plus `ZZ_GL_BIND_*` flags. If the appended slot is
absent, the wrapper falls back to `gl_bind` when `flags` is 0, and returns
NULL when flags were requested. Gate: `ZZ_API_COVERS(api, gl_bind_flags)`
for a non-zero flags request. Core 1. Minimum minor: 1. Appended slot.

## zz_gl_swap

```c
void zz_gl_swap(void);
```

Flushes the GL front end, drains RasterIX, copies the context target into
the bound P96 bitmap, and flips. It blocks as `zz_display_flip` does. It
does not retarget RasterIX at the scanout address. Gate:
`ZZ_API_COVERS(api, gl_swap)`. Missing slot: return. Core 1. Minimum
minor: 0. A GL call that does not return is subject to the 10 second stuck
call limit. Calls longer than 50 ms are logged; they are not preempted.

## zz_gl_unbind

```c
void zz_gl_unbind(void);
```

Releases the app GL context. Other 68k MiniGL contexts are left alone. Does
not block past unbind. Gate: `ZZ_API_COVERS(api, gl_unbind)`. Missing slot:
return. Core 1. Minimum minor: 0.
