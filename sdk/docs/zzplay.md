# ZZPlay

ZZPlay is the ZZ9000 media player for AmigaOS. It decodes supported media on
the card and presents video through the Picasso96 overlay. On an accelerated
Zorro III system, decoded video stays on the card rather than crossing the
Zorro bus. Audio also plays over the network: `http://` and `https://`
streams, internet radio stations, and the music on a Plex Media Server,
which the [Browse window](#browse-window) lists and searches.

Start it as a desktop player from Workbench, or pass files or network URLs from
a shell for a one-shot playlist. A shell launch with media exits after playback;
a shell launch without arguments opens the player. Workbench launches open the
player unless `FULLSCREEN` or `PLAYER=NO` selects one-shot playback. Scripts can
control a running ZZPlay, including the Browse services, through its
[ARexx port](#arexx).

## Supported input

ZZPlay identifies media from its contents. Extensions only control drawer scans
and the file requester, so a correctly formed file can play under another name.
A WebM file must declare the `webm` DocType, and a Matroska file the
`matroska` DocType. Audio also plays from `http://` and `https://` addresses; see
[Network playback](#network-playback).

| Input | Extensions offered in scans and requesters | Supported content |
| --- | --- | --- |
| MPEG Program Stream | `.mpg`, `.mpeg`, `.vob` | MPEG-1 or MPEG-2 video in a Program Stream, up to a 1920-pixel long side and 1920x1088 pixels in either orientation. Supported audio is MP2, AC-3, or 16-bit 48 kHz mono or stereo LPCM. MPEG elementary streams are rejected. |
| WebM | `.webm`, `.mkv` | One VP8 or VP9 video track, with at most one Opus or Vorbis audio track. Video-only files are accepted. Opus is 48 kHz; Vorbis is mono or stereo at 8 to 96 kHz. The maximum is a 1920-pixel long side and 1920x1088 pixels. |
| Matroska | `.mkv` | One MPEG-1 or MPEG-2 video track, up to a 1920-pixel long side and 1920x1088 pixels in either orientation, with AC-3, MPEG audio (Layer I, II or III), or 16-bit 48 kHz mono or stereo little-endian PCM. Matroska with VP8 or VP9 video plays like WebM, with Opus or Vorbis audio. ZZPlay plays the first supported audio track that is enabled and ignores the others. Video-only files are accepted. MPEG-2 needs firmware that advertises it. |
| MPEG Layer III | `.mp3` | MP3 audio. |
| Native FLAC | `.flac` | Native `fLaC` streams with a STREAMINFO first block, mono or stereo, 4 to 24 bits, and 8 to 192 kHz. Ogg-FLAC is rejected. |
| Ogg Vorbis | `.ogg`, `.oga` | A mono or stereo Vorbis stream at 8 to 192 kHz. Ogg Opus, Ogg-FLAC, and other Ogg codecs are rejected. |
| Animated WebP | `.webp` | Animated WebP, 16 to 1920 pixels wide and 16 to 1080 pixels high. Still WebP images are rejected. |
| Playlist | `.m3u`, `.m3u8` | Files listed by the playlist. |
| Network address | None | `http://` and `https://` addresses of MP3, FLAC and Ogg Vorbis audio, and Shoutcast or Icecast stations. See [Network playback](#network-playback). |

WebM and MPEG video have separate realtime thresholds. VP8 is realtime through
1280x720, and VP9 through 854x480. Larger WebM files within the maximum can
play, but late frames may be skipped. MPEG playback also skips late frames and
can skip B-pictures until the next reference picture. Above 800x600, MPEG
playback is best effort: when video falls more than half a second behind, it
skips ahead to the next I-picture. Audio continues while frames are skipped.

WebM files with more than one video or audio track, unsupported codecs,
invalid audio layouts or oversized video are rejected. Matroska files may
carry other audio tracks; ZZPlay ignores them. WebM and Matroska files need a
video track. FLAC and Ogg Vorbis cannot be seeked. A chained or multiplexed
Ogg Vorbis file stops with an error when the card reaches the part it does
not support.

## Platform limits

ZZPlay requires a supported ZZ9000 Zorro II or Zorro III aperture, the
Picasso96 API, and the firmware media-decode services required by the selected
format. On Zorro II, the firmware must advertise an aperture layout and a PIP
pool that fits the decoded video.

On a matched 4 MiB Zorro II stack the PIP source pool is 224 KiB, which fits
an aligned 352x288 YUY2 frame but not a 640x360 one. The 2 MiB profile has
no PIP pool. MP3 plays on both.

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
| `--ahiunit=N` | `AHIUNIT=N` | Select AHI unit 0 to 3. |
| `--mhidriver=name` | `MHIDRIVER=name` | Select an MHI driver from `LIBS:MHI/`. The default is `mhizz9000.library`. |
| `--volume=N` | `VOLUME=N` | Set the initial volume, 0 to 100. |
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

## Network playback

ZZPlay plays MP3, FLAC and Ogg Vorbis from `http://` and `https://`
addresses, including Shoutcast and Icecast radio stations. It needs a running
TCP/IP stack that provides `bsdsocket.library` version 4, such as Roadshow,
AmiTCP or MiamiDX.

`https://` also needs AmiSSL 5. ZZPlay accepts TLS 1.2 or later and checks
the server's certificate and name against AmiSSL's certificate store. Without
AmiSSL, `https://` items fail with a message and `http://` keeps working.
ZZPlay never follows a redirect from `https://` to `http://`.

### Adding an address

- Choose **Open URL...** (Amiga-U) in the **Project** menu, type the address
  and press Return or click **Open**. ZZPlay adds it to the playlist and plays
  it. **Cancel** changes nothing; text that is not an `http://` or `https://`
  address is refused with a message.
- Give the address as a Shell argument, alone or mixed with files and drawers:
  `ZZPlay http://radio.example:8000/stream`
- List it in an M3U playlist. Addresses are kept as written when ZZPlay loads
  or saves the playlist.
- Use the ARexx `OPEN` or `ADD` command with the address.

An address that ends in `.pls`, `.m3u` or `.m3u8` is a station's playlist. It
stays one playlist entry. When the entry starts, ZZPlay downloads it and plays
the first stream in it that answers.

### What plays and how

A network item uses the same audio settings as a file: MP3 through MHI,
ZZ9000AX or AHI, and FLAC and Ogg Vorbis decoded on the card.

| Situation | What ZZPlay does |
| --- | --- |
| Shoutcast or Icecast station | Shows the station name in the playlist and the current track in the title line, updated when the station announces a new one. |
| A station drops the connection | Reconnects after 1, 2 and 4 seconds. Three failures in a row without any audio end the item, and the playlist moves on. |
| Seeking | Works for MP3 when the server reports the length and accepts byte ranges, as file servers normally do. Live stations cannot be seeked. |
| Video address (MPEG, WebM, WebP) | Refused with "video plays only from files". |
| AAC, HE-AAC or Opus station | Not playable. |
| Icecast Ogg Vorbis station | Stops at the first track change, because each new track starts a new Ogg stream. |
| Host-name lookup | Can hold the player for a moment, longer when the name server does not answer. Stop takes effect once the lookup returns. |

### Browse window

The Browse window lists and searches music services. Each service is a
module file in the `Providers` drawer next to ZZPlay (`PROGDIR:Providers/`).
The package ships two: `radio.zzprov` for [Internet radio](#internet-radio)
and `plex.zzprov` for [Plex](#plex). ZZPlay loads them the first time the
Browse window opens or a playlist entry needs one.

Choose **Browse...** (Amiga-R) in the **Project** menu to open the window.
It can also be used from the keyboard and from its own menu; see **Browse
menu** and **Browse keys** at the end of this section.

#### Finding music

- The cycle gadget at the top chooses the service. Type in the search field
  and press Return to search.
- Click a folder to open it. **Back** returns to the previous list with the
  same entry selected. A long list ends in a "--- More items ---" row; click
  it to load the next part.
- While a list loads, the status line shows each step (looking up the host,
  connecting, waiting for the service) and then "Loading 40 of 100
  items..." as rows arrive. Rows appear in groups as they come in. When the
  list is complete, the status line shows the number of items. On AmigaOS
  3.1 and newer the pointer is busy while ZZPlay waits.
- **Stop** cancels a list or search that is still loading and keeps the rows
  that already arrived. The player keeps playing while Browse waits.
- ZZPlay remembers the last 16 lists and searches until it quits. Opening
  one of them again shows it at once, with the same selection, without
  asking the service.
- **Reload** fetches the current list or search again.

#### Playing from Browse

- Double-click a track or station to add it to the playlist and play it.
  **Play** does the same for the selected entry, and **Add** only adds it.
  Entries the service marks as unplayable cannot be played or added.
- **Play next** puts the selected entry right after the one that is
  playing, without interrupting it. With nothing playing, it goes to the
  start of the playlist. This also works with shuffle on.
- **Play all** replaces the playlist with the tracks of the folder being
  shown and starts the first one. **Add all** appends them, and **Play all
  next** in the menu puts them after the playing entry. These work on
  folders the service marks as collections, such as a Plex album, artist or
  playlist. Because one click on a folder opens it, this is how you play a
  whole album or artist.
- While ZZPlay collects the tracks, the status line counts them ("Adding 37
  tracks..."). **Stop**, or opening another folder, cancels and leaves the
  playlist as it was. The playlist holds at most 4,096 entries; the status
  line says when tracks did not fit.
- **Favourite** adds the selected entry to the service's favourites, or
  removes it. Services without favourites have it disabled.
- **Settings** shows the service's settings, such as a server address or a
  token. **Save** stores them in `ENVARC:ZZPlay/<service>.prefs` and
  reconnects the service. If they cannot be saved, for example on a full
  disk, the service says so and keeps the old ones.

A service entry is stored in the playlist as `<service>://<id>`, for example
`radio://9610c519-0601-11e8-ae97-52543be04c81`, together with its title.
ZZPlay asks the module for the stream address each time the entry starts,
so these entries can be saved in M3U playlists and used from the Shell or
ARexx. Without the module, the entry fails with "no installed service module
handles ...".

#### Recently played

Once you have played something from a service, its top level starts with a
**Recently played** folder. It holds the last 25 tracks, stations and
collections started with **Play**, **Play next**, **Play all** or **Play
all next**, newest first. A double-click or Return counts as **Play**; **Add**
and **Add all** do not count. The folder opens without the network, so
recent stations are still there when a service does not answer.

Each service's list is kept in `ENVARC:ZZPlay/<service>.recent`. Delete the
file to empty the list.

#### Window layout

- The buttons sit in two rows of five, all the same width.
- Resize the window from its bottom right corner. The list and the buttons
  grow with it. The smallest size still shows every button and three list
  rows. ZZPlay remembers the window's place and size in `ZZPlay.prefs`.
- On AmigaOS 3.0 and newer, each row shows a marker, the title, the artist
  and album (or the service's details) and the length. Text that does not
  fit is cut. Older systems show one line of text per row, with "[+]" in
  front of folders.

| Marker | Meaning |
| --- | --- |
| Outline folder | A folder. |
| Folder with a play triangle | A collection that **Play all** and **Add all** work on. |
| Note | A track. |
| Mast with waves | A radio station. |

#### Browse menu

The Browse window has its own menu. An item is enabled when the matching
button is.

| Item | Shortcut | Action |
| --- | --- | --- |
| Play | Amiga-Y | Play the selected entry, as the **Play** button does. |
| Play next | Amiga-T | Queue the selected entry right after the playing one. |
| Add | Amiga-D | Append the selected entry, as the **Add** button does. |
| Play all | Amiga-G | Play the folder being shown. |
| Play all next | Amiga-H | Queue the folder being shown right after the playing entry. |
| Add all | Amiga-M | Append the folder being shown. |
| Favourite | Amiga-I | Add the selected entry to the favourites, or remove it. |
| Reload | Amiga-E | Fetch the list or search again. |
| Back | Amiga-K | Return to the previous list. |
| Close | Amiga-W | Close the Browse window. |

#### Browse keys

These keys work in the Browse window, except while you type in the search
field.

| Key | Action |
| --- | --- |
| Cursor up, Cursor down | Select the previous or next row. Down on the last row of a long list loads its next part. |
| Shift with Cursor up or down, Page Up, Page Down | Move the selection a page. |
| Alt or Ctrl with Cursor up or down, Home, End | Select the first or the last row. |
| Return | Open the selected folder, or play the selected track or station. |
| Backspace | Back to the previous list. |
| A letter or digit | Select the next row whose title starts with it, from the top again after the last. |
| Tab | Move to the search field. Return there searches. |

### Internet radio

`radio.zzprov` uses the radio-browser.info station directory over plain
`http://`, so it does not need AmiSSL. Its top level has four folders:

- **Favourites**: the stations you added with **Favourite** (up to 64).
- **Top voted**: the directory's most popular stations.
- **By tag**: genres and other tags, each opening a list of stations.
- **By country**: countries, each opening a list of stations.

Search finds stations by name and leaves out stations the directory marks
as broken. Each station shows its codec, bitrate and country. MP3 and Ogg
Vorbis stations play. Stations in other formats (AAC, HE-AAC, Opus) are
marked as unsupported.

Favourites are stored in `ENVARC:ZZPlay/radio.prefs`, one line per station:
`fav_<n>=<id>|<name>|<codec>|<bitrate>|<country>`.

### Plex

`plex.zzprov` plays the music libraries of a Plex Media Server.

To sign in from the Browse window:

1. Open Plex Media Server in the Browse window.
2. Open **Sign in with plex.tv**.
3. On another device (such as a phone or PC), open `plex.tv/link` in a browser
   and enter the code shown in ZZPlay.
4. Open **Continue after entering the code**.

ZZPlay then asks plex.tv for your servers. If your account has more than
one, open the one you want. ZZPlay remembers the server's `https` and plain
`http` addresses, and the **Connection** setting below picks which one it
uses. The music library opens right away, and later sessions start there.

To sign in again, clear **Token** in Settings. If the server stops
accepting the token, for example after it was revoked, opening Plex says so.

To set up the connection by hand, choose Plex Media Server in the Browse
window, click **Settings** and set:

- **Server URL**: the server's address with its port, for example
  `http://192.168.1.10:32400`.
- **Connection**: `Secure (https)`, the default, or `Plain (http, faster)`.
  Plain `http` starts tracks much faster on a 68k, but the token and what
  you play cross the home network unencrypted. After a plex.tv sign-in you
  can switch without signing in again. If the chosen address does not
  answer, the error message names this setting.
- **Token**: your Plex token. In Plex Web, open a track's menu, choose
  **Get Info**, then **View XML**. The address of the page that opens ends in
  `X-Plex-Token=` followed by the token.
- **Bitrate**: the MP3 bitrate in kbit/s for tracks the server has to
  convert. The default is 192.

The token is stored in `ENVARC:ZZPlay/plex.prefs` as plain text. ZZPlay sends
it only to that server and, during sign-in, to plex.tv. For a plain `http://`
connection, set the server's **Secure connections** option (Settings,
Network) to **Preferred**, not **Required**.

A server with one music library opens on that library's folders:
**Artists**, **Albums**, **Genres**, **Recently added** and **Playlists**.
With more than one library, the top level has a folder per library plus
**Playlists**.

- **Artists** and **Albums** open on letter folders with counts, such as
  "A (72)". "#" holds names that start with a digit or symbol. A letter
  lists 100 entries at a time.
- **Genres** lists the server's genres; a genre lists its artists.
- **Recently added** lists albums, newest first.
- An artist opens on its albums, and an album or playlist on its tracks.
  Album rows show the artist and year; track rows show artist, album and
  length.

A search returns up to three folders, **Artists (n)**, **Albums (n)** and
**Tracks (n)**, with the number of hits in each.

MP3, FLAC and Ogg Vorbis tracks play straight from the server, and MP3
tracks can be seeked. The server converts other formats (AAC, ALAC, WAV and
so on) to MP3 while they play; those cannot be seeked.

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

When the sound plays through the card (MHI with `mhizz9000.library`, or the
card's own output), the playing time follows frames the card has actually
output, divided by its 48 kHz rate, not the file's bitrate. A VBR file and a
file with a large ID3 tag therefore keep time with the audio. Over AHI the
time follows the frames the AHI unit has played, so it also runs on Paula
and other non-card units. Another MHI driver keeps a bitrate estimate.

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
| Project | Open URL... | Amiga-U | Add a network address to the playlist and play it. |
| Project | Browse... | Amiga-R | Open the Browse window for service modules. |
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
| `BROWSE` | `left,top,width,height` of the Browse window |
| `DRAWER` | the last drawer used in a file requester |

## Fullscreen video

**Fullscreen video** in the Control menu (Amiga-F) is a setting: while it is
checked, every video in the playlist plays fullscreen, and the desktop player
remembers it in the prefs file (`FULLSCREEN`) for the next launch. It can be
changed while nothing is playing. A one-shot launch plays fullscreen only when
`--fullscreen` or `FULLSCREEN` is given.

Fullscreen opens a screen of its own and scales the video up or down to fit
it, keeping the aspect ratio. When the Workbench display mode is usable
(15/16-bit or truecolor, not pixel-doubled), the fullscreen screen uses the
same mode, so the resolution and refresh rate do not change. On an 8-bit
(CLUT) Workbench it uses the same resolution in 16-bit. If that mode cannot
be opened, or the Workbench mode is pixel-doubled, ZZPlay picks another
available mode. If no mode can show the video, it stays in a window. Press
`F` to return to the window at its previous place and size.

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
| `PLAYALL service id` | Replace the playlist with a service collection and play its first entry. Returns the number added. |
| `ADDALL service id` | Append a service collection. Returns the number added. |
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
| `BROWSE` | Open the Browse window, or bring it to the front. |
| `SERVICES` | Return installed services, one `scheme name` per line. Modules load on first use. |
| `SEARCH service query...` | Return up to 100 playable matches, one `id title` per line. Collections end in `(collection)`. For Plex, the hits inside each result group are listed. |
| `VERSION`, `HELP` | Return the version string or the command list. |
| `QUIT` | Quit ZZPlay. |

The service commands take the service name that `SERVICES` lists (`radio`,
`plex`). The ids from `SEARCH` are the ones Browse uses. A track or station
plays with `OPEN service://id`, for example `OPEN plex://34291`. A
collection id such as `alb:11113` goes to `PLAYALL` or `ADDALL`. From the
Shell:

```text
rx "address ZZPLAY BROWSE"
rx "address ZZPLAY SERVICES"
rx "address ZZPLAY SEARCH plex paris"
rx "address ZZPLAY PLAYALL plex alb:1234"
rx "address ZZPLAY ADDALL plex alb:1234"
```

Values come back in `RESULT` when the script uses `OPTIONS RESULTS`.

| Return code | Meaning |
| --- | --- |
| 0 | Success. |
| 5 | The command cannot apply right now: `PAUSE` with nothing playing, `SEEK` on a format without seeking, a `SEARCH` with no matches, or a collection with nothing to add. |
| 10 | An unknown command, a wrong argument, a position outside the playlist, an unknown or failed service, or `ADD`/`OPEN` paths that add nothing (`OPEN` then leaves the playlist unchanged). |

`ADD`, `OPEN`, `SEARCH`, `PLAYALL` and `ADDALL` can take a while. Like a drop
on the window, they pause the playing item while they run. The window keeps
responding during network waits, so Stop or Quit cancels them. The script
waits for the reply, and commands from other scripts wait their turn.

## Diagnostics

`--benchmark` measures decode throughput without display pacing. `--fps` shows
rolling playback and decode-call frame rates. `--trace` writes per-frame timing
diagnostics.

`zz9k-mp3` remains the lower-level MP3 decoder diagnostic. Use it when raw or
WAV output is needed for exactness checks.
