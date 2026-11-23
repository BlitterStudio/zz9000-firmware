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
- Samples are signed 8-bit or signed 16-bit big-endian, mono, kept in a sample
  bank on the card of 1 byte to 32 MiB. Interleaved stereo formats exist too.
  Up to 256 samples (ids 0 to 255) can be defined over the bank.
- A voice plays at a Paula period, divided into the PAL or the NTSC clock
  chosen at open, or at a rate in Hz. Periods below 5 are ignored.
- Volume is set per side, left and right, each 0 to 65535, and a master gain
  scales the mix. A stereo sample uses the left volume on its left channel
  and the right volume on its right channel.
- A voice plays forward or backward and can loop. A loop is a start and a
  length in bytes inside the sample.
- Playback reads the nearest sample, or interpolates linearly between two
  samples when the session was opened with the linear flag.
- Commands carry the 48 kHz frame they take effect at, so a note starts on
  exactly that output frame.
- Optional ramps, set with a `CONFIG` record: volume changes, starts, stops
  and restarts fade over up to 512 frames, and a restarted voice lets its old
  sound fade out, so a cut or a retrigger does not click. Off by default.
- An optional static soft knee after the master gain, also set with a
  `CONFIG` record, that bends loud mixes toward the 16-bit rails instead of
  clipping them flat. Off by default.

The engine is integer only. Each voice contributes `sample * volume >> 16` to
the left and right sums, so a volume of 65535 is close to unity. The sums are
scaled by the master gain (Q8.8, 256 is unity) and clamped to 16 bits; with
the soft knee on, the bounded curve replaces that clamp, see
[Limiter](#limiter).

One voice session can be open at a time. The engine feeds the firmware-owned
fourth slot of the audio fabric and mixes with the other fabric producers.

## Using it

### From zz9k.library

The six calls need `zz9k.library` revision 42
(`ZZ9K_LIBRARY_MIN_REVISION_AUDIO_VOICE`); the low-latency open flag needs
revision 43 (`ZZ9K_LIBRARY_MIN_REVISION_VOICE_LOW_LATENCY`); the session
ownership below needs revision 44 (`ZZ9K_LIBRARY_MIN_REVISION_VOICE_CLIENT_ID`).
Open fills a `ZZ9KVoiceSession`, and every later call takes it.

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
| `ZZ9KVoiceJitter` and `zz9k_voice_jitter_read()` | The two jitter words of the status block, read apart from `ZZ9KVoiceStatus` so that struct keeps its size |
| `ZZ9KVoiceExtStatus` and `zz9k_voice_ext_status_read()` | The extended status words (features and reserved ones), also read apart from `ZZ9KVoiceStatus` |
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
linear interpolation (nearest otherwise). `ZZ9K_VOICE_OPEN_FLAG_LOW_LATENCY`
opens a [low-latency session](#low-latency-session). Reserved fields must be
zero.

The grant returns the session id, the voices, the bank size, the lead, the
lead floor and maximum, the ring capacity in records, the output rate (48000)
and the sample limit (256). The session id is nonzero and changes at every
open.

A low-latency grant reports the lead floor of its mode, 2880 frames, and a
request below it is raised to it. A request of 0 asks for the floor of the mode
the flag chose. Firmware without the mode treats the flag as an unknown bit and
answers `ZZ9K_STATUS_BAD_REQUEST`, as does a `zz9k.library` older than
revision 43; the client opens again without the flag and gets the standard
floor.

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
state "open" and the slot adds silence. `playing_frame` stays at 0 in that
state, so a client that paces on it must write a record before it waits: a
player whose song starts with silent rows can send a MASTER record with unity
gain, stamped at `lead_frames`, to start the timeline. `zz9k-voiceplay` does
this.

Every record carries a stamp, the engine frame it takes effect at. The
firmware renders whole 960-frame periods (20 ms) on its main loop, up to
`playing_frame + lead_frames`, into a staging ring, one period per pass of
the loop so that the 68k's register accesses are served between periods. A
session starts playing once the whole lead is staged, as long after the first
record as it takes to render the lead. Until then the status `horizon` climbs
one period per pass and `playing_frame` stays 0. The fabric copies those
periods into the transmit ring from its interrupt. A record must reach the
firmware before the
engine renders its frame. A record whose frame has already been rendered is
applied at the first frame of the next period the engine renders, and counts
as late.

Three numbers in the grant bound the lead:

| Number | Value | Meaning |
| --- | ---: | --- |
| `lead_floor_frames` | 7680, or 2880 in a low-latency session | The smallest lead: 160 ms, or 60 ms |
| `lead_max_frames` | 24000 | The largest lead: 500 ms |
| `lead_frames` | granted | The request, raised to the floor, capped at the maximum, then rounded up to a whole number of 960-frame periods |

A frame sounds the lead plus the formatter's FIFO delay after the engine
renders it. The FIFO delay is about 11 ms, inferred from the formatter's
design and not measured. A record stamped at the horizon sounds between
`lead - 20 ms` and `lead` plus that delay after it was written, depending on
where in a period the write falls. A larger lead adds to that, and tolerates
a busier 68k, because commands may arrive later relative to their stamp.

The fabric keeps its transmit ring a fill-ahead of periods ahead of the DMA,
six by default, so the engine has to be at least the fill-ahead plus one
period ahead of the playing frame for its audio to be staged in time, and
only if the firmware main loop polls the engine in every 20 ms window. The
poll gap the lead tolerates is `(lead - fill-ahead) * 20 ms`, less the time
the engine needs to render a period. The standard floor adds two periods to
the six, which tolerates a gap of 40 ms.

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

`playing_frame` is the first frame of the period the DMA is fetching. It
advances in whole periods, so it moves in steps of 960 frames. The DAC plays
that frame about 11 ms after the step, because of the formatter FIFO, so right
after a step `playing_frame` is ahead of the frame leaving the DAC by about
11 ms, and just before the next step it is about 9 ms behind it.

### Low-latency session

A session opened with `ZZ9K_VOICE_OPEN_FLAG_LOW_LATENCY` makes the fabric
keep its transmit ring 2 periods ahead of the DMA instead of 6 while the
session is open, and reports a lead floor of 3 periods, 2880 frames. The lead
is 100 ms shorter than at the standard floor, and so is the time from a record
written at the earliest frame to its sound. The measured runs are in
[Low-latency sessions](#low-latency-sessions).

- The shorter fill-ahead applies to every producer on the card while the
  session is open. Other audio, AHI and ZZPlay included, plays the same
  samples in the same order, only sooner. The audio interrupt may run late by
  less than 20 ms instead of less than 100 ms before the DMA catches up with
  the transmit ring, and the fabric then counts an underrun for each producer
  that was owed audio.
- The floor assumes a firmware main-loop poll in every 20 ms window and
  tolerates a gap of about 20 ms. A client that cannot promise that asks for a
  larger lead; each extra period of lead tolerates 20 ms more. The jitter words
  of the status block show the gap and the margin seen.
- Ending the session by any route in [Ending a session](#ending-a-session)
  returns the fabric to 6 periods, one period per audio interrupt, without a
  skipped or repeated period for the other producers. A refused open changes
  nothing.
- A live client, a keyboard player for example, stamps each record at the
  status `horizon`, or at 0. A late record is applied at the first frame of
  the next period the engine renders, the same frame a record stamped at the
  horizon gets, and the `late` counter shows how often it happened.
  `zz9k-voiceplay LIVE LOWLAT` plays that way.

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
| MODE | 7 | Reserved | Reserved | Reserved |
| ENVELOPE | 8 | Reserved | Reserved | Reserved |
| CONFIG | 9 | 1 sets the ramp length, 2 sets the limiter, 3 sets both | With flag 1, the ramp length in frames, 0 to 512. With flag 2 alone, 0 | With flag 2, 0 (off) or 1 (on). Ignored with flag 1 alone |

Behaviour of each op:

- START begins forward at the offset with a zero fraction and clears the
  loop. The rate and volume from earlier SET records stay. The first pass ends
  at offset plus length, or at the sample end. A START with an undefined
  sample id, or with an offset at or beyond the sample length, silences the
  voice and counts as rejected. Every byte quantity is divided into sample
  frames, rounding down: by 1 for S8, 2 for S16 and S8S, and 4 for S16S.
- LOOP changes the running voice at once without moving its position or the
  end of its current pass. The loop is used when the pass reaches its end.
  Without a loop, the loop start is the lower bound of backward play. A loop
  that lies outside the voice's sample is rejected and the voice keeps its
  previous loop. Send LOOP after START, because START clears the loop.
- SET with a period below 5, or with 0 Hz, is ignored and the voice keeps its
  previous step. Rates above 192000 Hz are clamped. A voice that never
  received a rate has step 0: it outputs nothing and does not advance. The
  engine divides the Paula clock by the period in integer arithmetic.
- STOP silences the voice; with a ramp length set it fades the voice out, see
  [Ramps and CONFIG](#ramps-and-config).
- MASTER sets the gain of the final mix, after all voices are summed; the
  voice field is ignored.
- DIRECTION sets the direction of a playing voice and does nothing to a
  silent one.

A record for a voice at or above the granted voice count, or with an unknown
op, is rejected and never applied.

### Ramps and CONFIG

CONFIG (op 9) carries session-wide settings. Its flags field names the
settings the record holds and its voice field is ignored. Flag 1
(`ZZ9K_VOICE_CONFIG_RAMP`) sets the ramp length in frames from arg0, 0 to
`ZZ9K_VOICE_RAMP_MAX_FRAMES` (512). Flag 2 (`ZZ9K_VOICE_CONFIG_LIMITER`) sets
the limiter from arg1, 0 for off and 1 for on. Each flag selects the
argument it reads, so a record holds one setting or both:

| flags | arg0 | arg1 | Sets |
| --- | --- | --- | --- |
| 1 | Ramp length, 0 to 512 | Ignored; send 0 | The ramp length |
| 2 | Must be 0 | 0 or 1 | The limiter |
| 3 | Ramp length, 0 to 512 | 0 or 1 | Both, together |

The firmware checks every setting the record names before it applies any. A
record is rejected whole, none of its settings applied, and counted in
`rejected` when it has no flag, a flag the firmware does not know, a ramp
length above 512 with flag 1, a limiter value above 1 with flag 2, or a
nonzero arg0 with flag 2 and without flag 1. A setting that the record does
not name keeps its value. Firmware without CONFIG counts every CONFIG record
as rejected, so a client checks the `CONFIG` bit of the features word first.
MODE (7) and ENVELOPE (8) are op numbers kept for later features; the engine
rejects them.

The builders in `zz9k/voice.h` fill these records:
`zz9k_voice_build_config_ramp()` (flag 1, arg1 0),
`zz9k_voice_build_config_limiter()` (flag 2, arg0 0, arg1 0 or 1) and
`zz9k_voice_build_config_ramp_limiter()` (flags 3). The generic
`zz9k_voice_build_config()` writes any flags and arguments, which is how a
test sends a record the firmware must reject.

With a ramp length R above 0, volume changes, starts, stops and restarts fade
over R frames instead of stepping, which removes the click of a cut or a
retrigger and the zipper noise of a stepped volume. With R at 0 and the
limiter off, the defaults, the output remains identical to an engine without
CONFIG, sample for sample.

Each side of a voice has a gain, kept as the volume times 256. A ramp from a
gain g to a target t over R frames adds (t - g) / R to the gain on every
frame, the division truncating toward zero, so frame j of the ramp plays at
volume (g + j * ((t - g) / R)) >> 8 and frame R and later at t exactly. A ramp
starts on the frame of its record and a new target while one runs starts a
new ramp from the gain the voice has on that frame.

- START on a silent voice makes it rise from silence to its volume.
- START on a sounding voice hands the old sound to a fade slot and starts the
  new one from silence. The slot is a copy that keeps playing from where the
  voice was and fades from its current gain to silence over R frames, then
  frees itself. A voice that was already stopping keeps the fade it had. No
  slot is taken for a sound that is silent or has no rate. A START the engine
  rejects still hands the old sound over, so it fades out.
- SET with a volume on a sounding voice ramps from its current gain to the new
  volume. On a voice that is stopping it only stores the volume. SET of the
  rate is not ramped.
- STOP on a sounding voice ramps its gain to silence over R frames and then
  deactivates it; a voice that is silent already stops at once. A second STOP
  while the voice fades changes nothing.
- LOOP, DIRECTION and SET records for a voice never touch the fade slots it
  has handed its earlier sounds to, and a STOP does not end earlier tails. A
  voice that reaches the end of its sample stops at once, ramp or not.
- There are 32 fade slots. A restart that finds all in use overwrites the
  oldest, which cuts that tail short.

Records with the same stamp apply in ring order, so a SET, STOP and START for
one voice at one frame give different sounds in different orders, as they do
without ramps. MASTER is not ramped. A ramp that is running when R is set to
0 runs out, and records after that act at once.

Ramping costs render time. A ramping voice, and each fade slot that is still
fading, costs about as much as a sounding voice plus a vector pass over the
frames it plays. The `RAMP` feature bit is set only for firmware whose render
time with 32 voices and 32 fade slots is inside the 5 ms budget on the card.

### Limiter

The limiter is a static soft knee on the final mix, on after a CONFIG record
with flag 2 and arg1 1, off after one with arg1 0 and off by default. It acts
on each side of each frame after the master gain (Q8.8) has scaled the sum
and before the 16-bit output. With it off the engine clamps to 16 bits as it
always did.

Let x be a side's sample after the master gain and d = |x| - 24576. With the
limiter on:

- A sample with |x| up to 24576 (three quarters of full scale) passes
  unchanged.
- A louder sample is bent toward the rail. The reference curve is
  `sign(x) * (24576 + 8191 * d / (8191 + d))`. It is continuous at the knee,
  rises with x, and approaches 32767 without reaching it.
- The firmware computes an odd integer approximation of the reference
  curve, within 2 LSB of it, that never decreases as |x| grows and whose
  magnitude never exceeds 32766, so a loud mix stays strictly inside the
  rails.

The curve has no state, no attack and no release: a frame's output depends on
that frame's sum and on whether the limiter is on, never on earlier frames or
on where the render splits its periods. Turning it on or off applies at the
stamp frame of the record, like every record, and records with one stamp
apply in ring order. A player that wants the knee for a whole piece sends the
CONFIG record at the lead stamp, ahead of the MASTER record and of every
note, so that the first sample already goes through it.

The knee shapes only the sum of the voices. It does not replace the master
gain or the per-voice volumes, and it is not the ADAU1701 limiter or the
calibrated saturation ceilings of the audio path, which still apply to the
mixed output, see [Audio saturation ceilings](../../docs/audio-saturation-ceiling.md).

Discovery. The firmware advertises the `LIMITER` feature bit after the card
timing gate in [Limiter timing](#limiter-timing). The bit says that the
render time of the knee with 32 voices and a full fade pool is inside the
budget; it says nothing about how the knee sounds, see the listening note
under [Limiter timing](#limiter-timing). A client that
uses the limiter checks the `CONFIG` and `LIMITER` bits first; firmware
without `LIMITER` may still accept the record, as it accepted ramp records
before `RAMP` was advertised, and that use is for testing only.

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
  Formats are `ZZ9K_VOICE_FORMAT_S8` and `ZZ9K_VOICE_FORMAT_S16` (big-endian),
  and `ZZ9K_VOICE_FORMAT_S8S` and `ZZ9K_VOICE_FORMAT_S16S` for interleaved
  stereo (see [Stereo samples](#stereo-samples)). A format of 4 or more is
  refused.
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

## Stereo samples

`ZZ9K_VOICE_FORMAT_S8S` (2) and `ZZ9K_VOICE_FORMAT_S16S` (3) hold interleaved
stereo frames: the left sample, then the right sample, with 16-bit samples
big-endian. This is how AHI stores stereo sounds. A frame is 2 bytes in S8S
and 4 bytes in S16S.

- Every byte quantity (the START offset and length, the LOOP start and
  length, and the sample length of a DEFINE) is divided into frames by the
  shift `zz9k_voice_format_frame_shift(format)` returns: 0 for S8, 1 for S16
  and S8S, 2 for S16S. The division rounds down. A START offset in an S16S
  sample that is not a multiple of 4 bytes plays from the frame it rounds
  down to. Positions, loops, the first-pass length and backward play all
  count frames.
- A stereo voice scales its left sample by the left volume and its right
  sample by the right volume, so volumes of (65535, 0) play the left channel
  only. A mono voice feeds its one sample to both sides, as before.
- Linear interpolation blends the left samples with each other and the right
  samples with each other.
- A DEFINE with a format at or above `ZZ9K_VOICE_FORMAT_COUNT` (4) is refused
  with `ZZ9K_STATUS_BAD_REQUEST` and changes nothing. The request builder
  refuses it too. `zz9k_voice_format_valid()` tests a format.
- A sample length that is not a multiple of the frame size loses the trailing
  partial frame, because the length is rounded down.

Firmware advertises the formats with `ZZ9K_SERVICE_FLAG_AUDIO_VOICE_STEREO`
(see [Discovery](#discovery)). It sets the flag on Zorro III, together with
`ZZ9K_SERVICE_FLAG_AUDIO_VOICE`, and not on Zorro II.

## Timing helpers

`zz9k/voice.h` carries the arithmetic a client needs to predict where the
engine is, as pure functions. They are the single source of that arithmetic
for clients, and they state what `advance()` in the firmware's
`audio_voice.c` does. The firmware engine test
(`test/audio/audio_voice_test.c`) checks them against the engine over a
sweep of rates, lengths and fractions, forward and backward, and the SDK
tests check them against an independent model of it.

- `zz9k_voice_rate_step(hz)` returns the engine's step for a rate in Hz: the
  number of 1/65536 sample frames a voice advances per output frame,
  `(min(hz, ZZ9K_VOICE_RATE_MAX_HZ) << 16) / 48000`. It returns 0 for 0 Hz.
  The engine ignores a SET of 0 Hz and keeps the old step, so a client reads
  the 0 as no change.
- `zz9k_voice_pass_frames(backward, pos, frac, bound, step)` returns how many
  frames a voice renders before its pass ends, for a voice about to render
  the frame at sample index `pos` with the 16-bit fraction `frac`. The first
  frame of the next pass is that many frames later. All arguments are
  frames, not bytes. A forward pass ends when `pos + ((frac + k * step) >>
  16)` reaches `bound`, the pass end. A backward pass ends when the position
  falls below `bound`, the loop start. A step that would carry the position
  below index 0 counts as falling below the loop start, which ends the pass.
  The result is at least 1. It is `ZZ9K_VOICE_PASS_NEVER` when `step` is 0 or
  the count does not fit in 32 bits.

Positions inside the helpers are frame indexes. To use them with START and
LOOP bytes, shift the bytes down with `zz9k_voice_format_frame_shift()`
first.

## AHI-style channel clients

A client that plays many short sounds on a few channels, as an AHI driver
does, does not need one engine sample per sound. The engine's 256-sample
limit then does not apply. The pattern:

1. At open, define one engine sample per sample format the client uses over
   the bank from offset 0 to its end. Each is a view of the bank in that
   format. Use DEFINE-only uploads (flags set to DEFINE) for this; the COPY
   half is not needed.
2. For each sound the client loads, find a range of the bank and copy the
   sound's bytes into it with COPY-only uploads. Keep the view for the
   sound's format, the byte offset of its range, and its length.
3. To play a sound, send START with the view's sample id, the sound's byte
   offset as arg0 and its length as arg1. Send LOOP with the offset and
   length the same way. The bytes are interpreted in the view's format, so a
   mono and a stereo sound need two views, and the next pass of a channel
   can be any sound of the same format.

The client keeps its own bounds for each sound, because the view reaches
past the sound. The engine does not check a pass against anything but the
view. A pass that is left to run reaches the next sound's bytes, so the
client sets the first-pass length, or the loop, on every START. The helpers
in [Timing helpers](#timing-helpers) predict when each pass ends.

The bank must not be rewritten while a voice plays the bytes. See
[Uploading samples](#uploading-samples).

## Status and the heartbeat

The firmware writes the status block from its main loop; the client only
reads it. `ZZ9KVoiceGetStatus()` reads it with each 32-bit word protected
against a torn read over Zorro II, and writes the heartbeat. `zz9k/voice.h`
has the byte layout.

| Field | Meaning |
| --- | --- |
| `playing_frame` | The first frame of the period the DMA is fetching |
| `horizon` | The next engine frame the engine will render |
| `read_cursor` | Records the firmware has taken from the ring |
| `state` | 0 closed, 1 open and waiting for the first record, 2 live |
| `late` | Records applied after their frame |
| `rejected` | Records refused: undefined sample, bad loop, a voice above the grant, an unknown op |
| `underruns` | Periods in which the voice slot had nothing staged |
| `dropped` | Records lost because the card queue was full or the client wrote over unread records |
| `session` | The session id |
| jitter `margin_min_us` | The smallest time a rendered period waited for the audio interrupt to take it, in microseconds since open, or `0xFFFFFFFF` until the first period is taken. Periods rendered before the slot goes live are not counted |
| jitter `poll_gap_max_us` | The largest gap between two firmware polls of the session since open, in microseconds |
| `features` | The features word, see below and [Discovery](#discovery) |

The words are consistent individually; the block is not one atomic snapshot.

The two jitter words sit at offsets 32 and 36, in bytes the block kept
reserved, so readers of the older block are unaffected. `ZZ9KVoiceStatus` does
not carry them, because the library fills that struct and a program built with
an older, smaller one must keep working. Read them with
`zz9k_voice_jitter_read()` on `session.status` after `ZZ9KVoiceGetStatus()`.
Firmware that does not report them leaves both words zero, which reads as a
poll gap of 0; the firmware never reports a gap of 0 once the session has been
polled twice. A margin near zero means a period was rendered just before the
interrupt needed it, so any further delay would have counted an underrun. A
poll gap above `(lead - fill-ahead) * 20 ms` less the render time is a lead
that was too small for that machine's load.

The extended words follow at offsets 40 to 53, also in formerly reserved
bytes. `features` (offset 40, 32 bits) is the features word described under
[Discovery](#discovery) and is kept in every status write. `active_mask` (44),
`ends` (48) and `output_delay_frames` (52, 16 bits) are reserved for later
features and read 0. Read them with `zz9k_voice_ext_status_read()` into a
`ZZ9KVoiceExtStatus`, with the same tear protection and cache rule as the
status words. Bytes 54 to 63 stay reserved.

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
| `ZZ9KVoiceReset()` | Drops every record the client had written, every queued command and the periods the slot has already queued; silences all voices. Samples, master gain and the timeline stay, and the frame counter keeps counting, so stamps after a reset are relative to a fresh status read. Audio of at most the period now playing remains. A reset while the session is still staging its first lead also takes back the staged periods and restarts the timeline at frame 0; nothing of the earlier voices plays. |
| `ZZ9KVoiceClose()` | Silences the voices, drops the queued periods, detaches the slot, frees the bank and unpins and frees the buffers. |
| Missed heartbeat | The firmware ends the session as Close does. The status block reads state 0. A later `ZZ9KVoiceClose()` returns `ZZ9K_STATUS_BAD_HANDLE` and still frees the buffers. |
| Client exit | The final `CloseLibrary()` releases the opener's client id, which ends its session and frees its buffers, because the session is owned by the id that opened it (revision 44 or later; before it a session was always owned by client 0 and only the heartbeat limit ended it). A task that never opened the library owns its session as client 0, which no release reaches, so the heartbeat limit applies. |
| Amiga reset | A warm reset ends the session and frees its buffers. |

A low-latency session gives the fabric its 6-period fill-ahead back by every
route in that table, one period per audio interrupt.

A second OPEN, from any client, fails with `ZZ9K_STATUS_BUSY` while a session
is open. A session id from another client is refused with
`ZZ9K_STATUS_BAD_HANDLE`.

A session belongs to the client id its Open carried. From revision 44 that is
the id of the calling task's library opener, on Open, Close, Reset and the
buffer allocations as on `ZZ9KCall()`, so an opener can send its own UPLOAD
requests with `ZZ9KCall()` on a session `ZZ9KVoiceOpen()` made. Before
revision 44 the Voice API sent client 0 whoever called, and `ZZ9KCall()` sent
the opener's id, so only a task without an opener, which sends 0 on both,
could mix them. Such a task still can.

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

The firmware sets `ZZ9K_SERVICE_FLAG_AUDIO_VOICE` (bit 28 of the audio
service flags, printed as `audio-voice`) on Zorro III. It does not set the
flag on Zorro II yet, because the engine has not been tested on a Zorro II
machine. The audio service opcode count is 28 and covers `0x0500` to
`0x051B`, including the voice opcodes. The opcodes dispatch on both buses
regardless of the flag.

A client should check the flag first, as it does for other optional services.
A client that opens without checking handles the refusal:
`ZZ9K_STATUS_UNSUPPORTED` means the firmware has no voice engine, and
`zz9k-voiceplay` reports it that way.

There is no service flag for the low-latency mode. The open flag is the
advertisement: a client opens with it and, on `ZZ9K_STATUS_BAD_REQUEST`, opens
again without it and keeps the standard floor. `zz9k-voiceplay LOWLAT` does
this and says so.

The stereo sample formats have their own flag,
`ZZ9K_SERVICE_FLAG_AUDIO_VOICE_STEREO` (bit 29, printed as
`audio-voice-stereo`), reported together with `audio-voice`. A client that
wants S8S or S16S samples checks it and defines only the mono formats when it
is clear. Firmware with stereo support sets the flag on Zorro III together
with `audio-voice`. Earlier firmware with the voice engine plays both formats
but never sets the flag, so a client does not use them there. A firmware
without the stereo support refuses a DEFINE in format 2 or 3 with
`ZZ9K_STATUS_BAD_REQUEST`. See [Stereo samples](#stereo-samples).

The status block's features word (`ZZ9K_VOICE_FEATURE_*`, read with
`zz9k_voice_ext_status_read()`) names what the firmware's engine can do beyond
the base contract, and a client checks a bit before it sends the records of
that feature. It reads 0 on firmware that predates it. `CONFIG` says the
`CONFIG` record is understood. `RAMP` says that a ramp length set with it has
been measured on the card with 32 voices and a full fade pool inside the
render budget; until the bit is set, a client that wants ramps for testing
sends the record without relying on it. `LIMITER` says the same of the soft
knee set with flag 2, see [Limiter](#limiter). The other bits (`CUBIC`,
`VOICE_INTERP`, `OVERSAMPLE`,
`ENVELOPE`, `EVENTS`) belong to features that are not in the firmware yet and
stay clear.

## zz9k-voiceplay

`zz9k-voiceplay` plays a ProTracker module, a click, the keyboard, or a
synthetic load through the engine with `zz9k.library`. The SDK build script
builds it as `build/zz9k-voiceplay`. It needs `zz9k.library` revision 42,
revision 43 for `LOWLAT`, and `timer.device`.

```text
zz9k-voiceplay MODULE [LEAD=frames] [NTSC] [LINEAR] [SECONDS=n] [LOWLAT]
zz9k-voiceplay CLICK [LEAD=frames] [SECONDS=n] [LOWLAT]
zz9k-voiceplay LIVE [LEAD=frames] [SECONDS=n] [LOWLAT]
zz9k-voiceplay LOAD VOICES=n SECONDS=n [LINEAR] [STEREO] [RATE=hz] [GAIN=n] [CHURN] [GROUPS=n] [LOWLAT]
zz9k-voiceplay <any of the above> [RAMP=frames] [LIMITER]
```

| Argument | Meaning |
| --- | --- |
| `MODULE` | The module file. The word `CLICK` in any case selects click mode, `LIVE` selects live mode and `LOAD` selects load mode |
| `VOICES` | LOAD mode only: the number of voices, 1 to 32 (default 32) |
| `STEREO` | LOAD mode only: the sample is interleaved stereo S16S instead of mono S16 |
| `RATE` | LOAD mode only: the centre rate in Hz, 1 to 192000 (default 22050) |
| `RAMP` | Any mode: send a `CONFIG` ramp record of this many frames (0 to 512) before any other record. Needs firmware with the `CONFIG` feature; the tool says when `RAMP` is not advertised yet and runs the ramp anyway, which is how the firmware is qualified |
| `LIMITER` | Any mode: switch the static soft knee on with a `CONFIG` record (flag 2, arg1 1) before any other record. Needs firmware with the `CONFIG` feature, otherwise the tool exits with code 20. When `LIMITER` is not advertised it prints a warning that the knee is unqualified and runs it anyway, which is how the firmware is qualified. There is no option to send arg1 0 |
| `GAIN` | LOAD mode only: the volume of every voice on each side, 0 to 65535, in place of 65535 divided by the voice count |
| `CHURN` | LOAD mode only: restart the voices continuously, see below |
| `GROUPS` | With `CHURN`: restart groups, 1 to 32 (default 32) |
| `LEAD` | The lead in 48 kHz frames; 0 or absent asks for the firmware floor |
| `NTSC` | The module uses the NTSC Paula clock (PAL otherwise) |
| `LINEAR` | Linear interpolation (nearest otherwise) |
| `SECONDS` | Stop after this many seconds of engine time, 1 to 86400 |
| `LOWLAT` | Open a low-latency session. The tool prints the granted floor and the latency range the model gives for it. It falls back to the standard floor, and says so, when `zz9k.library` is older than revision 43 or the firmware refuses the flag |

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

Load mode exists for the firmware render bench (`AUDIO_VOICE_BENCH`, see
`docs/audio-fabric.md`). It defines one looping 16-bit sample of 4096 frames,
a few sine cycles with a little noise, mono or interleaved stereo with
`STEREO`. It starts `VOICES` voices on it at rates spread evenly from half to
twice `RATE` (one voice plays `RATE`; a rate above 192000 is clamped), at
volume 65535 divided by the voice count on each side (or `GAIN`), and keeps
them playing for `SECONDS` (default 20) before it stops and closes. With a high `RATE`
some voices step above 48 kHz. The stereo sample needs a firmware that plays
the stereo formats.

`CHURN` (load mode only) restarts the voices over and over, so that with `RAMP`
the fade pool stays full. Each voice restarts every `RAMP` frames (256 without
`RAMP`, and never below 32), a START and a LOOP over the full length of the sample, as a
player does that retriggers a note. With a ramp every restart hands the old
sound to a fade slot and each tail lasts one interval, so the 32 slots are in
use all the time. `GROUPS` sets how many groups the voices restart in, spread
evenly over the interval. The default is 32: one voice every interval / 32
frames, which is also the most records and the most splits of the render.
`GROUPS=1` restarts all voices together. A restart writes 2 records per voice,
so the tool asks for an 8 KiB ring and falls back to the default ring when the
shared memory does not have room.

With `RAMP` or `LIMITER` the tool sends one `CONFIG` record at the granted
lead, the same stamp as the first notes, and writes it first in the pending
queue, ahead of the MASTER record and of every START. The ring keeps that
order, so the settings apply before any note. With both options the record
holds flags 3 (the ramp length in arg0, 1 in arg1) and the firmware
validates and applies the two together. `RAMP` alone sends flag 1 and
`LIMITER` alone sends flag 2 with arg0 0.

Saturation runs. `GAIN=65535` on 32 voices puts far more than full scale
into the mix, so the master clamp (without `LIMITER`) or the soft knee (with
it) is in use on most frames. With `CHURN` and `RAMP` every restart hands a
full-volume tail to a fade slot, which exercises the fade pool at the highest
level. For example:

```text
zz9k-voiceplay LOAD VOICES=32 GAIN=65535 LIMITER SECONDS=20
zz9k-voiceplay LOAD VOICES=32 GAIN=65535 CHURN RAMP=256 LIMITER STEREO LINEAR SECONDS=20
```

These runs are loud and harsh by design. Turn the amplifier or speakers down
before the run, and do not use headphones except at a low level. A mix pinned
near full scale can damage tweeters and ears. Without `GAIN` the mix of the
default load peaks near 20000, under the knee. The saturation ceilings of the
audio path are in `docs/audio-saturation-ceiling.md`.

In live mode the keys `zsxdcvgbhnjm` play one octave from middle C and
`q2w3er5t6y7u` the next, with `i` the top C; ESC stops. Each key writes a SET
and a START at the status horizon on the next of eight voices, a decaying
triangle wave of 4096 bytes whose pitch follows the key. It needs an
interactive console, and it reports the keys played and the records the engine
found late. It is for the ear test of a low-latency session next to a
standard one.

Ctrl-C, ESC in live mode, the end of the song, the time limit, a status read
that fails, a session the card closed (heartbeat lost or card reset), or an
engine frame counter that stops advancing for 3 seconds ends the run. The
tool resets the session unless the song ended, closes it and prints a report:

| Report field | Meaning |
| --- | --- |
| frames played | `playing_frame` at the end |
| ticks, records | Replayer ticks run and records written |
| ring-full waits | Passes in which the ring could not take all pending records |
| late, rejected, underruns, dropped | The firmware counters from the status block |
| session | `low latency` or `standard`, with the granted lead floor in frames |
| jitter | The two jitter words of the status block: the smallest wait of a rendered period for the audio interrupt and the largest gap between firmware polls, in microseconds |
| engine features | The features word of the status block, with `CONFIG`, `RAMP` and `LIMITER` named when they are set |
| load | LOAD mode only: the voices, sample type, interpolation, rate range and the volume per side (65535 divided by the voice count unless `GAIN` is given) |
| ramp, limiter, churn | Only with `RAMP`, `LIMITER` or `CHURN`: the ramp length, the limiter state, the restarts written, the groups and the interval between restarts |
| keys played | Live mode only |
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

These runs used a ZZ9000 on Zorro III in an A4000 with a 68060, under
AmigaOS 3.2, with the firmware lead floor of 7680 frames (standard sessions;
no low-latency session has run). The maintainer listened to each run. All
runs used `zz9k-voiceplay`.

| Run | Result |
| --- | --- |
| Click test, 10 s | 0 late, 0 underruns |
| `MOD.CannonFodder` (4 channels), 30 s | 0 late, 0 underruns, sound correct |
| A real 8-channel (`8CHN`) module, 30 s | 0 late, 0 underruns, sound correct. 68k time per replayer tick averaged 0.7 ms, maximum 4 ms |
| The synthetic 32-channel test module from libpt4, 30 s | 0 late, 0 underruns, average 1.35 ms per tick |
| The 8-channel module with an MPEG-2 video in ZZPlay (direct AX audio) at the same time, 30 s | 0 late, 1 underrun, sound and video clean to the ear and eye |
| The 8-channel module with a 440 Hz tone through AHI (ZZ9000AX 16 bit Stereo++ mode) at the same time | 0 late, 0 underruns, both clean |

The smallest scheduling margin seen was 4752 frames.

No real module with more than 8 channels was available, so the 32-voice run
used a synthetic test module. It shows that the engine and the replayer keep
up with 32 voices, not how a real 32-channel tune sounds.

`zz9k-voiceplay` queues records about one lead ahead of the floor, so its
record latency read 160 to 320 ms. The engine rate measured against the 68k
EClock read 47790 to 47989 Hz. That spread comes from the EClock comparison
and the status sampling, not from a measured DAC rate.

Zorro II has not been tested. The flag stays clear there until it is.

### Volume ramps

The ramp qualification runs used the combined bench image `8b6cf96e`
(`ramp-fw2`), with the voice renderer and the module-loader fixes, on the
same Zorro III A4000/68060 card. Each run lasted 20 s and measured 1016
rendered periods of 960 frames (20 ms). Every run had 0 late records,
0 rejected records, 0 underruns and 0 dropped records.

Every command below starts with `zz9k-voiceplay LOAD VOICES=32 CHURN`.
Without `STEREO`, the sample is mono; without `LINEAR`, interpolation is
nearest. RATE defaults to 22050 Hz.

| Remaining arguments | Render average (us) | Render maximum (us) |
| --- | ---: | ---: |
| `RAMP=256 STEREO LINEAR` | 2853.80 | 3166.51 |
| `RAMP=256 STEREO LINEAR RATE=96000` | 2861.20 | 3216.21 |
| `RAMP=256` | 1351.51 | 1663.68 |
| `RAMP=256 LINEAR` | 2109.18 | 2451.41 |
| `RAMP=256 STEREO` | 1444.51 | 1786.70 |
| `RAMP=256 GROUPS=1 STEREO LINEAR` | 2840.74 | 3179.44 |
| `RAMP=512 STEREO LINEAR` | 2742.59 | 3082.29 |
| `RAMP=0 STEREO LINEAR` (churn without ramps) | 1440.97 | 1748.10 |

The largest render maximum in these loads was 3216.21 us, below the
5000 us render budget. The firmware now advertises `ZZ9K_VOICE_FEATURE_RAMP`
alongside `ZZ9K_VOICE_FEATURE_CONFIG`. These measurements cover the listed
loads, not every command pattern or later feature combination.

### Limiter timing

The limiter runs used a combined bench image built from the limiter source
(`52ee4096`) plus the mailbox timing instrument (`027b163a`); the image
(`f3fdbf3b`) differs from the limiter source only in bench macros. The card
was the same Zorro III A4000/68060. The speakers were off. Each run lasted
20 s and measured 1016 rendered periods of 960 frames (20 ms), except the
low-latency run, which measured 1006. Every run had 0 late records,
0 rejected records, 0 underruns and 0 dropped records.

Every command below starts with
`zz9k-voiceplay LOAD VOICES=32 SECONDS=20 STEREO LINEAR RATE=96000 RAMP=256 GAIN=65535`.

| Remaining arguments | Render average (us) | Render maximum (us) |
| --- | ---: | ---: |
| `CHURN GROUPS=32 LIMITER` | 2977.79 | 3336.83 |
| `CHURN GROUPS=32` (limiter off) | 2902.56 | 3254.21 |
| `CHURN GROUPS=1 LIMITER LOWLAT` (lead 2880 frames) | 2979.80 | 3335.18 |

The limiter added about 75 us to the average and 83 us to the maximum, and
the largest maximum, 3336.83 us, is below the 5000 us render budget. The
firmware advertises `ZZ9K_VOICE_FEATURE_LIMITER` on this evidence. It covers
the listed loads only. `GAIN=65535` is a saturation test: the loads put the
mix above full scale, which the voiceplay default of 65535 divided by the
voice count never does.

Listening note. The maintainer listened to the load of
`LOAD VOICES=32 STEREO LINEAR RATE=96000 RAMP=256 GAIN=65535`, 10 s without
and 10 s with `LIMITER`, with the amplifier set very low, and heard no clear
difference. The runs had 0 late records, 0 rejected records, 0 underruns and
0 dropped records. This one comparison does not cover other material, levels
or listening setups.

### Low-latency sessions

The same machine, with a low-latency session (lead floor 2880 frames). Each
run lasted 60 s with `zz9k-voiceplay`, click test and 8-channel module.

| Load | Session | Result | Longest firmware poll gap |
| --- | --- | --- | --- |
| Idle | Standard | 0 late, 0 underruns | 3.6 ms |
| Idle | Low latency | 0 late, 0 underruns | 3.6 ms |
| MPEG-2 video in ZZPlay, after its first seconds | Standard | 0 late, 0 underruns | 18 ms |
| MPEG-2 video in ZZPlay, after its first seconds | Low latency | 0 late, 0 underruns | 6.1 ms |
| A Workbench window dragged with the mouse for a minute | Standard | 0 late, 0 underruns | 3.3 ms |

In LIVE mode, where each key starts a note at the earliest frame the engine
has not rendered, every record was 2880 frames (60 ms) ahead of the sound in
a low-latency session and 7680 frames (160 ms) in a standard one. The
maintainer judged the low-latency session tighter when playing from the
keyboard. Of 41 keys in the first low-latency run, 2 notes were late.

One gap remains. While a program opens its window, the firmware once stood
still for about 255 ms in these runs. A low-latency session underruns during
such a pause (48 underruns in one run), a standard session mostly rides it
out. Keep that in mind when a tracker window opens during playback.
