# ZZPlay

ZZPlay is the ZZ9000 media player for AmigaOS. It decodes supported media on
the card and presents video through the Picasso96 overlay. On an accelerated
Zorro III system, decoded video stays on the card rather than crossing the
Zorro bus.

Start it as a desktop player from Workbench, or pass files from a shell for a
one-shot playlist. A shell launch with files exits after playback; a shell
launch without files opens the player. Workbench launches open the player
unless `FULLSCREEN` or `PLAYER=NO` selects one-shot playback. Scripts can
control a running ZZPlay through its [ARexx port](#arexx).

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
it uses accelerated decode with AHI. For MPEG and WebM sound, `AUTO` tries the
ZZ9000AX daughterboard first and then AHI. FLAC and Ogg Vorbis try card output
and fall back to AHI if the card refuses the output or ZZ9000AX is busy; the
MP3 and video settings do not apply to them. An explicit `AUDIO=AX` request is
strict and reports failure instead of falling back. An explicit `AUDIO=MHI`
request for FLAC or Ogg Vorbis is rejected.

The required decoder service must be advertised by the firmware before
playback begins. Video-only files can play without an audio backend.

Only one backend can use the ZZ9000AX daughterboard at a time. A strict request
reports `BUSY` when another program owns it; automatic selection can fall back.

The volume control works when the output can change its level: AHI always,
card output for FLAC and Ogg Vorbis when the firmware offers stream gain, and
MHI when the driver reports volume control (`mhizz9000.library` does with
firmware that offers stream gain). Video sound on ZZ9000AX plays at a fixed
level. When the output cannot change its level, the slider is disabled and the
output line shows `(fixed volume)`. A new volume applies when you release the
slider.

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

## Player window

The player window shows the title, the format, the audio output, and the
position of the current item, followed by a position slider, the transport
buttons (`|<`, **Play**, **Pause**, **Stop**, `>|`), the volume slider, the
repeat cycle gadget, the **Shuffle** check box, and the playlist. A message
line reports errors and notices. The window remembers its position.

The playing time follows frames the card has actually output, divided by
the card's 48 kHz rate, not the file's bitrate. A VBR file and a file with
a large ID3 tag therefore keep time with the audio.

Drag the position slider to seek in MP3 files. The slider is unavailable for
MPEG video, WebM, FLAC, Ogg Vorbis, and WebP animations.

### Playlist

The playlist marks the current entry with a double arrow. Double-click an
entry to play it. The buttons below the list work on the playlist:

| Button | Action |
| --- | --- |
| **Add...** | Append files chosen in a file requester. |
| **Remove** | Remove the selected entry. A playing entry finishes playing, and the list continues after the entry's old place. |
| **Clear** | Empty the playlist. The playing item continues. |
| **Load...** | Append the entries of an M3U playlist. |
| **Save...** | Save the playlist as extended M3U. `.m3u` is added when the name has no playlist extension. |

Drop files, drawers, or playlists onto the player window to append them. When
nothing is playing, playback starts with the first dropped item. Reading a
large drawer pauses the playing item until the scan has finished.

The file requesters run beside playback, so the current item keeps playing
while one is open. ZZPlay does not quit while a file requester is open.

An item that cannot be played is marked and skipped, and the playlist moves
on. Changing the audio settings clears the marks; double-clicking the entry
tries it again.

### Menus

| Menu | Item | Shortcut | Action |
| --- | --- | --- | --- |
| Project | Open... | Amiga-O | Replace the playlist with the chosen files and play the first one. |
| Project | Add files... | Amiga-A | Append files. |
| Project | Add drawer... | | Append the media files of a drawer and its sub-drawers. |
| Project | Load playlist... | Amiga-L | Append the entries of an M3U playlist. |
| Project | Save playlist... | Amiga-S | Save the playlist as extended M3U. |
| Project | Settings... | Amiga-P | Open the settings window. |
| Project | About... | | Show the version, the ARexx port name, and the supported formats. |
| Project | Quit | Amiga-Q | Quit ZZPlay. |
| Control | Play/Pause | | Pause or resume playback. Start playback when stopped. |
| Control | Stop | | Stop. |
| Control | Next, Previous | Amiga-N, Amiga-B | Next or previous item. |
| Control | Seek forward, Seek back | | Seek 10 seconds in MP3 playback. |
| Control | Repeat | | Off, Track, or All. |
| Control | Shuffle | | Play the playlist in random order. |
| Control | Fullscreen video | Amiga-F | Play videos fullscreen. See [Fullscreen video](#fullscreen-video). |

### Keys

These keys work in the player window and in the video window.

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

## Settings

**Settings...** in the Project menu opens the settings window:

| Setting | Values | Effect |
| --- | --- | --- |
| MP3 output | Auto, MHI, AHI | Audio output for MP3 files. |
| Video sound | Auto, AHI, ZZ9000AX, None | Audio output for MPEG and WebM sound. |
| Preload whole file | on or off | Copy MPEG and WebM files to card memory before playback. See [Preload to card memory](#preload-to-card-memory). |
| AHI unit | 0 to 3 | The `ahi.device` unit for every AHI output. |
| MHI driver | drivers in `LIBS:MHI/` | The MHI driver for MP3 output. |

Changes apply from the next item. **Save** keeps them in
`ENVARC:ZZPlay.prefs` and `ENV:ZZPlay.prefs`, **Use** only in
`ENV:ZZPlay.prefs` until the next reboot, and **Cancel** discards them. A
saved output is a preference: when it cannot play an item, ZZPlay falls back
as `AUTO` would. Only an `AUDIO=` option for one launch fails instead.

When the desktop player quits, it also stores the volume, repeat, shuffle,
**Fullscreen video**, its window position, and the last requester drawer in
both files. A value that came from this launch's options (for example
`VOLUME=N` or `LOOP`) is not stored unless you changed it during the session.

The files hold one `KEY=VALUE` per line. Unknown keys are ignored, and an
invalid value resets only that key:

| Key | Values |
| --- | --- |
| `MP3OUTPUT` | `AUTO`, `MHI`, `AHI` |
| `VIDEOAUDIO` | `AUTO`, `AHI`, `AX`, `NONE` |
| `PRELOAD` | `YES`, `NO` |
| `AHIUNIT` | `0` to `3` |
| `MHIDRIVER` | a file name in `LIBS:MHI/` |
| `VOLUME` | `0` to `100` |
| `REPEAT` | `OFF`, `ONE`, `ALL` |
| `SHUFFLE` | `YES`, `NO` |
| `FULLSCREEN` | `YES`, `NO` |
| `WINDOW` | `left,top` of the player window, `-1,-1` for the default |
| `DRAWER` | the last drawer used in a file requester |

## Fullscreen video

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

## ARexx

ZZPlay opens an ARexx port named `ZZPLAY` when `rexxsyslib.library` is
available. A second copy running at the same time gets `ZZPLAY.1`, the next
`ZZPLAY.2`, and so on. The About requester shows the name in use. The port is
open in the desktop player and in one-shot runs, with or without a window.

```rexx
/* Queue a drawer and start the first track at half volume. */
options results
address 'ZZPLAY'
'OPEN "Work:Music/Albums/Some Album"'
'VOLUME 50'
'TITLE'
say 'Now playing:' result
```

Commands are not case-sensitive. Separate arguments with spaces and put
double quotes around a path that contains spaces. Playlist positions count
from 1. Times are whole seconds.

| Command | Action |
| --- | --- |
| `PLAY [n]` | Start playback, or resume a paused item. With `n`, play entry `n`. |
| `PAUSE`, `RESUME` | Pause or resume the playing item. |
| `PLAYPAUSE` | Same as Space: pause, resume, or start when stopped. |
| `STOP` | Stop. A one-shot run exits. |
| `NEXT`, `PREVIOUS` | Next or previous item. |
| `SEEK s` | Seek to second `s`. `SEEK +s` and `SEEK -s` move relative to the current position. Only where the position slider works. |
| `VOLUME [v]` | Set the volume (0 to 100), or change it with `+v`/`-v`. Without an argument, return it. |
| `REPEAT [mode]` | Set repeat to `OFF`, `TRACK` or `ALL`, or return it. |
| `SHUFFLE [mode]` | Set shuffle to `ON`, `OFF` or `TOGGLE`, or return it. |
| `FULLSCREEN [mode]` | Set the **Fullscreen video** setting to `ON`, `OFF` or `TOGGLE`, or return it. |
| `ADD path...` | Append files, drawers, patterns or playlists, as a drop on the window does. Playback does not start. Returns the number of entries added. |
| `OPEN path...` | Replace the playlist with the given paths and play the first entry. Returns the number of entries added. |
| `CLEAR` | Empty the playlist. The playing item continues. |
| `REMOVE n` | Remove entry `n`. |
| `SAVEPLAYLIST file` | Save the playlist as extended M3U under exactly the name given. |
| `STATUS` | Return `PLAYING`, `PAUSED` or `STOPPED`. |
| `TITLE`, `FILE`, `FORMAT` | Return the title, path or format text of the playing item. While stopped, `TITLE` and `FILE` describe the current entry, and are empty when there is none. |
| `POSITION`, `DURATION` | Return the position and length of the playing item in seconds (0 when stopped or unknown). |
| `COUNT` | Return the number of playlist entries. |
| `INDEX` | Return the position of the current entry, 0 when there is none. |
| `ENTRY n` | Return the path of entry `n`. |
| `SHOW` | Bring the player window to the front. |
| `VERSION`, `HELP` | Return the version string or the command list. |
| `QUIT` | Quit ZZPlay. |

Values come back in `RESULT` when the script uses `OPTIONS RESULTS`. The
return code is 0 on success. It is 5 when the command cannot apply right now,
for example `PAUSE` with nothing playing, `SEEK` on a format without seeking,
or `QUIT` while a file requester is open. It is 10 for an unknown command, a
wrong argument, a position outside the playlist, or `ADD`/`OPEN` paths that
add nothing (`OPEN` then leaves the playlist unchanged).

`ADD` and `OPEN` read drawers and playlist files, which can take a while.
Like a drop on the window, they pause the playing item while they run. The
script waits for the reply, and commands from other scripts wait their turn.

## Diagnostics

`--benchmark` measures decode throughput without display pacing. `--fps` shows
rolling playback and decode-call frame rates. `--trace` writes per-frame timing
diagnostics.

`zz9k-mp3` remains the lower-level MP3 decoder diagnostic. Use it when raw or
WAV output is needed for exactness checks.
