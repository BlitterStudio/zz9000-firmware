# ZZPlay — the ZZ9000 accelerated media player

ZZPlay plays MPEG-1 Program Streams, WebM, MP3, native FLAC and Ogg Vorbis files using the ZZ9000's ARM
coprocessor and FPGA video overlay. Video decoding happens on the card; on the
accelerated Zorro III path no decoded video ever crosses the Zorro bus.

It works two ways:

- as a **desktop player**: a window with a playlist, transport buttons, a
  position slider, volume, repeat and shuffle, which stays open until you quit
  it;
- as a **one-shot command**: `ZZPlay file` plays and exits, which is what
  scripts and programs showing a video sequence want.

## What it plays

| Input | Video | Audio |
| --- | --- | --- |
| MPEG-1 Program Stream (`.mpg`, `.mpeg`) | MPEG-1 video, card-decoded | MPEG-1 Layer II, card-decoded |
| MPEG-1 Program Stream, video only | MPEG-1 video, card-decoded | none (a warning is printed) |
| WebM (`.webm`, `.mkv`) | VP8 up to 1280x720 pixels, or VP9 up to 854x480, card-decoded, either orientation | Opus at 48 kHz, or Vorbis at 8 to 96 kHz, card-decoded; on the card when the firmware advertises it, otherwise AHI |
| WebM, larger than that but within 1920 on the long side and 1920x1088 pixels | same codecs, best effort | audio stays continuous; video may skip ahead to the next keyframe |
| WebM, video only | VP8 or VP9, card-decoded | none (a warning is printed) |
| MPEG Layer III (`.mp3`) | — | card-decoded, CBR and VBR, mono or stereo |
| Native FLAC (`.flac`) | — | card-decoded, mono or stereo, 4 to 24 bits, 8 to 192 kHz; on the card when the AX output can play the rate, otherwise AHI. Wider than 16 bits is narrowed to 16-bit |
| Ogg Vorbis (`.ogg`, `.oga`) | — | card-decoded, one logical stream, mono or stereo, 8 to 192 kHz; on the card when the AX output can play the rate, otherwise AHI |
| Animated WebP (`.webp`) | WebP animation, card-decoded | none |
| Playlist (`.m3u`, `.m3u8`) | the files it lists | |

The format is chosen by inspecting the file, not by its name. A WebM file is
recognised by its EBML header and a `webm` DocType, so a misnamed file still
plays and a Matroska file that is not WebM is refused. MPEG-1
elementary streams, standalone MP2, MPEG-2, Ogg-FLAC, Ogg Opus, multichannel FLAC/Vorbis, a second WebM video track, an unknown WebM codec and other codecs are rejected with
a specific message rather than being half-played. File extensions only decide
what a drawer scan or the file requester offers.

WebM larger than the realtime sizes above is not refused. The window says
that frames may be skipped. Audio, when there is any, keeps playing. A frame
that is decoded too late is dropped, but never more than two in a row, so
a stream the card can only just decode still moves at a third of its frame
rate or better. Video jumps to the next keyframe only when it is more than
about half a second behind the sound. A video-only file is paced by its
frame rate instead, so it does not skip. Anything past a 1920-pixel long
side, or past 1920x1088 pixels, is refused before playback starts. Portrait
frames, including odd sizes, are fitted to the window and to fullscreen with
the aspect preserved and black bars at the sides rather than stretched. If
the file names a display size different from the coded frame, that aspect
is what the window uses. A projection roll hint is printed when progress
output is on and is not applied.

ZZPlay keeps its formats in one registry, so support for further formats is
added as the card's decoders grow; the *About* requester lists what the
installed version plays.

Video requires the Picasso96 overlay. Zorro III uses the normal firmware
surface allocator. On a matched 4 MiB Zorro II stack, ZZPlay can instead use
the driver-reserved 224 KiB PIP source pool when one complete aligned YUY2
frame fits; 352x288 fits, while 640x360 does not. The 2 MiB profile has no PIP
pool. Unsupported Zorro II geometry is reported clearly, and MP3 continues to
work on both shipped Zorro II profiles.

## Starting it

**Workbench.** Double-click **ZZPlay** to open the player with an empty
playlist, or select media files, drawers or playlists (shift-click for
several), then double-click ZZPlay or drop them onto its icon: they are
loaded and the first one starts. Double-clicking a project icon whose default
tool is `ZZPlay` does the same for that file.

A copy of a suitable project icon ships as `Docs/ZZPlay-project.info`. To use
it, copy it next to your media file and rename it to match — for example, for
`Video:holiday.mpg`, copy it to `Video:holiday.mpg.info`.

**Shell.** `ZZPlay [options] [file|drawer|pattern|playlist ...]`. The
installer puts it in `SYS:Utilities/ZZ9000/` and offers to add that drawer to
the command path; AmigaDOS is case-insensitive, so `zzplay` works too.

- With files, ZZPlay plays them in order and exits — the one-shot behaviour
  scripts rely on. A video shows only its video window; an audio file opens
  the player window for the duration of the run.
- Without files, it opens the desktop player.
- `PLAYER` keeps the player open after the files have played; `PLAYER=NO`
  makes a Workbench launch behave like the one-shot command.

Every argument can be a file, a drawer (its media files are added in name
order, including sub-drawers up to four levels deep), an AmigaDOS pattern such
as `Music:Album/#?.mp3`, or an `.m3u` playlist.

Errors go to the shell when started from the shell, and to a requester when a
one-shot run was started from Workbench. In the desktop player they appear on
the player window's message line instead, and a file that cannot be played is
skipped so the rest of the playlist continues.

A Workbench launch is **quiet by default**: printing progress from Workbench
makes AmigaDOS open an output window that stays open, so every playback would
leave another one behind. Use `VERBOSE` if you want that output anyway.
`BENCHMARK` stays verbose regardless, since its numbers are the point.

## The player window

From top to bottom:

- **Now playing**: the title (artist and title from the MP3's ID3 tag when it
  has one, otherwise the file name), the format line (sample rate, channels
  and bitrate, or video size and frame rate), the audio output actually in
  use, the elapsed and total time and the playback state, and a message line
  for notices and errors.
- **Position slider**: drag it to seek. Seeking is available for MP3; it is
  greyed out for MPEG-1 video, WebM, FLAC and Ogg Vorbis, which play from the start.
- **Transport**: previous, play, pause, stop, next.
- **Volume**: greyed out when the active output cannot change volume (see
  below).
- **Repeat** (off, track, all) and **Shuffle**.
- **Playlist**: the current entry is marked with `»`. Double-click an entry to
  play it. *Add...*, *Remove*, *Clear*, *Load...* and *Save...* edit it.
  Removing the entry that is playing lets it finish; playback then
  continues with the entry that followed it.

Drop files, drawers or playlists from Workbench onto the window to add them;
if nothing is playing, the first dropped file starts. Playback pauses while
the dropped items are read and then continues, as it does while a file
requester is open.

The window position, volume, repeat, shuffle and the last drawer used in a
file requester are remembered between sessions.

### Menus

| Menu | Items |
| --- | --- |
| Project | Open..., Add files..., Add drawer..., Load playlist..., Save playlist..., Settings..., About..., Quit |
| Control | Play/Pause, Stop, Next, Previous, Seek forward, Seek back, Repeat (Off/Track/All), Shuffle, Fullscreen video |

*Open...* replaces the playlist and starts playing; *Add files...* appends.
Opening a requester while something plays pauses it, and playback resumes when
the requester closes.

### Keys

The same keys work in the player window and in the video window.

| Key | Action |
| --- | --- |
| Space | Pause / resume (play, when stopped) |
| Escape, Q | Stop |
| N | Next |
| P | Previous |
| L | Repeat: off, track, all |
| S | Shuffle on/off |
| + / - , Cursor up / down | Volume |
| Cursor right / left | Seek 10 seconds forward / back (MP3) |
| F | Toggle fullscreen and window (video only) |
| Ctrl-C | Quit (shell) |

Closing the player window quits ZZPlay. Closing the video window stops the
video; in a one-shot run that ends the run.

Previous on the first entry restarts it, unless repeat-all is on, in which
case it wraps to the last entry. Seeking to the very end moves to the next
entry.

## Settings

*Project > Settings...* chooses where sound goes. Changes apply from the next
item played.

| Setting | Choices |
| --- | --- |
| MP3 output | Auto, MHI, AHI |
| Video sound | Auto, AHI, ZZ9000AX direct, None |
| AHI unit | Units 0-3, each shown with the audio mode the AHI preferences editor bound to it |
| MHI driver | Any driver installed in `LIBS:MHI/` |

**Save** keeps the settings across reboots (`ENVARC:ZZPlay.prefs`), **Use**
keeps them until the next reboot (`ENV:ZZPlay.prefs`), **Cancel** discards
the changes. The file is plain text and safe to edit; an unknown or invalid
line is ignored rather than stopping ZZPlay.

A saved output is a preference: when it cannot play a particular file — MHI
cannot play the MP2 sound of a video, a driver is missing, another program
holds the ZZ9000AX — ZZPlay falls back as `Auto` would and shows the output
it really used. Only a backend named for one launch with `AUDIO=` (or
`--audio=`) is strict and reports an error instead of falling back.

**Volume** works through AHI (MP3, video sound, and FLAC or Ogg Vorbis when
they play through AHI) and through outputs that advertise a volume control.
`mhizz9000.library` does on firmware with per-stream audio gain and
zz9k.library 2.31 or newer, and on-card FLAC or Ogg Vorbis does on the same
firmware: the slider then scales the stream below the level set in ZZTop's
audio settings, where 100% is that level. With older firmware, and for
ZZ9000AX direct Program Stream output, the level belongs to ZZTop's audio
settings and the slider is greyed out.

## Options

Every option has a Workbench ToolType with the same name and meaning. The
shipped icon carries them disabled, in parentheses; remove the parentheses in
Workbench's *Information* window to enable one. Options apply to one launch;
they override the saved settings without changing them.

| Shell | ToolType | Meaning |
| --- | --- | --- |
| `--audio=auto` | `AUDIO=AUTO` | Pick the best available backend (default) |
| `--audio=ahi` | `AUDIO=AHI` | Card-accelerated decode, output through AHI |
| `--audio=mhi` | `AUDIO=MHI` | The selected MHI driver; MP3 only |
| `--audio=ax` | `AUDIO=AX` | ZZ9000AX output: direct for a Program Stream, on-card for FLAC and Ogg Vorbis |
| `--audio=none` | `AUDIO=NONE` | Mute |
| `--ahiunit=N` | `AHIUNIT=N` | AHI unit 0-3 |
| `--mhidriver=name` | `MHIDRIVER=name` | MHI driver file in `LIBS:MHI/`, e.g. `mhizz9000.library` |
| `--volume=N` | `VOLUME=N` | Starting volume, 0-100 |
| `--player` | `PLAYER` | Stay open as a desktop player after the files have played |
| `--player=no` | `PLAYER=NO` | Play the files and exit, also from Workbench |
| `--loop` | `LOOP` | Repeat each file forever (repeat track) |
| `--loop=N` | `LOOP=N` | Repeat each file N times after the first play |
| `--fullscreen` | `FULLSCREEN` | Start videos fullscreen, no window furniture |
| `--quiet` | `QUIET` | No progress output. The default from Workbench |
| `--verbose` | `VERBOSE` | Force progress output even from Workbench |
| `--fps` | `FPS` | Rolling playback and decode-call frame rates |
| `--benchmark` | `BENCHMARK` | Remove pacing; implies `--fps`, and mutes audio unless a backend was named |
| `--trace[=P]` | `TRACE[=P]` | Write per-frame timing diagnostics to the trace file (default `T:zzplay.trace`) |
| `--help` | — | Print usage |

At most 64 files can be named on one command line; use a pattern, a drawer
or a playlist for more.

### Choosing an audio backend

`AUTO` never surprises you: it resolves the backend *before* playback starts
and the player window shows what it chose.

- For **MP3**, `AUTO` prefers MHI when the selected MHI driver is installed
  and free, and otherwise uses accelerated decode plus AHI.
- For **Program Stream MP2**, `AUTO` uses AHI, or direct AX where the firmware
  advertises it.
- MHI is never offered for Program Stream or WebM audio: it is a Layer III
  interface. Asking for it explicitly reports that rather than playing silently.
- **WebM Opus and Vorbis** use the video-sound choice: on the card when the
  firmware advertises the matching codec, otherwise AHI. Opus is always
  48 kHz. Vorbis uses the rate in the file, from 8 to 96 kHz. Firmware that
  does not advertise WebM reports that rather than starting.
- **FLAC** and **Ogg Vorbis** `AUTO` plays them on the card. The card decodes
  to 16-bit little-endian and the ZZ9000AX output consumes that PCM, so
  nothing is read back over the bus. If that output is refused — older
  firmware, a sample rate or channel count the AX pump cannot convert to
  48 kHz, or the ZZ9000AX already in use — ZZPlay falls back to accelerated
  decode plus AHI and the window shows the output it actually used. A saved
  preference is not strict and falls back the same way. `AUDIO=AX` does not
  fall back; it reports the refusal. `AUDIO=AHI` keeps AHI. `AUDIO=MHI` is
  refused, because MHI is MP3 only; the message names the outputs that do
  work (the card and AHI). The saved MP3 output does not apply. They need
  firmware that advertises the matching stream service; without it ZZPlay
  reports that accelerated FLAC or Ogg Vorbis streaming is unavailable. An
  Ogg Vorbis duration comes from the stream's last page and
  is shown only when that page belongs to the same stream. A chained Ogg
  file plays its first link to its end (the firmware reports the next link
  only after every byte of the first has been read), then stops with an
  error saying the card cannot decode the rest; it is never reported as
  completed. A multiplexed file is refused before anything plays.
- The decoded-audio ring is sized from the stream's largest decoder unit
  (an MP3 frame, a FLAC block, a Vorbis packet) and shrinks in bounded steps
  when the compact Zorro II host window is short; if even one unit does not
  fit, ZZPlay reports that there is not enough shared card memory for that
  stream instead of starting it.

Only one backend can own the ZZ9000AX daughterboard at a time. If another
program holds it, an explicitly requested backend reports `BUSY` instead of
stealing it, and `AUTO` falls back and says so.

Pause through MHI, and through on-card FLAC or Ogg Vorbis, stops essentially
instantly. Through AHI it stops after the already-queued audio finishes, up
to about 0.4 seconds, because that queue is what keeps playback gap-free.

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
the Workbench would break the effect. A Workbench launch with `FULLSCREEN`
therefore plays and exits like the shell command, unless `PLAYER` is also
set. Combine it with `--quiet` (already the default from Workbench) for a
completely silent, chrome-free playback:

```
ZZPlay --fullscreen --quiet Video:intro.mpg
```

### MP3 position and duration

The duration is estimated from the size of the audio data, so for a VBR file
both it and the elapsed time are approximate; an approximate position is
shown with a leading `~`. Through MHI the position is derived from the data
handed to the driver and is also shown as approximate. Seeking jumps to the
proportional position in the file, which is exact for CBR files and close
for VBR files.

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
