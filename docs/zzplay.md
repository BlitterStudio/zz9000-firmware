# ZZPlay — the ZZ9000 accelerated media player

ZZPlay plays MPEG-1 Program Streams and standalone MP3 files using the
ZZ9000's ARM coprocessor and FPGA video overlay. Video decoding happens on the
card; on the accelerated Zorro III path no decoded video ever crosses the
Zorro bus.

## What it plays

| Input | Video | Audio |
| --- | --- | --- |
| MPEG-1 Program Stream (`.mpg`, `.mpeg`) | MPEG-1 video, card-decoded | MPEG-1 Layer II, card-decoded |
| MPEG-1 Program Stream, video only | MPEG-1 video, card-decoded | none (a warning is printed) |
| MPEG Layer III (`.mp3`) | — | card-decoded, CBR and VBR, mono or stereo |

The format is chosen by inspecting the file, not by its name. MPEG-1
elementary streams, standalone MP2, MPEG-2 and other codecs are rejected with
a specific message rather than being half-played.

Video requires the Picasso96 overlay. Zorro III uses the normal firmware
surface allocator. On a matched 4 MiB Zorro II stack, ZZPlay can instead use
the driver-reserved 224 KiB PIP source pool when one complete aligned YUY2
frame fits; 352x288 fits, while 640x360 does not. The 2 MiB profile has no PIP
pool. Unsupported Zorro II geometry is reported clearly, and standalone MP3
continues to work on both shipped Zorro II profiles.

## Starting it

**Shell.** `ZZPlay [options] <file>`. The installer puts it in
`SYS:Utilities/ZZ9000/` and offers to add that drawer to the command path;
AmigaDOS is case-insensitive, so `zzplay` works too.

**Workbench.** Drop a media file onto the **ZZPlay** icon, double-click a
project icon whose default tool is `ZZPlay`, or double-click ZZPlay itself and
pick a file from the requester that appears.

A copy of a suitable project icon ships as `Docs/ZZPlay-project.info`. To use
it, copy it next to your media file and rename it to match — for example, for
`Video:holiday.mpg`, copy it to `Video:holiday.mpg.info`.

Errors go to the shell when started from the shell, and to a requester when
started from Workbench, which has no console.

A Workbench launch is **quiet by default**: printing progress from Workbench
makes AmigaDOS open an output window that stays open, so every playback would
leave another one behind. Use `VERBOSE` if you want that output anyway.
`BENCHMARK` stays verbose regardless, since its numbers are the point.

## Options

Every option has a Workbench ToolType with the same name and meaning. The
shipped icon carries all of them disabled, in parentheses; remove the
parentheses in Workbench's *Information* window to enable one.

| Shell | ToolType | Meaning |
| --- | --- | --- |
| `--audio=auto` | `AUDIO=AUTO` | Pick the best available backend (default) |
| `--audio=ahi` | `AUDIO=AHI` | Card-accelerated decode, output through AHI |
| `--audio=mhi` | `AUDIO=MHI` | `mhizz9000.library` on ZZ9000AX; MP3 only |
| `--audio=ax` | `AUDIO=AX` | Direct ZZ9000AX output; Program Stream only |
| `--audio=none` | `AUDIO=NONE` | Mute |
| `--loop` | `LOOP` | Repeat forever |
| `--loop=N` | `LOOP=N` | Repeat N times after the first play |
| `--fullscreen` | `FULLSCREEN` | Start fullscreen, no window furniture |
| `--quiet` | `QUIET` | No progress output. The default from Workbench |
| `--verbose` | `VERBOSE` | Force progress output even from Workbench |
| `--fps` | `FPS` | Rolling playback and decode-call frame rates |
| `--benchmark` | `BENCHMARK` | Remove pacing; implies `--fps`, and mutes audio unless a backend was named |
| `--trace[=P]` | `TRACE[=P]` | Write per-frame timing diagnostics to the trace file (default `T:zzplay.trace`) |
| `--help` | — | Print usage |

### Choosing an audio backend

`AUTO` never surprises you: it resolves the backend *before* playback starts
and prints what it chose and why.

- For **standalone MP3**, `AUTO` prefers MHI when a free ZZ9000AX and
  `mhizz9000.library` are both present, and otherwise uses accelerated decode
  plus AHI.
- For **Program Stream MP2**, `AUTO` uses AHI, or direct AX where the firmware
  advertises it.
- MHI is never offered for Program Stream audio: it is a Layer III interface.
  Asking for it explicitly reports that rather than playing silently.

Only one backend can own the ZZ9000AX daughterboard at a time. If another
program holds it, an explicitly requested backend reports `BUSY` instead of
stealing it, and `AUTO` falls back and says so.

## Controls

| Key | Action |
| --- | --- |
| Space | Pause / resume |
| Escape, Q, close gadget | Stop |
| F | Toggle fullscreen and window (video only) |
| L | Toggle looping |
| Ctrl-C | Stop (shell) |

### Fullscreen

Fullscreen opens a **dedicated screen** at the closest display mode and
scales the picture to fill it, aspect preserved and centred -- the same
scaling route a window resize takes, so 4:3 content on a 4:3 screen
fills it completely, and everything else letterboxes or pillarboxes.
Small dedicated modes render top-left on the card's minimum raster, so
exact-size 1:1 screens are not used for presentation.

Scaling is aspect-preserving -- the picture is never stretched -- and no
part of Workbench is visible, so it suits a program showing a video
sequence.

Pressing F again closes that screen and returns to the exact window position
and size you had before.

If no display mode matches the video, ZZPlay says so and stays windowed
rather than pretending the request succeeded.

**Starting fullscreen.** `--fullscreen`, or the `FULLSCREEN` ToolType, starts
that way with no window ever appearing on the desktop — which is what you
want when another program is showing a video sequence and a framed window on
the Workbench would break the effect. Combine it with `--quiet` (already the
default from Workbench) for a completely silent, chrome-free playback:

```
ZZPlay --fullscreen --quiet Video:intro.mpg
```

### Standalone MP3

MP3 playback has no video window, so it opens a small player window instead,
showing the file name; sample rate, channel mode and bitrate; an `Output:`
line naming the backend actually in use (MHI or AHI); elapsed and total time
with a position bar; and the control legend. The backend also appears in the
window title, so it stays readable when the window is partly covered.
The duration is estimated from the file size, so for a VBR file both it and
the elapsed time are approximate; an approximate position is shown with a
leading `~`.

Pause through MHI stops essentially instantly. Through AHI it stops after the
already-queued audio finishes, up to about 0.4 seconds, because that queue is
what keeps playback gap-free.

## Presentation path

ZZPlay reports which path actually presented your video, asked of the
firmware rather than guessed:

- **native 1:1** — exact size and fully visible, straight from the FPGA
  overlay plane. The fastest path.
- **native scaled** — resized or clipped, still handled by the overlay's
  hardware scaler. Also keeps decoded video off the Zorro bus.
- **card-local compositor** — the ARM software fallback, used only for source
  geometry the overlay cannot express.
- **unknown** — the installed firmware predates presentation reporting. This
  is informational only; playback is unaffected.

Resizing the window switches between the first two and the change is
reported. Restoring the window to exact size returns to native 1:1.

## Diagnostics

`zz9k-mp3` remains the low-level MP3 decode diagnostic, producing raw or WAV
output for exactness checking. ZZPlay is the player; `zz9k-mp3` is the
instrument.

`--benchmark` disables pacing so decode throughput can be measured
independently of display rate, and mutes audio unless a backend was named.
