# ZZ9000AX voice engine

The voice engine is a wavetable mixer on the card. An Amiga program uploads
samples once, then sends small timestamped commands ("start voice 3 at this
frame", "change its period") and the firmware mixes the voices at 48 kHz into
the ZZ9000AX output. It suits tracker replayers, game audio and anything else
that would otherwise mix on the 68k.

This document is the one place for the voice contract. The wire layouts are
in `zz9k/voice.h`, the library calls in [zz9k-library.md](zz9k-library.md),
and the firmware side of the mixing path in
[audio-fabric.md](../../docs/audio-fabric.md).

## What the engine does

- Up to 32 voices, granted at open.
- Samples are signed 8-bit or signed 16-bit big-endian, kept in a sample bank
  on the card of 1 byte to 32 MiB. Up to 256 samples (ids 0 to 255) can be
  defined over the bank.
- A voice plays at a Paula period, divided into the PAL or the NTSC clock
  chosen at open, or at a rate in Hz. Periods below 5 are ignored.
- Volume is set per side, left and right, each 0 to 65535, and a master gain
  scales the mix.
- A voice plays forward or backward and can loop. A loop is a start and a
  length in bytes inside the sample.
- Playback reads the nearest sample, or interpolates linearly between two
  samples when the session was opened with the linear flag.
- Commands carry the 48 kHz frame they take effect at, so a note starts on
  exactly that output frame.

The engine is integer only. Each voice contributes `sample * volume >> 16` to
the left and right sums, so a volume of 65535 is close to unity. The sums are
scaled by the master gain (Q8.8, 256 is unity) and clamped to 16 bits.

One voice session can be open at a time. The engine feeds the firmware-owned
fourth slot of the audio fabric and mixes with the other fabric producers.

## Using it

### From zz9k.library

The six calls need `zz9k.library` revision 42
(`ZZ9K_LIBRARY_MIN_REVISION_AUDIO_VOICE`). Open fills a `ZZ9KVoiceSession`,
and every later call takes it.

| Call | LVO | Registers | Purpose |
| --- | ---: | --- | --- |
| `ZZ9KVoiceOpen(desc, session)` | -354 | a0, a1 | Allocate the buffers, open the session, fill `session` with the grant |
| `ZZ9KVoiceClose(session)` | -360 | a0 | End the session and free the buffers |
| `ZZ9KVoiceReset(session)` | -366 | a0 | Drop queued commands and silence all voices |
| `ZZ9KVoiceUpload(session, desc)` | -372 | a0, a1 | Copy one sample into the bank and define it |
| `ZZ9KVoiceWrite(session, records, count)` | -378 | a0, a1, d0 | Queue command records; returns how many were taken |
| `ZZ9KVoiceGetStatus(session, status)` | -384 | a0, a1 | Read the status block and bump the heartbeat |

Open, Close, Reset and Upload are mailbox calls and return a `ZZ9K_STATUS_*`
code. Write and GetStatus only touch shared memory. The resident library
skips its transport lock for them, so a player that feeds the ring is not
held up by another task blocked in a synchronous call.

The descriptors and the record builders are in `zz9k/voice.h`:

| Item | Use |
| --- | --- |
| `ZZ9KVoiceOpenDesc` | `voices`, `bank_bytes`, `lead_frames` (0 asks for the floor), `flags`, `ring_bytes` and `staging_bytes` (0 selects the defaults) |
| `ZZ9KVoiceUploadDesc` | `sample_id`, `bank_offset`, `data`, `length`, `format` |
| `ZZ9KVoiceGrant` (in `session.grant`) | What the firmware granted |
| `ZZ9KVoiceStatus` | Host-order copy of the status block |
| `zz9k_voice_build_start()` and the other `zz9k_voice_build_*()` helpers | Fill a `ZZ9KVoiceRecord` |

```c
#include "proto/zz9k.h"
#include "zz9k/sdk.h"
#include "zz9k/voice.h"

ZZ9KVoiceOpenDesc open_desc;
ZZ9KVoiceUploadDesc upload;
ZZ9KVoiceSession session;
ZZ9KVoiceStatus status;
ZZ9KVoiceRecord records[2];
uint32_t stamp;

memset(&open_desc, 0, sizeof(open_desc));
open_desc.voices = 4;
open_desc.bank_bytes = pcm_length;
if (ZZ9KVoiceOpen(&open_desc, &session) != ZZ9K_STATUS_OK) {
  /* no voice engine, or another session is open */
}

memset(&upload, 0, sizeof(upload));
upload.sample_id = 0;
upload.bank_offset = 0;
upload.data = pcm;
upload.length = pcm_length;
upload.format = ZZ9K_VOICE_FORMAT_S8;
ZZ9KVoiceUpload(&session, &upload);

/* The first frame of the timeline is the first record's arrival, so a
 * stamp at the granted lead is on time. */
stamp = session.grant.lead_frames;
zz9k_voice_build_set(&records[0], stamp, 0,
                     ZZ9K_VOICE_SET_RATE | ZZ9K_VOICE_SET_VOLUME,
                     428, 0xFFFF, 0xFFFF);
zz9k_voice_build_start(&records[1], stamp, 0, 0, 0, 0);
ZZ9KVoiceWrite(&session, records, 2);

do {
  Delay(1);                       /* well inside the 500 ms heartbeat */
  ZZ9KVoiceGetStatus(&session, &status);
} while (status.playing_frame < stamp + sample_frames); /* length in 48 kHz frames */

ZZ9KVoiceClose(&session);
```

### From the host SDK

Programs that talk to the mailbox directly use the same session through
`zz9k/host.h`: `zz9k_voice_open()`, `zz9k_voice_close()`,
`zz9k_voice_reset()`, `zz9k_voice_upload()`, `zz9k_voice_write()` and
`zz9k_voice_status()`. Each takes the `ZZ9KContext` (except Write and Status,
which need only the session) and behaves like the library call of the same
name. The library calls wrap them.

Open allocates the ring, the status block and the staging buffer in the host
window, sends OPEN and fills the session. A refused open frees everything it
took and leaves the session zeroed. Close frees the three buffers whatever
the close status was.

A client that builds requests itself can use `zz9k_request_audio_voice_open()`,
`zz9k_request_audio_voice_session()` and `zz9k_request_audio_voice_upload()`
from `zz9k/request.h`, and decode the grant with
`zz9k_reply_audio_voice_open_result()` from `zz9k/reply.h`. The mailbox
opcodes are `ZZ9K_OP_AUDIO_VOICE_OPEN` (`0x0518`), `_CLOSE` (`0x0519`),
`_RESET` (`0x051A`) and `_UPLOAD` (`0x051B`). OPEN, CLOSE, RESET and UPLOAD
use 48-byte inline payloads, big-endian, laid out in `zz9k/voice.h`.

### Open

OPEN takes the three buffers, the bank size, the voice count, the requested
lead and the flags. `ZZ9K_VOICE_OPEN_FLAG_NTSC` selects the NTSC Paula clock
for period input (PAL otherwise). `ZZ9K_VOICE_OPEN_FLAG_LINEAR` selects
linear interpolation (nearest otherwise). Reserved fields must be zero.

The grant returns the session id, the voices, the bank size, the lead, the
lead floor and maximum, the ring capacity in records, the output rate (48000)
and the sample limit (256). The session id is nonzero and changes at every
open.

OPEN fails with:

| Status | When |
| --- | --- |
| `ZZ9K_STATUS_BAD_REQUEST` | Voices not 1 to 32, bank not 1 byte to 32 MiB, an unknown flag, a nonzero reserved field, a ring below 16 records or a status buffer below 64 bytes |
| `ZZ9K_STATUS_BAD_HANDLE` | A buffer is missing, belongs to another client, is card-only or media memory, is the same buffer as another of the three, or on Zorro II is not host-window memory |
| `ZZ9K_STATUS_BUSY` | A session is already open, a register-driven (AHI) client holds the audio output, or a buffer cannot be pinned |
| `ZZ9K_STATUS_NO_MEMORY` | The card cannot allocate the bank |

A refused open leaves nothing allocated or pinned on the card.

## The timeline

The engine counts frames at 48 kHz. Frame 0 is the first frame handed to the
fabric after the first command record arrives. Until then the session is in
state "open" and the slot adds silence.

Every record carries a stamp, the engine frame it takes effect at. The
firmware renders whole 960-frame periods (20 ms) on its main loop, up to
`playing_frame + lead_frames`, into a staging ring. The fabric copies those
periods into the transmit ring from its interrupt. A record must reach the
firmware before the engine renders its frame. A record whose frame has already
been rendered is applied at the first frame of the next period the engine
renders, and counts as late.

Three numbers in the grant bound the lead:

| Number | Value | Meaning |
| --- | ---: | --- |
| `lead_floor_frames` | 7680 | The smallest lead: 160 ms |
| `lead_max_frames` | 24000 | The largest lead: 500 ms |
| `lead_frames` | granted | The request, raised to the floor, capped at the maximum, then rounded up to a whole number of 960-frame periods |

The floor exists because the fabric keeps its transmit ring filled six
periods ahead of the DMA, so the engine has to be at least six periods ahead
of the playing frame for its audio to be staged in time. Two more periods
cover jitter in the firmware main loop. Latency from a stamp to sound is
therefore about 160 ms at the floor, and a larger lead adds to it. A larger
lead tolerates a busier 68k, because commands may arrive later relative to
their stamp.

Stamping rules:

- Stamp a command at `playing_frame + lead_frames` or later. The engine
  renders up to that frame, so it is the earliest stamp that has not been
  rendered yet, provided the playing frame is current. A period boundary can
  pass between the status read and the firmware taking the record, so the
  `late` counter and the horizon margin show whether a stamp at exactly that
  distance is enough. The first records of a session can use `lead_frames`
  itself.
- Stamps must not decrease. The engine applies records in ring order and
  stops at the first record that is not yet due.
- Records for one frame apply in ring order, so write a SET before the START
  that should use it, and a LOOP after the START it belongs to.
- The status `horizon` is the next frame the engine will render. A record
  with a stamp below the horizon when the firmware takes it is late.
  Subtracting the horizon from the stamp at write time gives the scheduling
  margin that `zz9k-voiceplay` reports.

`playing_frame` is the frame now leaving the DAC. It advances in whole
periods, so it moves in steps of 960 frames.

## Command records

A record is 16 bytes: a stamp, a voice, an op, a flags field and two
arguments. `zz9k/voice.h` has the byte layout, `ZZ9KVoiceRecord` in host
order, the builders, and the encode and decode helpers. The ring write
helper `ZZ9KVoiceWrite()` encodes records to the wire form.

| Op | Value | flags | arg0 | arg1 |
| --- | ---: | --- | --- | --- |
| START | 1 | Sample id, 0 to 255 | Start offset in bytes from the sample start | First-pass length in bytes, 0 for to the end of the sample |
| LOOP | 2 | Unused | Loop start in bytes from the sample start | Loop length in bytes, 0 for no loop |
| SET | 3 | 1 sets the rate, 2 sets the volume, 4 makes the rate Hz | The rate: a Paula period, or Hz with flag 4 | Left volume in the high 16 bits, right volume in the low 16 bits, each 0 to 65535 |
| STOP | 4 | Unused | Unused | Unused |
| MASTER | 5 | Unused | Gain in Q8.8, 256 is unity, at most 0xFFFF | Unused |
| DIRECTION | 6 | 1 plays backward, 0 plays forward | Unused | Unused |

Behaviour of each op:

- START begins forward at the offset with a zero fraction and clears the
  loop. The rate and volume from earlier SET records stay. The first pass ends
  at offset plus length, or at the sample end. A START with an undefined
  sample id, or with an offset at or beyond the sample length, silences the
  voice and counts as rejected. For 16-bit samples every byte quantity is
  halved, rounding down, to a sample index.
- LOOP changes the running voice at once without moving its position or the
  end of its current pass. The loop is used when the pass reaches its end.
  Without a loop, the loop start is the lower bound of backward play. A loop
  that lies outside the voice's sample is rejected and the voice keeps its
  previous loop. Send LOOP after START, because START clears the loop.
- SET with a period below 5, or with 0 Hz, is ignored and the voice keeps its
  previous step. Rates above 192000 Hz are clamped. A voice that never
  received a rate has step 0: it outputs nothing and does not advance. The
  engine divides the Paula clock by the period in integer arithmetic.
- STOP silences the voice.
- MASTER sets the gain of the final mix, after all voices are summed; the
  voice field is ignored.
- DIRECTION sets the direction of a playing voice and does nothing to a
  silent one.

A record for a voice at or above the granted voice count, or with an unknown
op, is rejected and never applied.

### The ring

The client owns the command ring: a host-window buffer with a 16-byte header
followed by 16-byte records. The ring has a capacity of the largest power of
two that fits, at least 16 and at most 4096 records. The grant reports it in
`ring_records`. The default 2 KiB ring holds 64 records. The client writes the
records first, then the write cursor in one 16-bit store, and may have at
most `ring_records` records outstanding. `ZZ9KVoiceWrite()` does this: it
reads the firmware's read cursor from the status block, writes no more
records than fit, publishes the cursor and returns the number written. A
return below `count` means the ring is full; write the rest on a later pass.

The firmware takes every published record on its main loop and queues it on
the card, which holds up to 4096 commands. Records beyond that, or records the
client wrote over before the firmware took them, are dropped and counted.

## Uploading samples

The bank is card memory allocated at open, 32-byte aligned, 1 byte to 32 MiB.
Uploads move bytes through the staging buffer, which is the third host-window
buffer of the session:

1. `ZZ9KVoiceUpload()` copies the data into the staging buffer, then sends an
   UPLOAD with the COPY flag so the firmware copies the bytes into the bank at
   `bank_offset`.
2. When the sample is larger than the staging buffer it repeats step 1 in
   chunks of the staging size rounded down to a multiple of 4. The 4 KiB
   default makes 256 copy round trips for a 1 MiB sample, and a larger
   staging buffer makes fewer.
3. A final UPLOAD with the DEFINE flag records `sample_id` as the
   `length` bytes at `bank_offset` in the given format.

A COPY or DEFINE whose range lies outside the staging buffer or the bank is
refused with `ZZ9K_STATUS_BAD_REQUEST` and changes nothing.

Rules:

- Sample ids run from 0 to the granted `max_samples` minus one, which is 255.
  Formats are `ZZ9K_VOICE_FORMAT_S8` and `ZZ9K_VOICE_FORMAT_S16` (big-endian).
- Defining an id again replaces its geometry for later STARTs. A voice that
  is already playing keeps the geometry it started with.
- Do not rewrite bank bytes that a voice is playing. The firmware does not
  synchronise an upload with the mixer.
- `ZZ9KVoiceUpload()` defines one sample over the bytes it uploads. A client
  with many sounds in one arena can upload the arena as a single sample and
  address each sound by the START offset, which is what `zz9k-voiceplay` does
  for modules. A client that sends its own requests can send COPY and DEFINE
  separately and define several samples over one copied region.
- The call refuses a zero length and a sample id above the grant before it
  sends anything.

## Status and the heartbeat

The firmware writes the status block from its main loop; the client only
reads it. `ZZ9KVoiceGetStatus()` reads it with each 32-bit word protected
against a torn read over Zorro II, and writes the heartbeat. `zz9k/voice.h`
has the byte layout.

| Field | Meaning |
| --- | --- |
| `playing_frame` | The engine frame now leaving the DAC |
| `horizon` | The next engine frame the engine will render |
| `read_cursor` | Records the firmware has taken from the ring |
| `state` | 0 closed, 1 open and waiting for the first record, 2 live |
| `late` | Records applied after their frame |
| `rejected` | Records refused: undefined sample, bad loop, a voice above the grant, an unknown op |
| `underruns` | Periods in which the voice slot had nothing staged |
| `dropped` | Records lost because the card queue was full or the client wrote over unread records |
| `session` | The session id |

The words are consistent individually; the block is not one atomic snapshot.

The heartbeat is a counter in the ring header. `ZZ9KVoiceGetStatus()` adds
one to it on every call, so the one call both reads the status and keeps the
session alive. Call GetStatus at least every 500 ms
(`ZZ9K_VOICE_HEARTBEAT_MS`). The firmware closes a session after 2 seconds
without a changed heartbeat or a voice mailbox call; `ZZ9KVoiceUpload()` and
`ZZ9KVoiceReset()` count as liveness, `ZZ9KVoiceWrite()` does not. A session
that has not been polled since open counts from the open. A player that
paces on `playing_frame` already reads the status often enough.

## Ending a session

| Event | Result |
| --- | --- |
| `ZZ9KVoiceReset()` | Drops every record the client had written, every queued command and the periods the slot has already queued; silences all voices. Samples, master gain and the timeline stay, and the frame counter keeps counting, so stamps after a reset are relative to a fresh status read. Audio of at most the period now playing remains. |
| `ZZ9KVoiceClose()` | Silences the voices, drops the queued periods, detaches the slot, frees the bank and unpins and frees the buffers. |
| Missed heartbeat | The firmware ends the session as Close does. The status block reads state 0. A later `ZZ9KVoiceClose()` returns `ZZ9K_STATUS_BAD_HANDLE` and still frees the buffers. |
| Client exit | The final `CloseLibrary()` releases the opener's client id, which ends its session and frees its buffers. Without that the heartbeat limit applies. |
| Amiga reset | A warm reset ends the session and frees its buffers. |

A second OPEN, from any client, fails with `ZZ9K_STATUS_BUSY` while a session
is open. A session id from another client is refused with
`ZZ9K_STATUS_BAD_HANDLE`.

## Zorro II

On Zorro II the ring, the status block and the staging buffer must be in the
host window, the small CPU-visible heap that every SDK client shares.
`zz9k_voice_open()` asks for them with `ZZ9K_ALLOC_HOST_WINDOW`; on Zorro III
the library strips that flag and the firmware accepts any ordinary shared
buffer that is neither card-only nor media.

| Buffer | Default | Range |
| --- | ---: | --- |
| Command ring | 2 KiB (64 records) | 272 bytes (16 records) to 65,552 bytes (4096 records) |
| Staging buffer | 4 KiB | At least 4 bytes; larger buys fewer upload round trips |
| Status block | 64 bytes | 64 bytes |

The defaults take 6,208 bytes of the 16 KiB heap, plus alignment, so the
session fits next to other host-window clients. The bank is card memory and
does not use the window. See [zz9k-zorro2-services.md](zz9k-zorro2-services.md)
for the window sizes of each layout and the other clients that compete for
it. A 4096-record ring needs more than the 16 KiB window.

## Discovery

The firmware defines `ZZ9K_SERVICE_FLAG_AUDIO_VOICE` (bit 28 of the audio
service flags, printed as `audio-voice`) but does not set it yet. It will
advertise the engine after hardware qualification. The audio service opcode
count stays 23 until then, and does not cover `0x0518` to `0x051B`. The
opcodes dispatch regardless.

Until the flag is set, a client that wants the engine calls OPEN and handles
the refusal. `ZZ9K_STATUS_UNSUPPORTED` means the firmware has no voice engine;
`zz9k-voiceplay` reports it that way. Once the flag is advertised, a client
should check it first, as it does for other optional services.

## zz9k-voiceplay

`zz9k-voiceplay` plays a ProTracker module, or a click, through the engine
with `zz9k.library`. The SDK build script builds it as
`build/zz9k-voiceplay`. It needs `zz9k.library` revision 42 and
`timer.device`.

```text
zz9k-voiceplay MODULE [LEAD=frames] [NTSC] [LINEAR] [SECONDS=n]
zz9k-voiceplay CLICK [LEAD=frames] [SECONDS=n]
```

| Argument | Meaning |
| --- | --- |
| `MODULE` | The module file. The word `CLICK` in any case selects click mode instead |
| `LEAD` | The lead in 48 kHz frames; 0 or absent asks for the firmware floor |
| `NTSC` | The module uses the NTSC Paula clock (PAL otherwise) |
| `LINEAR` | Linear interpolation (nearest otherwise) |
| `SECONDS` | Stop after this many seconds of engine time, 1 to 86400 |

In module mode the tool reads the file and runs libpt4, the ProTracker
replayer in `sdk/tools/libpt4/`, one tick at a time. Each tick's voice events
become records. The module's sample arena is uploaded as sample 0, and a
module whose arena does not fit the 32 MiB bank limit is refused. Modules with DYN
volume envelopes or the EFx funk effect are refused, because neither can be
expressed as voice records. The first tick is stamped at the granted lead, and
each later tick keeps its distance from the first. A tick runs when its song
frame is closer to the playing frame than the granted lead, which keeps the
ring a lead ahead of the player and no further. Each pass reads the status
(which is also the heartbeat), writes the pending records and sleeps about
5 ms. A status that reads closed ends the run, as the card closes a session
whose heartbeat stops.

In click mode it plays a 1 ms pulse on voice 0 once per second at 48000 Hz,
the first at the granted lead. It runs for 10 seconds unless `SECONDS` says
otherwise. It is for listening and for checking the scheduling margin.

Ctrl-C, the end of the song, the time limit, a status read that fails, a
session the card closed (heartbeat lost or card reset), or an engine frame
counter that stops advancing for 3 seconds ends the run. The tool resets the
session unless the song ended, closes it and prints a report:

| Report field | Meaning |
| --- | --- |
| frames played | `playing_frame` at the end |
| ticks, records | Replayer ticks run and records written |
| ring-full waits | Passes in which the ring could not take all pending records |
| late, rejected, underruns, dropped | The firmware counters from the status block |
| smallest scheduling margin | The smallest stamp minus horizon at the moment a record was written, in frames and microseconds |
| record latency | Minimum, average and maximum of each record's stamp minus the playing frame of the status read before its write, in frames and milliseconds: how far ahead of the sound the records were queued. Click records are included |
| engine rate | The playing frames advanced per EClock time between the first and the last live status read, in Hz with two decimals and as the difference from 48000 in ppm; it prints a note instead when less than a second of live playback was seen |
| 68k time per tick | Average and maximum time for a replayer tick, the record mapping and the ring writes, with the EClock rate used to convert it |

Exit codes: 0 after a normal run, 5 when no module was named, 10 for a bad
argument or a module the tool cannot play, and 20 for a failure to open the
library, the timer or the session, or for an upload, status, closed-session or
stall failure.

## For tracker authors

A replayer tick maps onto the engine as follows. The `zz9k-voice-pt4.c` and
`zz9k-voice-pt4.h` files in `sdk/tools/` show it complete for libpt4, and
`sdk/tools/libpt4/ORIGIN.md` records where libpt4 comes from.

- Keep the song position as a running count of 48 kHz frames, derived from the
  tempo, not from the Amiga clock. Each tick starts at the song frame where
  the previous one ended.
- Stamp every record of a tick with the tick's start frame plus a fixed
  offset. The first tick uses the granted lead as the offset, so the engine
  frames before it are silence.
- Run a tick when its start frame is less than `playing_frame + lead_frames`.
  This keeps the ring a lead ahead and no further. Read the status on every
  pass; that also feeds the heartbeat.
- A tick can hold up to six events per voice, so up to 192 records for 32
  voices, more than the 64 of a default ring. Keep the records a tick did
  not fit and write them on the next pass. Count the ring-full waits; if they
  are frequent, ask for a bigger ring.
- After a RESET the frame counter continues. Start the stamps of the new song
  position from `playing_frame + lead_frames` of a new status read.

Events map to records as in `zz9k-voiceplay`:

| Event | Record |
| --- | --- |
| New note | START with sample 0, the note's offset in the arena as arg0 and its length as arg1. A LOOP follows when the note loops |
| Note stop | STOP |
| Loop change | LOOP |
| Direction | DIRECTION |
| Period change | SET with the rate flag, arg0 the Paula period |
| Volume change | SET with the volume flag. `zz9k-voiceplay` scales the volume 0 to 64 as `volume * 1024 / voices` and puts it on the voice's own side only, with voices 0 and 3 of each group of four on the left and voices 1 and 2 on the right (hard pan) |

A replayer sends only what changed on a voice, the period or the volume, not
every tick. A replayer that rewrites sample bytes while a voice plays, as the
funk effect does, cannot use the engine, because a playing voice must not
have its bytes rewritten.

## Hardware measurements

Measurements on hardware are recorded here after qualification.
