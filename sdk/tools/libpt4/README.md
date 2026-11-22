# libpt4

Portable C99 replayer for ProTracker 4.1 modules. ProTracker 4.1 by Tom Beyer, Oliver Ickler, Haavard Pedersen and
Akustikrausch (Andreas Wendorf); based on ProTracker by Lars Hamre / Amiga Freelancers and ProTracker 3.15 by
CryptoBurners. zlib license (`LICENSE`).

The library plays modules the way ProTracker 4.1 plays them. It is a port of the 68000 replayer (`pt_Rp*`), the
8xy sub-effects, the PT4X sample table, the DYN envelopes and the 020 software mixer, not a generic MOD player.
Format and playback rules: `docs/PT41_FORMAT.md` in the ProTracker 4.1 repository.

## Formats

All ids the PT 4.1 loader takes: `M.K.`, `M!K!`, `!PM!`, `FLT4`, `EXO4`, `4CHN`, `TDZ1`..`TDZ4`, `CD61`,
`1CHN`..`7CHN`, `8CHN`, `OCTA`, `CD81`, `FLT8`, `9CHN`, `09CH`..`32CH`, and 15-sample modules (unknown id, sane
header). 4.1 extensions: samples 32..63 (`PT4X` block), DYN volume envelopes, 8xy sub-effects 81y..87y, the
`PT41` footer of PT 4.1.2 (detected and stripped). Truncated files load; missing data is silence.

`pt4_detect` classifies a buffer without allocating: `PT4_EXT` (footer, valid `PT4X` block or valid DYN
envelope), `PT4_PLAIN_MOD` (anything else PT loads, including 8xy modules without footer: other players read
8xx as panning there), `PT4_NONE`. `pt4_open` always plays 8xy as PT 4.1 does.

## Output paths

| Path | Used for (AUTO) | Semantics |
|---|---|---|
| PAULA | 4 channel modules | Paula: loop latched at the trigger, DYN chunk lists, hard LRRL panning |
| MIXER | 5..8 channels | PT 4.1 020 mixer: voices n/n+4 summed per Paula channel as int8, byte exact to PT |
| WIDE | 9..32 channels | AmiGUS: mixer voice logic, every voice on its own, AmiGUS panning, DYN applied, no rate below period 5 |

Stereo PCM (int16 or float) at any rate 8 000..192 000 Hz without a resampler: the voice step is computed for the
output rate (`((3546895 / period) << 16) / rate`) as the PT mixer does it for its own rate. `PT4_OPT_STEREO`
narrows the image in quarters like PT's AHI output. Options: see `include/pt4.h`.

## Use

```c
#include "pt4.h"
int err;
pt4_song *s = pt4_open(data, size, NULL, &err);   /* NULL allocator = malloc/free */
float buf[2 * 1024];
while (pt4_render_f32(s, buf, 1024, 48000) > 0) { /* ... */ }
pt4_close(s);
```

No global mutable state, no stdio, no file access; several songs may play at once (one song per thread at a
time). Memory only through the allocator hook. The integer core needs no floating point (`src/float.c` is the only
float code, for `pt4_render_f32`).

## Build

- gcc/clang: `make` (library, `pt4play`, `pt4test`, `pt4sum`), `make test`, `make fuzz` (ASan/UBSan),
  `sh test/portcheck.sh`.
- CMake/MSVC: `cmake -S . -B build && cmake --build build`, or `add_subdirectory()` -> target `pt4`.
  Sources: `src/load.c src/replay.c src/voice.c src/api.c src/tables.c src/float.c`.
- AmigaOS 68000 (vbcc, `VBCC=/opt/vbcc`): `make -f Makefile.amiga` -> `build/amiga/pt4.lib`, `pt4sum`.
  `pt4_open` keeps about 3 KB on the stack; give programs 16 KB (`pt4sum` sets `__stack`).
  `pt4sum MODULE` prints frames and an Adler-32 of the output: the same line as on the host.

## pt4play

`pt4play MODULE -o out.wav [--rate HZ] [--float] [--path auto|paula|mixer|wide] [--seconds S] [--loops N]
[--mute 1,5] [--stereo 0..4] [--interp] [--dyn-mixer] [--notes-ext] [--row0] [--tempo BPM] [--pairs out.raw] [--info]`

`--pairs` writes the four int8 pair streams of the mixer path one after the other, for comparing with
recordings of PT's mixer.

## Tests

`make test` runs the unit tests in `test/pt4test.c` on the modules in `testdata/`, `make fuzz` feeds the loader
and the replayer random data under ASan/UBSan. During development the output was compared byte for byte with what
ProTracker 4.1 itself plays: the mixer output and tick traces of 4 and 8 channel modules, and for 9 to 32 channels
the register writes to the AmiGUS.

## Where the library goes its own way

libpt4 is made for players and tools on any system. A few things only make sense on a real Amiga or are faults of
the original program; these are left out on purpose:

- Amiga hardware: Paula's analogue output filter and its period limits are not emulated, so E0x (LED filter) and the
  8x0 colour command change nothing in the sound.
- Mute: `pt4_mute` silences a channel while its notes keep running, so it comes back in time. In PT the track switch
  stops the notes.
- Song end: a player needs an end, so a song stops at F00 or when an order comes a second time.
  `pt4_set_loop(s, 1)` loops forever, as PT does.
- Start: every song starts with an empty tick remainder. PT carries one over from before play started, so a
  recording from PT can be off by one sample per tick; the music is the same.
- Two faults of PT 4.1 are not copied. In PT, 81y (backwards) can lose its place when it crosses a 32 KB boundary, and a
  DYN envelope whose loop segment lies outside the envelope loops endlessly or reads garbage. libpt4 plays 81y as
  documented and treats such an envelope as one without a loop.
