# ZZ9000 Sound DataType

Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio

`zz9k-sound.datatype 42.1` is the SDK v2 MP3, native FLAC and Ogg Vorbis sound DataType. It is packaged
as a side-by-side subclass of the system `sound.datatype` and must not
replace `Classes/DataTypes/sound.datatype`. OS3.1 remains the minimum
target. The class always subclasses `sound.datatype` and chooses its output
from that class's version at open time, not from OS branding: version 47 or
later gets 16-bit output; anything earlier gets the legacy 8-bit mono path.
On OS 3.2 the separate `v41sound.datatype` compatibility class ignores
`SDTA_BitsPerSample`; the system's own 16-bit WAVE loader subclasses
`sound.datatype`, and so does this class.

The class binary installs as:

```text
Classes/DataTypes/zz9k-sound.datatype
```

The recognition descriptors are packaged inactive under `Storage/DataTypes`
so activation is an explicit install step:

```text
Storage/DataTypes/ZZ9000-MP3
Storage/DataTypes/ZZ9000-MP3.info
Storage/DataTypes/ZZ9000-FLAC
Storage/DataTypes/ZZ9000-FLAC.info
Storage/DataTypes/ZZ9000-OggVorbis
Storage/DataTypes/ZZ9000-OggVorbis.info
```

To activate the validated DataType path on a test or release-install system,
install the class and copy the descriptors into `DEVS:DataTypes`:

```text
copy Classes/DataTypes/zz9k-sound.datatype TO SYS:Classes/DataTypes/
copy Storage/DataTypes/ZZ9000-MP3#? TO DEVS:DataTypes/
copy Storage/DataTypes/ZZ9000-FLAC#? TO DEVS:DataTypes/
copy Storage/DataTypes/ZZ9000-OggVorbis#? TO DEVS:DataTypes/
AddDataTypes DEVS:DataTypes/ZZ9000-MP3 DEVS:DataTypes/ZZ9000-FLAC DEVS:DataTypes/ZZ9000-OggVorbis
AddDataTypes LIST
```

`AddDataTypes LIST` should show `ZZ9000-MP3`, `ZZ9000-FLAC` and
`ZZ9000-OggVorbis` before sound-capable clients are expected to route
matching files to `zz9k-sound.datatype`. Each descriptor can be activated on
its own. Keeping the descriptors in `Storage/DataTypes` by default prevents
accidental global routing on systems that only want the SDK tools, ZZPlay,
or manual smoke tests. Deactivating works the other way round: remove the
descriptor from `DEVS:DataTypes` and run `AddDataTypes REFRESH`.

## Decoded formats and the recognition envelope

The class decodes MPEG audio **Layer III only**: MPEG-1 (44100/48000/32000
Hz), MPEG-2 (22050/24000/16000 Hz), and MPEG-2.5 (11025/12000/8000 Hz)
Layer III, mono or stereo, CBR and VBR. Layer I, Layer II, and reserved
version/layer combinations are rejected. Streams may carry a leading ID3v2
tag (skipped with a synchsafe, footer-aware, 16 MiB-bounded size parse) and
a trailing ID3v1 `TAG` block (stripped). VBR duration is never trusted for
allocation; output size grows from what the decoder actually produces.

Recognition is split between the descriptor and the class:

- A bytewise `dth_Mask` cannot express "optional ID3v2 tag, then a Layer
  III frame", and widening it to `ID3` would claim arbitrary ID3 data. The
  `ZZ9000-MP3` descriptor therefore carries an empty mask and an embedded
  recognition hook (`DTCD` chunk, built from
  `amiga/datatypes/zz9k_mp3_dthook.c`). The hook claims a file only when a
  bounded ID3v2 tag, or none, is followed directly by two consecutive Layer
  III frame headers with matching sample rate and channel mode. It reads at
  most 2,896 bytes at the audio start and restores the file position. Layer
  I/II streams, ID3-only data, and streams whose first frame does not follow
  the tag are not claimed.
- `zz9k-sound.datatype` uses the same recognizer
  (`amiga/datatypes/zz9k_sound_mp3.h`) and re-validates every routed file,
  additionally scanning a 16 KiB window after the tag for the first frame
  pair. Files that reach the class but fail re-validation fail object
  creation with `DTERROR_INVALID_DATA`.

The hook has no startup code, takes library bases from the hook context,
and avoids hardware division, so it runs on 68000 systems.

### Native FLAC

The class decodes native FLAC (`fLaC` marker) with mono or stereo audio,
4 to 24 bits per sample, at 8 to 192 kHz. Ogg-FLAC, multichannel streams,
and wider samples are not claimed. The `ZZ9000-FLAC` descriptor matches the
4-byte `fLaC` marker; the class re-validates the 34-byte `STREAMINFO` block
with the recognizer it shares with ZZPlay
(`amiga/datatypes/zz9k_sound_flac.h`) and feeds the whole file, metadata
included, to the card's FLAC stream decoder through the codec-aware
`ZZ9KAudioStreamBeginEx()`. Sources up to 16 bits decode to S16BE; wider
sources decode to MSB-justified S32BE so the modern path keeps their
precision.

### Ogg Vorbis

The class decodes one logical Ogg Vorbis stream, mono or stereo, at 8 to
192 kHz, to S16BE. Vorbis I puts only its 30-byte identification packet on
the first Ogg page, so the `ZZ9000-OggVorbis` descriptor matches that
58-byte page's fixed fields (`OggS`, version 0, BOS, granule and sequence
0, one 30-byte segment, `\x01vorbis`, Vorbis version 0) with the serial
number and CRC as wildcards. The class re-validates the page CRC, channel
count, rate, block sizes and framing bit with the recognizer it shares with
ZZPlay (`amiga/datatypes/zz9k_sound_vorbis.h`), then feeds the whole file to
the card's Vorbis stream decoder through `ZZ9KAudioStreamBeginEx()`. Ogg
Opus, Ogg-FLAC and other Ogg codecs are not claimed. A chained or
multiplexed file (a second logical stream anywhere) fails object creation:
the firmware reports the extra stream instead of returning only the first
link.

## Whole-sample memory model

The class is a whole-sample decoder, not a streaming one. Neither probed
superclass stack offers `DTM_SAMPLE`, so there is no progressive playback
contract to claim: `OM_NEW` decodes the complete file and publishes one
sample buffer before returning.

Peak allocation during construction:

- the sample: one `AllocVec` plane per channel on modern stereo, one plane
  otherwise, each starting at 64 KiB, growing geometrically,
  multiplier-checked, hard-capped at 256 MiB;
- a 128 KiB card-only compressed input ring;
- a host-window PCM ring plus staging pair, 64 KiB each, shrinking in
  bounded halving steps toward a 4 KiB staging floor when the host-visible
  heap is compact (the same shrink ladder the archive client uses). The PCM
  ring never shrinks below the decoder's largest output unit: an MP3 frame
  (4608 bytes), the stream's largest FLAC block, or half a Vorbis long
  block, per channel and sample width. If even that cannot be allocated,
  creation fails with `ERROR_NO_FREE_STORE` before any decoding;
- one file-read staging `AllocVec` of the staging-pair size.

Every firmware resource (session, rings) is closed and freed before the
sample is published; only the sample planes survive construction. Steady
state, for N samples per channel:

- modern output: `N × 2` bytes per channel plane (16-bit), or `N × 4` for
  FLAC sources wider than 16 bits (32-bit);
- legacy output: `N` bytes (8-bit mono).

A 44.1 kHz stereo file therefore holds roughly 176 KiB of sample per second
of audio on modern systems and 44 KiB on legacy systems. Whole-file MP3s of
a few minutes are fine; leave hour-long material to ZZPlay's streaming
engine.

The decode follows the same audio-stream protocol as `zz9k-mp3`: each feed
hands the firmware a whole staged chunk, `bytes_consumed` is the decoder's
cumulative consumption from the input ring, PCM credit is returned before the
ring fills, and a `BACKPRESSURE` result is answered with a forced `Read`
before the same chunk is fed again. Loops give up only after 64 consecutive
iterations without progress, so long files on the 4 KiB compact Zorro II
window are not cut short.

## Modern and legacy output

All sample attributes are passed as creation tags to the superclass
`OM_NEW`: the class decodes the file named by `DTA_Name` first (before the
superclass constructs the object, `DTA_Handle` carries datatypes.library's
lock, not a file handle), then constructs the object with:

| Property | Modern (`sound.datatype` ≥ 47) | Legacy (`sound.datatype` < 47) |
| --- | --- | --- |
| Sample data | stereo: `SDTA_LeftSample` + `SDTA_RightSample` (separate `AllocVec` planes); mono: `SDTA_Sample` | `SDTA_Sample`, planar 8-bit mono |
| Container | signed big endian: 16-bit, or 32-bit MSB-justified for FLAC wider than 16 bits | signed 8-bit |
| `SDTA_BitsPerSample` | 16, or 32 for FLAC wider than 16 bits | not set |
| Rate | `SDTA_SamplesPerSec` = source rate, no period | `SDTA_Period` from the system colour clock (5 × `ex_EClockFrequency`) |
| `SDTA_SampleLength` | bytes in one channel plane, as the system WAVE loader publishes 16-bit data | samples (= bytes) |
| `SDTA_Volume` | 64 | 64 |

On the qualified OS 3.2 stack the superclass reports and plays back these
objects exactly like the system WAVE loader's 16-bit objects, including its
own Paula rate clamp (a 44.1 kHz source reports 28603 Hz at period 124 on the
emulated A4000). That clamp is superclass playback behaviour, not a class
setting.

Stereo on the legacy path is averaged and quantized to signed 8-bit with a
saturating clamp that covers the full -32768..32767 range without overflow;
32-bit FLAC output is narrowed to its top 16 bits first.
Memory-source objects (`DTST_RAM`/`DTST_MEMORY`) are refused: the class
decodes only named files (`DTST_FILE` with `DTA_Name`).

## Ownership and disposal contract

Each published plane is handed to the superclass exactly once and freed by
the superclass exactly once:

- `OM_NEW` publishes exact `AllocVec` bases with `SDTA_FreeSampleData =
  TRUE`; the class keeps no instance data.
- `OM_DISPOSE` frees nothing itself; the superclass frees every published
  plane.
- If the superclass `OM_NEW` fails, the unpublished planes are freed once.
  Construction never hands out an interior pointer, and failure cleanup
  closes the firmware session, frees the rings, and frees unpublished planes
  exactly once, idempotently.

The single-free behaviour was first probed on 2026-10-09 with guard-byte
buffers and an instrumented exec `FreeVec` LVO hook (datatypes.library 40.6
/ sound.datatype 40.6 on an OS 3.1 copy; datatypes.library 47.3 on a KS
47.115 copy). The installed class was then qualified on both stacks with
repeated open/dispose cycles of whole decoded files (hundreds of KiB per
object, stereo planes on v47) leaving free memory unchanged.

## Errors

- Hook rejects the file (Layer I/II, ID3-only data, no Layer III frame
  directly after the tag): `datatypes.library` routes it elsewhere; no class
  code runs.
- Descriptor matched but content invalid (broken frame chain, Layer I/II
  body, truncated stream, a FLAC `STREAMINFO` outside the envelope above,
  corrupt FLAC frames, a corrupt or truncated Ogg stream, a chained or
  multiplexed Ogg file): object creation fails with `DTERROR_INVALID_DATA`.
- Firmware or `zz9k.library` without the matched audio-stream service
  fails creation with `ERROR_NOT_IMPLEMENTED` rather than falling back to
  software decode. MP3 needs the `MP3_DECODE`, `MP3_STREAM`, and
  `PCM16_STEREO` flags; FLAC and Ogg Vorbis need `FLAC_STREAM` or
  `VORBIS_STREAM` and `zz9k.library` revision 33
  (`ZZ9K_LIBRARY_MIN_REVISION_AUDIO_STREAM_EX`). Current firmware implements
  both decoders but does not advertise them before physical qualification.
- Audio-stream session busy (another client holds it): `ERROR_OBJECT_IN_USE`.
  Shared-memory, ring or sample allocation failure: `ERROR_NO_FREE_STORE`.

## Coexistence with players

The class never uses the MHI path or audio ring acquire/release opcodes: it
opens its own unbound audio-stream session (begin/feed/read/close only),
copies PCM out of the host ring before each acknowledgement, and closes the
session before publishing the sample. If the audio service cannot grant a
session while a player is active, the datatype decode fails with
`ERROR_OBJECT_IN_USE` instead of stealing a session or sink. On the emulated
v47 stack a datatype decode during ZZPlay MHI playback failed that way and
ZZPlay's playback completed undisturbed. Physical Zorro II/III coexistence
(card heap pressure, core-1 scheduling, bound-card clients, and whether the
card grants concurrent sessions) is qualified separately on hardware.

## Building descriptors

The `.dtid` sources in `amiga/datatypes/descriptors/` follow the picture
descriptor workflow: both package scripts discover them, generate the IFF
`DTYP` files, and copy the matching `.info` icons. `ZZ9000-MP3.dtid` names
its hook with `Code=zz9k-mp3-recog` and `Recog=none`; both m68k build
scripts produce `build/dtcode/zz9k-mp3-recog`, and the package scripts pass
`--code-dir build/dtcode` so the generator embeds it as the `DTCD` chunk.
The generator rejects an empty mask without a hook and a hook that is not
an AmigaDOS hunk executable. Descriptor structure is covered by the host
`datatype_descriptor_test`; recognition, conversion, growth, and cleanup
logic are covered by `tests/sound_datatype_contract_test.c`, which can be
run on the host via CTest or cross-built and executed under Vamos with:

```text
sh tests/run_sound_datatype_contract_test.sh
```

Emulator qualification (Amiberry, emulated ZZ9000 Z3) covered both stacks:
recognition through the `DTCD` hook, whole-file decode, published sample
layout compared against an independent decode, playback duration against the
system WAVE loader on v47, disposal, and coexistence with ZZPlay. Physical
Zorro II/III results are a separate release gate.
