# ZZ9000 Sound DataType

Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio

`zz9k-sound.datatype 42.1` lets Amiga DataTypes clients decode MP3, native
FLAC, and Ogg Vorbis files through the ZZ9000 audio-stream service. It reads a
complete named file during object creation, then gives the decoded PCM sample
to the system `sound.datatype` for playback.

Install it beside the system class. Do not replace
`Classes/DataTypes/sound.datatype`. OS 3.1 is the minimum target. The class
opens `datatypes/sound.datatype` and selects its output from that class's
version: version 47 and later receives modern 16-bit or 32-bit sample tags;
earlier versions receive 8-bit mono data. The OS 3.2
`v41sound.datatype` compatibility class ignores `SDTA_BitsPerSample`, so this
class subclasses `sound.datatype` instead.

| Input | Accepted envelope | PCM published on `sound.datatype` ≥ 47 |
| --- | --- | --- |
| MP3 | MPEG-1, MPEG-2, or MPEG-2.5 Layer III; mono or stereo; CBR or VBR | S16BE |
| Native FLAC | `fLaC`; mono or stereo; 4–24 bits per sample; 8–192 kHz | S16BE for up to 16-bit input; MSB-justified S32BE for 17–24-bit input |
| Ogg Vorbis | One mono or stereo Vorbis I logical stream; 8–192 kHz | S16BE |

Layer I, Layer II, Ogg-FLAC, Ogg Opus, multichannel streams, and FLAC samples
wider than 24 bits are rejected.

## Install and activate

The package places the class here:

```text
Classes/DataTypes/zz9k-sound.datatype
```

Its descriptors remain inactive until you copy them from
`Storage/DataTypes`:

```text
Storage/DataTypes/ZZ9000-MP3
Storage/DataTypes/ZZ9000-MP3.info
Storage/DataTypes/ZZ9000-FLAC
Storage/DataTypes/ZZ9000-FLAC.info
Storage/DataTypes/ZZ9000-OggVorbis
Storage/DataTypes/ZZ9000-OggVorbis.info
```

Copy the class and the descriptors you want to use, then register them:

```text
copy Classes/DataTypes/zz9k-sound.datatype TO SYS:Classes/DataTypes/
copy Storage/DataTypes/ZZ9000-MP3#? TO DEVS:DataTypes/
copy Storage/DataTypes/ZZ9000-FLAC#? TO DEVS:DataTypes/
copy Storage/DataTypes/ZZ9000-OggVorbis#? TO DEVS:DataTypes/
AddDataTypes DEVS:DataTypes/ZZ9000-MP3 DEVS:DataTypes/ZZ9000-FLAC DEVS:DataTypes/ZZ9000-OggVorbis
AddDataTypes LIST
```

Confirm that `AddDataTypes LIST` includes each descriptor you installed before
opening matching files in a sound-capable client. You may activate each
descriptor separately. To deactivate one, remove it from `DEVS:DataTypes` and
run `AddDataTypes REFRESH`.

## Recognition and decoding

Descriptors select this class, and the class validates the source again before
it opens an audio-stream session.

| Format | Descriptor recognition | Class validation and decode |
| --- | --- | --- |
| MP3 | `ZZ9000-MP3` has `Recog=none` and the embedded `zz9k-mp3-recog` `DTCD` hook. The hook accepts an optional bounded ID3v2 tag followed immediately by two Layer III frame headers with matching rate and channel count. | Parses a synchsafe ID3v2 size, including the optional footer, up to 16 MiB; removes a trailing 128-byte ID3v1 `TAG`; then searches the first 16 KiB after the tag for a valid frame pair. It feeds audio from the first frame to the card MP3 stream decoder. |
| FLAC | `ZZ9000-FLAC` matches `fLaC`. | Checks the 42-byte marker and `STREAMINFO` prefix, then sends the file and its metadata to the FLAC stream decoder with `ZZ9KAudioStreamBeginEx()`. |
| Ogg Vorbis | `ZZ9000-OggVorbis` matches the fixed fields of the 58-byte Vorbis I identification page. The serial number and CRC are wildcards. | Checks the first-page CRC, channel count, rate, block sizes, and framing bit, then sends the file to the Vorbis stream decoder with `ZZ9KAudioStreamBeginEx()`. A chained or multiplexed stream fails rather than returning only its first logical stream. |

The MP3 hook restores the file position, has no startup code, receives library
bases from its hook context, and avoids hardware division for 68000 systems.
A file rejected by the hook is left for `datatypes.library` to route elsewhere.
A selected file that fails class validation returns `DTERROR_INVALID_DATA`.

The decoder uses an unbound audio-stream session. It supplies staged input,
copies PCM from the host-visible ring before acknowledging it, and closes the
session before it publishes the sample. A `BACKPRESSURE` result triggers a
forced read before the same input chunk is retried. The loop stops after 64
successive iterations with no consumed input and no produced PCM.

## Sample layout and limits

The class accepts `DTST_FILE` sources with `DTA_Name`. It refuses
`DTST_RAM` and `DTST_MEMORY`; no progressive `DTM_SAMPLE` path is provided.

| Property | `sound.datatype` ≥ 47 | Earlier `sound.datatype` |
| --- | --- | --- |
| Sample planes | Mono: `SDTA_Sample`. Stereo: separate `SDTA_LeftSample` and `SDTA_RightSample`. | `SDTA_Sample`, 8-bit mono. |
| Sample format | Signed big-endian 16-bit, or 32-bit MSB-justified FLAC output for 17–24-bit sources. | Signed 8-bit. Stereo is averaged; 32-bit FLAC is narrowed to its top 16 bits first. |
| Rate | `SDTA_SamplesPerSec` is the source rate. | `SDTA_Period` uses `5 × ex_EClockFrequency`. |
| `SDTA_SampleLength` | Bytes in one channel plane. | Samples, which equal bytes. |
| `SDTA_BitsPerSample` | 16 or 32. | Not set. |
| `SDTA_Volume` | 64 | 64 |

Each published plane is an `AllocVec` allocation passed to the superclass with
`SDTA_FreeSampleData = TRUE`. The superclass frees published planes on
`OM_DISPOSE`. If `OM_NEW` fails, the class releases only unpublished planes
and all temporary stream resources.

Sample planes start at 64 KiB, grow geometrically, and cannot exceed 256 MiB
per plane. The decoder also allocates a 128 KiB card-only input ring and a
host-window PCM ring with a staging buffer. Both host allocations start at
64 KiB; the staging buffer may shrink by halving to 4 KiB when the
host-visible heap is tight. The PCM ring always retains enough space for the
largest decoder output unit.

For a sample with `N` frames per channel, modern output uses `N × 2` bytes per
channel, or `N × 4` for FLAC wider than 16 bits. Legacy output uses `N` bytes.
A 44.1 kHz stereo sample occupies about 176 KiB per second on the modern path
and 44 KiB per second on the legacy path. Long material uses memory in direct
proportion to its duration; use ZZPlay's streaming playback for hour-long
files.

## Service requirements and errors

MP3 needs `ZZ9K_SERVICE_FLAG_AUDIO_MP3_DECODE` and
`ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM`. FLAC and Vorbis need, respectively,
`ZZ9K_SERVICE_FLAG_AUDIO_FLAC_STREAM` or
`ZZ9K_SERVICE_FLAG_AUDIO_VORBIS_STREAM`, plus `zz9k.library` revision 33
(`ZZ9K_LIBRARY_MIN_REVISION_AUDIO_STREAM_EX`). A missing library revision or
service flag returns `ERROR_NOT_IMPLEMENTED`; the class does not perform a
software decode.

Firmware implements the FLAC and Vorbis stream decoders, but its source marks
their service flags for physical Zorro II and Zorro III qualification. Builds
that do not advertise the required flag therefore return
`ERROR_NOT_IMPLEMENTED` for that format.

| Condition | Result |
| --- | --- |
| Hook rejects the source | `datatypes.library` may route it to another DataType. |
| Class validation or stream decode fails | `DTERROR_INVALID_DATA` unless a resource error is available. |
| Another client owns the audio-stream session | `ERROR_OBJECT_IN_USE` |
| Shared-memory, ring, staging, or sample allocation fails | `ERROR_NO_FREE_STORE` |

The class does not use MHI or the audio ring acquire/release opcodes. A decode
that collides with a player keeps the player's session intact and fails with
`ERROR_OBJECT_IN_USE` if the service does not grant another session.

## Packaging and recorded qualification

The `.dtid` files in `amiga/datatypes/descriptors/` define the inactive
descriptors. Both m68k build scripts generate
`build/dtcode/zz9k-mp3-recog`; both package scripts call
`generate-datatype-descriptors.py` with `--code-dir build/dtcode` to embed the
hook in the MP3 descriptor. The generator rejects an empty mask without a
hook and rejects a hook that is not an AmigaDOS hunk executable.

`datatype_descriptor_test` covers descriptor generation. The host
`sound_datatype_contract_test` covers recognition, conversion, allocation
growth, and cleanup. Run the m68k/Vamos harness from `sdk/` with:

```text
sh tests/run_sound_datatype_contract_test.sh
```

| Environment or date | Recorded result |
| --- | --- |
| 2026-10-09, datatypes.library 40.6 / sound.datatype 40.6 on an OS 3.1 copy; datatypes.library 47.3 on KS 47.115 | Guard-byte buffers and an instrumented exec `FreeVec` LVO hook confirmed one free per published plane. Repeated open/dispose cycles of decoded files left free memory unchanged. |
| Amiberry with emulated ZZ9000 Z3 | The class was exercised on both superclass paths for descriptor-hook recognition, complete-file decode, sample layout against an independent decode, v47 playback duration against the system WAVE loader, disposal, and coexistence with ZZPlay. |
| Amiberry v47 during ZZPlay MHI playback | A concurrent DataType decode returned `ERROR_OBJECT_IN_USE`; ZZPlay playback completed. |
| Physical Zorro II and Zorro III | Coexistence, card-heap pressure, core-1 scheduling, bound-card clients, and concurrent-session behavior remain release-gate checks. |
