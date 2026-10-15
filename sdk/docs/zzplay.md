# ZZPlay

ZZPlay is the ZZ9000 media player for AmigaOS. It decodes supported media on
the card and presents video through the Picasso96 overlay. On an accelerated
Zorro III system, decoded video stays on the card rather than crossing the
Zorro bus.

Start it as a desktop player from Workbench, or pass files from a shell for a
one-shot playlist. A shell launch with files exits after playback; a shell
launch without files opens the player. Workbench launches open the player
unless `FULLSCREEN` or `PLAYER=NO` selects one-shot playback.

## Supported input

ZZPlay identifies media from its contents. Extensions only control drawer scans
and the file requester, so a correctly formed file can play under another name.
A WebM file must declare the `webm` DocType, and a Matroska file the
`matroska` DocType.

| Input | Extensions offered in scans and requesters | Supported content |
| --- | --- | --- |
| MPEG Program Stream | `.mpg`, `.mpeg`, `.vob` | MPEG-1 or MPEG-2 video in a Program Stream, up to a 1920-pixel long side and 1920x1088 pixels in either orientation. Supported audio is MP2, AC-3, or 16-bit 48 kHz mono or stereo LPCM. MPEG elementary streams are rejected. |
| WebM | `.webm`, `.mkv` | One VP8 or VP9 video track, with at most one Opus or Vorbis audio track. Video-only files are accepted. Opus is 48 kHz; Vorbis is mono or stereo at 8–96 kHz. The maximum is a 1920-pixel long side and 1920x1088 pixels. |
| Matroska | `.mkv` | One MPEG-1 or MPEG-2 video track, up to a 1920-pixel long side and 1920x1088 pixels in either orientation, with AC-3, MPEG audio (Layer I, II or III), or 16-bit 48 kHz mono or stereo little-endian PCM. Matroska with VP8 or VP9 video plays like WebM, with Opus or Vorbis audio. ZZPlay plays the first supported, enabled audio track and ignores the rest; disabled tracks are ignored. Video-only files are accepted. MPEG-2 needs firmware that advertises it. |
| MPEG Layer III | `.mp3` | MP3 audio. |
| Native FLAC | `.flac` | Native `fLaC` streams with a STREAMINFO first block, mono or stereo, 4–24 bits, and 8–192 kHz. Ogg-FLAC is rejected. |
| Ogg Vorbis | `.ogg`, `.oga` | A mono or stereo Vorbis stream at 8–192 kHz. Ogg Opus, Ogg-FLAC, and other Ogg codecs are rejected. |
| Animated WebP | `.webp` | Animated WebP, 16–1920 pixels wide and 16–1080 pixels high. Still WebP images are rejected. |
| Playlist | `.m3u`, `.m3u8` | Files listed by the playlist. |

WebM and MPEG video have separate realtime thresholds. VP8 is realtime through
1280x720, and VP9 through 854x480. Larger WebM files within the maximum can
play, but late frames may be skipped. MPEG playback also skips late frames and
can skip B-pictures until the next reference picture. Above 800x600, MPEG
playback is best effort: when video falls more than half a second behind, it
skips ahead to the next I-picture. Audio continues while frames are skipped.

WebM files with multiple video or audio tracks, unsupported codecs, invalid
audio layouts, or oversized video are rejected; Matroska files may carry
other audio tracks, which ZZPlay ignores. A WebM or Matroska file needs a video
track; audio-only files are rejected. FLAC and Vorbis do not support seeking. A chained
or multiplexed Ogg Vorbis file stops with an error when the card reaches an
unsupported continuation.

## Platform limits

ZZPlay requires a supported ZZ9000 Zorro II or Zorro III aperture, the
Picasso96 API, and the firmware media-decode services required by the selected
format. On Zorro II, the firmware must advertise an aperture layout and a PIP
pool that fits the decoded video.

Recorded compatibility: a matched 4 MiB Zorro II stack provides a 224 KiB PIP
source pool, where an aligned 352x288 YUY2 frame fits and a 640x360 frame does
not. The 2 MiB profile has no PIP pool. MP3 remains available on both profiles.

## Start playback

### Workbench

Double-click **ZZPlay** to open an empty player. Select media files, drawers,
or playlists and then double-click ZZPlay, or drop them on its icon. ZZPlay
loads the selection and starts the first item.

`Docs/ZZPlay-project.info` is a project icon template. Copy it beside a media
file and give it the media file's name plus `.info`. For example, copy it to
`Video:holiday.mpg.info` for `Video:holiday.mpg`.

### Shell

Run:

```
ZZPlay [options] [file|drawer|pattern|playlist ...]
```

The installer places ZZPlay in `SYS:Utilities/ZZ9000/` and can add that drawer
to the command path. File arguments, drawers, AmigaDOS patterns, and playlists
can be mixed. A drawer adds matching files from sub-drawers up to four levels
deep. At most 64 paths may appear on one command line; use a pattern, drawer,
or playlist when more are needed.

For example:

```
ZZPlay --fullscreen --quiet Video:intro.mpg
```

A one-shot shell launch reports errors to the shell. A one-shot Workbench
launch uses a requester. The desktop player shows errors in its message line
and skips an unplayable item so that the playlist can continue.

## Options

Shell options and Workbench ToolTypes use the same validation. ToolTypes are
case-insensitive. The shipped icon keeps its ToolTypes disabled in parentheses;
remove the parentheses in Workbench's Information window to enable one.

| Shell option | ToolType | Effect |
| --- | --- | --- |
| `--fps` | `FPS` | Show rolling paced-playback and decode-call frame rates. |
| `--benchmark` | `BENCHMARK` | Disable pacing, enable `--fps`, and mute audio unless `--audio` explicitly selected a backend. |
| `--loop` | `LOOP` | Repeat the current item forever. |
| `--loop=N` | `LOOP=N` | Repeat the item `N` times after its first playback. `N` must be greater than zero. |
| `--fullscreen` | `FULLSCREEN` | Start video fullscreen with preserved aspect ratio. In the desktop player this applies to this launch only; the saved **Fullscreen video** setting is unchanged unless you change it during the session. |
| `--player` | `PLAYER`, `PLAYER=YES`, `PLAYER=1`, `PLAYER=TRUE` | Keep the desktop player open after the playlist ends. |
| `--player=no` | `PLAYER=NO`, `PLAYER=0`, `PLAYER=FALSE` | Play named files and exit. |
| `--audio=auto` | `AUDIO=AUTO` | Select the available backend automatically. This is the default. |
| `--audio=ahi` | `AUDIO=AHI` | Use AHI output. |
| `--audio=mhi` | `AUDIO=MHI` | Use the selected MHI driver. MHI supports MP3 only. |
| `--audio=ax` | `AUDIO=AX` | Request ZZ9000AX output. |
| `--audio=none` | `AUDIO=NONE` | Decode without audio output. |
| `--ahiunit=N` | `AHIUNIT=N` | Select AHI unit 0–3. |
| `--mhidriver=name` | `MHIDRIVER=name` | Select an MHI driver from `LIBS:MHI/`. The default is `mhizz9000.library`. |
| `--volume=N` | `VOLUME=N` | Set the initial volume from 0–100. |
| `--quiet` | `QUIET` | Suppress progress output. This is the default for Workbench launches unless `--fps` or `BENCHMARK` applies. |
| `--verbose` | `VERBOSE` | Print progress output from Workbench as well as shell launches. |
| `--trace` | `TRACE` | Write per-frame timing diagnostics to `T:zzplay.trace`. |
| `--trace=path` | `TRACE=path` | Write per-frame timing diagnostics to `path`. |
| `--help` | None | Print command usage. |

`--audio` must include one of `auto`, `ahi`, `mhi`, `ax`, or `none`. Options
and file paths can be interleaved on the shell command line. Unknown options
that begin with `--` are errors.

## Audio output

`AUTO` uses MHI for MP3 when the selected MHI driver is available, otherwise
it uses accelerated decode with AHI. FLAC and Ogg Vorbis try card output and
fall back to AHI if the card refuses the output or ZZ9000AX is busy. An
explicit `AUDIO=AX` request is strict and reports failure instead of falling
back. An explicit `AUDIO=MHI` request for FLAC or Ogg Vorbis is rejected.

Program Stream and WebM audio use the video audio path. The required decoder
service must be advertised by the firmware before playback begins. Video-only
files can play without an audio backend.

Only one backend can use the ZZ9000AX daughterboard at a time. A strict request
reports `BUSY` when another program owns it; automatic selection can fall back.

The player stores settings in `ENV:ZZPlay.prefs` for the current boot and in
`ENVARC:ZZPlay.prefs` after **Save**. Invalid preference lines are ignored.

## Preload to card memory

**Preload whole file** in the player's settings window (stored as `PRELOAD`
in the prefs file) is off by default. When it is on, ZZPlay copies an MPEG or
WebM file to card memory before playback and plays it from there, so the
Amiga does no file reads during playback. The copy shows its progress and can
be cancelled with **Stop** or Ctrl-C; playback starts only after it completes.

Preloading needs Zorro III and firmware that advertises it. The file must fit
the card's 64 MiB preload budget, which is separate from decoder memory.
Otherwise ZZPlay prints one notice and streams the file as usual, including
on Zorro II, on older firmware, and when card memory is short.

Repeat Track and `LOOP=N` replay from the copy already on the card. Repeat
All, Previous and a new start preload the file again. The copy is freed when
the item ends. If ZZPlay crashes before it frees the copy, the card keeps it
until the next Amiga reset.

## Player controls

The player window has a playlist, transport controls, a position slider,
volume, repeat, shuffle, and status text. Drag the position slider to seek in
MP3 files. The slider is unavailable for MPEG video, WebM, FLAC, and Ogg
Vorbis. Drop files, drawers, or playlists onto the player window to append
them to the playlist.

| Key | Action |
| --- | --- |
| Space | Pause or resume playback. Start playback when stopped. |
| Escape, Q | Stop. |
| N, P | Next or previous item. |
| L | Cycle repeat through off, track, and all. |
| S | Toggle shuffle. |
| `+`, `-`, Cursor up, Cursor down | Change volume. |
| Cursor right, Cursor left | Seek 10 seconds forward or back in MP3 playback. |
| F | Turn the **Fullscreen video** setting on or off. A playing video switches immediately. |
| Ctrl-C | Quit from a shell launch. |

**Fullscreen video** in the Control menu (Amiga-F) is a setting: while it is
checked, every video in the playlist plays fullscreen, and the desktop player
remembers it in the prefs file (`FULLSCREEN`) for the next launch. It can be
changed while nothing is playing. A one-shot launch plays fullscreen only when
`--fullscreen` or `FULLSCREEN` is given.

Fullscreen uses a dedicated screen and preserves the video's aspect ratio,
scaling up or down to fit the screen. When the Workbench display mode is
usable (15/16-bit or truecolor, not pixel-doubled), fullscreen opens in that
same display mode without changing output resolution or refresh rate. An 8-bit
(CLUT) Workbench switches depth only, opening at the Workbench's resolution in
16-bit. If the mode cannot be opened or the Workbench mode is pixel-doubled,
ZZPlay falls back to finding an available display mode. Press `F` to return to
the previous window position and size. If no display mode can present the video,
ZZPlay remains windowed.

When one video follows another in fullscreen, ZZPlay keeps the screen open
between them, so the playlist does not return to the Workbench. It returns
when playback stops or the playlist ends, before an audio-only item, and when
an item fails, so that the message or requester is visible.

## Diagnostics

`--benchmark` measures decode throughput without display pacing. `--fps` shows
rolling playback and decode-call frame rates. `--trace` writes per-frame timing
diagnostics.

`zz9k-mp3` remains the lower-level MP3 decoder diagnostic. Use it when raw or
WAV output is needed for exactness checks.
