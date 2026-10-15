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

## Current status

The generated tables are committed source. Regenerate them with:

```
python util/gen_audio_convert_tables.py
```

`make -C test/audio` builds and runs the converter, capture, AHI playback-rate,
and pump-preconversion host tests along with the other audio test targets.
The converter tests cover table integrity, response limits, alias and image
rejection, chunked conversion, period geometry, group delay, mono output,
saturation, reset behavior, and the additional source-to-48 kHz banks.

