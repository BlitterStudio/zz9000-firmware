# ZZ9000AX audio conversion

The firmware converts PCM between supported source rates and the 48 kHz
stereo period used for playback and capture. `audio_convert.c` provides the
shared fixed-point converter for legacy AHI playback, audio-fabric sources,
SDK-pump preconversion, lease preconversion, and capture. A 48,000 Hz source
uses the bypass path.

## Supported conversions

The converter has generated FIR banks for these rate pairs. A pair not in the
table selects no bank.

| Input rate | Output rate | Use |
|---|---:|---|
| 8,000, 12,000, 24,000, 32,000, 44,100 Hz | 48,000 Hz | Standard playback rates |
| 11,025, 16,000, 22,050, 88,200, 96,000 Hz | 48,000 Hz | Additional source-to-playback rates |
| 48,000 Hz | 8,000, 12,000, 24,000, 32,000, 44,100 Hz | Capture output rates |
| 48,000 Hz | 48,000 Hz | Bypass |

`zz_audio_convert_init()` returns `0` when it selects a bank. For an identity
or other nonzero unlisted pair it returns `1`, leaves `ctx->ratio` null, and
resets the instance. `zz_audio_convert_stream()` then copies the available
frames and clears any remaining output frames. A zero input or output rate
returns `-1`.

The caller decides how an unlisted rate reaches an output path:

- The AHI path writes a silent 48 kHz period.
- The pump writes silence and advances its source cursor.
- The fabric fill path writes a silent period.
- Capture treats an unsupported frame count as native 48 kHz and performs an
  endian swap.

## How conversion works

`util/gen_audio_convert_tables.py` generates
`ZZ9000_proto.sdk/ZZ9000OS/src/audio_convert_tables.c` and
`ZZ9000_proto.sdk/ZZ9000OS/src/audio_convert_tables.h`. The generated banks
use a Kaiser windowed-sinc polyphase FIR with an 80 dB design attenuation, a
passband edge of `0.45 * min(fs_in, fs_out)`, and a stopband edge of
`min(fs_in, fs_out) / 2`.

Each phase uses signed 16-bit coefficients and a Q16 reciprocal. The converter
accumulates in `int64_t`, rounds after normalization, saturates to `int16_t`,
and increments the instance clip counter on saturation. Phase stepping uses
the reduced rational input/output-rate ratio. The FIR reads its input sample
and preceding samples, so the instance retains a history across calls.

`zz_audio_convert_stream()` accepts interleaved stereo `int16_t` frames. Mono
expansion and byte-order conversion occur at the caller. Input and output
buffers MUST NOT overlap when a bank is active. The instance resets its phase,
history, and clip counter through `zz_audio_convert_reset()`; initialization
also resets that state.

## Period rules

The standard AHI geometry is one 20 ms source period to one 960-frame 48 kHz
period. The source-frame counts are 160, 240, 480, 640, 882, and 960 for
8, 12, 24, 32, 44.1, and 48 kHz respectively.

The pump converts 11,025 Hz in 40 ms quanta because 20 ms is 220.5 frames.
One pump conversion reads 441 frames and produces 1,920 frames, which it
writes as two 960-frame periods. Other pump rates use `rate / 50` input frames
per 960-frame output period.

## Capture register contract

Capture receives 960 S16LE stereo frames per 20 ms. Frame counts `160`, `240`,
`480`, `640`, and `882` select conversion from 48 kHz. A count of `960`, a
count outside `1` through `960`, or any other value in that range selects the
48 kHz identity path, which writes S16BE samples.

Capture resets its converter at record start, when the frame-count value
changes, and when the RX buffer is reassigned. Repeating the same frame-count
value keeps the existing history.

## Stream reduction

`AUDIO_STREAM_BEGIN` and `AUDIO_STREAM_BEGIN_EX` accept `output_hz` and
`output_channels` and the firmware advertises `SDK_SERVICE_FLAG_AUDIO_STREAM_REDUCE`
(bit 27). The reduction lives in `audio_reduce.c`, apart from the polyphase
converter above: it runs on S16 PCM as the stream decoder finishes a frame or
a pass, before the PCM enters the stream ring. The filter always sees native
S16 samples: an S16BE stream is swapped after the reduction for MP3 and swapped
around it for FLAC and Vorbis. MP3 frames are reduced from the session's frame buffer.
FLAC and Vorbis write decoder PCM into the ring and `audio_stream_reduce_ring()`
shrinks it in place, one chunk at a time, through the session's frame and
scratch buffers; no pass allocates memory. S32 formats are not reduced and a
non-zero request with one returns `UNSUPPORTED`.

`output_channels` of 1 mixes stereo to mono as `(L + R + 1) >> 1`; 2 copies a
mono stream to both channels. `output_hz` is a maximum: the divisor is the
smallest of 1, 2 and 4 with `stream_rate / divisor <= output_hz`, or 4 when
none fits, and the ring rate is `stream_rate / divisor` rounded to the nearest
integer. Stereo mixdown happens at the input rate, before the filter. A mono
source copied to stereo is filtered once.

Decimation by 2 uses a 59-tap Kaiser half-band FIR (beta 9.0, Q30 taps whose
sum is exactly 2^30, `int64_t` accumulation, rounding and saturation to
`int16_t`). Only 15 tap pairs and the centre tap are non-zero, so a stage costs
16 multiplies per output sample and channel. A divisor of 4 runs two stages
back to back. The taps come from `util/gen_audio_reduce_table.py` into
`audio_reduce_table.h`:

```
python util/gen_audio_reduce_table.py
```

The design edges are a flat passband to 0.20 x the stage input rate (0.40 x its
output rate) and a stop-band from 0.30 x the input rate, the first input
frequency that folds onto the passband edge. `make -C test/audio` runs
`audio_reduce_test`, which measures sine gain through the production code and
compares it with a double-precision model of the same taps. At 48 kHz stereo
the measured figures are:

| Divisor | Worst passband deviation, 0 to 0.40 x output rate | 1 kHz | Weakest alias rejection |
| ---: | ---: | ---: | ---: |
| 2 | 0.0003 dB | 0.0001 dB | 86.7 dB |
| 4 | 0.0003 dB | -0.0006 dB | 87.4 dB |

The rejection figures include the 16-bit rounding floor of the output. The
FIR history and decimation phase belong to the session and persist across
decode calls, so odd-sized passes give the same output as one call; a change
in decoder rate or channel count selects the divisor again and clears the
history. Each stage delays the signal by 29 input samples and the final few
samples of a stream remain in the filter. The decode pass cap of 24 KiB counts
the decoder PCM before reduction, so reduction does not lengthen a pass; the
client's low and high water marks count ring bytes.

## Current status

The generated tables are committed source. Regenerate them with:

```
python util/gen_audio_convert_tables.py
```

`make -C test/audio` builds and runs the converter, capture, AHI playback-rate,
pump-preconversion, and stream reduction host tests along with the other audio
test targets.
The converter tests cover table integrity, response limits, alias and image
rejection, chunked conversion, period geometry, group delay, mono output,
saturation, reset behavior, and the additional source-to-48 kHz banks.

