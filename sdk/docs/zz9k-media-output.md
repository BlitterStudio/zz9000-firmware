# Shared-buffer output of a media session

A media session can write each presented picture into memory the Amiga can
read, instead of showing it on the ZZ9000 display. The card scales the picture
to the size the client asks for and converts it to INDEX8, bitplanes, RGB565
or RGB555. A program can draw the result itself, copy it into a native AGA
screen, or hand it to another graphics card. Timing, audio and the decode
loop are the ones of the media session described in
[zz9k-library.md](zz9k-library.md#streaming-video-sessions).

The request came from luvwagn in issue 238. The constants and layouts follow
the proposal in that issue. Where this document differs, it says so.

## Discovery

The video service flags word has no free bit. Firmware that has the output
lists 15 opcodes: `opcode_count >= ZZ9K_VIDEO_OPCODE_COUNT_SHARED_OUTPUT`
means `ZZ9K_OP_MEDIA_SESSION_OUTPUT_CONFIG` (`0x0b0e`) exists.
`zz9k_video_shared_output_available()` in `zz9k/host.h` tests it.

Firmware without the output answers BEGIN with `output_format = 2` with
`ZZ9K_STATUS_BAD_REQUEST` (the proposal expected `UNSUPPORTED`). Treat both as
"not available". Status page 6 reports the output flags the firmware accepts in
`value[3]`, so a client can check a flag before it asks for it. Today that mask
leaves out `ZZ9K_MEDIA_OUTPUT_DITHER_DIFFUSION`.

## Calls

1. `zz9k_media_session_begin()` with
   `output_format = ZZ9K_VIDEO_OUTPUT_SHARED_BUFFER`. The session does not use
   the overlay: presentation page 4 reports it as not `OWNED`, and PRESENT never
   waits for an overlay. `width` and `height` are the size of the stream, as
   for an overlay session.
2. `zz9k_media_session_output()` (`ZZ9K_OP_MEDIA_SESSION_OUTPUT_CONFIG`,
   `ZZ9KMediaSessionOutputDesc`) after BEGIN and before the first PRESENT. It
   may be sent again between pictures, for example after a resize. It answers
   `ZZ9K_STATUS_BUSY` while a picture is being written. The reply is the main
   result with `bytes_written` set to the size of one picture. After a new
   configuration no picture counts as complete and the next picture goes to
   slot 0.
3. DECODE and PRESENT as for an overlay session. A PRESENT before OUTPUT_CONFIG
   answers `ZZ9K_STATUS_BAD_REQUEST`.

PRESENT queues the conversion on core 1, behind the session's DECODE, and
releases the decoder frame at once. The reply carries `PRESENTED`,
`ZZ9K_MEDIA_SESSION_RESULT_OUTPUT_PENDING` and, when the picture goes to slot 1,
`ZZ9K_MEDIA_SESSION_RESULT_OUTPUT_SLOT1`. `bytes_written` is the size of one
picture. The picture is complete when status page 6
(`ZZ9K_MEDIA_STATUS_OUTPUT`) no longer has `ZZ9K_MEDIA_OUTPUT_STATUS_PENDING`.
Core 0 publishes the slot at that point, after core 1 has flushed it from the
L1 and L2 caches, so a client that reads a slot only after it is complete sees
the whole picture. PRESENT answers `BUSY` while the previous picture is still
being written, and CLOSE answers `BUSY` for the same reason.

I added `PENDING` to the status page and put counters and the conversion time
in its values. The proposal had only `CONFIGURED`, `COMPLETE` and `SLOT1`. With
`PENDING` a client does not have to count its own PRESENT calls.

| Page 6 field | Meaning |
| --- | --- |
| `flags` | `CONFIGURED`, `COMPLETE` (a picture of the current layout is written), `SLOT1` (the newest complete picture is in slot 1), `PENDING` |
| `value[0]` | Pictures written since BEGIN |
| `value[1]` | Pictures presented since BEGIN |
| `value[2]` | Conversion time of the newest written picture, in microseconds, as core 1 measured it |
| `value[3]` | Mask of `ZZ9KMediaOutputFlags` the firmware accepts |

An overlay session answers the page with every field zero.

## Slots

Slot 0 starts at `buffer_offset` in the buffer named by `buffer_handle`. Slot 1
starts at `buffer_offset + slot_stride`; `slot_stride = 0` means one slot. Both
are multiples of 4, and the stride is at least the size of one picture. With
two slots the pictures alternate 0, 1, 0, 1 from the first picture after a
configuration, so the client can read the newest complete slot while the card
writes the other. With one slot the card overwrites it, and the client must not
read while `PENDING` is set.

The buffer must be readable by the 68k:

| Bus | Buffer |
| --- | --- |
| Zorro III | Any shared buffer that is not `ZZ9K_ALLOC_CARD_ONLY`. `zz9k_alloc_shared()` drops `ZZ9K_ALLOC_HOST_WINDOW` on Zorro III, so a plain allocation comes from the mapped shared heap and slots larger than the 64 KiB host window work. A firmware allocation that does ask for the window gets at most those 64 KiB. |
| Zorro II | A `ZZ9K_ALLOC_HOST_WINDOW` buffer. The generation-2 window is 16 KiB on the 2 MiB and 4 MiB layouts and 80 KiB on the 8 MiB layout (see [zz9k-zorro2-services.md](zz9k-zorro2-services.md)), shared with every other client. 160x100 in 5 planes is 10,000 bytes a slot, so one slot fits the 16 KiB window and two do not. |

Firmware refuses a buffer that is card-only or media-class, and on Zorro II one
that is not in the host window, with `ZZ9K_STATUS_BAD_REQUEST`, as it does a
range that does not hold `buffer_offset` plus the slots. The buffer and the
palette buffer stay pinned until the output is bound again or the session
closes, so `FREE_SHARED` answers `BUSY` for them. The Zorro II path has not been
run on hardware.

## Formats

| Format | One picture | Layout |
| --- | --- | --- |
| `ZZ9K_SURFACE_FORMAT_INDEX8` | `pitch * height` | One pen per byte, below `1 << depth` |
| `ZZ9K_SURFACE_FORMAT_PLANAR` | `pitch * height * depth` | The leftmost pixel is the top bit of a byte. Separate planes (default): plane p, row y at `(p * height + y) * pitch`, a normal BitMap. Interleaved: row y, plane p at `(y * depth + p) * pitch`, a BMF_INTERLEAVED BitMap |
| `ZZ9K_SURFACE_FORMAT_RGB565`, `_RGB555` | `pitch * height` | Big-endian 16-bit words |

`pitch` is in bytes: for PLANAR it is the pitch of one plane row. It must be at
least `width` (INDEX8), `(width + 7) / 8` (PLANAR) or `2 * width` (RGB). Bytes
between the end of a row and the pitch are not written. The bits of the last
byte of a PLANAR row beyond `width` are written as zero. The output is at most
1920x1088; the stream may be scaled up or down freely.

`ZZ9KMediaOutputFlags`:

| Flag | Meaning |
| --- | --- |
| `ZZ9K_MEDIA_OUTPUT_DEPTH_MASK` (`0x0f`) | INDEX8 and PLANAR: 1 to 8 bits, 0 means 8. Must be 0 for RGB. |
| `ZZ9K_MEDIA_OUTPUT_PLANAR_INTERLEAVED` | The interleaved layout. PLANAR only. |
| `ZZ9K_MEDIA_OUTPUT_DITHER_ORDERED` | 4x4 Bayer threshold, anchored to the output pixel grid, so it does not change from picture to picture. Any format. |
| `ZZ9K_MEDIA_OUTPUT_DITHER_DIFFUSION` | Floyd-Steinberg. Not implemented: firmware answers `ZZ9K_STATUS_UNSUPPORTED`. It stays clear in the mask on status page 6. With `ORDERED` it is `BAD_REQUEST`. |
| `ZZ9K_MEDIA_OUTPUT_PALETTE_HOST` | The card reads the palette from `palette_handle` and `palette_offset`. INDEX8 and PLANAR only. |
| `ZZ9K_MEDIA_OUTPUT_SCALE_NEAREST` | Point sampling. The default is bilinear, and a box average on an axis that shrinks by a factor of two or more. |

Error diffusion does not fit as a quick addition: it needs a second quantiser
with its own error rows, its output changes from picture to picture, and the
ordered dither covers the 25 fps case. A client can see that it is missing
before it asks, through the mask on page 6.

## Palettes

Without `PALETTE_HOST` the card uses a fixed palette that depends on the depth
(`zz9k_media_output_fixed_palette()` in `zz9k/media_output.h` builds it; load
it into the screen before showing the pictures):

| Depth | Colours | Layout |
| --- | --- | --- |
| 1, 2 | 2, 4 | Gray ramp, the colour of a pixel is its luma |
| 3 | 8 | 2x2x2 cube |
| 4 | 16 | 2x4x2 |
| 5 | 32 | 4x4x2 |
| 6 | 64 | 4x4x4 |
| 7 | 125 | 5x5x5 |
| 8 | 252 | 6x7x6 |

A cube of R x G x B levels has colour `(r * G + g) * B + b`. A channel with L
levels puts level k at `round(k * 255 / (L - 1))`. Entries beyond the colours in
use are black and are never produced. I chose 6x7x6 over 8x8x4 because blue has
four levels in the latter. That answers the question about the fixed palette
in the issue.

With `PALETTE_HOST` the palette is 256 big-endian `0x00RRGGBB` words (1024
bytes, the layout `ZZ9K_OP_QUERY_PALETTE` writes); only the first `1 << depth`
words are used. The card reads it at every picture, so a client may change it
without reconfiguring. The card maps colours to the nearest entry by squared
RGB distance (lowest index on a tie), through a table of 5-5-5 cells it builds
when the palette changes. Building it takes about 3 ms on a PC for 256 colours.
A per-picture adaptive palette with a palette for each slot is not part of
this version.

## Scaling and conversion

The result is defined by `test/media/media_output_test.c`, which compares the
firmware code with a plain scalar reference. In order:

1. Each plane is scaled to the output size, vertically first, rounded to 8 bits,
   then horizontally. Chroma is scaled from its own half-size plane, its samples
   taken as sitting between pairs of luma pixels.
2. Bilinear: sample position `(i + 1/2) * src / dst - 1/2`, weights in 1/256.
   Nearest: sample `floor((i + 1/2) * src / dst)`. On an axis where
   `src >= 2 * dst` bilinear becomes an exact box average with a 24-bit
   reciprocal.
3. BT.601 limited range, the constants of the overlay compositor:
   `r = (298 (y - 16) + 409 (cr - 128) + 128) >> 8`, `g = (298 (y - 16) -
   100 (cb - 128) - 208 (cr - 128) + 128) >> 8`, `b = (298 (y - 16) + 516
   (cb - 128) + 128) >> 8`, clamped to 0..255.
4. Each channel is quantised to L levels as
   `min(L - 1, floor(v * (L - 1) / 255 + t))`, with `t = (2 * bayer + 1) / 32`
   when `DITHER_ORDERED` is set and `t = 1/2` (plain rounding) when it is not.
   RGB565 uses 32, 64 and 32 levels, RGB555 32 each. The Bayer matrix is
   `0 8 2 10 / 12 4 14 6 / 3 11 1 9 / 15 7 13 5`, indexed by output x and y
   modulo 4. Gray palettes quantise `(77 r + 150 g + 29 b + 128) >> 8`.
5. With a host palette the dither adds `(2 * bayer - 15) * step / 32` to each
   channel before the lookup, `step` being 255, 255, 255, 170, 128, 85, 64, 48
   for depths 1 to 8.

## Cost

Measured with `build/media_output_test --bench` on a PC (x86-64, `-O2`, one
picture, best of 20); the card figures are estimates, not measurements.

| Picture | PC |
| --- | ---: |
| 320x240 to 320x240, RGB565, INDEX8 or 8 planes | 0.27 to 0.32 ms |
| 352x288 to 320x240, 8 planes, bilinear | 0.39 ms |
| 320x240 to 640x480, RGB565 | 1.25 ms |
| 640x480 to 640x480, RGB565 or 8 planes | 1.1 to 1.3 ms |
| 640x480 to 320x240, 8 planes, box average | 0.62 ms |

The loop is scalar C compiled for the Cortex-A9 with `-O2`; there is no NEON
kernel in this version. [INFERENCE] At 50 to 80 cycles a pixel and 666 MHz that
is about 6 to 9 ms for 320x240 and 25 to 35 ms for 640x480, on core 1 next to
the decoder. 25 fps leaves 40 ms a picture, so 320x240 output leaves room for
the decode and 640x480 does not. `value[2]` of status page 6 and `zz9k-mediaout`
give the real figure. The first picture with a 256-colour host palette also pays
for building its colour table. The conversion tables are static data on core 1:
about 107 KB of BSS (`sdk_media_output.o`).

## Testing

`make -C test/media test` runs the converter against the reference (geometries
from 1:1 to 1x1, every depth, both planar layouts, odd widths, host palettes),
the slot, pending and BUSY state machine, and the firmware and SDK mirrors.
`sdk/tests/media_output_abi_test.c` and `sdk/tests/mediaout_logic_test.c` cover
the request builder, the reply decoders and the tool logic.

### Card test with zz9k-mediaout

`zz9k-mediaout FILE.mpg [options]` plays a video-only MPEG-1 or MPEG-2 program
stream into a shared buffer and prints, for every picture, the slot, the card's
conversion time, the time until the picture was complete and an FNV-1a checksum
of the slot. Options: `-f rgb565|rgb555|index8|planar`, `-s WxH`, `-d DEPTH`,
`-i` (interleaved), `-o` (ordered dither), `-n` (nearest), `-H` (gray ramp as
host palette), `-1` (one slot), `-c N`, `-x` (no checksum), `-w` (window,
needs cybergraphics).

1. Check the service: `zz9k-services` lists the video service with 15 opcodes.
2. `zz9k-mediaout test.mpg -c 25`: expect `slot 0` and `slot 1` alternating, no
   `note:` line, `average card conversion` of a few ms and `run checksum`.
3. Repeat for `-f planar -d 8 -o`, `-f planar -d 5 -i`, `-f index8 -d 6 -H`, `-s
   160x100` and `-1`. The run must end with `pictures in` and exit code 0.
4. Run a configuration twice and compare the per-picture sums: they must match.
5. `-w` shows the pictures in a window; a 8-plane picture should look like the
   stream in the fixed palette colours.
6. Zorro II (untested): `-f planar -d 5 -s 160x100 -1 -x`.

## Not in this version

Error-diffusion dither, HAM output, an adaptive palette per picture, a NEON
conversion kernel and audio in `zz9k-mediaout`.
