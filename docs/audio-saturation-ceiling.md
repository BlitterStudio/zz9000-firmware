# ZZ9000AX DAC saturation ceiling and the enforced boundary

The firmware-authoritative control plane (`audio_scene.c`, plan R6/R7)
enforces a gain-staging boundary in combined-level units instead of
the community-forum figure both drivers used to quote. This document
defines the boundary's units, the bench method that replaces the
forum figure with a measured one, the analysis helper, the headroom
policy, the hardware smoke checklist for the verification session, and
the runbook that lands the measured value and the capability
advertising after a recorded pass.

Host tests in `test/audio` enforce the staging math; nothing on this
page is enforced until the hardware session records it.

## The combined-level model

The boundary applies to the composed level the staging code evaluates
(`audio_scene.c`: `resolve_output_volume`, `compute_mixer_stage`):

    combined = (baseline_paula + baseline_ax + sum(owner trims))
               * prefactor_gain * (volume / 100) * eq_worst_boost

- Mixer legs (`audio_adau_set_mixer_vol` scale): 0..255 each,
  127 = 0 dB. Power-on baseline is Paula 128 / AX 64 (the state
  `audio_adau_init` writes).
- Prefactor: 0..100, -12 dB .. +12 dB, 50 = 0 dB
  (`audio_dsp_gain.h`).
- Scene volume: 0..100, 0 dB at 100. Pan does not enter the model.
- EQ: 0..100, ±12 dB per band, 50 = 0 dB; the model uses the single
  worst positive band (`eq_worst_boost_linear`).

`ZZ9K_AUDIO_BALANCE_NEUTRAL` is the keep-baseline request: AHI and
MHI add no owner trim, so their allocation leaves the operator's
baseline pair applied verbatim.

## Provisional enforced boundary (current state)

The single named definition is:

    ZZ9000_proto.sdk/ZZ9000OS/src/audio_scene.h
    #define AUDIO_SCENE_ENFORCED_BOUNDARY 192.0

Provisional derivation: both drivers documented that summed mixer
values above ~0x100 (256) saturate the DAC (`zz9000ax-ahi.c`,
`mhizz9000.c`, since removed); scaled by 3/4 that gives ~2.5 dB of
stated headroom below the forum figure. This value ships until the
bench session below replaces it; `ZZ9K_OP_AUDIO_CONTROL_STATE_GET`
reports it as the `ceiling` field, and the staging host tests pin
behavior against it.

A composition over the boundary is reduced — the mixer sum first
(reported back to the requesting owner as a bound), the applied output
volume when the scene alone exceeds it — and each reduction emits
exactly one gain-reduction telemetry event.

## Measurement method (bench session)

Single session on target hardware, on the operator's own card. The
measurement calibrates the model above in its own units, so the
stimulus must exercise the mixer sum the way the model assumes: both
legs carrying the same tone at the same applied leg value.

Stimulus and routing:

1. **Loopback.** Cable the ZZ9000AX line-out RCA pair to the line-in
   RCA pair. Set both auxiliary jumpers to **IN**; the capture input is
   the fixed-gain `RCA In` path. Add an in-line passive attenuator if
   step 2 shows the capture itself nearing rail.
2. **Level sanity.** At a low combined level (e.g. prefactor -6 dB),
   record and check the capture peaks at or below roughly -6 dBFS and
   the analyzer reports no at-rail samples. At-rail samples in a
   capture mean the ADC clipped, which is a setup fault, not DAC
   saturation — attenuate and restart if this appears at any step.
3. **Tones.** Drive both mixer legs with the same 1 kHz sine at equal
   level: Paula plays a looped 1 kHz sine sample through any Amiga-side
   player; AHI plays a 48 kHz 16-bit stereo 1 kHz sine (full digital
   level, ~0.99 FS to stay off the source rails) through any AHI
   player. Both legs carry the same frequency so the summed signal is
   the worst case the boundary models.
4. **Applied legs.** Before starting playback, set ZZTop's external
   baseline sliders to Paula 127 / AX 127. AHI's keep-baseline trim
   leaves that pair unchanged, so the applied mixer sum stays fixed at
   254 for the primary sweep. Do not Save this temporary bench baseline
   and do not move either baseline slider during the primary sweep.
5. **Scene setup.** Use scene 0 with LPF 23900, all EQ bands 50,
   volume 100, and pan 50. Sweep only the prefactor. Integer slider
   settings map to the model as follows:

   | Prefactor | Combined level |
   |---:|---:|
   | 33 | 158.8 |
   | 37 | 177.4 |
   | 40 | 192.7 |
   | 43 | 209.3 |
   | 46 | 227.4 |
   | 48 | 240.3 |
   | 50 | 254.0 |
   | 52 | 268.4 |
   | 54 | 283.7 |
   | 56 | 299.8 |
   | 58 | 316.8 |
   | 60 | 334.8 |
   | 62 | 353.9 |
   | 64 | 374.0 |
   | 66 | 395.2 |

   Each step is one staged scene-write commit in ZZTop's Audio window
   (glitch-free; playback keeps running). Record at least 2 seconds per
   step to a raw capture file named by its rounded combined level, and
   watch the Audio-window meters as a live cross-check.
6. **Model cross-check.** Reach onset-relevant combined values a
   second way through the mixer legs at unity chain: restore prefactor
   50 and volume 100, then set equal baseline pairs. Examples are
   80/80 = 160, 96/96 = 192, 112/112 = 224, 128/128 = 256, and
   160/160 = 320. Because the current AHI trim is keep-baseline, no
   historical owner-delta adjustment is required. If onset metrics
   agree at the same combined value reached both ways, saturation is
   level-dependent at the DAC and the combined-level model is the
   right currency. A disagreement places the limit at the mixer
   summing node and blocks any constant update. Record both
   observations.
7. **Detection.** Analyze the captures on the host with
   `util/analyze_audio_saturation.py` (below) in sweep order with the
   combined levels. The measured ceiling is the highest combined level
   whose capture is CLEAN; the true onset lies in the interval between
   it and the first non-clean step, so record both values plus the step
   size.
8. **Record.** Append the measurement to this file: measured ceiling,
   onset interval, sweep step size, loopback attenuation, capture rate,
   the cross-check result, the analyzer invocation, and the firmware
   commit measured.

The ZZTop meters are a live complement, not the detector: they see the
digital-domain rail (`audio_scene.c` meter accumulators count at-rail
regions of the int16 path), while the capture THD/flat-top analysis
sees the analog DAC output through the loopback. Both are worth
watching during the sweep; only the capture analysis decides the
ceiling.

## Analysis helper

    python3 util/analyze_audio_saturation.py --levels 160,176,192,... \
        cap_0160.raw cap_0176.raw cap_0192.raw ...
    python3 util/test_analyze_audio_saturation.py   # self-check prints PASS

Reads raw signed-16-bit PCM (`--endian be` for Amiga-side AHI dumps,
`--endian le` for host-side captures; `--channels`, `--rate`,
`--tone`, `--rail`, `--thd-pct` adjustable). Per channel it reports:

| Metric | Meaning |
|---|---|
| peak dBFS | captured peak (attenuation makes absolute level meaningless; trend only) |
| dc | mean offset |
| at-rail samples/regions | capture-path clipping — a setup fault (add loopback attenuation) |
| flat-top regions (longest run) | consecutive samples parked within ~0.4% of the file's own peak — the clipped/hard-saturated crest signature, scale-invariant |
| THD proxy | coherent H2..H5 against the fundamental over an integer-period window — catches soft saturation before hard clipping |
| notch residual dBFS | floor after removing DC, fundamental and harmonics |

Verdicts: `FAULT` (at-rail: fix the setup), `SATURATED` (flat-top or
THD above `--thd-pct`, default 0.5%), `CLEAN`. With `--levels` the
tool reports the onset step, the measured ceiling (last clean level),
the onset interval, and the suggested `AUDIO_SCENE_ENFORCED_BOUNDARY`
at the 3/4 headroom policy. `--json` emits the same report
machine-readably.

## Headroom policy and unit-to-unit variance

The bench measures one card. The enforced boundary carries 3/4
headroom (~2.5 dB) below the measured ceiling, the same policy the
provisional value used, chosen to cover expected component variance
across boards (mixer summing-stage and codec gain tolerances). This
single-unit scope and the variance rationale are adopted as-is for
this release; if field reports ever show cards whose clean ceiling
sits inside another card's headroom band, re-measure on a second unit
and widen the policy deliberately rather than silently.

## Hardware smoke checklist (verification session)

Run on the release-candidate bench build from the runbook below. No
item here is done until the session records its observation; each
expected observation traces to the plan's acceptance examples.

| # | Item | Steps | Expected observation |
|---|---|---|---|
| S1 | Boot/warm-reset scene apply (AE5) | Save a distinctive scene (e.g. LPF 12 kHz, prefactor 45, volume 70), power-cycle, then warm-reset with Ctrl-Amiga-Amiga | After both resets the master chain returns to the saved scene, not DSP defaults, before any app allocates; ZZTop's Audio window shows the saved scene active |
| S2 | AHI/MHI handoff, no stamps (AE3) | Play via MHI, stop, then allocate AHI and play | Master chain stays scene-owned throughout: no 20 kHz LPF appears at MHI Play start, no mixer stamp at AHI allocate; audio uninterrupted |
| S3 | Live switch glitch-free (R5/F3) | Switch scenes and commit scene edits during active playback | No interruption, click, or partial assignment; rapid double-switches serialize (no torn state) |
| S4 | Clamp event visible (AE1) | Request a trim that, with baseline and scene level, exceeds the enforced boundary | Applied trim is bounded and the requester can observe it; ZZTop's gain-reduction indicator lights and the event (requested/applied/boundary) is readable from telemetry |
| S5 | Meters update (AE4) | Play a deliberately hot signal; read meters between periods from the Audio window | Peak-hold and clip count reflect the signal; repeated reads are self-consistent; no audible or timing disturbance from reading |
| S6 | Save round-trip through reboot (AE6) | Set operator baseline balance (e.g. 100/90), Save, cold boot | Baseline and scenes persist and apply to every owner's playback with no env var anywhere |
| S7 | Leftover `ZZ9K_MIX_LEVELS` no effect (AE2) | Set `ENV:ZZ9K_MIX_LEVELS` to an extreme pair, keep CFG audio keys present, boot | Scene applies; the env var changes nothing in either driver or firmware |
| S8 | Non-AX entry disabled (R18) | On a ZZ9000 without the AX daughterboard, open ZZTop | The Audio button is present but disabled (greyed), not hidden; all other ZZTop features unaffected |

S8 needs non-AX hardware (or the AX-absent probe path); if the session
only has an AX card, record it as not exercised rather than passed.

## Flip-after-pass runbook

The capability flip lands only in the commit following a recorded
hardware pass (plan R12/KTD6). Until then nothing advertises
`ZZ9K_CAP_AUDIO_CONTROL` or `ZZ9K_CAP_AUDIO_METERING`, and ZZTop's
Audio button stays disabled on every matched set — discoverable, not
hidden.

Bench builds (local working-tree edits, never committed as-is):

- **Instrument build** (ceiling measurement): set
  `AUDIO_SCENE_ENFORCED_BOUNDARY` to `4096.0` (above any reachable
  composition, so staging never reduces during the sweep) **and** apply
  the advertising flip below, so ZZTop's Audio window and meters are
  live for the sweep. Build with `./build_firmware.sh` and flash.
- **Release-candidate build** (smoke checklist): restore the working
  tree, then set `AUDIO_SCENE_ENFORCED_BOUNDARY` to the measured
  ceiling x 3/4 **and** apply the advertising flip. Re-flash and run
  S1-S8.

After a recorded pass, in this order:

1. **Constant commit.** Restore the working tree completely. Update the
   single site `ZZ9000_proto.sdk/ZZ9000OS/src/audio_scene.h`
   (`#define AUDIO_SCENE_ENFORCED_BOUNDARY`) to the measured boundary,
   re-derive the boundary-dependent fixtures that pin `192` literals
   (`test/audio/audio_scene_test.c` staging/event/headroom checks,
   `test/audio/audio_control_test.c` ceiling report), re-run
   `make -C test/audio test` and `make -C test/config test`, and append
   the measurement record to this document.
2. **Advertising commit.** In `ZZ9000_proto.sdk/ZZ9000OS/src/sdk_mailbox.c`
   only:
   - `SDK_MAILBOX_BASE_CAPABILITY_BITS` gains
     `| SDK_CAP_AUDIO_CONTROL | SDK_CAP_AUDIO_METERING` — this word is
     what `SDK_OP_QUERY_CAPS` returns via `mailbox_capability_bits()`,
     and what ZZTop, AHI and MHI gate on.
   - The `SDK_SERVICE_AUDIO` entry in `sdk_services[]` gains the same
     two caps in its capability field, gains
     `| SDK_SERVICE_FLAG_AUDIO_CONTROL` (bit 21) in its flags field,
     and its opcode count rises from `9` to `15` so `QUERY_SERVICE`
     reports the full `0x0500..0x050e` range including the control
     plane.
   Optional same-commit polish: add `"audio-control"` /
   `"audio-metering"` names for the two bits to
   `zz9k_capability_name()` in the SDK's `include/zz9k/caps.h` so
   `zz9k-services` prints them.
3. Both commits run `git diff --check` clean; firmware CI green.

## Matched-set release coordination

- The drivers' `sdk/SDK_REF` pins SDK commit `2873f1d`
  (`feat(abi): define audio control-plane and metering opcodes and
  caps`), which currently exists **only on the SDK's local branch**
  `feat/audio-control-plane-scenes-metering`. That branch must be
  pushed and merged before drivers CI runs on fresh runners — the `sdk`
  and `amissl` jobs clone the pinned commit, and the ZZTop/AHI build
  scripts hard-fail when the staged headers lack
  `ZZ9K_OP_AUDIO_SCENE_SELECT` / `ZZ9K_OP_AUDIO_TRIM_SUBMIT`
  (`ZZTop/build-gcc.sh`, `ahi/driver/build.sh`).
- Tag order is the drivers' `RELEASING.md` dependency order: SDK first
  (merge + tag + push), then drivers `SDK_REF` bump to the SDK release
  commit (CI green on the pin), then firmware, then drivers. Firmware
  and drivers carry the same `vX.Y.Z`.
- ZZTop's Audio window, both drivers' capability gates, and their
  warning strings already reference the new surface; they activate the
  moment the advertising commit's firmware ships in the matched set.
  Mixed-version pairings degrade per the AHI README: old firmware +
  new drivers means no trim and a disabled Audio button (normal, not an
  error); new firmware + old drivers means the old stamps are rejected
  by the authority gate and playback continues unaffected.
