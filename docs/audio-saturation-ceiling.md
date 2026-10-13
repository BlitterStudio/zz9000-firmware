# ZZ9000AX saturation ceiling

Firmware starts with a per-card fallback calibration of Paula 48 and AX 80.
If configuration has no baseline, it derives a 36/72 Paula/AX baseline from
that pair. These figures came from one R1 card. A saved per-card calibration
replaces the fallback. The default weighted boundary is 160 AX-equivalent
mixer units.

Firmware clamps each source leg before mixing, then bounds the weighted mix.
The ADAU1701 limiter is enabled at boot with a 0.47 FS threshold. It bounds
the summed output. Distortion in an individual source can occur before that
stage, so the limiter is not a substitute for calibrated per-leg ceilings.

## Default configuration

| Setting | Default | Source |
|---|---:|---|
| Paula clean ceiling | 48 | `AUDIO_SCENE_DEFAULT_CEILING_PAULA` |
| AX clean ceiling | 80 | `AUDIO_SCENE_DEFAULT_CEILING_AX` |
| Paula baseline without a saved baseline | 36 | three quarters of the Paula ceiling |
| AX baseline without a saved baseline | 72 | twice the Paula baseline, capped at the AX ceiling |
| Weighted boundary | 160 | `2 * ceiling_ax` |
| Scene 0 LPF | 23,900 Hz | `default_scenes[0]` |
| Scene 0 EQ and prefactor | 50 | 0 dB |
| Scene 0 volume | 100 | 0 dB |
| Limiter boot threshold | 0.47 FS, Q23 `3942646` | `LIMITER_BOOT_THRESHOLD_Q23` |

The ceiling pair is a conservative first-run setting from one measured R1
card. It is not a clean-output guarantee for every board revision. Firmware
accepts saved ceiling values from 1 through 4095; the mixer legs remain
8-bit values.

## How the boundary is enforced

Let `C_P` and `C_A` be the configured Paula and AX ceilings. Firmware assigns
Paula a weight of `C_A / C_P`, so the weighted mixer level is:

$$W(P, A) = P\frac{C_A}{C_P} + A$$

The enforced boundary is:

$$B = W(C_P, C_A) = 2C_A$$

With the fallback pair, Paula has weight $80/48$, and $B = 160$.

For every mixer update, firmware:

1. Adds each owner's trim to the operator baseline and clamps the result to
   the 0 through 255 mixer range.
2. Clamps the Paula and AX legs independently to `C_P` and `C_A`.
3. Multiplies the weighted result by the scene gain.
4. Reduces both legs proportionally if the result exceeds `B`.

The scene gain is the product of prefactor gain, scene volume, and the
largest positive EQ-band gain. Prefactor and each EQ band use 0 through 100,
where 50 is 0 dB and the endpoints are -12 dB and +12 dB. Scene volume uses
0 through 100, where 100 is 0 dB. EQ cuts do not reduce the calculated gain.

Firmware also evaluates the baseline with the scene gain before it applies
source trims. If that scene-only level exceeds the boundary, it reduces the
applied scene volume. The stored scene value remains unchanged.

## Calibration, persistence, and status

The configuration writer stores a calibrated pair as:

```text
audio_ceiling_paula = <value>
audio_ceiling_ax = <value>
```

Firmware loads the pair only when both keys are present. It stores the
baseline separately as `audio_baseline`. When that key is absent, firmware
uses the parity calculation in the default-configuration table.

`ZZ9K_OP_AUDIO_CONTROL_STATE_GET` reports the active scene, baseline, applied
mixer pair, boundary, per-leg ceilings, and whether the current trim was
bounded. The boundary in that report is calculated by
`audio_scene_enforced_boundary()`.

The SDK ABI comment beside `ZZ9KAudioControlStateResultPayload` still says
that the boundary is three quarters of the AX ceiling. The firmware instead
returns $2C_A$. Consumers should use the returned value rather than that
comment.

`ZZ9K_AUDIO_BALANCE_NEUTRAL` (`0x7f7f`) releases an owner's trim and keeps
the operator baseline. It reserves an absolute 127/127 trim request.

## Measuring a card

Measure the Paula and AX paths independently on the card being calibrated.
Use an external capture path with sufficient attenuation to avoid clipping the
capture ADC. Record the last `CLEAN` step, the first `SATURATED` step, and the
step size for each source. A capture at rail is a setup fault and requires a
new capture with more attenuation.

`util/analyze_audio_saturation.py` reads raw interleaved signed 16-bit PCM.
Its default input is big-endian stereo, 48 kHz, with a 1 kHz tone. For each
selected channel it marks a capture `FAULT` for at-rail regions,
`SATURATED` for three or more flat-top regions or THD above 0.5%, and `CLEAN`
otherwise. With `--reference-channel`, it compares THD rise against the
simultaneous reference channel.

```text
python3 util/analyze_audio_saturation.py --levels <ascending-levels> <captures...>
python3 util/analyze_audio_saturation.py --auto-tone --channel 1 \
    --reference-channel 2 --levels <ascending-levels> <captures...>
```

The analyzer still prints a three-quarter headroom suggestion. That suggestion
is historical analysis output and is not the firmware boundary rule.

## Recorded R1 result

The fallback pair comes from the corrected `a205` session on one R1 card.
These measurements establish the fallback only; they do not qualify another
card or revision.

| Source path | Last clean level | First saturated level | Recorded onset interval | Decisive observation |
|---|---:|---:|---|---|
| AX-only | 80 | 89 | `(80, 89]` | THD rose from about 0.42% at 80 to 0.73% at 89; no capture reached rail. |
| Paula, cross-channel reference | 48 | 56 | `(48, 56]` | Returned-output THD rise was within 0.18 percentage points through 48 and 0.564 points at 56; no capture reached rail. |

The unequal intervals require the weighted model. The 48/80 pair gives Paula
a weight of $80/48$ and the current boundary of 160.

## History

| Period | Finding | Effect on current design |
|---|---|---|
| 2026-08, pre-`a205` sweep | The Paula branch bypassed scene controls in the measured topology. | That sweep was superseded; the corrected `a205` result supplies the 48/80 fallback. |
| 2026-08, early calibration policy | Analysis used a three-quarter headroom suggestion, giving 60 for AX 80. | The current limiter-era implementation uses the weighted sum of both per-leg ceilings, giving 160 for 48/80. |
| Limiter-era firmware | A boot-time limiter threshold of 0.47 FS was added. | Per-leg clamps remain in place because post-mix limiting cannot correct earlier source-path distortion. |
