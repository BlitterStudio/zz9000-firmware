# ZZ9000 Sound DataType

Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio

`zz9k-sound.datatype 42.1` is the SDK v2 MP3 sound DataType. It is packaged
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

The MP3 recognition descriptor is packaged inactive under `Storage/DataTypes`
so activation is an explicit install step:

```text
Storage/DataTypes/ZZ9000-MP3
Storage/DataTypes/ZZ9000-MP3.info
```

To activate the validated DataType path on a test or release-install system,
install the class and copy the descriptor into `DEVS:DataTypes`:

```text
copy Classes/DataTypes/zz9k-sound.datatype TO SYS:Classes/DataTypes/
copy Storage/DataTypes/ZZ9000-MP3#? TO DEVS:DataTypes/
AddDataTypes DEVS:DataTypes/ZZ9000-MP3
AddDataTypes LIST
```

`AddDataTypes LIST` should show `ZZ9000-MP3` before sound-capable clients
are expected to route matching files to `zz9k-sound.datatype`. Keeping the
descriptor in `Storage/DataTypes` by default prevents accidental global
routing on systems that only want the SDK tools, ZZPlay, or manual smoke
tests. Deactivating works the other way round: remove the descriptor from
`DEVS:DataTypes` and run `AddDataTypes REFRESH`.

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

## Whole-sample memory model

The class is a whole-sample decoder, not a streaming one. Neither probed
superclass stack offers `DTM_SAMPLE`, so there is no progressive playback
contract to claim: `OM_NEW` decodes the complete file and publishes one
sample buffer before returning.

Peak allocation during construction:

- the sample: one `AllocVec` plane per channel on modern stereo, one plane
  otherwise, each starting at 64 KiB, growing geometrically,
  multiplier-checked, hard-capped at 256 MiB;
- a 128 KiB card-only MP3 input ring;
- a host-window PCM ring plus staging pair, 64 KiB each, shrinking in
  bounded halving steps down to a 4 KiB floor when the host-visible heap is
  compact (the same shrink ladder the archive client uses);
- one file-read staging `AllocVec` of the staging-pair size.

Every firmware resource (session, rings) is closed and freed before the
sample is published; only the sample planes survive construction. Steady
state, for N samples per channel:

- modern output: `N × 2` bytes per channel plane (16-bit);
- legacy output: `N` bytes (8-bit mono).

A 44.1 kHz stereo file therefore holds roughly 176 KiB of sample per second
of audio on modern systems and 44 KiB on legacy systems. Whole-file MP3s of
a few minutes are fine; leave hour-long material to ZZPlay's streaming
engine.

The decode follows the same audio-stream protocol as `zz9k-mp3`: each feed
hands the firmware a whole staged chunk, `bytes_consumed` is the decoder's
cumulative consumption from the MP3 ring, PCM credit is returned before the
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
| Container | signed 16-bit big endian | signed 8-bit |
| `SDTA_BitsPerSample` | 16 | not set |
| Rate | `SDTA_SamplesPerSec` = source rate, no period | `SDTA_Period` from the system colour clock (5 × `ex_EClockFrequency`) |
| `SDTA_SampleLength` | bytes in one channel plane, as the system WAVE loader publishes 16-bit data | samples (= bytes) |
| `SDTA_Volume` | 64 | 64 |

On the qualified OS 3.2 stack the superclass reports and plays back these
objects exactly like the system WAVE loader's 16-bit objects, including its
own Paula rate clamp (a 44.1 kHz source reports 28603 Hz at period 124 on the
emulated A4000). That clamp is superclass playback behaviour, not a class
setting.

Stereo on the legacy path is averaged and quantized to signed 8-bit with a
saturating clamp that covers the full -32768..32767 range without overflow.
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
  body, truncated stream): object creation fails with `DTERROR_INVALID_DATA`.
- Firmware or `zz9k.library` without the matched audio-stream service
  (`MP3_DECODE`, `MP3_STREAM`, `PCM16_STEREO` flags): creation fails with
  `ERROR_NOT_IMPLEMENTED` rather than falling back to software decode.
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
